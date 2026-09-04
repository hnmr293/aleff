#include "api.h"
#include "io.h"

#include <limits.h>
#include <stddef.h>

typedef enum {
    IO_ITER_WAIT_CLOSED,
    IO_ITER_WAIT_CLOSED_TRUTH,
} IoIterPhase;

typedef enum {
    IO_TRUTH_BOOL,
    IO_TRUTH_LENGTH,
} IoTruthKind;

typedef struct {
    PyObject *receiver;
    PyObject *receiver_state;
    PyObject *closed;
    IoIterPhase phase;
    IoTruthKind truth_kind;
} IoIterState;

typedef struct {
    PyObject *receiver;
    PyObject *receiver_state;
    PyObject *closed;
    IoTruthKind truth_kind;
    int phase;
} IoCloseState;

enum {
    IO_CLOSE_WAIT_CLOSED,
    IO_CLOSE_WAIT_CLOSED_TRUTH,
    IO_CLOSE_WAIT_FLUSH,
};

typedef enum {
    IO_NEXT_WAIT_READLINE,
    IO_NEXT_WAIT_LINE_SIZE,
} IoNextPhase;

typedef struct {
    PyObject *receiver;
    PyObject *receiver_state;
    PyObject *line;
    IoNextPhase phase;
} IoNextState;

typedef enum {
    IO_READLINES_WAIT_ITER,
    IO_READLINES_WAIT_NEXT,
    IO_READLINES_WAIT_LINE_SIZE,
} IoReadlinesPhase;

typedef struct {
    PyObject *receiver;
    PyObject *receiver_state;
    PyObject *iterator;
    PyObject *result;
    PyObject *line;
    Py_ssize_t hint;
    Py_ssize_t length;
    IoReadlinesPhase phase;
} IoReadlinesState;

typedef enum {
    IO_WRITELINES_WAIT_CLOSED,
    IO_WRITELINES_WAIT_CLOSED_TRUTH,
    IO_WRITELINES_WAIT_ITER,
    IO_WRITELINES_WAIT_NEXT,
    IO_WRITELINES_WAIT_WRITE,
    IO_WRITELINES_WAIT_RESULT_RELEASE,
    IO_WRITELINES_WAIT_LINE_RELEASE,
} IoWritelinesPhase;

typedef struct {
    PyObject *receiver;
    PyObject *receiver_state;
    PyObject *closed;
    PyObject *lines;
    PyObject *iterator;
    PyObject *line;
    PyObject *write_result;
    Py_ssize_t index;
    int sequence;
    IoTruthKind truth_kind;
    IoWritelinesPhase phase;
} IoWritelinesState;

typedef enum {
    IO_OPEN_WAIT_PATH,
    IO_OPEN_WAIT_OPENER,
} IoOpenPhase;

typedef struct {
    PyObject *file;
    PyObject *args;
    PyObject *kwargs;
    int file_in_args;
    int opener_in_args;
    IoOpenPhase phase;
} IoOpenState;

typedef struct {
    PyTypeObject *type;
    const char *name;
    PyObject *original;
} IoMethodBackup;

static const AleffAdapterVTable io_iter_vtable;
static const AleffAdapterVTable io_close_vtable;
static const AleffAdapterVTable io_next_vtable;
static const AleffAdapterVTable io_readlines_vtable;
static const AleffAdapterVTable io_writelines_vtable;
static const AleffAdapterVTable io_open_vtable;
static PyObject *installed_io;
static PyObject *original_io_open;
static PyMethodDef io_open_method;
static PyMethodDef io_replacement_methods[4];
static IoMethodBackup io_method_backups[4];
static Py_ssize_t io_method_backup_count;
static getiterfunc original_iobase_iter;
static iternextfunc original_iobase_next;
static PyObject *original_iobase_close;
static PyTypeObject *installed_iobase;
static int io_installed;

static int
io_lookup_closed(PyObject *receiver, PyObject **value)
{
#if PY_VERSION_HEX >= 0x030d0000
    return PyObject_GetOptionalAttrString(receiver, "closed", value);
#else
    PyObject *name = PyUnicode_FromString("closed");
    if (name == NULL) {
        return -1;
    }
    int result = _PyObject_LookupAttr(receiver, name, value);
    Py_DECREF(name);
    return result;
#endif
}

static PyObject *
io_copy_receiver_state(PyObject *receiver, PyObject *saved)
{
    if (saved != NULL) {
        return PyDict_Copy(saved);
    }
    PyObject *state = PyObject_GetAttrString(receiver, "__dict__");
    if (state == NULL) {
        PyErr_Clear();
        return NULL;
    }
    if (!PyDict_Check(state)) {
        Py_DECREF(state);
        return NULL;
    }
    PyObject *copy = PyDict_Copy(state);
    Py_DECREF(state);
    return copy;
}

static int
io_restore_receiver_state(PyObject *receiver, PyObject *saved)
{
    if (saved == NULL) {
        return 0;
    }
    PyObject *state = PyObject_GetAttrString(receiver, "__dict__");
    if (state == NULL) {
        return -1;
    }
    if (!PyDict_Check(state)) {
        Py_DECREF(state);
        return 0;
    }
    PyDict_Clear(state);
    int result = PyDict_Update(state, saved);
    Py_DECREF(state);
    return result;
}

static int
io_prepare_receiver_resume(void *raw_state)
{
    IoIterState *state = raw_state;
    return io_restore_receiver_state(
        state->receiver,
        state->receiver_state
    );
}

static IoTruthKind
io_truth_kind(PyObject *value)
{
    if (Py_TYPE(value)->tp_as_number != NULL &&
        Py_TYPE(value)->tp_as_number->nb_bool != NULL) {
        return IO_TRUTH_BOOL;
    }
    return IO_TRUTH_LENGTH;
}

static int
io_length_from_result(PyObject *value, Py_ssize_t *length)
{
    Py_ssize_t result = PyNumber_AsSsize_t(value, PyExc_OverflowError);
    if (result < 0) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_ValueError, "__len__() should return >= 0");
        }
        return -1;
    }
    *length = result;
    return 0;
}

static int
io_truth_from_result(
    PyObject *value,
    IoTruthKind kind,
    int *truth
)
{
    if (kind == IO_TRUTH_BOOL) {
        if (!PyBool_Check(value)) {
            PyErr_Format(
                PyExc_TypeError,
                "__bool__ should return bool, returned %.200s",
                Py_TYPE(value)->tp_name
            );
            return -1;
        }
        *truth = value == Py_True;
        return 0;
    }
    Py_ssize_t length;
    if (io_length_from_result(value, &length) < 0) {
        return -1;
    }
    *truth = length != 0;
    return 0;
}

static void *
io_iter_copy_state(const void *raw_state)
{
    const IoIterState *source = raw_state;
    IoIterState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->receiver = Py_NewRef(source->receiver);
    copy->receiver_state = io_copy_receiver_state(
        source->receiver,
        source->receiver_state
    );
    if (copy->receiver_state == NULL && PyErr_Occurred()) {
        Py_DECREF(copy->receiver);
        PyMem_Free(copy);
        return NULL;
    }
    copy->closed = Py_XNewRef(source->closed);
    copy->phase = source->phase;
    copy->truth_kind = source->truth_kind;
    return copy;
}

static void
io_iter_clear_state(IoIterState *state)
{
    Py_DECREF(state->receiver);
    Py_XDECREF(state->receiver_state);
    Py_XDECREF(state->closed);
}

static void
io_iter_free_state(void *raw_state)
{
    IoIterState *state = raw_state;
    if (state == NULL) {
        return;
    }
    io_iter_clear_state(state);
    PyMem_Free(state);
}

static PyObject *
io_iter_finish(IoIterState *state, int closed)
{
    Py_CLEAR(state->closed);
    if (closed) {
        PyErr_SetString(PyExc_ValueError, "I/O operation on closed file.");
        return NULL;
    }
    return Py_NewRef(state->receiver);
}

static PyObject *
io_iter_continue(IoIterState *state, PyObject *value, int is_resumed)
{
    if (is_resumed) {
        if (value == NULL) {
            return NULL;
        }
        if (state->phase == IO_ITER_WAIT_CLOSED) {
            state->closed = Py_NewRef(value);
            state->truth_kind = io_truth_kind(value);
            state->phase = IO_ITER_WAIT_CLOSED_TRUTH;
            int closed = PyObject_IsTrue(state->closed);
            if (closed < 0) {
                return NULL;
            }
            return io_iter_finish(state, closed);
        }
        int closed;
        if (io_truth_from_result(value, state->truth_kind, &closed) < 0) {
            return NULL;
        }
        return io_iter_finish(state, closed);
    }

    PyObject *closed = NULL;
    int found = io_lookup_closed(state->receiver, &closed);
    if (found < 0) {
        return NULL;
    }
    if (!found) {
        return Py_NewRef(state->receiver);
    }
    state->closed = closed;
    state->truth_kind = io_truth_kind(closed);
    state->phase = IO_ITER_WAIT_CLOSED_TRUTH;
    int result = PyObject_IsTrue(state->closed);
    if (result < 0) {
        return NULL;
    }
    return io_iter_finish(state, result);
}

static PyObject *
io_iter_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    IoIterState *state = io_iter_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &io_iter_vtable, state) < 0) {
        io_iter_free_state(state);
        return NULL;
    }
    PyObject *result = io_iter_continue(state, value, 1);
    adapter_leave(&frame);
    io_iter_free_state(state);
    return result;
}

static const AleffAdapterVTable io_iter_vtable = {
    .copy_state = io_iter_copy_state,
    .free_state = io_iter_free_state,
    .resume = io_iter_resume,
    .prepare_resume = io_prepare_receiver_resume,
};

static PyObject *
io_iter_slot(PyObject *receiver)
{
    IoIterState state = {
        .receiver = Py_NewRef(receiver),
        .phase = IO_ITER_WAIT_CLOSED,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &io_iter_vtable, &state) < 0) {
        io_iter_clear_state(&state);
        return NULL;
    }
    PyObject *result = io_iter_continue(&state, NULL, 0);
    adapter_leave(&frame);
    io_iter_clear_state(&state);
    return result;
}

static void *
io_close_copy_state(const void *raw_state)
{
    const IoCloseState *source = raw_state;
    IoCloseState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->receiver = Py_NewRef(source->receiver);
    copy->receiver_state = io_copy_receiver_state(
        source->receiver,
        source->receiver_state
    );
    if (copy->receiver_state == NULL && PyErr_Occurred()) {
        Py_DECREF(copy->receiver);
        PyMem_Free(copy);
        return NULL;
    }
    copy->closed = Py_XNewRef(source->closed);
    copy->truth_kind = source->truth_kind;
    copy->phase = source->phase;
    return copy;
}

static void
io_close_free_state(void *raw_state)
{
    IoCloseState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->receiver);
    Py_XDECREF(state->receiver_state);
    Py_XDECREF(state->closed);
    PyMem_Free(state);
}

static int
io_close_mark_closed(PyObject *receiver)
{
    PyObject *dictionary = PyObject_GetAttrString(receiver, "__dict__");
    if (dictionary == NULL) {
        return -1;
    }
    if (!PyDict_Check(dictionary)) {
        Py_DECREF(dictionary);
        PyErr_SetString(PyExc_TypeError, "I/O base object has no instance dictionary");
        return -1;
    }
    int result = PyDict_SetItemString(dictionary, "__IOBase_closed", Py_True);
    Py_DECREF(dictionary);
    return result;
}

static PyObject *
io_close_finish_flush(IoCloseState *state, PyObject *value)
{
    PyObject *pending_exception = value == NULL
        ? PyErr_GetRaisedException() : NULL;
    if (io_close_mark_closed(state->receiver) < 0) {
        if (pending_exception != NULL) {
            PyObject *close_exception = PyErr_GetRaisedException();
            if (close_exception != NULL) {
                PyException_SetContext(close_exception, pending_exception);
                PyErr_SetRaisedException(close_exception);
            }
            else {
                PyErr_SetRaisedException(pending_exception);
            }
        }
        return NULL;
    }
    if (pending_exception != NULL) {
        PyErr_SetRaisedException(pending_exception);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
io_close_flush(IoCloseState *state)
{
    state->phase = IO_CLOSE_WAIT_FLUSH;
    PyObject *result = PyObject_CallMethod(state->receiver, "flush", NULL);
    PyObject *finished = io_close_finish_flush(state, result);
    Py_XDECREF(result);
    return finished;
}

static PyObject *
io_close_continue(IoCloseState *state, PyObject *value, int resumed)
{
    if (resumed) {
        if (value == NULL) {
            if (state->phase == IO_CLOSE_WAIT_FLUSH) {
                return io_close_finish_flush(state, NULL);
            }
            return NULL;
        }
        if (state->phase == IO_CLOSE_WAIT_CLOSED) {
            state->closed = Py_NewRef(value);
            state->truth_kind = io_truth_kind(value);
            state->phase = IO_CLOSE_WAIT_CLOSED_TRUTH;
            int closed = PyObject_IsTrue(value);
            if (closed < 0) return NULL;
            return closed ? Py_NewRef(Py_None) : io_close_flush(state);
        }
        if (state->phase == IO_CLOSE_WAIT_CLOSED_TRUTH) {
            int closed;
            if (io_truth_from_result(value, state->truth_kind, &closed) < 0) {
                return NULL;
            }
            return closed ? Py_NewRef(Py_None) : io_close_flush(state);
        }
        return io_close_finish_flush(state, value);
    }

    PyObject *closed = NULL;
    state->phase = IO_CLOSE_WAIT_CLOSED;
    int found = io_lookup_closed(state->receiver, &closed);
    if (found < 0) return NULL;
    if (found) {
        state->closed = closed;
        state->truth_kind = io_truth_kind(closed);
        state->phase = IO_CLOSE_WAIT_CLOSED_TRUTH;
        int is_closed = PyObject_IsTrue(closed);
        if (is_closed < 0) return NULL;
        if (is_closed) return Py_NewRef(Py_None);
    }
    return io_close_flush(state);
}

static PyObject *
io_close_resume(const void *raw_state, PyObject *value)
{
    IoCloseState *state = io_close_copy_state(raw_state);
    if (state == NULL) return NULL;
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &io_close_vtable, state) < 0) {
        io_close_free_state(state);
        return NULL;
    }
    PyObject *result = io_close_continue(state, value, 1);
    adapter_leave(&frame);
    io_close_free_state(state);
    return result;
}

static const AleffAdapterVTable io_close_vtable = {
    .copy_state = io_close_copy_state,
    .free_state = io_close_free_state,
    .resume = io_close_resume,
    .prepare_resume = io_prepare_receiver_resume,
};

static PyObject *
io_close_wrapper(PyObject *receiver, PyObject *args)
{
    if (PyTuple_GET_SIZE(args) != 0) {
        PyObject *arguments = PyTuple_New(PyTuple_GET_SIZE(args) + 1);
        if (arguments == NULL) {
            return NULL;
        }
        PyTuple_SET_ITEM(arguments, 0, Py_NewRef(receiver));
        for (Py_ssize_t index = 0; index < PyTuple_GET_SIZE(args); index++) {
            PyTuple_SET_ITEM(
                arguments,
                index + 1,
                Py_NewRef(PyTuple_GET_ITEM(args, index))
            );
        }
        PyObject *result = PyObject_Call(original_iobase_close, arguments, NULL);
        Py_DECREF(arguments);
        return result;
    }

    IoCloseState state = {
        .receiver = Py_NewRef(receiver),
        .phase = IO_CLOSE_WAIT_CLOSED,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &io_close_vtable, &state) < 0) {
        Py_DECREF(state.receiver);
        return NULL;
    }
    PyObject *result = io_close_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.closed);
    Py_DECREF(state.receiver);
    return result;
}

static void *
io_next_copy_state(const void *raw_state)
{
    const IoNextState *source = raw_state;
    IoNextState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->receiver = Py_NewRef(source->receiver);
    copy->receiver_state = io_copy_receiver_state(
        source->receiver,
        source->receiver_state
    );
    if (copy->receiver_state == NULL && PyErr_Occurred()) {
        Py_DECREF(copy->receiver);
        PyMem_Free(copy);
        return NULL;
    }
    copy->line = Py_XNewRef(source->line);
    copy->phase = source->phase;
    return copy;
}

static void
io_next_clear_state(IoNextState *state)
{
    Py_DECREF(state->receiver);
    Py_XDECREF(state->receiver_state);
    Py_XDECREF(state->line);
}

static void
io_next_free_state(void *raw_state)
{
    IoNextState *state = raw_state;
    if (state == NULL) {
        return;
    }
    io_next_clear_state(state);
    PyMem_Free(state);
}

static PyObject *
io_next_after_line(IoNextState *state, PyObject *line, int owned)
{
    state->line = owned ? line : Py_NewRef(line);
    state->phase = IO_NEXT_WAIT_LINE_SIZE;
    Py_ssize_t size = PyObject_Size(state->line);
    if (size < 0) {
        return NULL;
    }
    if (size == 0) {
        Py_CLEAR(state->line);
        return NULL;
    }
    if (owned) {
        PyObject *result = state->line;
        state->line = NULL;
        return result;
    }
    PyObject *result = Py_NewRef(state->line);
    Py_CLEAR(state->line);
    return result;
}

static PyObject *
io_next_continue(IoNextState *state, PyObject *value, int is_resumed)
{
    if (is_resumed) {
        if (value == NULL) {
            return NULL;
        }
        if (state->phase == IO_NEXT_WAIT_READLINE) {
            return io_next_after_line(state, value, 0);
        }
        Py_ssize_t size = 0;
        if (io_length_from_result(value, &size) < 0) {
            return NULL;
        }
        PyObject *line = state->line;
        state->line = NULL;
        if (size == 0) {
            Py_DECREF(line);
            return NULL;
        }
        return line;
    }

    state->phase = IO_NEXT_WAIT_READLINE;
    PyObject *line = PyObject_CallMethod(state->receiver, "readline", NULL);
    if (line == NULL) {
        return NULL;
    }
    return io_next_after_line(state, line, 1);
}

static PyObject *
io_next_resume(const void *raw_state, PyObject *value)
{
    IoNextState *state = io_next_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &io_next_vtable, state) < 0) {
        io_next_free_state(state);
        return NULL;
    }
    PyObject *result = io_restore_receiver_state(
        state->receiver,
        state->receiver_state
    ) < 0 ? NULL : io_next_continue(state, value, 1);
    adapter_leave(&frame);
    io_next_free_state(state);
    return result;
}

static const AleffAdapterVTable io_next_vtable = {
    .copy_state = io_next_copy_state,
    .free_state = io_next_free_state,
    .resume = io_next_resume,
    .prepare_resume = io_prepare_receiver_resume,
};

static PyObject *
io_next_slot(PyObject *receiver)
{
    IoNextState state = {
        .receiver = Py_NewRef(receiver),
        .phase = IO_NEXT_WAIT_READLINE,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &io_next_vtable, &state) < 0) {
        io_next_clear_state(&state);
        return NULL;
    }
    PyObject *result = io_next_continue(&state, NULL, 0);
    adapter_leave(&frame);
    io_next_clear_state(&state);
    return result;
}

static void *
io_readlines_copy_state(const void *raw_state)
{
    const IoReadlinesState *source = raw_state;
    IoReadlinesState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->receiver = Py_NewRef(source->receiver);
    copy->receiver_state = io_copy_receiver_state(
        source->receiver,
        source->receiver_state
    );
    if (copy->receiver_state == NULL && PyErr_Occurred()) {
        Py_DECREF(copy->receiver);
        PyMem_Free(copy);
        return NULL;
    }
    copy->iterator = source->iterator == NULL
        ? NULL : clone_iterator_for_snapshot(source->iterator);
    if (source->iterator != NULL && copy->iterator == NULL) {
        Py_DECREF(copy->receiver);
        Py_XDECREF(copy->receiver_state);
        PyMem_Free(copy);
        return NULL;
    }
    copy->result = PyList_GetSlice(
        source->result,
        0,
        PyList_GET_SIZE(source->result)
    );
    if (copy->result == NULL) {
        Py_DECREF(copy->receiver);
        Py_XDECREF(copy->receiver_state);
        Py_XDECREF(copy->iterator);
        PyMem_Free(copy);
        return NULL;
    }
    copy->line = Py_XNewRef(source->line);
    copy->hint = source->hint;
    copy->length = source->length;
    copy->phase = source->phase;
    return copy;
}

static void
io_readlines_clear_state(IoReadlinesState *state)
{
    Py_DECREF(state->receiver);
    Py_XDECREF(state->receiver_state);
    Py_XDECREF(state->iterator);
    Py_DECREF(state->result);
    Py_XDECREF(state->line);
}

static void
io_readlines_free_state(void *raw_state)
{
    IoReadlinesState *state = raw_state;
    if (state == NULL) {
        return;
    }
    io_readlines_clear_state(state);
    PyMem_Free(state);
}

static PyObject *
io_readlines_continue(
    IoReadlinesState *state,
    PyObject *value,
    int is_resumed
);

static PyObject *
io_readlines_resume(const void *raw_state, PyObject *value)
{
    PyObject *pending_exception = value == NULL
        ? PyErr_GetRaisedException() : NULL;
    IoReadlinesState *state = io_readlines_copy_state(raw_state);
    if (state == NULL) {
        if (pending_exception != NULL) {
            PyObject *copy_exception = PyErr_GetRaisedException();
            if (copy_exception != NULL) {
                PyException_SetContext(copy_exception, pending_exception);
                PyErr_SetRaisedException(copy_exception);
            }
            else {
                PyErr_SetRaisedException(pending_exception);
            }
        }
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &io_readlines_vtable, state) < 0) {
        if (pending_exception != NULL) {
            PyObject *enter_exception = PyErr_GetRaisedException();
            PyException_SetContext(enter_exception, pending_exception);
            PyErr_SetRaisedException(enter_exception);
        }
        io_readlines_free_state(state);
        return NULL;
    }
    if (pending_exception != NULL) {
        PyErr_SetRaisedException(pending_exception);
    }
    PyObject *result = io_readlines_continue(state, value, 1);
    adapter_leave(&frame);
    io_readlines_free_state(state);
    return result;
}

static const AleffAdapterVTable io_readlines_vtable = {
    .copy_state = io_readlines_copy_state,
    .free_state = io_readlines_free_state,
    .resume = io_readlines_resume,
    .prepare_resume = io_prepare_receiver_resume,
};

static PyObject *
io_readlines_after_line(IoReadlinesState *state, PyObject *line, int owned)
{
    state->line = owned ? line : Py_NewRef(line);
    if (PyList_Append(state->result, state->line) < 0) {
        return NULL;
    }
    state->phase = IO_READLINES_WAIT_LINE_SIZE;
    Py_ssize_t line_length = PyObject_Size(state->line);
    if (line_length < 0) {
        return NULL;
    }
    Py_CLEAR(state->line);
    if (state->hint > 0 && line_length > state->hint - state->length) {
        return Py_NewRef(state->result);
    }
    state->length += line_length;
    state->phase = IO_READLINES_WAIT_NEXT;
    return NULL;
}

static PyObject *
io_readlines_continue(
    IoReadlinesState *state,
    PyObject *value,
    int is_resumed
)
{
    if (is_resumed) {
        if (value == NULL) {
            return NULL;
        }
        if (state->phase == IO_READLINES_WAIT_ITER) {
            if (!PyIter_Check(value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "iter() returned non-iterator of type '%.200s'",
                    Py_TYPE(value)->tp_name
                );
                return NULL;
            }
            state->iterator = Py_NewRef(value);
            state->phase = IO_READLINES_WAIT_NEXT;
        }
        else if (state->phase == IO_READLINES_WAIT_LINE_SIZE) {
            Py_ssize_t line_length = 0;
            if (io_length_from_result(value, &line_length) < 0) {
                return NULL;
            }
            Py_CLEAR(state->line);
            if (state->hint > 0 && line_length > state->hint - state->length) {
                return Py_NewRef(state->result);
            }
            state->length += line_length;
            state->phase = IO_READLINES_WAIT_NEXT;
        }
        else {
            PyObject *result = io_readlines_after_line(state, value, 0);
            if (result != NULL || PyErr_Occurred()) {
                return result;
            }
        }
    }
    else {
        state->phase = IO_READLINES_WAIT_ITER;
        state->iterator = PyObject_GetIter(state->receiver);
        if (state->iterator == NULL) {
            return NULL;
        }
        state->phase = IO_READLINES_WAIT_NEXT;
    }

    while (1) {
        state->phase = IO_READLINES_WAIT_NEXT;
        PyObject *line = PyIter_Next(state->iterator);
        if (line == NULL) {
            if (PyErr_Occurred()) {
                return NULL;
            }
            return Py_NewRef(state->result);
        }
        PyObject *result = io_readlines_after_line(state, line, 1);
        if (result != NULL) {
            return result;
        }
        if (PyErr_Occurred()) {
            return NULL;
        }
    }
}

static Py_ssize_t
io_readlines_hint(PyObject *args)
{
    PyObject *value = NULL;
    if (!PyArg_ParseTuple(args, "|O:readlines", &value)) {
        return -2;
    }
    if (value == NULL || value == Py_None) {
        return -1;
    }
    return PyNumber_AsSsize_t(value, PyExc_OverflowError);
}

static PyObject *
io_readlines_wrapper(PyObject *receiver, PyObject *args)
{
    Py_ssize_t hint = io_readlines_hint(args);
    if (PyErr_Occurred()) {
        return NULL;
    }
    IoReadlinesState state = {
        .receiver = Py_NewRef(receiver),
        .result = PyList_New(0),
        .hint = hint,
        .phase = IO_READLINES_WAIT_ITER,
    };
    if (state.result == NULL) {
        Py_DECREF(state.receiver);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &io_readlines_vtable, &state) < 0) {
        io_readlines_clear_state(&state);
        return NULL;
    }
    PyObject *result = io_readlines_continue(&state, NULL, 0);
    adapter_leave(&frame);
    io_readlines_clear_state(&state);
    return result;
}

static void *
io_writelines_copy_state(const void *raw_state)
{
    const IoWritelinesState *source = raw_state;
    IoWritelinesState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->receiver = Py_NewRef(source->receiver);
    copy->receiver_state = io_copy_receiver_state(
        source->receiver,
        source->receiver_state
    );
    if (copy->receiver_state == NULL && PyErr_Occurred()) {
        Py_DECREF(copy->receiver);
        PyMem_Free(copy);
        return NULL;
    }
    copy->closed = Py_XNewRef(source->closed);
    copy->lines = source->sequence
        ? PySequence_List(source->lines) : Py_NewRef(source->lines);
    if (copy->lines == NULL) {
        Py_DECREF(copy->receiver);
        Py_XDECREF(copy->receiver_state);
        Py_XDECREF(copy->closed);
        PyMem_Free(copy);
        return NULL;
    }
    copy->iterator = source->iterator == NULL
        ? NULL : clone_iterator_for_snapshot(source->iterator);
    if (source->iterator != NULL && copy->iterator == NULL) {
        Py_DECREF(copy->receiver);
        Py_XDECREF(copy->receiver_state);
        Py_XDECREF(copy->closed);
        Py_DECREF(copy->lines);
        PyMem_Free(copy);
        return NULL;
    }
    copy->line = Py_XNewRef(source->line);
    copy->write_result = Py_XNewRef(source->write_result);
    copy->index = source->index;
    copy->sequence = source->sequence;
    copy->phase = source->phase;
    return copy;
}

static void
io_writelines_clear_state(IoWritelinesState *state)
{
    Py_DECREF(state->receiver);
    Py_XDECREF(state->receiver_state);
    Py_XDECREF(state->closed);
    Py_DECREF(state->lines);
    Py_XDECREF(state->iterator);
    Py_XDECREF(state->line);
    Py_XDECREF(state->write_result);
}

static void
io_writelines_free_state(void *raw_state)
{
    IoWritelinesState *state = raw_state;
    if (state == NULL) {
        return;
    }
    io_writelines_clear_state(state);
    PyMem_Free(state);
}

static PyObject *
io_writelines_continue(
    IoWritelinesState *state,
    PyObject *value,
    int is_resumed
);

static PyObject *
io_writelines_resume(const void *raw_state, PyObject *value)
{
    PyObject *pending_exception = value == NULL
        ? PyErr_GetRaisedException() : NULL;
    IoWritelinesState *state = io_writelines_copy_state(raw_state);
    if (state == NULL) {
        if (pending_exception != NULL) {
            PyObject *copy_exception = PyErr_GetRaisedException();
            if (copy_exception != NULL) {
                PyException_SetContext(copy_exception, pending_exception);
                PyErr_SetRaisedException(copy_exception);
            }
            else {
                PyErr_SetRaisedException(pending_exception);
            }
        }
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &io_writelines_vtable, state) < 0) {
        if (pending_exception != NULL) {
            PyObject *enter_exception = PyErr_GetRaisedException();
            PyException_SetContext(enter_exception, pending_exception);
            PyErr_SetRaisedException(enter_exception);
        }
        io_writelines_free_state(state);
        return NULL;
    }
    if (pending_exception != NULL) {
        PyErr_SetRaisedException(pending_exception);
    }
    PyObject *result = io_writelines_continue(state, value, 1);
    adapter_leave(&frame);
    io_writelines_free_state(state);
    return result;
}

static const AleffAdapterVTable io_writelines_vtable = {
    .copy_state = io_writelines_copy_state,
    .free_state = io_writelines_free_state,
    .resume = io_writelines_resume,
    .prepare_resume = io_prepare_receiver_resume,
};

static int
io_writelines_start_lines(IoWritelinesState *state)
{
    state->phase = IO_WRITELINES_WAIT_ITER;
    if (PyList_CheckExact(state->lines) || PyTuple_CheckExact(state->lines)) {
        state->sequence = 1;
    }
    else {
        state->iterator = PyObject_GetIter(state->lines);
        if (state->iterator == NULL) {
            return -1;
        }
    }
    state->phase = IO_WRITELINES_WAIT_NEXT;
    return 0;
}

static PyObject *
io_writelines_next_line(IoWritelinesState *state)
{
    if (state->sequence) {
        Py_ssize_t size = PySequence_Fast_GET_SIZE(state->lines);
        if (state->index >= size) {
            return NULL;
        }
        return Py_NewRef(PySequence_Fast_GET_ITEM(state->lines, state->index++));
    }
    return PyIter_Next(state->iterator);
}

static PyObject *
io_writelines_continue(
    IoWritelinesState *state,
    PyObject *value,
    int is_resumed
)
{
    if (is_resumed) {
        if (state->phase == IO_WRITELINES_WAIT_CLOSED) {
            if (value == NULL) {
                return NULL;
            }
            state->closed = Py_NewRef(value);
            state->truth_kind = io_truth_kind(value);
            state->phase = IO_WRITELINES_WAIT_CLOSED_TRUTH;
            int closed = PyObject_IsTrue(state->closed);
            if (closed < 0) {
                return NULL;
            }
            Py_CLEAR(state->closed);
            if (closed) {
                PyErr_SetString(PyExc_ValueError, "I/O operation on closed file.");
                return NULL;
            }
            if (io_writelines_start_lines(state) < 0) {
                return NULL;
            }
        }
        else if (state->phase == IO_WRITELINES_WAIT_CLOSED_TRUTH) {
            if (value == NULL) {
                return NULL;
            }
            int closed;
            if (io_truth_from_result(value, state->truth_kind, &closed) < 0) {
                return NULL;
            }
            Py_CLEAR(state->closed);
            if (closed) {
                PyErr_SetString(PyExc_ValueError, "I/O operation on closed file.");
                return NULL;
            }
            if (io_writelines_start_lines(state) < 0) {
                return NULL;
            }
        }
        else if (state->phase == IO_WRITELINES_WAIT_ITER) {
            if (value == NULL) {
                return NULL;
            }
            if (!PyIter_Check(value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "iter() returned non-iterator of type '%.200s'",
                    Py_TYPE(value)->tp_name
                );
                return NULL;
            }
            state->iterator = Py_NewRef(value);
            state->phase = IO_WRITELINES_WAIT_NEXT;
        }
        else if (state->phase == IO_WRITELINES_WAIT_NEXT) {
            if (value == NULL) {
                return NULL;
            }
            state->line = Py_NewRef(value);
            state->phase = IO_WRITELINES_WAIT_WRITE;
            PyObject *result = PyObject_CallMethod(
                state->receiver,
                "write",
                "O",
                state->line
            );
            if (result == NULL) {
                return NULL;
            }
            state->write_result = result;
            state->phase = IO_WRITELINES_WAIT_RESULT_RELEASE;
            Py_CLEAR(state->write_result);
            state->phase = IO_WRITELINES_WAIT_LINE_RELEASE;
            Py_CLEAR(state->line);
            state->phase = IO_WRITELINES_WAIT_NEXT;
        }
        else if (state->phase == IO_WRITELINES_WAIT_WRITE) {
            if (value == NULL) {
                return NULL;
            }
            state->write_result = Py_NewRef(value);
            state->phase = IO_WRITELINES_WAIT_RESULT_RELEASE;
            Py_CLEAR(state->write_result);
            state->phase = IO_WRITELINES_WAIT_LINE_RELEASE;
            Py_CLEAR(state->line);
            state->phase = IO_WRITELINES_WAIT_NEXT;
        }
        else if (state->phase == IO_WRITELINES_WAIT_RESULT_RELEASE) {
            if (value == NULL) {
                return NULL;
            }
            Py_CLEAR(state->write_result);
            state->phase = IO_WRITELINES_WAIT_LINE_RELEASE;
            Py_CLEAR(state->line);
            state->phase = IO_WRITELINES_WAIT_NEXT;
        }
        else {
            if (value == NULL) {
                return NULL;
            }
            Py_CLEAR(state->line);
            state->phase = IO_WRITELINES_WAIT_NEXT;
        }
    }
    else {
        PyObject *closed = NULL;
        state->phase = IO_WRITELINES_WAIT_CLOSED;
        int found = io_lookup_closed(state->receiver, &closed);
        if (found < 0) {
            return NULL;
        }
        if (found) {
            state->closed = closed;
            state->truth_kind = io_truth_kind(closed);
            state->phase = IO_WRITELINES_WAIT_CLOSED_TRUTH;
            int result = PyObject_IsTrue(state->closed);
            if (result < 0) {
                return NULL;
            }
            Py_CLEAR(state->closed);
            if (result) {
                PyErr_SetString(PyExc_ValueError, "I/O operation on closed file.");
                return NULL;
            }
        }
        if (io_writelines_start_lines(state) < 0) {
            return NULL;
        }
    }

    while (1) {
        state->phase = IO_WRITELINES_WAIT_NEXT;
        PyObject *line = io_writelines_next_line(state);
        if (line == NULL) {
            if (PyErr_Occurred()) {
                return NULL;
            }
            Py_RETURN_NONE;
        }
        state->line = line;
        state->phase = IO_WRITELINES_WAIT_WRITE;
        PyObject *result = PyObject_CallMethod(
            state->receiver,
            "write",
            "O",
            state->line
        );
        if (result == NULL) {
            return NULL;
        }
        state->write_result = result;
        state->phase = IO_WRITELINES_WAIT_RESULT_RELEASE;
        Py_CLEAR(state->write_result);
        state->phase = IO_WRITELINES_WAIT_LINE_RELEASE;
        Py_CLEAR(state->line);
        state->phase = IO_WRITELINES_WAIT_NEXT;
    }
}

static PyObject *
io_writelines_wrapper(PyObject *receiver, PyObject *args)
{
    PyObject *lines;
    if (!PyArg_UnpackTuple(args, "writelines", 1, 1, &lines)) {
        return NULL;
    }
    IoWritelinesState state = {
        .receiver = Py_NewRef(receiver),
        .lines = Py_NewRef(lines),
        .phase = IO_WRITELINES_WAIT_ITER,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &io_writelines_vtable, &state) < 0) {
        io_writelines_clear_state(&state);
        return NULL;
    }
    PyObject *result = io_writelines_continue(&state, NULL, 0);
    adapter_leave(&frame);
    io_writelines_clear_state(&state);
    return result;
}

static void
io_open_clear_state(IoOpenState *state)
{
    Py_XDECREF(state->file);
    Py_XDECREF(state->args);
    Py_XDECREF(state->kwargs);
    state->file = NULL;
    state->args = NULL;
    state->kwargs = NULL;
}

static void *
io_open_copy_state(const void *raw_state)
{
    const IoOpenState *source = raw_state;
    IoOpenState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->file = Py_NewRef(source->file);
    copy->args = PyTuple_GetSlice(
        source->args,
        0,
        PyTuple_GET_SIZE(source->args)
    );
    copy->kwargs = source->kwargs == NULL ? NULL : PyDict_Copy(source->kwargs);
    if (copy->args == NULL ||
        (source->kwargs != NULL && copy->kwargs == NULL)) {
        Py_DECREF(copy->file);
        Py_XDECREF(copy->args);
        Py_XDECREF(copy->kwargs);
        PyMem_Free(copy);
        return NULL;
    }
    copy->file_in_args = source->file_in_args;
    copy->opener_in_args = source->opener_in_args;
    copy->phase = source->phase;
    return copy;
}

static void
io_open_free_state(void *raw_state)
{
    IoOpenState *state = raw_state;
    if (state == NULL) {
        return;
    }
    io_open_clear_state(state);
    PyMem_Free(state);
}

static PyObject *
io_open_path_arguments(IoOpenState *state, PyObject *path, int owned)
{
    if (state->file_in_args) {
        Py_ssize_t count = PyTuple_GET_SIZE(state->args);
        PyObject *normalized = PyTuple_New(count);
        if (normalized == NULL) {
            if (owned) {
                Py_DECREF(path);
            }
            return NULL;
        }
        PyTuple_SET_ITEM(normalized, 0, Py_NewRef(path));
        for (Py_ssize_t i = 1; i < count; i++) {
            PyTuple_SET_ITEM(normalized, i, Py_NewRef(PyTuple_GET_ITEM(state->args, i)));
        }
        Py_SETREF(state->args, normalized);
    }
    else {
        PyObject *normalized = state->kwargs == NULL ? PyDict_New() : PyDict_Copy(state->kwargs);
        if (normalized == NULL || PyDict_SetItemString(normalized, "file", path) < 0) {
            Py_XDECREF(normalized);
            if (owned) {
                Py_DECREF(path);
            }
            return NULL;
        }
        Py_XSETREF(state->kwargs, normalized);
    }
    if (owned) {
        Py_DECREF(path);
    }
    state->phase = IO_OPEN_WAIT_OPENER;
    return PyObject_Call(original_io_open, state->args, state->kwargs);
}

static PyObject *
io_open_fd_arguments(IoOpenState *state, PyObject *descriptor)
{
    PyObject *args = PyTuple_New(PyTuple_GET_SIZE(state->args));
    if (args == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(state->args); i++) {
        PyObject *item = PyTuple_GET_ITEM(state->args, i);
        if (i == 0) {
            item = descriptor;
        }
        else if (state->opener_in_args && i == 7) {
            item = Py_None;
        }
        PyTuple_SET_ITEM(args, i, Py_NewRef(item));
    }
    PyObject *kwargs = state->kwargs == NULL ? NULL : PyDict_Copy(state->kwargs);
    if (state->kwargs != NULL && kwargs == NULL) {
        Py_DECREF(args);
        return NULL;
    }
    if (!state->file_in_args && PyDict_SetItemString(kwargs, "file", descriptor) < 0) {
        Py_DECREF(args);
        Py_DECREF(kwargs);
        return NULL;
    }
    if (!state->opener_in_args && kwargs != NULL &&
        PyDict_DelItemString(kwargs, "opener") < 0) {
        if (!PyErr_ExceptionMatches(PyExc_KeyError)) {
            Py_DECREF(args);
            Py_DECREF(kwargs);
            return NULL;
        }
        PyErr_Clear();
    }
    PyObject *result = PyObject_Call(original_io_open, args, kwargs);
    Py_DECREF(args);
    Py_XDECREF(kwargs);
    return result;
}

static PyObject *
io_open_continue(IoOpenState *state, PyObject *value, int is_resumed)
{
    if (is_resumed) {
        if (value == NULL) {
            return NULL;
        }
        if (state->phase == IO_OPEN_WAIT_PATH) {
            if (!PyUnicode_Check(value) && !PyBytes_Check(value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "expected %.200s.__fspath__() to return str or bytes, not %.200s",
                    Py_TYPE(state->file)->tp_name,
                    Py_TYPE(value)->tp_name
                );
                return NULL;
            }
            return io_open_path_arguments(state, value, 0);
        }
        if (!PyLong_Check(value)) {
            PyErr_SetString(PyExc_TypeError, "expected integer from opener");
            return NULL;
        }
        long descriptor = PyLong_AsLong(value);
        if (descriptor == -1 && PyErr_Occurred()) {
            return NULL;
        }
        if (descriptor < 0) {
            PyErr_Format(PyExc_ValueError, "opener returned %ld", descriptor);
            return NULL;
        }
        if (descriptor > INT_MAX) {
            PyErr_SetString(
                PyExc_OverflowError,
                "Python int too large to convert to C int"
            );
            return NULL;
        }
        return io_open_fd_arguments(state, value);
    }

    state->phase = IO_OPEN_WAIT_PATH;
    PyObject *path = PyOS_FSPath(state->file);
    if (path == NULL) {
        return NULL;
    }
    return io_open_path_arguments(state, path, 1);
}

static PyObject *
io_open_resume(const void *raw_state, PyObject *value)
{
    IoOpenState *state = io_open_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &io_open_vtable, state) < 0) {
        io_open_free_state(state);
        return NULL;
    }
    PyObject *result = io_open_continue(state, value, 1);
    adapter_leave(&frame);
    io_open_free_state(state);
    return result;
}

static const AleffAdapterVTable io_open_vtable = {
    .copy_state = io_open_copy_state,
    .free_state = io_open_free_state,
    .resume = io_open_resume,
    .prepare_resume = NULL,
};

static PyObject *
io_open_wrapper(PyObject *self, PyObject *args, PyObject *kwargs)
{
    (void)self;
    PyObject *file = PyTuple_GET_SIZE(args) > 0
        ? PyTuple_GET_ITEM(args, 0)
        : kwargs == NULL ? NULL : PyDict_GetItemString(kwargs, "file");
    PyObject *opener = NULL;
    int file_in_args = PyTuple_GET_SIZE(args) > 0;
    int opener_in_args = PyTuple_GET_SIZE(args) > 7;
    if (opener_in_args) {
        opener = PyTuple_GET_ITEM(args, 7);
    }
    else if (kwargs != NULL) {
        opener = PyDict_GetItemString(kwargs, "opener");
    }
    if (file == NULL || opener == NULL || opener == Py_None ||
        PyNumber_Check(file) || PyTuple_GET_SIZE(args) > 8 ||
        (file_in_args && kwargs != NULL && PyDict_GetItemString(kwargs, "file") != NULL) ||
        (opener_in_args && kwargs != NULL && PyDict_GetItemString(kwargs, "opener") != NULL)) {
        return PyObject_Call(original_io_open, args, kwargs);
    }

    IoOpenState state = {
        .file = Py_NewRef(file),
        .args = Py_NewRef(args),
        .kwargs = Py_XNewRef(kwargs),
        .file_in_args = file_in_args,
        .opener_in_args = opener_in_args,
        .phase = IO_OPEN_WAIT_PATH,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &io_open_vtable, &state) < 0) {
        io_open_clear_state(&state);
        return NULL;
    }
    PyObject *result = io_open_continue(&state, NULL, 0);
    adapter_leave(&frame);
    io_open_clear_state(&state);
    return result;
}

static int
io_replace_method(PyTypeObject *type, const char *name, PyCFunction function)
{
    PyObject *dict = PyType_GetDict(type);
    if (dict == NULL) {
        return -1;
    }
    PyObject *original = PyDict_GetItemString(dict, name);
    if (original == NULL) {
        Py_DECREF(dict);
        PyErr_Format(PyExc_RuntimeError, "io.%s is not defined", name);
        return -1;
    }
    PyObject *saved = Py_NewRef(original);
    PyMethodDef *method = NULL;
    if (Py_IS_TYPE(original, &PyMethodDescr_Type)) {
        method = ((PyMethodDescrObject *)original)->d_method;
    }
    if (method == NULL) {
        Py_DECREF(saved);
        Py_DECREF(dict);
        PyErr_Format(PyExc_RuntimeError, "io.%s is not a method descriptor", name);
        return -1;
    }
    IoMethodBackup *backup = &io_method_backups[io_method_backup_count++];
    backup->type = type;
    backup->name = name;
    backup->original = saved;
    PyMethodDef *replacement = &io_replacement_methods[io_method_backup_count - 1];
    *replacement = *method;
    replacement->ml_name = name;
    replacement->ml_meth = function;
    replacement->ml_flags = METH_VARARGS;
    PyObject *descriptor = PyDescr_NewMethod(type, replacement);
    if (descriptor == NULL || aleff_adapter_register_callable(descriptor) < 0 ||
        PyDict_SetItemString(dict, name, descriptor) < 0) {
        Py_XDECREF(descriptor);
        Py_CLEAR(backup->original);
        io_method_backup_count--;
        Py_DECREF(dict);
        return -1;
    }
    Py_DECREF(descriptor);
    Py_DECREF(dict);
    PyType_Modified(type);
    return 0;
}

static int
io_replace_wrapper(PyTypeObject *type, const char *name, getiterfunc function)
{
    PyObject *dict = PyType_GetDict(type);
    if (dict == NULL) {
        return -1;
    }
    PyObject *original = PyDict_GetItemString(dict, name);
    if (original == NULL || !Py_IS_TYPE(original, &PyWrapperDescr_Type)) {
        Py_DECREF(dict);
        PyErr_Format(PyExc_RuntimeError, "io.%s is not a slot wrapper", name);
        return -1;
    }
    PyWrapperDescrObject *wrapper = (PyWrapperDescrObject *)original;
    union {
        getiterfunc function;
        void *pointer;
    } wrapped = {.function = function};
    IoMethodBackup *backup = &io_method_backups[io_method_backup_count++];
    backup->type = type;
    backup->name = name;
    backup->original = Py_NewRef(original);
    PyObject *descriptor = PyDescr_NewWrapper(
        type,
        wrapper->d_base,
        wrapped.pointer
    );
    if (descriptor == NULL || PyDict_SetItemString(dict, name, descriptor) < 0) {
        Py_XDECREF(descriptor);
        Py_CLEAR(backup->original);
        io_method_backup_count--;
        Py_DECREF(dict);
        return -1;
    }
    Py_DECREF(descriptor);
    Py_DECREF(dict);
    PyType_Modified(type);
    return 0;
}

int
adapter_io_install(PyObject *io_module)
{
    if (io_installed) {
        return 0;
    }
    installed_io = Py_NewRef(io_module);
    original_io_open = PyObject_GetAttrString(io_module, "open");
    if (original_io_open == NULL || !PyCFunction_Check(original_io_open)) {
        PyErr_SetString(PyExc_RuntimeError, "io.open is not a C function");
        adapter_io_rollback();
        return -1;
    }
    io_open_method = *((PyCFunctionObject *)original_io_open)->m_ml;
    io_open_method.ml_meth = _PyCFunction_CAST(io_open_wrapper);
    io_open_method.ml_flags = METH_VARARGS | METH_KEYWORDS;
    PyObject *module_name = PyObject_GetAttrString(original_io_open, "__module__");
    PyObject *replacement = module_name == NULL ? NULL : PyCFunction_NewEx(
        &io_open_method,
        PyCFunction_GET_SELF(original_io_open),
        module_name
    );
    Py_XDECREF(module_name);
    if (replacement == NULL || aleff_adapter_register_callable(replacement) < 0 ||
        PyObject_SetAttrString(io_module, "open", replacement) < 0) {
        Py_XDECREF(replacement);
        adapter_io_rollback();
        return -1;
    }
    Py_DECREF(replacement);

    PyObject *native_io = PyImport_ImportModule("_io");
    PyObject *iobase_object = native_io == NULL
        ? NULL : PyObject_GetAttrString(native_io, "_IOBase");
    Py_XDECREF(native_io);
    if (iobase_object == NULL || !PyType_Check(iobase_object)) {
        Py_XDECREF(iobase_object);
        PyErr_SetString(PyExc_RuntimeError, "io.IOBase is not a type");
        adapter_io_rollback();
        return -1;
    }
    installed_iobase = (PyTypeObject *)Py_NewRef(iobase_object);
    original_iobase_iter = installed_iobase->tp_iter;
    original_iobase_next = installed_iobase->tp_iternext;
    original_iobase_close = PyObject_GetAttrString(iobase_object, "close");
    if (original_iobase_iter == NULL || original_iobase_next == NULL ||
        original_iobase_close == NULL ||
        io_replace_wrapper(installed_iobase, "__iter__", io_iter_slot) < 0 ||
        io_replace_method(installed_iobase, "close", io_close_wrapper) < 0 ||
        io_replace_method(installed_iobase, "readlines", io_readlines_wrapper) < 0 ||
        io_replace_method(installed_iobase, "writelines", io_writelines_wrapper) < 0) {
        Py_DECREF(iobase_object);
        adapter_io_rollback();
        return -1;
    }
    installed_iobase->tp_iter = io_iter_slot;
    installed_iobase->tp_iternext = io_next_slot;
    PyType_Modified(installed_iobase);
    Py_DECREF(iobase_object);
    io_installed = 1;
    return 0;
}

void
adapter_io_rollback(void)
{
    if (installed_iobase != NULL) {
        if (original_iobase_iter != NULL) {
            installed_iobase->tp_iter = original_iobase_iter;
        }
        if (original_iobase_next != NULL) {
            installed_iobase->tp_iternext = original_iobase_next;
        }
        PyType_Modified(installed_iobase);
    }
    for (Py_ssize_t index = io_method_backup_count - 1; index >= 0; index--) {
        IoMethodBackup *backup = &io_method_backups[index];
        PyObject *dict = PyType_GetDict(backup->type);
        if (dict != NULL &&
            PyDict_SetItemString(dict, backup->name, backup->original) < 0) {
            PyErr_Clear();
        }
        Py_XDECREF(dict);
        PyType_Modified(backup->type);
        Py_CLEAR(backup->original);
    }
    io_method_backup_count = 0;
    if (installed_io != NULL && original_io_open != NULL &&
        PyObject_SetAttrString(installed_io, "open", original_io_open) < 0) {
        PyErr_Clear();
    }
    Py_CLEAR(original_io_open);
    Py_CLEAR(original_iobase_close);
    Py_CLEAR(installed_io);
    Py_CLEAR(installed_iobase);
    original_iobase_iter = NULL;
    original_iobase_next = NULL;
    io_installed = 0;
}
