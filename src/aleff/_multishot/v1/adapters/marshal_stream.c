#include "marshal_stream.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"
#include "marshal_reader.h"

/* CPython's marshal.load uses a temporary RFILE buffer for every read.  That
 * buffer belongs to the C call stack and cannot be used by a restored Python
 * frame.  ReadBuffer is consequently independent of RFILE and is retained by
 * every continuation state which has exposed its memoryview to Python. */
typedef struct MarshalReadBuffer MarshalReadBuffer;

struct MarshalReadBuffer {
    _Atomic unsigned int references;
    size_t size;
    unsigned char data[];
};

typedef enum {
    MARSHAL_STREAM_READY,
    MARSHAL_STREAM_WAIT_READ,
    MARSHAL_STREAM_WAIT_READINTO,
    /* The readinto call has returned.  A later continuation at this phase is
     * the result of PyNumber_AsSsize_t/__index__, not the readinto result. */
    MARSHAL_STREAM_AFTER_READINTO,
    MARSHAL_STREAM_DONE,
} MarshalStreamPhase;

struct AleffMarshalStream {
    PyObject *load;
    PyObject *loads;
    PyObject *reader;

    unsigned char *data;
    size_t size;
    size_t capacity;

    MarshalReadBuffer *pending;
    unsigned char *pending_snapshot;
    size_t pending_size;
    MarshalStreamPhase phase;
    int allow_code;
    int running;
};

typedef struct {
    PyObject_HEAD
    AleffMarshalStream *stream;
} MarshalStreamProxy;

static const AleffAdapterVTable marshal_stream_vtable;

static void
marshal_stream_free_state(void *raw_state);

static MarshalReadBuffer *
marshal_read_buffer_new(size_t size)
{
    if (size > SIZE_MAX - sizeof(MarshalReadBuffer)) {
        PyErr_NoMemory();
        return NULL;
    }
    MarshalReadBuffer *buffer = PyMem_Malloc(
        sizeof(*buffer) + size
    );
    if (buffer == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    atomic_init(&buffer->references, 1);
    buffer->size = size;
    if (size != 0) {
        memset(buffer->data, 0, size);
    }
    return buffer;
}

static void
marshal_read_buffer_retain(MarshalReadBuffer *buffer)
{
    if (buffer != NULL) {
        atomic_fetch_add_explicit(
            &buffer->references,
            1,
            memory_order_relaxed
        );
    }
}

static void
marshal_read_buffer_release(MarshalReadBuffer *buffer)
{
    if (buffer != NULL && atomic_fetch_sub_explicit(
            &buffer->references,
            1,
            memory_order_acq_rel
        ) == 1) {
        PyMem_Free(buffer);
    }
}

static void
marshal_stream_clear_pending(AleffMarshalStream *stream)
{
    marshal_read_buffer_release(stream->pending);
    stream->pending = NULL;
    PyMem_Free(stream->pending_snapshot);
    stream->pending_snapshot = NULL;
    stream->pending_size = 0;
}

static int
marshal_stream_reserve(AleffMarshalStream *stream, size_t additional)
{
    if (additional > SIZE_MAX - stream->size) {
        PyErr_NoMemory();
        return -1;
    }
    size_t needed = stream->size + additional;
    if (needed <= stream->capacity) {
        return 0;
    }

    size_t capacity = stream->capacity == 0 ? 64 : stream->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    unsigned char *data = PyMem_Realloc(stream->data, capacity);
    if (data == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    stream->data = data;
    stream->capacity = capacity;
    return 0;
}

static int
marshal_stream_append_pending(
    AleffMarshalStream *stream,
    Py_ssize_t count
)
{
    if (count != (Py_ssize_t)stream->pending_size) {
        if (count > (Py_ssize_t)stream->pending_size) {
            PyErr_Format(
                PyExc_ValueError,
                "read() returned too much data: %zd bytes requested, %zd returned",
                (Py_ssize_t)stream->pending_size,
                count
            );
        }
        else {
            PyErr_SetString(PyExc_EOFError, "EOF read where not expected");
        }
        return -1;
    }
    if (marshal_stream_reserve(stream, stream->pending_size) < 0) {
        return -1;
    }
    if (stream->pending_size != 0) {
        memcpy(
            stream->data + stream->size,
            stream->pending->data,
            stream->pending_size
        );
    }
    stream->size += stream->pending_size;
    marshal_stream_clear_pending(stream);
    stream->phase = MARSHAL_STREAM_READY;
    return 0;
}

static int
marshal_stream_accept_index_result(
    AleffMarshalStream *stream,
    PyObject *value
)
{
    if (!PyLong_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "__index__ returned non-int (type %.200s)",
            Py_TYPE(value)->tp_name
        );
        return -1;
    }
    Py_ssize_t count = PyNumber_AsSsize_t(value, PyExc_ValueError);
    if (count == -1 && PyErr_Occurred()) {
        return -1;
    }
    return marshal_stream_append_pending(stream, count);
}

static int
marshal_stream_accept_readinto_result(
    AleffMarshalStream *stream,
    PyObject *value
)
{
    /* This conversion is deliberately performed while the phase says
     * AFTER_READINTO.  If __index__ suspends, the next adapter resume can
     * therefore distinguish its result from the readinto return itself. */
    stream->phase = MARSHAL_STREAM_AFTER_READINTO;
    Py_ssize_t count = PyNumber_AsSsize_t(value, PyExc_ValueError);
    if (count == -1 && PyErr_Occurred()) {
        return -1;
    }
    return marshal_stream_append_pending(stream, count);
}

static PyObject *
marshal_stream_call_readinto(AleffMarshalStream *stream)
{
    if (stream->pending == NULL) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "marshal stream has no pending readinto buffer"
        );
        return NULL;
    }
    PyObject *view = PyMemoryView_FromMemory(
        (char *)stream->pending->data,
        (Py_ssize_t)stream->pending->size,
        PyBUF_WRITE
    );
    if (view == NULL) {
        return NULL;
    }
    PyObject *result = PyObject_CallMethod(
        stream->reader,
        "readinto",
        "O",
        view
    );
    Py_DECREF(view);
    return result;
}

static PyObject *
marshal_stream_start_read(AleffMarshalStream *stream, size_t size)
{
    if (size > (size_t)PY_SSIZE_T_MAX) {
        PyErr_SetString(PyExc_OverflowError, "marshal read size is too large");
        return NULL;
    }
    marshal_stream_clear_pending(stream);
    stream->pending = marshal_read_buffer_new(size);
    if (stream->pending == NULL) {
        return NULL;
    }
    stream->pending_size = size;
    stream->phase = MARSHAL_STREAM_WAIT_READINTO;
    PyObject *result = marshal_stream_call_readinto(stream);
    if (result == NULL) {
        return NULL;
    }
    stream->phase = MARSHAL_STREAM_AFTER_READINTO;
    return result;
}

static PyObject *
marshal_stream_call_loads(AleffMarshalStream *stream, size_t boundary)
{
    if (boundary > (size_t)PY_SSIZE_T_MAX || boundary > stream->size) {
        PyErr_SetString(PyExc_RuntimeError, "invalid marshal reader boundary");
        return NULL;
    }
    PyObject *data = PyBytes_FromStringAndSize(
        (const char *)stream->data,
        (Py_ssize_t)boundary
    );
    if (data == NULL) {
        return NULL;
    }

    PyObject *args = PyTuple_Pack(1, data);
    Py_DECREF(data);
    if (args == NULL) {
        return NULL;
    }
    PyObject *kwargs = NULL;
#if PY_VERSION_HEX >= 0x030d0000
    kwargs = PyDict_New();
    if (kwargs == NULL || PyDict_SetItemString(
            kwargs,
            "allow_code",
            stream->allow_code ? Py_True : Py_False
        ) < 0) {
        Py_DECREF(args);
        Py_XDECREF(kwargs);
        return NULL;
    }
#endif
    PyObject *result = PyObject_Call(stream->loads, args, kwargs);
    Py_DECREF(args);
    Py_XDECREF(kwargs);
    return result;
}

static PyObject *
marshal_stream_drive(AleffMarshalStream *stream)
{
    for (;;) {
        AleffMarshalReaderResult examined = aleff_marshal_reader_examine(
            stream->size == 0 ? NULL : stream->data,
            stream->size,
            stream->allow_code
        );
        if (examined.status != ALEFF_MARSHAL_READER_NEED_READ) {
            stream->phase = MARSHAL_STREAM_DONE;
            return marshal_stream_call_loads(stream, examined.boundary);
        }

        PyObject *read_result = marshal_stream_start_read(
            stream,
            examined.next_read_size
        );
        if (read_result == NULL) {
            return NULL;
        }
        /* The first-shot proxy must return the readinto result to CPython;
         * resumed shots consume it here and never re-enter that parser. */
        int status = marshal_stream_accept_readinto_result(
            stream,
            read_result
        );
        Py_DECREF(read_result);
        if (status < 0) {
            return NULL;
        }
        /* Continue examining the newly completed exact prefix. */
    }
}

static PyObject *
marshal_stream_resume(const void *raw_state, PyObject *value)
{
    const AleffMarshalStream *source = raw_state;
    AleffMarshalStream *stream = PyMem_Calloc(1, sizeof(*stream));
    if (stream == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *stream = *source;
    stream->load = Py_NewRef(source->load);
    stream->loads = Py_NewRef(source->loads);
    stream->reader = Py_NewRef(source->reader);
    stream->data = NULL;
    stream->pending = source->pending;
    stream->pending_snapshot = NULL;
    marshal_read_buffer_retain(stream->pending);
    if (source->size != 0) {
        stream->data = PyMem_Malloc(source->size);
        if (stream->data == NULL) {
            PyErr_NoMemory();
            marshal_stream_free_state(stream);
            return NULL;
        }
        memcpy(stream->data, source->data, source->size);
    }
    stream->capacity = source->size;

    if (value == NULL) {
        marshal_stream_free_state(stream);
        return NULL;
    }

    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &marshal_stream_vtable, stream) < 0) {
        marshal_stream_free_state(stream);
        return NULL;
    }

    PyObject *result = NULL;
    if (stream->phase == MARSHAL_STREAM_WAIT_READ) {
        if (!PyBytes_Check(value)) {
            PyErr_Format(
                PyExc_TypeError,
                "file.read() returned not bytes but %.100s",
                Py_TYPE(value)->tp_name
            );
        }
        else {
            result = marshal_stream_drive(stream);
        }
    }
    else if (stream->phase == MARSHAL_STREAM_WAIT_READINTO) {
        if (marshal_stream_accept_readinto_result(stream, value) == 0) {
            result = marshal_stream_drive(stream);
        }
    }
    else if (stream->phase == MARSHAL_STREAM_AFTER_READINTO) {
        if (marshal_stream_accept_index_result(stream, value) == 0) {
            result = marshal_stream_drive(stream);
        }
    }
    else {
        PyErr_SetString(PyExc_RuntimeError, "invalid marshal stream resume phase");
    }

    adapter_leave(&frame);
    marshal_stream_free_state(stream);
    return result;
}

static void *
marshal_stream_copy_state(const void *raw_state)
{
    const AleffMarshalStream *source = raw_state;
    AleffMarshalStream *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->load = Py_NewRef(source->load);
    copy->loads = Py_NewRef(source->loads);
    copy->reader = Py_NewRef(source->reader);
    copy->data = NULL;
    copy->pending = source->pending;
    copy->pending_snapshot = NULL;
    marshal_read_buffer_retain(copy->pending);
    if (source->size != 0) {
        copy->data = PyMem_Malloc(source->size);
        if (copy->data == NULL) {
            PyErr_NoMemory();
            marshal_stream_free_state(copy);
            return NULL;
        }
        memcpy(copy->data, source->data, source->size);
    }
    copy->capacity = source->size;
    if (source->pending != NULL && source->pending_size != 0) {
        copy->pending_snapshot = PyMem_Malloc(source->pending_size);
        if (copy->pending_snapshot == NULL) {
            PyErr_NoMemory();
            marshal_stream_free_state(copy);
            return NULL;
        }
        memcpy(
            copy->pending_snapshot,
            source->pending_snapshot == NULL
                ? source->pending->data
                : source->pending_snapshot,
            source->pending_size
        );
    }
    return copy;
}

static int
marshal_stream_prepare_resume(void *raw_state)
{
    AleffMarshalStream *stream = raw_state;
    if (stream->pending_snapshot == NULL) {
        return 0;
    }
    if (stream->pending == NULL ||
        stream->pending->size != stream->pending_size) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "marshal readinto buffer changed size"
        );
        return -1;
    }
    memcpy(
        stream->pending->data,
        stream->pending_snapshot,
        stream->pending_size
    );
    return 0;
}

static void
marshal_stream_free_state(void *raw_state)
{
    AleffMarshalStream *stream = raw_state;
    if (stream == NULL) {
        return;
    }
    Py_XDECREF(stream->reader);
    Py_XDECREF(stream->loads);
    Py_XDECREF(stream->load);
    marshal_read_buffer_release(stream->pending);
    PyMem_Free(stream->pending_snapshot);
    PyMem_Free(stream->data);
    PyMem_Free(stream);
}

static const AleffAdapterVTable marshal_stream_vtable = {
    .copy_state = marshal_stream_copy_state,
    .free_state = marshal_stream_free_state,
    .resume = marshal_stream_resume,
    .prepare_resume = marshal_stream_prepare_resume,
};

static PyObject *
marshal_stream_proxy_read(PyObject *object, PyObject *args)
{
    Py_ssize_t size;
    if (!PyArg_ParseTuple(args, "n:read", &size)) {
        return NULL;
    }
    MarshalStreamProxy *self = (MarshalStreamProxy *)object;
    AleffMarshalStream *stream = self->stream;
    stream->phase = MARSHAL_STREAM_WAIT_READ;
    PyObject *result = PyObject_CallMethod(stream->reader, "read", "n", size);
    if (result != NULL) {
        stream->phase = MARSHAL_STREAM_READY;
    }
    return result;
}

static PyObject *
marshal_stream_proxy_readinto(PyObject *object, PyObject *argument)
{
    MarshalStreamProxy *self = (MarshalStreamProxy *)object;
    AleffMarshalStream *stream = self->stream;
    if (!PyMemoryView_Check(argument)) {
        PyErr_SetString(PyExc_TypeError, "readinto expects a memoryview");
        return NULL;
    }
    Py_buffer *target = PyMemoryView_GET_BUFFER(argument);
    if (target->len < 0) {
        PyErr_SetString(PyExc_ValueError, "negative readinto buffer size");
        return NULL;
    }

    if (stream->phase == MARSHAL_STREAM_AFTER_READINTO &&
        stream->pending != NULL &&
        marshal_stream_append_pending(
            stream,
            (Py_ssize_t)stream->pending_size
        ) < 0) {
        return NULL;
    }
    marshal_stream_clear_pending(stream);
    stream->pending = marshal_read_buffer_new((size_t)target->len);
    if (stream->pending == NULL) {
        return NULL;
    }
    stream->pending_size = (size_t)target->len;
    stream->phase = MARSHAL_STREAM_WAIT_READINTO;
    PyObject *result = marshal_stream_call_readinto(stream);
    if (result == NULL) {
        return NULL;
    }
    stream->phase = MARSHAL_STREAM_AFTER_READINTO;

    /* Do not run __index__ here.  CPython must retain ownership of the
     * readinto return conversion on the first shot.  The complete temporary
     * buffer is copied because CPython may consume it only after converting a
     * non-exact integer return. */
    if (target->len != 0) {
        memcpy(target->buf, stream->pending->data, (size_t)target->len);
    }
    if (PyLong_CheckExact(result)) {
        Py_ssize_t count = PyLong_AsSsize_t(result);
        if (!(count == -1 && PyErr_Occurred())) {
            if (count == (Py_ssize_t)stream->pending_size) {
                if (marshal_stream_reserve(stream, stream->pending_size) < 0) {
                    Py_DECREF(result);
                    return NULL;
                }
                memcpy(
                    stream->data + stream->size,
                    stream->pending->data,
                    stream->pending_size
                );
                stream->size += stream->pending_size;
            }
            marshal_stream_clear_pending(stream);
            stream->phase = MARSHAL_STREAM_READY;
        }
        else {
            PyErr_Clear();
        }
    }
    return result;
}

static PyMethodDef marshal_stream_proxy_methods[] = {
    {"read", marshal_stream_proxy_read, METH_VARARGS, NULL},
    {"readinto", marshal_stream_proxy_readinto, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot marshal_stream_proxy_slots[] = {
    {Py_tp_methods, (void *)marshal_stream_proxy_methods},
    {0, NULL},
};

static PyType_Spec marshal_stream_proxy_spec = {
    .name = "aleff._marshal_stream_proxy",
    .basicsize = sizeof(MarshalStreamProxy),
    .flags = Py_TPFLAGS_DEFAULT,
    .slots = marshal_stream_proxy_slots,
};

AleffMarshalStream *
aleff_marshal_stream_new(
    PyObject *load,
    PyObject *loads,
    PyObject *reader,
    int allow_code
)
{
    if (load == NULL || loads == NULL || reader == NULL) {
        PyErr_SetString(PyExc_TypeError, "marshal stream arguments must not be NULL");
        return NULL;
    }
    if (!PyCallable_Check(load) || !PyCallable_Check(loads)) {
        PyErr_SetString(PyExc_TypeError, "marshal stream functions must be callable");
        return NULL;
    }
    AleffMarshalStream *stream = PyMem_Calloc(1, sizeof(*stream));
    if (stream == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    stream->load = Py_NewRef(load);
    stream->loads = Py_NewRef(loads);
    stream->reader = Py_NewRef(reader);
    stream->allow_code = allow_code != 0;
    stream->phase = MARSHAL_STREAM_READY;
    return stream;
}

PyObject *
aleff_marshal_stream_run(AleffMarshalStream *stream)
{
    if (stream == NULL || stream->running) {
        PyErr_SetString(PyExc_RuntimeError, "marshal stream is already running");
        return NULL;
    }
    stream->running = 1;

    PyObject *proxy_type = PyType_FromSpec(&marshal_stream_proxy_spec);
    if (proxy_type == NULL) {
        return NULL;
    }
    MarshalStreamProxy *proxy = PyObject_New(MarshalStreamProxy, (PyTypeObject *)proxy_type);
    Py_DECREF(proxy_type);
    if (proxy == NULL) {
        return NULL;
    }
    proxy->stream = stream;

    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &marshal_stream_vtable, stream) < 0) {
        Py_DECREF((PyObject *)proxy);
        return NULL;
    }
    PyObject *args = PyTuple_Pack(1, proxy);
    Py_DECREF((PyObject *)proxy);
    if (args == NULL) {
        adapter_leave(&frame);
        return NULL;
    }
    PyObject *kwargs = NULL;
#if PY_VERSION_HEX >= 0x030d0000
    kwargs = PyDict_New();
    if (kwargs == NULL || PyDict_SetItemString(
            kwargs,
            "allow_code",
            stream->allow_code ? Py_True : Py_False
        ) < 0) {
        Py_DECREF(args);
        Py_XDECREF(kwargs);
        adapter_leave(&frame);
        return NULL;
    }
#endif
    PyObject *result = PyObject_Call(stream->load, args, kwargs);
    Py_DECREF(args);
    Py_XDECREF(kwargs);
    adapter_leave(&frame);
    return result;
}

void
aleff_marshal_stream_free(AleffMarshalStream *stream)
{
    marshal_stream_free_state(stream);
}
