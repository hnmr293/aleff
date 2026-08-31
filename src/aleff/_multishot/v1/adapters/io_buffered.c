#include "io_buffered.h"

#ifndef MS_WINDOWS
#  include <sys/types.h>
#endif
#include <stddef.h>
#include <string.h>

#ifdef MS_WINDOWS
typedef long long AleffBufferedOff_t;
#else
typedef off_t AleffBufferedOff_t;
#endif

static AleffBufferedOff_t
buffered_as_off_t(PyObject *value)
{
    PyObject *index = PyNumber_Index(value);
    if (index == NULL) return (AleffBufferedOff_t)-1;
    long long result = PyLong_AsLongLong(index);
    Py_DECREF(index);
    if (result == -1 && PyErr_Occurred()) return (AleffBufferedOff_t)-1;
    return (AleffBufferedOff_t)result;
}

static PyObject *
buffered_from_off_t(AleffBufferedOff_t value)
{
    return PyLong_FromLongLong((long long)value);
}

/*
 * CPython deliberately keeps this structure private.  The prefix is stable
 * in 3.12, 3.13, 3.14, and their free-threaded builds; keeping the mirror
 * here lets a continuation restore the state that the missing C frame would
 * have updated.  No instance is registered anywhere: every copy belongs to
 * one adapter snapshot.
 */
typedef struct {
    PyObject_HEAD
    PyObject *raw;
    int ok;
    int detached;
    int readable;
    int writable;
    char finalizing;
    int fast_closed_checks;
    AleffBufferedOff_t abs_pos;
    char *buffer;
    AleffBufferedOff_t pos;
    AleffBufferedOff_t raw_pos;
    AleffBufferedOff_t read_end;
    AleffBufferedOff_t write_pos;
    AleffBufferedOff_t write_end;
    PyThread_type_lock lock;
    volatile unsigned long owner;
    Py_ssize_t buffer_size;
    Py_ssize_t buffer_mask;
    PyObject *dict;
    PyObject *weakreflist;
} BufferedObject;

typedef struct {
    PyObject_HEAD
    BufferedObject *reader;
    BufferedObject *writer;
    PyObject *dict;
    PyObject *weakreflist;
} BufferedRWPairObject;

typedef enum {
    BUFFERED_READ,
    BUFFERED_READ1,
    BUFFERED_READINTO,
    BUFFERED_READINTO1,
    BUFFERED_PEEK,
    BUFFERED_WRITE,
    BUFFERED_SEEK,
    BUFFERED_TELL,
    BUFFERED_FLUSH,
    BUFFERED_CLOSE,
    BUFFERED_PAIR_CLOSE,
} BufferedOperation;

typedef enum {
    BUFFERED_RAW_READ,
    BUFFERED_RAW_READINTO,
    BUFFERED_RAW_WRITE,
    BUFFERED_RAW_SEEK,
    BUFFERED_RAW_TELL,
    BUFFERED_RAW_FLUSH,
    BUFFERED_RAW_CLOSE,
    BUFFERED_PAIR_WRITER,
    BUFFERED_PAIR_READER,
    BUFFERED_DONE,
} BufferedPhase;

typedef struct {
    int valid;
    int lock_held;
    int ok;
    int detached;
    int readable;
    int writable;
    char finalizing;
    int fast_closed_checks;
    AleffBufferedOff_t abs_pos;
    AleffBufferedOff_t pos;
    AleffBufferedOff_t raw_pos;
    AleffBufferedOff_t read_end;
    AleffBufferedOff_t write_pos;
    AleffBufferedOff_t write_end;
    Py_ssize_t buffer_size;
    Py_ssize_t buffer_mask;
    char *buffer;
} BufferedObjectSnapshot;

typedef struct {
    PyObject *original;
    PyObject *receiver;
    PyObject *arguments;
    BufferedOperation operation;
    BufferedPhase phase;
    BufferedPhase after_phase;
    BufferedPhase after_write_phase;
    BufferedObjectSnapshot initial;
    BufferedObjectSnapshot resume;
    PyObject *result;
    PyObject *chunks;
    PyObject *target_bytes;
    PyObject *pending_exception;
    PyObject *raw_state;
    PyObject *direct_view;
    PyObject *direct_owner;
    Py_ssize_t total;
    Py_ssize_t written;
    Py_ssize_t raw_length;
    int raw_direct;
    int one_raw_read;
    int source_is_buffer;
    int seek_rewind;
    int live_state;
} BufferedCallState;

typedef struct {
    PyTypeObject *type;
    const char *name;
    PyObject *original;
} BufferedMethodBackup;

static const AleffAdapterVTable buffered_call_vtable;
static PyObject *installed_io;
static PyObject *buffered_types[4];
static BufferedMethodBackup backups[48];
static PyMethodDef replacement_methods[48];
static Py_ssize_t backup_count;
static int buffered_installed;

static BufferedObject *
buffered_object(PyObject *object)
{
    for (PyObject **type = buffered_types;
         type < buffered_types + 3;
         type++) {
        if (*type != NULL && PyObject_TypeCheck(object, (PyTypeObject *)*type)) {
            return (BufferedObject *)object;
        }
    }
    return NULL;
}

static BufferedRWPairObject *
buffered_pair_object(PyObject *object)
{
    if (buffered_types[3] != NULL &&
        PyObject_TypeCheck(object, (PyTypeObject *)buffered_types[3])) {
        return (BufferedRWPairObject *)object;
    }
    return NULL;
}

static void
buffered_snapshot_clear(BufferedObjectSnapshot *snapshot)
{
    PyMem_Free(snapshot->buffer);
    memset(snapshot, 0, sizeof(*snapshot));
}

static int
buffered_snapshot_capture(
    BufferedObjectSnapshot *snapshot,
    BufferedObject *object
)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->valid = 1;
    snapshot->lock_held = object->owner != 0;
    snapshot->ok = object->ok;
    snapshot->detached = object->detached;
    snapshot->readable = object->readable;
    snapshot->writable = object->writable;
    snapshot->finalizing = object->finalizing;
    snapshot->fast_closed_checks = object->fast_closed_checks;
    snapshot->abs_pos = object->abs_pos;
    snapshot->pos = object->pos;
    snapshot->raw_pos = object->raw_pos;
    snapshot->read_end = object->read_end;
    snapshot->write_pos = object->write_pos;
    snapshot->write_end = object->write_end;
    snapshot->buffer_size = object->buffer_size;
    snapshot->buffer_mask = object->buffer_mask;
    if (object->buffer != NULL && object->buffer_size > 0) {
        snapshot->buffer = PyMem_Malloc((size_t)object->buffer_size);
        if (snapshot->buffer == NULL) {
            buffered_snapshot_clear(snapshot);
            PyErr_NoMemory();
            return -1;
        }
        memcpy(snapshot->buffer, object->buffer, (size_t)object->buffer_size);
    }
    return 0;
}

static int
buffered_snapshot_copy(
    BufferedObjectSnapshot *destination,
    const BufferedObjectSnapshot *source
)
{
    *destination = *source;
    destination->buffer = NULL;
    if (source->buffer != NULL && source->buffer_size > 0) {
        destination->buffer = PyMem_Malloc((size_t)source->buffer_size);
        if (destination->buffer == NULL) {
            memset(destination, 0, sizeof(*destination));
            PyErr_NoMemory();
            return -1;
        }
        memcpy(destination->buffer, source->buffer, (size_t)source->buffer_size);
    }
    return 0;
}

static int
buffered_snapshot_restore(
    const BufferedObjectSnapshot *snapshot,
    BufferedObject *object
)
{
    if (!snapshot->valid) {
        return 0;
    }
    object->ok = snapshot->ok;
    object->detached = snapshot->detached;
    object->readable = snapshot->readable;
    object->writable = snapshot->writable;
    object->finalizing = snapshot->finalizing;
    object->fast_closed_checks = snapshot->fast_closed_checks;
    object->abs_pos = snapshot->abs_pos;
    object->pos = snapshot->pos;
    object->raw_pos = snapshot->raw_pos;
    object->read_end = snapshot->read_end;
    object->write_pos = snapshot->write_pos;
    object->write_end = snapshot->write_end;
    object->buffer_size = snapshot->buffer_size;
    object->buffer_mask = snapshot->buffer_mask;
    if (snapshot->buffer != NULL && snapshot->buffer_size > 0) {
        if (object->buffer == NULL) {
            object->buffer = PyMem_Malloc((size_t)snapshot->buffer_size);
            if (object->buffer == NULL) {
                PyErr_NoMemory();
                return -1;
            }
        }
        memcpy(object->buffer, snapshot->buffer, (size_t)snapshot->buffer_size);
    }
    if (snapshot->lock_held) {
        if (object->owner == 0) {
            if (!PyThread_acquire_lock(object->lock, 0)) {
                PyErr_SetString(PyExc_RuntimeError,
                                "cannot restore buffered I/O lock");
                return -1;
            }
            object->owner = PyThread_get_thread_ident();
        }
        else if (object->owner != PyThread_get_thread_ident()) {
            PyErr_SetString(PyExc_RuntimeError,
                            "buffered I/O lock belongs to another thread");
            return -1;
        }
    }
    return 0;
}

static void
buffered_leave_lock(BufferedCallState *state)
{
    BufferedObject *object = buffered_object(state->receiver);
    if (object != NULL && state->resume.lock_held && object->owner != 0) {
        object->owner = 0;
        PyThread_release_lock(object->lock);
    }
}

static void
buffered_chain_exception(PyObject *exception)
{
    PyObject *current;
    if (exception == NULL) return;
    current = PyErr_GetRaisedException();
    if (current == NULL) {
        PyErr_SetRaisedException(exception);
        return;
    }
    PyException_SetContext(current, exception);
    PyErr_SetRaisedException(current);
}

static void
buffered_clear_state(BufferedCallState *state)
{
    Py_XDECREF(state->original);
    Py_XDECREF(state->receiver);
    Py_XDECREF(state->arguments);
    Py_XDECREF(state->result);
    Py_XDECREF(state->chunks);
    Py_XDECREF(state->target_bytes);
    Py_XDECREF(state->pending_exception);
    Py_XDECREF(state->raw_state);
    Py_XDECREF(state->direct_view);
    Py_XDECREF(state->direct_owner);
    buffered_snapshot_clear(&state->initial);
    buffered_snapshot_clear(&state->resume);
    memset(state, 0, sizeof(*state));
}

static Py_ssize_t
buffered_read_block_size(
    const BufferedObjectSnapshot *snapshot,
    Py_ssize_t remaining
)
{
    if (snapshot->buffer_mask != 0) {
        return remaining & ~snapshot->buffer_mask;
    }
    return snapshot->buffer_size * (remaining / snapshot->buffer_size);
}

static int
buffered_capture_direct_view(BufferedCallState *state)
{
    PyFrameObject *frame;
    if (state->phase != BUFFERED_RAW_READINTO ||
        !state->raw_direct ||
        (state->operation != BUFFERED_READ &&
         state->operation != BUFFERED_READ1) ||
        state->raw_length <= 0) {
        return 0;
    }
    frame = PyThreadState_GetFrame(PyThreadState_Get());
    while (frame != NULL) {
        PyObject *locals = PyFrame_GetLocals(frame);
        PyObject *values = locals == NULL ? NULL : PyMapping_Values(locals);
        Py_XDECREF(locals);
        if (values == NULL) {
            Py_DECREF(frame);
            return -1;
        }
        for (Py_ssize_t index = 0; index < PyList_GET_SIZE(values); index++) {
            PyObject *candidate = PyList_GET_ITEM(values, index);
            Py_buffer *view;
            PyObject *owner;
            char *base;
            if (!PyMemoryView_Check(candidate)) {
                continue;
            }
            view = PyMemoryView_GET_BUFFER(candidate);
            if (view->readonly || view->len != state->raw_length) {
                continue;
            }
            if (view->obj != NULL) {
                state->direct_view = Py_NewRef(candidate);
                state->direct_owner = Py_NewRef(view->obj);
                Py_DECREF(values);
                Py_DECREF(frame);
                return 0;
            }
            base = (char *)view->buf - state->written;
            owner = (PyObject *)(
                base - offsetof(PyBytesObject, ob_sval)
            );
            if (PyBytes_CheckExact(owner) &&
                PyBytes_GET_SIZE(owner) == state->total) {
                state->direct_view = Py_NewRef(candidate);
                state->direct_owner = Py_NewRef(owner);
                Py_DECREF(values);
                Py_DECREF(frame);
                return 0;
            }
        }
        Py_DECREF(values);
        PyFrameObject *back = PyFrame_GetBack(frame);
        Py_DECREF(frame);
        frame = back;
    }
    PyErr_SetString(
        PyExc_RuntimeError,
        "cannot preserve the direct buffered read destination"
    );
    return -1;
}

static int
buffered_detect_active_phase(BufferedCallState *state, PyObject *raw)
{
    static const struct {
        const char *name;
        BufferedPhase phase;
    } methods[] = {
        {"read", BUFFERED_RAW_READ},
        {"readinto", BUFFERED_RAW_READINTO},
        {"write", BUFFERED_RAW_WRITE},
        {"seek", BUFFERED_RAW_SEEK},
        {"tell", BUFFERED_RAW_TELL},
        {"flush", BUFFERED_RAW_FLUSH},
        {"close", BUFFERED_RAW_CLOSE},
    };
    PyFrameObject *frame = PyThreadState_GetFrame(PyThreadState_Get());
    while (frame != NULL) {
        int matched = 0;
        PyObject *locals = PyFrame_GetLocals(frame);
        PyObject *receiver = NULL;
        PyCodeObject *code;
        if (locals == NULL) {
            Py_DECREF(frame);
            return -1;
        }
        receiver = PyMapping_GetItemString(locals, "self");
        Py_DECREF(locals);
        if (receiver == NULL) {
            PyErr_Clear();
        }
        code = PyFrame_GetCode(frame);
        if (code == NULL) {
            Py_XDECREF(receiver);
            Py_DECREF(frame);
            return -1;
        }
        if (receiver == raw) {
            for (size_t index = 0;
                 index < sizeof(methods) / sizeof(methods[0]);
                 index++) {
                if (PyUnicode_CompareWithASCIIString(
                        code->co_name, methods[index].name
                    ) == 0) {
                    state->phase = methods[index].phase;
                    matched = 1;
                    break;
                }
            }
        }
        Py_DECREF(code);
        Py_XDECREF(receiver);
        if (matched) {
            Py_DECREF(frame);
            return 0;
        }
        PyFrameObject *back = PyFrame_GetBack(frame);
        Py_DECREF(frame);
        frame = back;
    }
    return 0;
}

static PyObject *
buffered_copy_object_dict(PyObject *object, PyObject *saved)
{
    PyObject *dictionary;
    PyObject *copy;
    if (saved != NULL) {
        return PyDict_Copy(saved);
    }
    dictionary = PyObject_GetAttrString(object, "__dict__");
    if (dictionary == NULL) {
        PyErr_Clear();
        return NULL;
    }
    if (!PyDict_Check(dictionary)) {
        Py_DECREF(dictionary);
        return NULL;
    }
    copy = PyDict_Copy(dictionary);
    Py_DECREF(dictionary);
    return copy;
}

static int
buffered_restore_object_dict(PyObject *object, PyObject *saved)
{
    PyObject *dictionary;
    int result;
    if (saved == NULL) {
        return 0;
    }
    dictionary = PyObject_GetAttrString(object, "__dict__");
    if (dictionary == NULL) {
        return -1;
    }
    if (!PyDict_Check(dictionary)) {
        Py_DECREF(dictionary);
        return 0;
    }
    PyDict_Clear(dictionary);
    result = PyDict_Update(dictionary, saved);
    Py_DECREF(dictionary);
    return result;
}

static int
buffered_operation_from_name(const char *name, BufferedOperation *operation)
{
    static const char *const names[] = {
        "read", "read1", "readinto", "readinto1", "peek", "write",
        "seek", "tell", "flush", "close",
    };
    for (int index = 0; index < 10; index++) {
        if (strcmp(name, names[index]) == 0) {
            *operation = (BufferedOperation)index;
            return 1;
        }
    }
    return 0;
}

static Py_ssize_t
buffered_readahead(const BufferedObjectSnapshot *snapshot)
{
    if (snapshot->readable && snapshot->read_end != -1) {
        return Py_SAFE_DOWNCAST(snapshot->read_end - snapshot->pos,
                                AleffBufferedOff_t, Py_ssize_t);
    }
    return 0;
}

static Py_ssize_t
buffered_raw_offset(const BufferedObjectSnapshot *snapshot)
{
    if ((snapshot->readable && snapshot->read_end != -1) ||
        (snapshot->writable && snapshot->write_end != -1)) {
        if (snapshot->raw_pos >= 0) {
            return Py_SAFE_DOWNCAST(snapshot->raw_pos - snapshot->pos,
                                    AleffBufferedOff_t, Py_ssize_t);
        }
    }
    return 0;
}

static AleffBufferedOff_t
buffered_write_rewind(const BufferedObjectSnapshot *snapshot)
{
    if (snapshot->writable && snapshot->write_end != -1 &&
        snapshot->write_pos < snapshot->write_end) {
        return (AleffBufferedOff_t)buffered_raw_offset(snapshot) +
            snapshot->pos - snapshot->write_pos;
    }
    return 0;
}

static int
buffered_buffer_input(BufferedCallState *state, Py_ssize_t count)
{
    BufferedObject *object = buffered_object(state->receiver);
    Py_buffer view;
    if (count <= 0) return 0;
    if (PyObject_GetBuffer(
            PyTuple_GET_ITEM(state->arguments, 0), &view, PyBUF_SIMPLE
        ) < 0) {
        return -1;
    }
    memcpy(object->buffer, (char *)view.buf + state->written, (size_t)count);
    PyBuffer_Release(&view);
    state->written += count;
    object->write_pos = 0;
    object->write_end = count;
    object->pos = count;
    object->raw_pos = 0;
    return 0;
}

static int
buffered_argument_size(BufferedCallState *state, Py_ssize_t *size)
{
    PyObject *argument;
    if (PyTuple_GET_SIZE(state->arguments) == 0) {
        *size = -1;
        return 0;
    }
    argument = PyTuple_GET_ITEM(state->arguments, 0);
    if (argument == Py_None) {
        *size = -1;
        return 0;
    }
    *size = PyLong_AsSsize_t(argument);
    if (*size == -1 && PyErr_Occurred()) {
        return -1;
    }
    return 0;
}

static int
buffered_prepare_result(BufferedCallState *state)
{
    Py_ssize_t size;
    Py_ssize_t available;
    const BufferedObjectSnapshot *initial = &state->initial;
    available = buffered_readahead(initial);
    switch (state->operation) {
        case BUFFERED_READ:
            if (buffered_argument_size(state, &size) < 0) {
                return -1;
            }
            if (size < 0) {
                state->chunks = PyList_New(0);
                if (state->chunks == NULL) return -1;
                if (available > 0) {
                    PyObject *data = PyBytes_FromStringAndSize(
                        initial->buffer + initial->pos, available);
                    if (data == NULL || PyList_Append(state->chunks, data) < 0) {
                        Py_XDECREF(data);
                        return -1;
                    }
                    Py_DECREF(data);
                    state->written = available;
                }
            }
            else {
                state->total = size;
                state->result = PyByteArray_FromStringAndSize(NULL, size);
                if (state->result == NULL) return -1;
                state->written = Py_MIN(available, size);
                if (state->written > 0) {
                    memcpy(PyByteArray_AS_STRING(state->result),
                           initial->buffer + initial->pos,
                           (size_t)state->written);
                }
            }
            break;
        case BUFFERED_READ1:
            if (buffered_argument_size(state, &size) < 0) return -1;
            if (size < 0) size = initial->buffer_size;
            state->total = size;
            state->one_raw_read = 1;
            state->result = PyByteArray_FromStringAndSize(NULL, size);
            if (state->result == NULL) return -1;
            state->written = Py_MIN(available, size);
            if (state->written > 0) {
                memcpy(PyByteArray_AS_STRING(state->result),
                       initial->buffer + initial->pos,
                       (size_t)state->written);
            }
            break;
        case BUFFERED_READINTO:
        case BUFFERED_READINTO1:
            if (PyTuple_GET_SIZE(state->arguments) != 1) return 0;
            {
                Py_buffer view;
                if (PyObject_GetBuffer(
                        PyTuple_GET_ITEM(state->arguments, 0),
                        &view, PyBUF_WRITABLE
                    ) < 0) return -1;
                state->total = view.len;
                state->target_bytes = PyBytes_FromStringAndSize(
                    view.buf, view.len
                );
                PyBuffer_Release(&view);
                if (state->target_bytes == NULL) return -1;
            }
            state->written = Py_MIN(available, state->total);
            state->one_raw_read = state->operation == BUFFERED_READINTO1;
            break;
        case BUFFERED_PEEK:
            break;
        default:
            break;
    }
    return 0;
}

static int
buffered_copy_target(BufferedCallState *state)
{
    if (state->target_bytes == NULL || PyTuple_GET_SIZE(state->arguments) == 0) {
        return 0;
    }
    Py_buffer view;
    if (PyObject_GetBuffer(
            PyTuple_GET_ITEM(state->arguments, 0), &view, PyBUF_WRITABLE
        ) < 0) {
        return -1;
    }
    Py_ssize_t size = PyBytes_GET_SIZE(state->target_bytes);
    memcpy(view.buf, PyBytes_AS_STRING(state->target_bytes), (size_t)size);
    PyBuffer_Release(&view);
    return 0;
}

static void *
buffered_copy_state(const void *raw_state)
{
    const BufferedCallState *source = raw_state;
    BufferedCallState *copy = PyMem_Calloc(1, sizeof(*copy));
    BufferedObject *object;
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->original = Py_NewRef(source->original);
    copy->receiver = Py_NewRef(source->receiver);
    copy->arguments = Py_NewRef(source->arguments);
    copy->result = Py_XNewRef(source->result);
    copy->chunks = Py_XNewRef(source->chunks);
    copy->target_bytes = Py_XNewRef(source->target_bytes);
    copy->pending_exception = Py_XNewRef(source->pending_exception);
    copy->raw_state = NULL;
    copy->direct_view = Py_XNewRef(source->direct_view);
    copy->direct_owner = Py_XNewRef(source->direct_owner);
    copy->initial.buffer = NULL;
    copy->resume.buffer = NULL;
    if (buffered_snapshot_copy(&copy->initial, &source->initial) < 0) {
        buffered_clear_state(copy);
        PyMem_Free(copy);
        return NULL;
    }
    object = buffered_object(source->receiver);
    if (source->live_state) {
        if (object != NULL && buffered_snapshot_capture(&copy->resume, object) < 0) {
            buffered_clear_state(copy);
            PyMem_Free(copy);
            return NULL;
        }
    }
    else if (source->resume.valid) {
        if (buffered_snapshot_copy(&copy->resume, &source->resume) < 0) {
            buffered_clear_state(copy);
            PyMem_Free(copy);
            return NULL;
        }
    }
    else {
        if (object != NULL && buffered_snapshot_capture(&copy->resume, object) < 0) {
            buffered_clear_state(copy);
            PyMem_Free(copy);
            return NULL;
        }
    }
    object = buffered_object(source->receiver);
    if (object != NULL && object->raw != NULL) {
        PyObject *saved_raw_state = source->live_state
            ? NULL : source->raw_state;
        copy->raw_state = buffered_copy_object_dict(
            object->raw, saved_raw_state
        );
        if (copy->raw_state == NULL && PyErr_Occurred()) {
            buffered_clear_state(copy);
            PyMem_Free(copy);
            return NULL;
        }
        if (source->live_state &&
            buffered_detect_active_phase(copy, object->raw) < 0) {
            buffered_clear_state(copy);
            PyMem_Free(copy);
            return NULL;
        }
    }
    if (copy->direct_view == NULL &&
        buffered_capture_direct_view(copy) < 0) {
        buffered_clear_state(copy);
        PyMem_Free(copy);
        return NULL;
    }
    if (copy->chunks != NULL) {
        PyObject *chunks = PyList_GetSlice(
            source->chunks, 0, PyList_GET_SIZE(source->chunks)
        );
        Py_DECREF(copy->chunks);
        copy->chunks = chunks;
    }
    if (copy->result != NULL) {
        PyObject *result = PyByteArray_FromObject(source->result);
        Py_DECREF(copy->result);
        copy->result = result;
    }
    if (copy->result == NULL && source->result != NULL) {
        buffered_clear_state(copy);
        PyMem_Free(copy);
        return NULL;
    }
    if (copy->chunks == NULL && source->chunks != NULL) {
        buffered_clear_state(copy);
        PyMem_Free(copy);
        return NULL;
    }
    copy->live_state = 0;
    return copy;
}

static void
buffered_free_state(void *raw_state)
{
    BufferedCallState *state = raw_state;
    if (state == NULL) return;
    buffered_clear_state(state);
    PyMem_Free(state);
}

static PyObject *buffered_resume(const void *, PyObject *);
static PyObject *buffered_process_resume_value(BufferedCallState *, PyObject *);
static PyObject *buffered_raw_call(BufferedCallState *, BufferedPhase);
static int buffered_is_reader(PyObject *);

static PyObject *
buffered_resume_next(BufferedCallState *state, PyObject *next)
{
    PyObject *result = buffered_process_resume_value(state, next);
    Py_XDECREF(next);
    return result;
}

static PyObject *
buffered_close_after_error(BufferedCallState *state)
{
    PyObject *exception = PyErr_GetRaisedException();
    if (exception == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "buffered close failed without an exception");
        return NULL;
    }
    Py_XSETREF(state->pending_exception, exception);
    state->source_is_buffer = 0;
    state->after_phase = BUFFERED_DONE;
    return buffered_resume_next(
        state, buffered_raw_call(state, BUFFERED_RAW_CLOSE)
    );
}

static PyObject *
buffered_raw_call(BufferedCallState *state, BufferedPhase phase)
{
    BufferedObject *object = buffered_object(state->receiver);
    PyObject *argument = NULL;
    PyObject *result;
    Py_ssize_t length;
    if (object == NULL || object->raw == NULL) {
        PyErr_SetString(PyExc_ValueError, "I/O operation on uninitialized object");
        return NULL;
    }
    state->phase = phase;
    switch (phase) {
        case BUFFERED_RAW_READ:
            return PyObject_CallMethod(object->raw, "read", NULL);
        case BUFFERED_RAW_READINTO:
            length = state->raw_length;
            if (length < 0) length = 0;
            if (state->raw_direct) {
                if (state->operation == BUFFERED_READ ||
                    state->operation == BUFFERED_READ1) {
                    argument = PyMemoryView_FromMemory(
                        PyByteArray_AS_STRING(state->result) + state->written,
                        length, PyBUF_WRITE
                    );
                }
                else {
                    Py_buffer view;
                    if (PyObject_GetBuffer(
                            PyTuple_GET_ITEM(state->arguments, 0),
                            &view, PyBUF_WRITABLE
                        ) < 0) {
                        buffered_leave_lock(state);
                        return NULL;
                    }
                    argument = PyMemoryView_FromMemory(
                        (char *)view.buf + state->written, length, PyBUF_WRITE
                    );
                    PyBuffer_Release(&view);
                }
            }
            else {
                argument = PyMemoryView_FromMemory(
                    object->buffer, length, PyBUF_WRITE
                );
            }
            if (argument == NULL) {
                buffered_leave_lock(state);
                return NULL;
            }
            result = PyObject_CallMethod(object->raw, "readinto", "O", argument);
            Py_DECREF(argument);
            return result;
        case BUFFERED_RAW_WRITE:
            if (state->source_is_buffer) {
                argument = PyMemoryView_FromMemory(
                    object->buffer + object->write_pos,
                    length = Py_SAFE_DOWNCAST(
                        object->write_end - object->write_pos,
                        AleffBufferedOff_t, Py_ssize_t
                    ), PyBUF_READ
                );
            }
            else {
                Py_buffer view;
                if (PyObject_GetBuffer(
                        PyTuple_GET_ITEM(state->arguments, 0),
                        &view, PyBUF_SIMPLE
                    ) < 0) {
                    buffered_leave_lock(state);
                    return NULL;
                }
                length = view.len - state->written;
                argument = PyMemoryView_FromMemory(
                    (char *)view.buf + state->written, length, PyBUF_READ
                );
                PyBuffer_Release(&view);
            }
            if (argument == NULL) {
                buffered_leave_lock(state);
                return NULL;
            }
            result = PyObject_CallMethod(object->raw, "write", "O", argument);
            Py_DECREF(argument);
            return result;
        case BUFFERED_RAW_SEEK:
            {
                PyObject *target = PyTuple_GET_SIZE(state->arguments) > 0
                    ? PyTuple_GET_ITEM(state->arguments, 0) : PyLong_FromLong(0);
                PyObject *whence = PyTuple_GET_SIZE(state->arguments) > 1
                    ? PyTuple_GET_ITEM(state->arguments, 1) : PyLong_FromLong(0);
                PyObject *target_copy = PyNumber_Long(target);
                PyObject *whence_copy = PyNumber_Long(whence);
                int relative = 0;
                if (target_copy == NULL || whence_copy == NULL) {
                    Py_XDECREF(target_copy); Py_XDECREF(whence_copy);
                    if (PyTuple_GET_SIZE(state->arguments) == 0) Py_DECREF(target);
                    if (PyTuple_GET_SIZE(state->arguments) <= 1) Py_DECREF(whence);
                    return NULL;
                }
                if (PyTuple_GET_SIZE(state->arguments) > 1) {
                    long whence_value = PyLong_AsLong(whence_copy);
                    if (whence_value == -1 && PyErr_Occurred()) {
                        Py_DECREF(target_copy); Py_DECREF(whence_copy);
                        if (PyTuple_GET_SIZE(state->arguments) == 0) Py_DECREF(target);
                        if (PyTuple_GET_SIZE(state->arguments) <= 1) Py_DECREF(whence);
                        return NULL;
                    }
                    relative = whence_value == 1;
                }
                if (state->after_phase == BUFFERED_RAW_READ ||
                    state->after_phase == BUFFERED_RAW_READINTO ||
                    state->after_phase == BUFFERED_RAW_CLOSE ||
                    state->seek_rewind ||
                    (state->source_is_buffer &&
                     state->after_phase == BUFFERED_RAW_WRITE)) {
                    AleffBufferedOff_t rewind = state->seek_rewind == 2
                        ? 0 : buffered_raw_offset(&state->resume);
                    if (state->source_is_buffer) {
                        rewind += state->resume.pos - state->resume.write_pos;
                    }
                    Py_DECREF(target_copy);
                    target_copy = buffered_from_off_t(-rewind);
                    Py_DECREF(whence_copy);
                    whence_copy = PyLong_FromLong(1);
                }
                else if (state->operation == BUFFERED_SEEK && relative) {
                    AleffBufferedOff_t offset = buffered_raw_offset(&state->resume);
                    Py_DECREF(target_copy);
                    target_copy = buffered_from_off_t(
                        buffered_as_off_t(target) - offset
                    );
                }
                result = PyObject_CallMethod(object->raw, "seek", "OO",
                                             target_copy, whence_copy);
                Py_DECREF(target_copy); Py_DECREF(whence_copy);
                if (PyTuple_GET_SIZE(state->arguments) == 0) Py_DECREF(target);
                if (PyTuple_GET_SIZE(state->arguments) <= 1) Py_DECREF(whence);
                return result;
            }
        case BUFFERED_RAW_TELL:
            return PyObject_CallMethod(object->raw, "tell", NULL);
        case BUFFERED_RAW_FLUSH:
            return PyObject_CallMethod(object->raw, "flush", NULL);
        case BUFFERED_RAW_CLOSE:
            return PyObject_CallMethod(object->raw, "close", NULL);
        default:
            PyErr_SetString(PyExc_RuntimeError, "invalid buffered raw phase");
            return NULL;
    }
}

static PyObject *
buffered_number_result(PyObject *value, Py_ssize_t maximum, const char *name)
{
    if (value == NULL) return NULL;
    if (value == Py_None) return Py_NewRef(Py_None);
    Py_ssize_t number = PyNumber_AsSsize_t(value, PyExc_ValueError);
    if (number == -1 && PyErr_Occurred()) return NULL;
    if (number < 0 || number > maximum) {
        PyErr_Format(PyExc_OSError,
                     "raw %s() returned invalid length %zd "
                     "(should have been between 0 and %zd)",
                     name, number, maximum);
        return NULL;
    }
    return PyLong_FromSsize_t(number);
}

static void
buffered_set_blocking_error(Py_ssize_t written)
{
    PyObject *error;
    PyErr_Clear();
    error = PyObject_CallFunction(
        PyExc_BlockingIOError, "isn", 0,
        "write could not complete without blocking", written
    );
    if (error != NULL) {
        PyErr_SetObject(PyExc_BlockingIOError, error);
        Py_DECREF(error);
    }
}

static PyObject *
buffered_finish(BufferedCallState *state, PyObject *value)
{
    buffered_leave_lock(state);
    state->phase = BUFFERED_DONE;
    return value;
}

static PyObject *
buffered_read_result(BufferedCallState *state, Py_ssize_t amount)
{
    BufferedObject *object = buffered_object(state->receiver);
    Py_ssize_t remaining = state->total - state->written;
    Py_ssize_t copied = Py_MIN(amount, remaining);
    if (!state->raw_direct && amount > 0) {
        if (object->abs_pos != -1) object->abs_pos += amount;
        object->read_end = amount;
        object->raw_pos = amount;
        if (state->operation == BUFFERED_READ ||
            state->operation == BUFFERED_READ1) {
            memcpy(PyByteArray_AS_STRING(state->result) + state->written,
                   object->buffer, (size_t)copied);
            object->pos += copied;
        }
        else if (state->operation != BUFFERED_PEEK) {
            Py_buffer view;
            if (PyObject_GetBuffer(
                    PyTuple_GET_ITEM(state->arguments, 0), &view, PyBUF_WRITABLE
                ) < 0) {
                buffered_leave_lock(state);
                return NULL;
            }
            memcpy((char *)view.buf + state->written, object->buffer,
                   (size_t)copied);
            PyBuffer_Release(&view);
            object->pos += copied;
        }
    }
    else if (state->raw_direct && amount > 0) {
        if ((state->operation == BUFFERED_READ ||
             state->operation == BUFFERED_READ1) &&
            state->direct_view != NULL) {
            Py_buffer *view = PyMemoryView_GET_BUFFER(state->direct_view);
            memcpy(
                PyByteArray_AS_STRING(state->result) + state->written,
                view->buf,
                (size_t)copied
            );
        }
        if (object->abs_pos != -1) object->abs_pos += amount;
    }
    state->written += copied;
    if (state->written >= state->total || state->one_raw_read) {
        if (state->operation == BUFFERED_READ || state->operation == BUFFERED_READ1) {
            PyObject *result = PyBytes_FromStringAndSize(
                PyByteArray_AS_STRING(state->result), state->written
            );
            return buffered_finish(state, result);
        }
        if (state->operation == BUFFERED_PEEK) {
            PyObject *result = PyBytes_FromStringAndSize(
                object->buffer, amount
            );
            object->pos = 0;
            return buffered_finish(state, result);
        }
        return buffered_finish(state, PyLong_FromSsize_t(state->written));
    }
    object->read_end = -1;
    object->pos = 0;
    state->raw_direct = state->total - state->written > object->buffer_size;
    state->raw_length = state->raw_direct
        ? state->total - state->written : object->buffer_size;
    return buffered_resume_next(
        state, buffered_raw_call(state, BUFFERED_RAW_READINTO)
    );
}

static PyObject *
buffered_process_readinto(BufferedCallState *state, PyObject *value)
{
    if (value == NULL) {
        buffered_leave_lock(state);
        return NULL;
    }
    if (value == Py_None) {
        if (state->operation == BUFFERED_READ ||
            state->operation == BUFFERED_READ1) {
            if (state->operation == BUFFERED_READ && state->written == 0) {
                return buffered_finish(state, Py_NewRef(Py_None));
            }
            PyObject *result = PyBytes_FromStringAndSize(
                PyByteArray_AS_STRING(state->result), state->written
            );
            return buffered_finish(state, result);
        }
        if (state->operation == BUFFERED_PEEK) {
            return buffered_finish(state, PyBytes_FromStringAndSize(NULL, 0));
        }
        if (state->written == 0) {
            return buffered_finish(state, Py_NewRef(Py_None));
        }
        return buffered_finish(state, PyLong_FromSsize_t(state->written));
    }
    PyObject *number = buffered_number_result(value, state->raw_length, "readinto");
    if (number == NULL) {
        buffered_leave_lock(state);
        return NULL;
    }
    Py_ssize_t amount = PyLong_AsSsize_t(number);
    Py_DECREF(number);
    if (amount == 0) {
        if (state->operation == BUFFERED_PEEK) {
            BufferedObject *object = buffered_object(state->receiver);
            return buffered_finish(state, PyBytes_FromStringAndSize(object->buffer, 0));
        }
        if (state->operation == BUFFERED_READ ||
            state->operation == BUFFERED_READ1) {
            return buffered_finish(state, PyBytes_FromStringAndSize(
                PyByteArray_AS_STRING(state->result), state->written));
        }
        return buffered_finish(state, PyLong_FromSsize_t(state->written));
    }
    return buffered_read_result(state, amount);
}

static PyObject *
buffered_process_read(BufferedCallState *state, PyObject *value)
{
    if (value == NULL) {
        buffered_leave_lock(state);
        return NULL;
    }
    if (value == Py_None || !PyBytes_Check(value)) {
        if (value != Py_None) {
            PyErr_SetString(PyExc_TypeError, "read() should return bytes");
            buffered_leave_lock(state);
            return NULL;
        }
        if (state->chunks != NULL && PyList_GET_SIZE(state->chunks) > 0) {
            PyObject *empty = PyBytes_FromStringAndSize(NULL, 0);
            PyObject *joined;
            if (empty == NULL) {
                buffered_leave_lock(state);
                return NULL;
            }
            joined = PyObject_CallMethod(empty, "join", "O", state->chunks);
            Py_DECREF(empty);
            return buffered_finish(state, joined);
        }
        return buffered_finish(state, Py_NewRef(Py_None));
    }
    if (PyBytes_GET_SIZE(value) == 0) {
        PyObject *empty = PyBytes_FromStringAndSize(NULL, 0);
        if (empty == NULL) {
            buffered_leave_lock(state);
            return NULL;
        }
        if (state->chunks != NULL && PyList_GET_SIZE(state->chunks) > 0) {
            PyObject *joined = PyObject_CallMethod(
                empty, "join", "O", state->chunks
            );
            Py_DECREF(empty);
            return buffered_finish(state, joined);
        }
        return buffered_finish(state, empty);
    }
    if (state->chunks == NULL) state->chunks = PyList_New(0);
    if (state->chunks == NULL || PyList_Append(state->chunks, value) < 0) {
        buffered_leave_lock(state);
        return NULL;
    }
    BufferedObject *object = buffered_object(state->receiver);
    if (object->abs_pos != -1) object->abs_pos += PyBytes_GET_SIZE(value);
    return buffered_resume_next(
        state, buffered_raw_call(state, BUFFERED_RAW_READ)
    );
}

static PyObject *
buffered_process_write(BufferedCallState *state, PyObject *value)
{
    BufferedObject *object = buffered_object(state->receiver);
    if (value == NULL) {
        if (state->operation == BUFFERED_CLOSE) {
            return buffered_close_after_error(state);
        }
        buffered_leave_lock(state);
        return NULL;
    }
    if (value == Py_None) {
        Py_ssize_t remaining = state->total - state->written;
        if (!state->source_is_buffer && remaining <= object->buffer_size) {
            if (buffered_buffer_input(state, remaining) < 0) {
                buffered_leave_lock(state);
                return NULL;
            }
            if (state->operation != BUFFERED_CLOSE) {
                return buffered_finish(
                    state, PyLong_FromSsize_t(state->total)
                );
            }
        }
        else if (!state->source_is_buffer && object->buffer_size > 0) {
            if (buffered_buffer_input(state, object->buffer_size) < 0) {
                buffered_leave_lock(state);
                return NULL;
            }
        }
        buffered_set_blocking_error(state->written);
        if (state->operation == BUFFERED_CLOSE) {
            return buffered_close_after_error(state);
        }
        buffered_leave_lock(state);
        return NULL;
    }
    PyObject *number = buffered_number_result(
        value, state->raw_length, "write"
    );
    if (number == NULL) {
        if (state->operation == BUFFERED_CLOSE) {
            return buffered_close_after_error(state);
        }
        buffered_leave_lock(state);
        return NULL;
    }
    Py_ssize_t amount = PyLong_AsSsize_t(number);
    Py_DECREF(number);
    if (amount > 0 && object->abs_pos != -1) {
        object->abs_pos += amount;
    }
    if (amount == 0) {
        if (state->source_is_buffer) {
            buffered_set_blocking_error(0);
            if (state->operation == BUFFERED_CLOSE) {
                return buffered_close_after_error(state);
            }
            buffered_leave_lock(state);
            return NULL;
        }
        if (state->total - state->written <= object->buffer_size) {
            if (buffered_buffer_input(
                    state, state->total - state->written
                ) < 0) {
                buffered_leave_lock(state);
                return NULL;
            }
            return buffered_finish(
                state, PyLong_FromSsize_t(state->total)
            );
        }
        if (buffered_buffer_input(state, object->buffer_size) < 0) {
            buffered_leave_lock(state);
            return NULL;
        }
        buffered_set_blocking_error(state->written);
        if (state->operation == BUFFERED_CLOSE) {
            return buffered_close_after_error(state);
        }
        buffered_leave_lock(state);
        return NULL;
    }
    if (state->source_is_buffer) {
        object->write_pos += amount;
        object->raw_pos = object->write_pos;
        if (object->write_pos < object->write_end) {
            state->raw_length = Py_SAFE_DOWNCAST(
                        object->write_end - object->write_pos,
                        AleffBufferedOff_t, Py_ssize_t
            );
            return buffered_resume_next(
                state, buffered_raw_call(state, BUFFERED_RAW_WRITE)
            );
        }
        object->write_pos = 0;
        object->write_end = -1;
        state->source_is_buffer = 0;
        if ((state->operation == BUFFERED_FLUSH ||
             state->operation == BUFFERED_CLOSE) &&
            state->after_phase == BUFFERED_RAW_SEEK) {
            state->seek_rewind = 2;
        }
        else {
            state->seek_rewind = 0;
        }
        if (state->operation == BUFFERED_WRITE &&
            state->after_phase == BUFFERED_DONE) {
            if (state->total == 0) {
                return buffered_finish(state, PyLong_FromLong(0));
            }
            if (state->total < object->buffer_size) {
                Py_buffer view;
                if (PyObject_GetBuffer(
                        PyTuple_GET_ITEM(state->arguments, 0),
                        &view, PyBUF_SIMPLE
                    ) < 0) {
                    buffered_leave_lock(state);
                    return NULL;
                }
                memcpy(object->buffer, view.buf, (size_t)state->total);
                PyBuffer_Release(&view);
                object->write_pos = 0;
                object->write_end = state->total;
                object->pos = state->total;
                object->raw_pos = 0;
                return buffered_finish(
                    state, PyLong_FromSsize_t(state->total)
                );
            }
            state->raw_length = state->total;
            state->written = 0;
            return buffered_resume(
                state, buffered_raw_call(state, BUFFERED_RAW_WRITE)
            );
        }
        if (state->after_phase != BUFFERED_DONE) {
            BufferedPhase phase = state->after_phase;
            if (state->operation == BUFFERED_CLOSE &&
                phase == BUFFERED_RAW_SEEK) {
                state->after_phase = BUFFERED_RAW_CLOSE;
            }
            else {
                state->after_phase = BUFFERED_DONE;
            }
            return buffered_resume_next(
                state, buffered_raw_call(state, phase)
            );
        }
        return buffered_finish(state, Py_NewRef(Py_None));
    }
    state->written += amount;
    state->raw_length = state->total - state->written;
    if (state->raw_length >= object->buffer_size) {
        return buffered_resume_next(
            state, buffered_raw_call(state, BUFFERED_RAW_WRITE)
        );
    }
    if (state->raw_length > 0) {
        Py_buffer view;
        if (PyObject_GetBuffer(
                PyTuple_GET_ITEM(state->arguments, 0), &view, PyBUF_SIMPLE
            ) < 0) {
            buffered_leave_lock(state);
            return NULL;
        }
        memcpy(object->buffer, (char *)view.buf + state->written,
               (size_t)state->raw_length);
        PyBuffer_Release(&view);
    }
    object->write_pos = 0;
    object->write_end = state->raw_length;
    object->pos = state->raw_length;
    object->raw_pos = 0;
    return buffered_finish(state, PyLong_FromSsize_t(state->total));
}

static PyObject *
buffered_process_seek(BufferedCallState *state, PyObject *value)
{
    if (value == NULL) {
        if (state->operation == BUFFERED_CLOSE) {
            return buffered_close_after_error(state);
        }
        buffered_leave_lock(state);
        return NULL;
    }
    BufferedObject *object = buffered_object(state->receiver);
    AleffBufferedOff_t position = buffered_as_off_t(value);
    if (position < 0) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_OSError, "raw stream returned invalid position");
        }
        if (state->operation == BUFFERED_CLOSE) {
            return buffered_close_after_error(state);
        }
        buffered_leave_lock(state);
        return NULL;
    }
    if (state->after_phase == BUFFERED_RAW_WRITE) {
        object->abs_pos = position;
        object->raw_pos = object->write_pos;
        state->after_phase = state->after_write_phase;
        return buffered_resume_next(
            state, buffered_raw_call(state, BUFFERED_RAW_WRITE)
        );
    }
    object->abs_pos = position;
    object->raw_pos = -1;
    object->read_end = object->readable ? -1 : object->read_end;
    object->write_end = object->writable ? -1 : object->write_end;
    if (state->after_phase != BUFFERED_DONE) {
        BufferedPhase phase = state->after_phase;
        state->after_phase = BUFFERED_DONE;
        if (phase == BUFFERED_RAW_READINTO) {
            object->read_end = -1;
        }
        return buffered_resume_next(
            state, buffered_raw_call(state, phase)
        );
    }
    if (state->operation == BUFFERED_FLUSH || state->operation == BUFFERED_CLOSE) {
        return buffered_finish(state, Py_NewRef(Py_None));
    }
    return buffered_finish(state, buffered_from_off_t(position));
}

static PyObject *
buffered_process_tell(BufferedCallState *state, PyObject *value)
{
    if (value == NULL) {
        buffered_leave_lock(state);
        return NULL;
    }
    BufferedObject *object = buffered_object(state->receiver);
    AleffBufferedOff_t position = buffered_as_off_t(value);
    if (position < 0) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_OSError, "raw stream returned invalid position");
        }
        buffered_leave_lock(state);
        return NULL;
    }
    object->abs_pos = position;
    position -= buffered_raw_offset(&state->resume);
    if (position < 0) position = 0;
    return buffered_finish(state, buffered_from_off_t(position));
}

static PyObject *
buffered_pair_close_next(BufferedCallState *state)
{
    BufferedRWPairObject *pair = buffered_pair_object(state->receiver);
    if (pair == NULL || pair->reader == NULL || pair->writer == NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "I/O operation on uninitialized object");
        return NULL;
    }
    state->phase = BUFFERED_PAIR_READER;
    return PyObject_CallMethod((PyObject *)pair->reader, "close", NULL);
}

static PyObject *
buffered_process_pair_close(BufferedCallState *state, PyObject *value)
{
    if (state->phase == BUFFERED_PAIR_WRITER) {
        if (value == NULL) {
            state->pending_exception = PyErr_GetRaisedException();
        }
        PyObject *result = buffered_pair_close_next(state);
        if (result == NULL) {
            if (state->pending_exception != NULL) {
                buffered_chain_exception(state->pending_exception);
                state->pending_exception = NULL;
            }
            return NULL;
        }
        if (state->pending_exception != NULL) {
            buffered_chain_exception(state->pending_exception);
            state->pending_exception = NULL;
            Py_DECREF(result);
            return NULL;
        }
        return buffered_finish(state, result);
    }
    if (value == NULL) {
        buffered_leave_lock(state);
        return NULL;
    }
    return buffered_finish(state, value);
}

static int
buffered_result_wrote_bytes(BufferedCallState *state)
{
    BufferedObject *object = buffered_object(state->receiver);
    if (object != NULL && state->initial.buffer != NULL &&
        state->resume.buffer != NULL &&
        state->initial.buffer_size == state->resume.buffer_size &&
        memcmp(state->initial.buffer, state->resume.buffer,
               (size_t)state->initial.buffer_size) != 0) {
        return 1;
    }
    if (state->target_bytes != NULL &&
        PyTuple_GET_SIZE(state->arguments) == 1) {
        Py_buffer view;
        if (PyObject_GetBuffer(
                PyTuple_GET_ITEM(state->arguments, 0), &view, PyBUF_SIMPLE
            ) == 0) {
            int changed = view.len == PyBytes_GET_SIZE(state->target_bytes) &&
                memcmp(view.buf, PyBytes_AS_STRING(state->target_bytes),
                       (size_t)view.len) != 0;
            PyBuffer_Release(&view);
            return changed;
        }
        PyErr_Clear();
    }
    return 0;
}

static PyObject *
buffered_process_result(BufferedCallState *state, PyObject *value)
{
    switch (state->phase) {
        case BUFFERED_RAW_READ: return buffered_process_read(state, value);
        case BUFFERED_RAW_READINTO: return buffered_process_readinto(state, value);
        case BUFFERED_RAW_WRITE: return buffered_process_write(state, value);
        case BUFFERED_RAW_SEEK: return buffered_process_seek(state, value);
        case BUFFERED_RAW_TELL: return buffered_process_tell(state, value);
        case BUFFERED_RAW_FLUSH:
            if (value == NULL) {
                if (state->operation == BUFFERED_CLOSE) {
                    return buffered_close_after_error(state);
                }
                buffered_leave_lock(state);
                return NULL;
            }
            if (state->operation == BUFFERED_CLOSE &&
                state->after_phase == BUFFERED_RAW_CLOSE) {
                state->after_phase = BUFFERED_DONE;
                return buffered_resume_next(
                    state, buffered_raw_call(state, BUFFERED_RAW_CLOSE)
                );
            }
            if (state->operation == BUFFERED_FLUSH &&
                buffered_is_reader(state->receiver)) {
                return buffered_finish(state, value);
            }
            return buffered_finish(state, Py_NewRef(Py_None));
        case BUFFERED_RAW_CLOSE:
            {
                BufferedObject *object = buffered_object(state->receiver);
                if (object != NULL) {
                    PyMem_Free(object->buffer);
                    object->buffer = NULL;
                    object->read_end = 0;
                    object->pos = 0;
                    object->write_end = -1;
                }
                if (value == NULL) {
                    PyObject *exception = PyErr_GetRaisedException();
                    if (state->pending_exception != NULL && exception != NULL) {
                        PyException_SetContext(
                            exception, state->pending_exception
                        );
                        state->pending_exception = NULL;
                    }
                    else if (state->pending_exception != NULL) {
                        exception = state->pending_exception;
                        state->pending_exception = NULL;
                    }
                    if (exception != NULL) {
                        PyErr_SetRaisedException(exception);
                    }
                    buffered_leave_lock(state);
                    return NULL;
                }
                if (state->pending_exception != NULL) {
                    PyObject *exception = state->pending_exception;
                    state->pending_exception = NULL;
                    PyErr_SetRaisedException(exception);
                    buffered_leave_lock(state);
                    return NULL;
                }
                return buffered_finish(state, Py_NewRef(Py_None));
            }
        case BUFFERED_PAIR_WRITER:
        case BUFFERED_PAIR_READER:
            return buffered_process_pair_close(state, value);
        default:
            if (state->phase == BUFFERED_DONE &&
                buffered_pair_object(state->receiver) != NULL) {
                if (value == NULL) return NULL;
                Py_INCREF(value);
                return value;
            }
            PyErr_SetString(PyExc_RuntimeError,
                            "buffered continuation reached an invalid phase");
            buffered_leave_lock(state);
            return NULL;
    }
}

static PyObject *
buffered_process_resume_value(BufferedCallState *state, PyObject *value)
{
    if (state->phase == BUFFERED_RAW_SEEK && value != NULL &&
        state->after_phase == BUFFERED_RAW_READ &&
        (PyBytes_Check(value) || value == Py_None)) {
        state->phase = BUFFERED_RAW_READ;
    }
    else if (state->phase == BUFFERED_RAW_SEEK && value == Py_None &&
             state->after_phase == BUFFERED_RAW_CLOSE) {
        state->phase = BUFFERED_RAW_CLOSE;
    }
    else if (state->phase == BUFFERED_RAW_SEEK &&
        state->after_phase == BUFFERED_RAW_READINTO &&
        buffered_result_wrote_bytes(state)) {
        state->phase = BUFFERED_RAW_READINTO;
    }
    return buffered_process_result(state, value);
}

static PyObject *
buffered_resume(const void *raw_state, PyObject *value)
{
    BufferedCallState *state;
    AleffAdapterFrame frame;
    PyObject *result;
    PyObject *pending_exception = NULL;
    if (raw_state == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "buffered continuation has no operation state");
        return NULL;
    }
    if (value == NULL) {
        pending_exception = PyErr_GetRaisedException();
    }
    state = buffered_copy_state(raw_state);
    if (state == NULL) {
        buffered_chain_exception(pending_exception);
        return NULL;
    }
    state->live_state = 1;
    if (adapter_enter(&frame, &buffered_call_vtable, state) < 0) {
        buffered_chain_exception(pending_exception);
        buffered_free_state(state);
        return NULL;
    }
    if (pending_exception != NULL) {
        PyErr_SetRaisedException(pending_exception);
    }
    result = buffered_process_resume_value(state, value);
    adapter_leave(&frame);
    buffered_free_state(state);
    return result;
}

static int
buffered_prepare_resume(void *raw_state)
{
    BufferedCallState *state = raw_state;
    BufferedObject *object = buffered_object(state->receiver);
    if (object != NULL) {
        if (buffered_snapshot_restore(&state->resume, object) < 0) {
            return -1;
        }
        if (object->raw != NULL &&
            buffered_restore_object_dict(object->raw, state->raw_state) < 0) {
            buffered_leave_lock(state);
            return -1;
        }
    }
    if (buffered_copy_target(state) < 0) {
        buffered_leave_lock(state);
        return -1;
    }
    return 0;
}

static const AleffAdapterVTable buffered_call_vtable = {
    .copy_state = buffered_copy_state,
    .free_state = buffered_free_state,
    .resume = buffered_resume,
    .prepare_resume = buffered_prepare_resume,
};

static PyObject *
buffered_find_original(PyObject *receiver, const char *name)
{
    PyObject *mro = Py_TYPE(receiver)->tp_mro;
    if (mro == NULL) {
        return NULL;
    }
    for (Py_ssize_t mro_index = 0; mro_index < PyTuple_GET_SIZE(mro); mro_index++) {
        PyTypeObject *candidate = (PyTypeObject *)PyTuple_GET_ITEM(mro, mro_index);
        for (Py_ssize_t index = 0; index < backup_count; index++) {
            BufferedMethodBackup *backup = &backups[index];
            if (backup->type == candidate && strcmp(backup->name, name) == 0) {
                return Py_NewRef(backup->original);
            }
        }
    }
    PyErr_Format(PyExc_RuntimeError, "missing buffered adapter method %s", name);
    return NULL;
}

static int
buffered_is_random(PyObject *receiver)
{
    return buffered_types[2] != NULL &&
        PyObject_TypeCheck(receiver, (PyTypeObject *)buffered_types[2]);
}

static int
buffered_is_reader(PyObject *receiver)
{
    return buffered_types[0] != NULL &&
        PyObject_TypeCheck(receiver, (PyTypeObject *)buffered_types[0]) &&
        !buffered_is_random(receiver);
}

static int
buffered_setup_state(
    BufferedCallState *state,
    PyObject *receiver,
    PyObject *arguments,
    const char *name
)
{
    BufferedObject *object = buffered_object(receiver);
    if (!buffered_operation_from_name(name, &state->operation)) {
        state->operation = BUFFERED_PAIR_CLOSE;
    }
    if (object == NULL) {
        state->phase = BUFFERED_DONE;
        return 0;
    }
    if (buffered_snapshot_capture(&state->initial, object) < 0) {
        return -1;
    }
    if (buffered_prepare_result(state) < 0) {
        return -1;
    }
    state->after_phase = BUFFERED_DONE;
    state->after_write_phase = BUFFERED_DONE;
    switch (state->operation) {
        case BUFFERED_READ:
            if (PyTuple_GET_SIZE(arguments) > 0) {
                Py_ssize_t size;
                if (buffered_argument_size(state, &size) < 0) return -1;
                if (size < 0) {
                    state->phase = BUFFERED_RAW_READ;
                }
                else {
                    Py_ssize_t block_size = buffered_read_block_size(
                        &state->initial, size - state->written
                    );
                    state->raw_direct = block_size > 0;
                    state->raw_length = state->raw_direct
                        ? block_size : object->buffer_size;
                    state->phase = BUFFERED_RAW_READINTO;
                    if (buffered_is_random(receiver)) {
                        state->after_phase = BUFFERED_RAW_READINTO;
                        state->phase = BUFFERED_RAW_SEEK;
                    }
                }
            }
            else {
                if (buffered_is_random(receiver)) {
                    state->after_phase = BUFFERED_RAW_READ;
                    state->phase = BUFFERED_RAW_SEEK;
                }
                else {
                    state->phase = BUFFERED_RAW_READ;
                }
            }
            break;
        case BUFFERED_READ1:
            state->raw_direct = 1;
            state->raw_length = state->total - state->written;
            state->phase = BUFFERED_RAW_READINTO;
            if (buffered_is_random(receiver)) {
                state->after_phase = BUFFERED_RAW_READINTO;
                state->phase = BUFFERED_RAW_SEEK;
            }
            break;
        case BUFFERED_READINTO:
        case BUFFERED_READINTO1:
            state->raw_direct = state->total - state->written > object->buffer_size;
            state->raw_length = state->raw_direct
                ? state->total - state->written : object->buffer_size;
            state->phase = BUFFERED_RAW_READINTO;
            if (buffered_is_random(receiver)) {
                state->after_phase = BUFFERED_RAW_READINTO;
                state->phase = BUFFERED_RAW_SEEK;
            }
            break;
        case BUFFERED_PEEK:
            state->total = object->buffer_size;
            state->one_raw_read = 1;
            state->raw_length = object->buffer_size;
            state->phase = BUFFERED_RAW_READINTO;
            if (buffered_is_random(receiver)) {
                state->after_phase = BUFFERED_RAW_READINTO;
                state->phase = BUFFERED_RAW_SEEK;
            }
            break;
        case BUFFERED_WRITE:
            {
                Py_buffer view;
                if (PyTuple_GET_SIZE(arguments) != 1 ||
                    PyObject_GetBuffer(
                        PyTuple_GET_ITEM(arguments, 0), &view, PyBUF_SIMPLE
                    ) < 0) return -1;
                state->total = view.len;
                PyBuffer_Release(&view);
                state->written = 0;
                if (object->write_end != -1 &&
                    object->write_pos < object->write_end) {
                    state->source_is_buffer = 1;
                    state->raw_length = Py_SAFE_DOWNCAST(
                        object->write_end - object->write_pos,
                        AleffBufferedOff_t, Py_ssize_t
                    );
                    if (buffered_write_rewind(&state->initial) != 0) {
                        state->after_phase = BUFFERED_RAW_WRITE;
                        state->phase = BUFFERED_RAW_SEEK;
                    }
                    else {
                        state->phase = BUFFERED_RAW_WRITE;
                    }
                }
                else {
                    state->raw_length = state->total;
                    state->phase = BUFFERED_RAW_WRITE;
                }
            }
            break;
        case BUFFERED_SEEK:
            if (object->writable && object->write_end != -1 &&
                object->write_pos < object->write_end) {
                state->source_is_buffer = 1;
                state->raw_length = Py_SAFE_DOWNCAST(
                    object->write_end - object->write_pos,
                        AleffBufferedOff_t, Py_ssize_t
                );
                state->after_write_phase = BUFFERED_RAW_SEEK;
                if (buffered_write_rewind(&state->initial) != 0) {
                    state->after_phase = BUFFERED_RAW_WRITE;
                    state->phase = BUFFERED_RAW_SEEK;
                }
                else {
                    state->after_phase = BUFFERED_RAW_SEEK;
                    state->phase = BUFFERED_RAW_WRITE;
                }
            }
            else {
                state->phase = BUFFERED_RAW_SEEK;
            }
            break;
        case BUFFERED_TELL:
            state->phase = BUFFERED_RAW_TELL;
            break;
        case BUFFERED_FLUSH:
            if (buffered_is_reader(receiver)) {
                state->phase = BUFFERED_RAW_FLUSH;
            }
            else if (object->writable && object->write_end != -1 &&
                object->write_pos < object->write_end) {
                state->source_is_buffer = 1;
                state->raw_length = Py_SAFE_DOWNCAST(
                    object->write_end - object->write_pos,
                        AleffBufferedOff_t, Py_ssize_t
                );
                state->phase = BUFFERED_RAW_WRITE;
                if (buffered_is_random(receiver)) {
                    state->after_phase = BUFFERED_RAW_SEEK;
                    state->seek_rewind = 2;
                }
                if (buffered_write_rewind(&state->initial) != 0) {
                    state->after_write_phase = state->after_phase;
                    state->after_phase = BUFFERED_RAW_WRITE;
                    state->seek_rewind = 0;
                    state->phase = BUFFERED_RAW_SEEK;
                }
            }
            else if (object->readable) {
                state->seek_rewind = 1;
                state->phase = BUFFERED_RAW_SEEK;
            }
            break;
        case BUFFERED_CLOSE:
            if (buffered_is_reader(receiver)) {
                state->after_phase = BUFFERED_RAW_CLOSE;
                state->phase = BUFFERED_RAW_FLUSH;
            }
            else if (object->writable && object->write_end != -1 &&
                object->write_pos < object->write_end) {
                state->source_is_buffer = 1;
                state->raw_length = Py_SAFE_DOWNCAST(
                    object->write_end - object->write_pos,
                        AleffBufferedOff_t, Py_ssize_t
                );
                state->after_write_phase = buffered_is_random(receiver)
                    ? BUFFERED_RAW_SEEK : BUFFERED_RAW_CLOSE;
                if (buffered_write_rewind(&state->initial) != 0) {
                    state->after_phase = BUFFERED_RAW_WRITE;
                    state->phase = BUFFERED_RAW_SEEK;
                }
                else {
                    state->after_phase = BUFFERED_RAW_CLOSE;
                    state->phase = BUFFERED_RAW_WRITE;
                }
            }
            else if (object->readable) {
                state->after_phase = BUFFERED_RAW_CLOSE;
                state->seek_rewind = 1;
                state->phase = BUFFERED_RAW_SEEK;
            }
            else {
                state->phase = BUFFERED_RAW_CLOSE;
            }
            break;
        default:
            break;
    }
    return 0;
}

static PyObject *
buffered_call_method(PyObject *receiver, PyObject *arguments, const char *name)
{
    PyObject *original = buffered_find_original(receiver, name);
    if (original == NULL) {
        return NULL;
    }
    Py_ssize_t count = PyTuple_GET_SIZE(arguments);
    PyObject *call_arguments = PyTuple_New(count + 1);
    if (call_arguments == NULL) {
        Py_DECREF(original);
        return NULL;
    }
    PyTuple_SET_ITEM(call_arguments, 0, Py_NewRef(receiver));
    for (Py_ssize_t index = 0; index < count; index++) {
        PyTuple_SET_ITEM(
            call_arguments,
            index + 1,
            Py_NewRef(PyTuple_GET_ITEM(arguments, index))
        );
    }
    BufferedCallState state = {
        .original = original,
        .receiver = Py_NewRef(receiver),
        .arguments = Py_NewRef(arguments),
        .live_state = 1,
    };
    if (buffered_setup_state(&state, receiver, arguments, name) < 0) {
        Py_DECREF(call_arguments);
        buffered_clear_state(&state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &buffered_call_vtable, &state) < 0) {
        Py_DECREF(call_arguments);
        buffered_clear_state(&state);
        return NULL;
    }
    PyObject *result = PyObject_Call(state.original, call_arguments, NULL);
    adapter_leave(&frame);
    Py_DECREF(call_arguments);
    buffered_clear_state(&state);
    return result;
}

static PyObject *
buffered_pair_close_method(PyObject *receiver)
{
    BufferedCallState state = {
        .original = Py_NewRef(Py_None),
        .receiver = Py_NewRef(receiver),
        .arguments = PyTuple_New(0),
        .operation = BUFFERED_PAIR_CLOSE,
        .phase = BUFFERED_PAIR_WRITER,
        .live_state = 1,
    };
    if (state.arguments == NULL) {
        buffered_clear_state(&state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &buffered_call_vtable, &state) < 0) {
        buffered_clear_state(&state);
        return NULL;
    }
    BufferedRWPairObject *pair = buffered_pair_object(receiver);
    PyObject *result = PyObject_CallMethod(
        (PyObject *)pair->writer, "close", NULL
    );
    PyObject *processed = buffered_process_pair_close(&state, result);
    Py_XDECREF(result);
    result = processed;
    adapter_leave(&frame);
    buffered_clear_state(&state);
    return result;
}

#define BUFFERED_WRAPPER(name) \
    static PyObject *buffered_##name(PyObject *self, PyObject *args) \
    { \
        return buffered_call_method(self, args, #name); \
    }

BUFFERED_WRAPPER(read)
BUFFERED_WRAPPER(read1)
BUFFERED_WRAPPER(readinto)
BUFFERED_WRAPPER(readinto1)
BUFFERED_WRAPPER(peek)
BUFFERED_WRAPPER(write)
BUFFERED_WRAPPER(seek)
BUFFERED_WRAPPER(tell)
BUFFERED_WRAPPER(flush)
static PyObject *
buffered_close(PyObject *self, PyObject *args)
{
    if (buffered_pair_object(self) != NULL) {
        if (PyTuple_GET_SIZE(args) != 0) {
            PyErr_SetString(PyExc_TypeError, "close() takes no arguments");
            return NULL;
        }
        return buffered_pair_close_method(self);
    }
    return buffered_call_method(self, args, "close");
}

static PyCFunction buffered_functions[] = {
    (PyCFunction)buffered_read,
    (PyCFunction)buffered_read1,
    (PyCFunction)buffered_readinto,
    (PyCFunction)buffered_readinto1,
    (PyCFunction)buffered_peek,
    (PyCFunction)buffered_write,
    (PyCFunction)buffered_seek,
    (PyCFunction)buffered_tell,
    (PyCFunction)buffered_flush,
    (PyCFunction)buffered_close,
};

static const char *const buffered_names[] = {
    "read", "read1", "readinto", "readinto1", "peek",
    "write", "seek", "tell", "flush", "close",
};

static int
buffered_replace_method(PyTypeObject *type, const char *name, PyCFunction function)
{
    PyObject *dict = PyType_GetDict(type);
    if (dict == NULL) {
        return -1;
    }
    PyObject *original = PyDict_GetItemString(dict, name);
    if (original == NULL) {
        Py_DECREF(dict);
        return 0;
    }
    if (!Py_IS_TYPE(original, &PyMethodDescr_Type)) {
        Py_DECREF(dict);
        PyErr_Format(PyExc_RuntimeError, "io.%s is not a method descriptor", name);
        return -1;
    }
    if (backup_count >= (Py_ssize_t)(sizeof(backups) / sizeof(*backups))) {
        Py_DECREF(dict);
        PyErr_SetString(PyExc_RuntimeError, "too many buffered adapter methods");
        return -1;
    }
    BufferedMethodBackup *backup = &backups[backup_count];
    backup->type = type;
    backup->name = name;
    backup->original = Py_NewRef(original);
    PyMethodDef *replacement = &replacement_methods[backup_count];
    *replacement = *((PyMethodDescrObject *)original)->d_method;
    replacement->ml_name = name;
    replacement->ml_meth = function;
    replacement->ml_flags = METH_VARARGS;
    PyObject *descriptor = PyDescr_NewMethod(type, replacement);
    if (descriptor == NULL || PyDict_SetItemString(dict, name, descriptor) < 0) {
        Py_XDECREF(descriptor);
        Py_CLEAR(backup->original);
        Py_DECREF(dict);
        return -1;
    }
    Py_DECREF(descriptor);
    Py_DECREF(dict);
    PyType_Modified(type);
    backup_count++;
    return 0;
}

int
adapter_io_buffered_install(PyObject *io_module)
{
    if (buffered_installed) {
        return 0;
    }
    installed_io = Py_NewRef(io_module);
    static const char *const type_names[] = {
        "BufferedReader", "BufferedWriter", "BufferedRandom", "BufferedRWPair",
    };
    for (int type_index = 0; type_index < 4; type_index++) {
        PyObject *type_object = PyObject_GetAttrString(
            io_module,
            type_names[type_index]
        );
        if (type_object == NULL || !PyType_Check(type_object)) {
            Py_XDECREF(type_object);
            PyErr_Format(PyExc_RuntimeError, "io.%s is not a type", type_names[type_index]);
            adapter_io_buffered_rollback();
            return -1;
        }
        buffered_types[type_index] = type_object;
        for (int method_index = 0; method_index < 10; method_index++) {
            if (buffered_replace_method(
                    (PyTypeObject *)type_object,
                    buffered_names[method_index],
                    buffered_functions[method_index]
                ) < 0) {
                adapter_io_buffered_rollback();
                return -1;
            }
        }
    }
    buffered_installed = 1;
    return 0;
}

void
adapter_io_buffered_rollback(void)
{
    for (Py_ssize_t index = backup_count - 1; index >= 0; index--) {
        BufferedMethodBackup *backup = &backups[index];
        PyObject *dict = PyType_GetDict(backup->type);
        if (dict != NULL &&
            PyDict_SetItemString(dict, backup->name, backup->original) < 0) {
            PyErr_Clear();
        }
        Py_XDECREF(dict);
        PyType_Modified(backup->type);
        Py_CLEAR(backup->original);
    }
    backup_count = 0;
    for (PyObject **type = buffered_types;
         type < buffered_types + 4;
         type++) {
        Py_CLEAR(*type);
    }
    Py_CLEAR(installed_io);
    buffered_installed = 0;
}
