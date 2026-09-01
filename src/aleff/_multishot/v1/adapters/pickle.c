#include "pickle.h"

#include <stddef.h>
#include <string.h>

/*
 * The C pickle accelerator deliberately keeps almost all of its execution
 * state in C stack frames.  A Python callback from that implementation can
 * therefore not be resumed by simply calling _pickle again: doing so would
 * run the callback which already returned to the effect handler a second
 * time.
 *
 * pickle.py contains the reference implementation.  Its pickler and
 * unpickler keep their execution state in Python frames, which are precisely
 * the frames saved by the continuation machinery.  The adapter uses those
 * implementations for calls which enter through the public functions and
 * for subclassed accelerator objects.  The accelerator types and their
 * ordinary base-method behaviour remain exposed unchanged.
 */

typedef enum {
    PICKLE_DUMP,
    PICKLE_DUMPS,
    PICKLE_LOAD,
    PICKLE_LOADS,
} PickleOperation;

typedef struct {
    const AleffAdapterVTable *vtable;
    void *state;
    int outer_frame_index;
    int prepared;
} PickleAdapterSnapshotItem;

struct AleffAdapterSnapshot {
    PickleAdapterSnapshotItem *items;
    Py_ssize_t count;
};

/* These are the private layouts used by CPython's _pickle.c.  Only fields
 * needed to construct the equivalent Python implementation are read.  The
 * field order through these members is stable in the CPython versions for
 * which the continuation adapters are built (3.12--3.14). */
typedef struct {
    PyObject_HEAD
    void *memo;
    PyObject *persistent_function;
    PyObject *persistent_self_or_attr;
    PyObject *dispatch_table;
    PyObject *reducer_override;
    PyObject *write;
    PyObject *output_buffer;
    Py_ssize_t output_len;
    Py_ssize_t max_output_len;
    int proto;
    int bin;
    int framing;
    Py_ssize_t frame_start;
    Py_ssize_t buffer_size;
    int fast;
    int fast_nesting;
    int fix_imports;
#if PY_VERSION_HEX >= 0x030d0000
    int running;
#endif
    PyObject *fast_memo;
    PyObject *buffer_callback;
} PicklePicklerObject;

typedef struct {
    PyObject_HEAD
    void *stack;
    PyObject **memo;
    size_t memo_size;
    size_t memo_len;
    PyObject *persistent_function;
    PyObject *persistent_self_or_attr;
    Py_buffer buffer;
    char *input_buffer;
    char *input_line;
    Py_ssize_t input_len;
    Py_ssize_t next_read_idx;
    Py_ssize_t prefetched_idx;
#if ((PY_VERSION_HEX >= 0x030d0f00 && PY_VERSION_HEX < 0x030e0000) || \
     PY_VERSION_HEX >= 0x030e0700)
    Py_ssize_t frame_end;
    Py_ssize_t saved_input_len;
#endif
    PyObject *read;
    PyObject *readinto;
    PyObject *readline;
    PyObject *peek;
    PyObject *buffers;
    char *encoding;
    char *errors;
#if PY_VERSION_HEX >= 0x030d0000
    Py_ssize_t *marks;
    Py_ssize_t num_marks;
    Py_ssize_t marks_size;
    int proto;
    int fix_imports;
    int running;
#else
    Py_ssize_t *marks;
    Py_ssize_t num_marks;
    Py_ssize_t marks_size;
    int proto;
    int fix_imports;
#endif
} PickleUnpicklerObject;

typedef struct {
    PyObject_HEAD
    PyObject *write;
    PyObject *read;
    PyObject *readline;
    PyObject *pending;
    PyObject *before_external;
    PyObject *before_external_state;
    Py_ssize_t before_external_position;
    int before_external_state_kind;
    int before_external_readline;
} PickleFileProxy;

static PyObject *installed_pickle_module;
static PyObject *installed__pickle_module;
static PyObject *original_functions[4];
static PyObject *original_pickler_dump;
static PyObject *original_unpickler_load;
static PyTypeObject *pickler_type;
static PyTypeObject *unpickler_type;
static PyObject *pure_pickler;
static PyObject *pure_unpickler;
static PyObject *bytesio_type;
static PyTypeObject PickleFileProxyType;
static int pickle_installed;
static PyMethodDef pickle_replacement_methods[4];
static PyMethodDef pickle_pickler_dump_method;
static PyMethodDef pickle_unpickler_load_method;

static PyObject *pickle_call_dump_method(PyObject *, PyObject *);
static PyObject *pickle_call_load_method(PyObject *);
static int pickle_sync_owner_memo(PyObject *, PyObject *);
static PyObject *pickle_clone_bytesio(PyObject *);
static PickleFileProxy *pickle_file_proxy_new(PyObject *, PyObject *, PyObject *);
static PyObject *pickle_copy_external_dict(PyObject *);
static int pickle_restore_external_value(PyObject *, PyObject *, Py_ssize_t, int);

PyObject *
adapter_pickle_complete_default_reduce(
    PyObject *worker,
    PyObject *object,
    PyObject *state
)
{
    PyObject *protocol = PyObject_GetAttrString(worker, "proto");
    if (protocol == NULL) {
        return NULL;
    }
    long version = PyLong_AsLong(protocol);
    Py_DECREF(protocol);
    if (version == -1 && PyErr_Occurred()) {
        return NULL;
    }
    PyObject *copyreg = PyImport_ImportModule("copyreg");
    if (copyreg == NULL) {
        return NULL;
    }
    PyObject *function = PyObject_GetAttrString(
        copyreg,
        version < 2 ? "_reconstructor" : "__newobj__"
    );
    if (function == NULL) {
        Py_DECREF(copyreg);
        return NULL;
    }
    PyObject *class_object = PyObject_Type(object);
    if (class_object == NULL) {
        Py_DECREF(function);
        Py_DECREF(copyreg);
        return NULL;
    }
    PyObject *arguments;
    if (version < 2) {
        arguments = PyTuple_Pack(3, class_object, &PyBaseObject_Type, Py_None);
    }
    else {
        arguments = PyTuple_Pack(1, class_object);
    }
    Py_DECREF(class_object);
    Py_DECREF(copyreg);
    if (arguments == NULL) {
        Py_DECREF(function);
        return NULL;
    }
    PyObject *result;
    if (version < 2) {
        result = PyTuple_Pack(3, function, arguments, state);
    }
    else {
        result = PyTuple_Pack(5, function, arguments, state, Py_None, Py_None);
    }
    Py_DECREF(arguments);
    Py_DECREF(function);
    return result;
}

static PyObject *
pickle_bytes_from_object(PyObject *value)
{
    if (PyBytes_Check(value)) {
        return Py_NewRef(value);
    }
    return PyBytes_FromObject(value);
}

typedef enum {
    PICKLE_WORKER_PICKLER,
    PICKLE_WORKER_UNPICKLER,
} PickleWorkerKind;

typedef struct {
    PickleWorkerKind kind;
    PyObject *worker;
    PyObject *owner;
    PyObject *data;
    PyObject *external;
    PyObject *worker_state;
    PyObject *data_state;
    Py_ssize_t data_position;
    PyObject *external_state;
    Py_ssize_t external_position;
    int external_state_kind;
    PyObject *external_aux;
    PyObject *external_aux_state;
    Py_ssize_t external_aux_position;
    int external_aux_state_kind;
    int external_aux_readline;
    int external_aux_consumed;
    int return_bytes;
} PickleContinuationState;

enum {
    PICKLE_EXTERNAL_NONE,
    PICKLE_EXTERNAL_BYTES,
    PICKLE_EXTERNAL_DICT,
    PICKLE_EXTERNAL_POSITION,
};

static const AleffAdapterVTable pickle_vtable;
static int pickle_capture_external_state(PickleContinuationState *);
static int pickle_capture_external_aux(PickleContinuationState *);

static PyObject *
pickle_normalize_external_result(
    const PickleContinuationState *state,
    PyObject *value
)
{
    if (state->kind != PICKLE_WORKER_UNPICKLER ||
        state->external_aux_state_kind != PICKLE_EXTERNAL_BYTES ||
        state->external_aux_state == NULL || !PyBytes_Check(value)) {
        return Py_NewRef(value);
    }
    if (state->external_aux_consumed) {
        return Py_NewRef(value);
    }
    ((PickleContinuationState *)state)->external_aux_consumed = 1;
    const char *data = PyBytes_AS_STRING(state->external_aux_state);
    Py_ssize_t length = PyBytes_GET_SIZE(state->external_aux_state);
    Py_ssize_t position = state->external_aux_position;
    if (position < 0 || position > length) {
        return Py_NewRef(value);
    }
    Py_ssize_t result_length;
    if (state->external_aux_readline) {
        result_length = 0;
        while (position + result_length < length &&
               data[position + result_length] != '\n') {
            result_length++;
        }
        if (position + result_length < length) {
            result_length++;
        }
    }
    else {
        result_length = PyBytes_GET_SIZE(value);
        if (result_length > length - position) {
            result_length = length - position;
        }
    }
    PyObject *expected = PyBytes_FromStringAndSize(
        data + position,
        result_length
    );
    if (expected == NULL) {
        return NULL;
    }
    int same = PyObject_RichCompareBool(value, expected, Py_EQ);
    if (same < 0 || pickle_restore_external_value(
            state->external_aux,
            state->external_aux_state,
            position + result_length,
            PICKLE_EXTERNAL_BYTES
        ) < 0) {
        Py_DECREF(expected);
        return NULL;
    }
    if (!same) {
        return expected;
    }
    Py_DECREF(expected);
    return Py_NewRef(value);
}

static int
pickle_proxy_pending_length(PickleFileProxy *proxy, Py_ssize_t *length)
{
    if (proxy->pending == NULL) {
        *length = 0;
        return 0;
    }
    PyObject *position = PyObject_CallMethod(proxy->pending, "tell", NULL);
    if (position == NULL) {
        return -1;
    }
    *length = PyLong_AsSsize_t(position);
    Py_DECREF(position);
    return PyErr_Occurred() ? -1 : 0;
}

static int
pickle_proxy_append(PickleFileProxy *proxy, PyObject *value)
{
    if (proxy->pending == NULL) {
        proxy->pending = PyObject_CallNoArgs(bytesio_type);
        if (proxy->pending == NULL) {
            return -1;
        }
    }
    PyObject *result = PyObject_CallMethod(proxy->pending, "write", "O", value);
    if (result == NULL) {
        return -1;
    }
    Py_DECREF(result);
    return 0;
}

static PyObject *
pickle_proxy_pending_bytes(PickleFileProxy *proxy)
{
    if (proxy->pending == NULL) {
        return PyBytes_FromStringAndSize(NULL, 0);
    }
    return PyObject_CallMethod(proxy->pending, "getvalue", NULL);
}

static PyObject *
pickle_proxy_expected_read(
    PickleFileProxy *proxy,
    PyObject *result,
    int readline,
    Py_ssize_t size
)
{
    if (proxy->before_external_state_kind != PICKLE_EXTERNAL_BYTES ||
        !PyBytes_Check(proxy->before_external_state) ||
        !PyBytes_Check(result)) {
        return Py_NewRef(result);
    }
    const char *data = PyBytes_AS_STRING(proxy->before_external_state);
    Py_ssize_t length = PyBytes_GET_SIZE(proxy->before_external_state);
    Py_ssize_t position = proxy->before_external_position;
    if (position < 0 || position > length) {
        return Py_NewRef(result);
    }
    Py_ssize_t result_length;
    if (readline) {
        result_length = 0;
        while (position + result_length < length &&
               data[position + result_length] != '\n') {
            result_length++;
        }
        if (position + result_length < length) {
            result_length++;
        }
    }
    else {
        result_length = size < 0 ? length - position : size;
        if (result_length > length - position) {
            result_length = length - position;
        }
    }
    PyObject *expected = PyBytes_FromStringAndSize(
        data + position,
        result_length
    );
    if (expected == NULL) {
        return NULL;
    }
    int same = PyObject_RichCompareBool(result, expected, Py_EQ);
    if (same < 0) {
        Py_DECREF(expected);
        return NULL;
    }
    if (pickle_restore_external_value(
            proxy->before_external,
            proxy->before_external_state,
            proxy->before_external_position + result_length,
            PICKLE_EXTERNAL_BYTES
        ) < 0) {
        Py_DECREF(expected);
        return NULL;
    }
    if (same) {
        Py_DECREF(expected);
        return Py_NewRef(result);
    }
    return expected;
}

static PyObject *
pickle_proxy_clone(PickleFileProxy *source)
{
    PickleFileProxy *copy = pickle_file_proxy_new(
        source->write,
        source->read,
        source->readline
    );
    if (copy == NULL) {
        return NULL;
    }
    if (source->pending != NULL) {
        copy->pending = pickle_clone_bytesio(source->pending);
        if (copy->pending == NULL) {
            Py_DECREF(copy);
            return NULL;
        }
    }
    copy->before_external = Py_XNewRef(source->before_external);
    copy->before_external_state = Py_XNewRef(source->before_external_state);
    copy->before_external_position = source->before_external_position;
    copy->before_external_state_kind = source->before_external_state_kind;
    copy->before_external_readline = source->before_external_readline;
    return (PyObject *)copy;
}

static PyObject *
pickle_clone_bytesio(PyObject *source)
{
    PyObject *value = PyObject_CallMethod(source, "getvalue", NULL);
    if (value == NULL) {
        return NULL;
    }
    PyObject *copy = PyObject_CallOneArg(bytesio_type, value);
    Py_DECREF(value);
    if (copy == NULL) {
        return NULL;
    }
    PyObject *position = PyObject_CallMethod(source, "tell", NULL);
    if (position == NULL) {
        Py_DECREF(copy);
        return NULL;
    }
    PyObject *result = PyObject_CallMethod(copy, "seek", "O", position);
    Py_DECREF(position);
    if (result == NULL) {
        Py_DECREF(copy);
        return NULL;
    }
    Py_DECREF(result);
    return copy;
}

static int
pickle_restore_bytesio(
    PyObject *target,
    PyObject *value,
    Py_ssize_t position
)
{
    PyObject *result = PyObject_CallMethod(target, "seek", "i", 0);
    if (result == NULL) {
        return -1;
    }
    Py_DECREF(result);
    result = PyObject_CallMethod(target, "truncate", "i", 0);
    if (result == NULL) {
        return -1;
    }
    Py_DECREF(result);
    result = PyObject_CallMethod(target, "write", "O", value);
    if (result == NULL) {
        return -1;
    }
    Py_DECREF(result);
    result = PyObject_CallMethod(target, "seek", "n", position);
    if (result == NULL) {
        return -1;
    }
    Py_DECREF(result);
    return 0;
}

static PyObject *
pickle_clone_framer(PyObject *source)
{
    PyObject *file_write = PyObject_GetAttrString(source, "file_write");
    if (file_write == NULL) {
        return NULL;
    }
    PyObject *proxy_self = PyObject_GetAttrString(file_write, "__self__");
    PyObject *cloned_write = NULL;
    if (proxy_self != NULL && Py_IS_TYPE(proxy_self, &PickleFileProxyType)) {
        PyObject *proxy = pickle_proxy_clone((PickleFileProxy *)proxy_self);
        if (proxy != NULL) {
            cloned_write = PyObject_GetAttrString(proxy, "write");
            Py_DECREF(proxy);
        }
    }
    else if (proxy_self == NULL && PyErr_Occurred()) {
        PyErr_Clear();
    }
    Py_XDECREF(proxy_self);
    if (cloned_write != NULL) {
        Py_DECREF(file_write);
        file_write = cloned_write;
    }
    else if (PyErr_Occurred()) {
        Py_DECREF(file_write);
        return NULL;
    }
    PyObject *copy = PyObject_CallOneArg((PyObject *)Py_TYPE(source), file_write);
    Py_DECREF(file_write);
    if (copy == NULL) {
        return NULL;
    }
    PyObject *current = PyObject_GetAttrString(source, "current_frame");
    if (current == NULL) {
        Py_DECREF(copy);
        return NULL;
    }
    if (current != Py_None) {
        PyObject *current_copy = pickle_clone_bytesio(current);
        if (current_copy == NULL || PyObject_SetAttrString(
                copy,
                "current_frame",
                current_copy
            ) < 0) {
            Py_XDECREF(current_copy);
            Py_DECREF(current);
            Py_DECREF(copy);
            return NULL;
        }
        Py_DECREF(current_copy);
    }
    Py_DECREF(current);
    return copy;
}

static PyObject *
pickle_clone_unframer(PyObject *source)
{
    PyObject *file_read = PyObject_GetAttrString(source, "file_read");
    if (file_read == NULL) {
        return NULL;
    }
    PyObject *file_readline = PyObject_GetAttrString(source, "file_readline");
    if (file_readline == NULL) {
        Py_DECREF(file_read);
        return NULL;
    }
    PyObject *copy = PyObject_CallFunctionObjArgs(
        (PyObject *)Py_TYPE(source),
        file_read,
        file_readline,
        NULL
    );
    Py_DECREF(file_readline);
    Py_DECREF(file_read);
    if (copy == NULL) {
        return NULL;
    }
    PyObject *current = PyObject_GetAttrString(source, "current_frame");
    if (current == NULL) {
        Py_DECREF(copy);
        return NULL;
    }
    if (current != Py_None) {
        PyObject *current_copy = pickle_clone_bytesio(current);
        if (current_copy == NULL || PyObject_SetAttrString(
                copy,
                "current_frame",
                current_copy
            ) < 0) {
            Py_XDECREF(current_copy);
            Py_DECREF(current);
            Py_DECREF(copy);
            return NULL;
        }
        Py_DECREF(current_copy);
    }
    Py_DECREF(current);
    return copy;
}

static PyObject *
pickle_clone_worker_state(PyObject *worker, PickleWorkerKind kind)
{
    PyObject *source = PyObject_GetAttrString(worker, "__dict__");
    if (source == NULL) {
        return NULL;
    }
    if (!PyDict_Check(source)) {
        Py_DECREF(source);
        PyErr_SetString(PyExc_RuntimeError, "pickle worker has no dictionary state");
        return NULL;
    }
    PyObject *copy = PyDict_Copy(source);
    Py_DECREF(source);
    if (copy == NULL) {
        return NULL;
    }

    PyObject *memo = PyDict_GetItemString(copy, "memo");
    if (memo != NULL) {
        PyObject *memo_copy = PyDict_Copy(memo);
        if (memo_copy == NULL || PyDict_SetItemString(copy, "memo", memo_copy) < 0) {
            Py_XDECREF(memo_copy);
            Py_DECREF(copy);
            return NULL;
        }
        Py_DECREF(memo_copy);
    }

    const char *state_name = kind == PICKLE_WORKER_PICKLER
        ? "framer" : "_unframer";
    PyObject *state_object = PyDict_GetItemString(copy, state_name);
    if (state_object != NULL) {
        PyObject *state_copy = kind == PICKLE_WORKER_PICKLER
            ? pickle_clone_framer(state_object)
            : pickle_clone_unframer(state_object);
        if (state_copy == NULL || PyDict_SetItemString(copy, state_name, state_copy) < 0) {
            Py_XDECREF(state_copy);
            Py_DECREF(copy);
            return NULL;
        }
        Py_DECREF(state_copy);
    }

    if (kind == PICKLE_WORKER_UNPICKLER) {
        PyObject *buffers = PyDict_GetItemString(copy, "_buffers");
        if (buffers != NULL && buffers != Py_None) {
            PyObject *buffers_copy = clone_iterator_for_snapshot(buffers);
            if (buffers_copy == NULL || PyDict_SetItemString(
                    copy,
                    "_buffers",
                    buffers_copy
                ) < 0) {
                Py_XDECREF(buffers_copy);
                Py_DECREF(copy);
                return NULL;
            }
            Py_DECREF(buffers_copy);
        }
        PyObject *stack = PyDict_GetItemString(copy, "stack");
        if (stack != NULL) {
            PyObject *stack_copy = PyList_GetSlice(
                stack,
                0,
                PyList_GET_SIZE(stack)
            );
            if (stack_copy == NULL || PyDict_SetItemString(copy, "stack", stack_copy) < 0) {
                Py_XDECREF(stack_copy);
                Py_DECREF(copy);
                return NULL;
            }
            Py_DECREF(stack_copy);
        }
        PyObject *metastack = PyDict_GetItemString(copy, "metastack");
        if (metastack != NULL) {
            PyObject *metastack_copy = PyList_New(PyList_GET_SIZE(metastack));
            if (metastack_copy == NULL) {
                Py_DECREF(copy);
                return NULL;
            }
            for (Py_ssize_t index = 0; index < PyList_GET_SIZE(metastack); index++) {
                PyObject *nested = PyList_GET_ITEM(metastack, index);
                PyObject *nested_copy = PyList_GetSlice(
                    nested,
                    0,
                    PyList_GET_SIZE(nested)
                );
                if (nested_copy == NULL) {
                    Py_DECREF(metastack_copy);
                    Py_DECREF(copy);
                    return NULL;
                }
                PyList_SET_ITEM(metastack_copy, index, nested_copy);
            }
            if (PyDict_SetItemString(copy, "metastack", metastack_copy) < 0) {
                Py_DECREF(metastack_copy);
                Py_DECREF(copy);
                return NULL;
            }
            Py_DECREF(metastack_copy);
        }
    }
    return copy;
}

static int
pickle_restore_state_object(PyObject *target, PyObject *source)
{
    PyObject *target_dict = PyObject_GetAttrString(target, "__dict__");
    PyObject *source_dict = PyObject_GetAttrString(source, "__dict__");
    if (target_dict == NULL || source_dict == NULL ||
        !PyDict_Check(target_dict) || !PyDict_Check(source_dict)) {
        Py_XDECREF(target_dict);
        Py_XDECREF(source_dict);
        PyErr_SetString(PyExc_RuntimeError, "pickle worker state is not a dictionary");
        return -1;
    }
    PyDict_Clear(target_dict);
    if (PyDict_Update(target_dict, source_dict) < 0) {
        Py_DECREF(target_dict);
        Py_DECREF(source_dict);
        return -1;
    }
    Py_DECREF(target_dict);
    Py_DECREF(source_dict);

    PyObject *current = PyObject_GetAttrString(source, "current_frame");
    if (current == NULL) {
        return -1;
    }
    if (current != Py_None) {
        PyObject *current_copy = pickle_clone_bytesio(current);
        if (current_copy == NULL || PyObject_SetAttrString(
                target,
                "current_frame",
                current_copy
            ) < 0) {
            Py_XDECREF(current_copy);
            Py_DECREF(current);
            return -1;
        }
        Py_DECREF(current_copy);
    }
    Py_DECREF(current);
    return 0;
}

static int
pickle_restore_worker_state(PickleContinuationState *state)
{
    PyObject *target = PyObject_GetAttrString(state->worker, "__dict__");
    if (target == NULL || !PyDict_Check(target)) {
        Py_XDECREF(target);
        PyErr_SetString(PyExc_RuntimeError, "pickle worker has no dictionary state");
        return -1;
    }
    PyObject *old_framer = PyDict_GetItemString(target, "framer");
    PyObject *old_unframer = PyDict_GetItemString(target, "_unframer");
    PyObject *old_stack = PyDict_GetItemString(target, "stack");
    Py_XINCREF(old_framer);
    Py_XINCREF(old_unframer);
    Py_XINCREF(old_stack);
    PyDict_Clear(target);
    if (PyDict_Update(target, state->worker_state) < 0) {
        Py_DECREF(target);
        Py_XDECREF(old_framer);
        Py_XDECREF(old_unframer);
        Py_XDECREF(old_stack);
        return -1;
    }
    Py_DECREF(target);

    PyObject *worker_dict = PyObject_GetAttrString(state->worker, "__dict__");
    if (worker_dict == NULL) {
        Py_XDECREF(old_framer);
        Py_XDECREF(old_unframer);
        Py_XDECREF(old_stack);
        return -1;
    }
    PyObject *memo = PyDict_GetItemString(state->worker_state, "memo");
    if (memo != NULL) {
        PyObject *memo_copy = PyDict_Copy(memo);
        if (memo_copy == NULL || PyDict_SetItemString(worker_dict, "memo", memo_copy) < 0) {
            Py_XDECREF(memo_copy);
            Py_DECREF(worker_dict);
            Py_XDECREF(old_framer);
            Py_XDECREF(old_unframer);
            Py_XDECREF(old_stack);
            return -1;
        }
        Py_DECREF(memo_copy);
    }

    if (state->kind == PICKLE_WORKER_PICKLER) {
        PyObject *framer = PyDict_GetItemString(state->worker_state, "framer");
        if (framer != NULL) {
            PyObject *framer_copy = pickle_clone_framer(framer);
            if (framer_copy == NULL || PyDict_SetItemString(
                    worker_dict,
                    "framer",
                    framer_copy
                ) < 0) {
                Py_XDECREF(framer_copy);
                Py_DECREF(worker_dict);
                Py_XDECREF(old_framer);
                Py_XDECREF(old_unframer);
                Py_XDECREF(old_stack);
                return -1;
            }
            PyObject *write = PyObject_GetAttrString(framer_copy, "write");
            PyObject *large_write = PyObject_GetAttrString(
                framer_copy,
                "write_large_bytes"
            );
            if (write == NULL || large_write == NULL ||
                PyDict_SetItemString(worker_dict, "write", write) < 0 ||
                PyDict_SetItemString(worker_dict, "_write_large_bytes", large_write) < 0) {
                Py_XDECREF(write);
                Py_XDECREF(large_write);
                Py_DECREF(framer_copy);
                Py_DECREF(worker_dict);
                Py_XDECREF(old_framer);
                Py_XDECREF(old_unframer);
                Py_XDECREF(old_stack);
                return -1;
            }
            Py_DECREF(write);
            Py_DECREF(large_write);
            Py_DECREF(framer_copy);
        }
    }
    else {
        PyObject *buffers = PyDict_GetItemString(state->worker_state, "_buffers");
        if (buffers != NULL && buffers != Py_None) {
            PyObject *buffers_copy = clone_iterator_for_snapshot(buffers);
            if (buffers_copy == NULL || PyDict_SetItemString(
                    worker_dict,
                    "_buffers",
                    buffers_copy
                ) < 0) {
                Py_XDECREF(buffers_copy);
                Py_DECREF(worker_dict);
                Py_XDECREF(old_framer);
                Py_XDECREF(old_unframer);
                Py_XDECREF(old_stack);
                return -1;
            }
            Py_DECREF(buffers_copy);
        }
        PyObject *unframer = PyDict_GetItemString(state->worker_state, "_unframer");
        if (unframer != NULL && old_unframer != NULL) {
            if (pickle_restore_state_object(old_unframer, unframer) < 0 ||
                PyDict_SetItemString(worker_dict, "_unframer", old_unframer) < 0) {
                Py_DECREF(worker_dict);
                Py_XDECREF(old_framer);
                Py_DECREF(old_unframer);
                Py_XDECREF(old_stack);
                return -1;
            }
            PyObject *read = PyObject_GetAttrString(old_unframer, "read");
            PyObject *readinto = PyObject_GetAttrString(old_unframer, "readinto");
            PyObject *readline = PyObject_GetAttrString(old_unframer, "readline");
            if (read == NULL || readinto == NULL || readline == NULL ||
                PyDict_SetItemString(worker_dict, "read", read) < 0 ||
                PyDict_SetItemString(worker_dict, "readinto", readinto) < 0 ||
                PyDict_SetItemString(worker_dict, "readline", readline) < 0) {
                Py_XDECREF(read);
                Py_XDECREF(readinto);
                Py_XDECREF(readline);
                Py_DECREF(worker_dict);
                Py_XDECREF(old_framer);
                Py_DECREF(old_unframer);
                Py_XDECREF(old_stack);
                return -1;
            }
            Py_DECREF(read);
            Py_DECREF(readinto);
            Py_DECREF(readline);

            PyObject *stack = PyDict_GetItemString(state->worker_state, "stack");
            if (stack != NULL && old_stack != NULL) {
                if (PyList_SetSlice(
                        old_stack,
                        0,
                        PyList_GET_SIZE(old_stack),
                        stack
                    ) < 0 || PyDict_SetItemString(worker_dict, "stack", old_stack) < 0) {
                    Py_DECREF(worker_dict);
                    Py_XDECREF(old_framer);
                    Py_DECREF(old_unframer);
                    Py_DECREF(old_stack);
                    return -1;
                }
                PyObject *append = PyObject_GetAttrString(old_stack, "append");
                if (append == NULL || PyDict_SetItemString(worker_dict, "append", append) < 0) {
                    Py_XDECREF(append);
                    Py_DECREF(worker_dict);
                    Py_XDECREF(old_framer);
                    Py_DECREF(old_unframer);
                    Py_DECREF(old_stack);
                    return -1;
                }
                Py_DECREF(append);
            }
        }
    }
    Py_DECREF(worker_dict);
    Py_XDECREF(old_framer);
    Py_XDECREF(old_unframer);
    Py_XDECREF(old_stack);
    return 0;
}

static int
pickle_capture_external_aux(PickleContinuationState *state)
{
    if (state->external_aux_state_kind != PICKLE_EXTERNAL_NONE ||
        state->external == NULL) {
        return 0;
    }
    const char *names[] = {"readline", "read", "write", NULL};
    for (int name_index = 0; names[name_index] != NULL; name_index++) {
        PyObject *method = PyObject_GetAttrString(state->external, names[name_index]);
        if (method == NULL) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                PyErr_Clear();
                continue;
            }
            return -1;
        }
        PyObject *function = PyObject_GetAttrString(method, "__func__");
        Py_XDECREF(method);
        if (function == NULL) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                PyErr_Clear();
                continue;
            }
            return -1;
        }
        PyObject *closure = PyObject_GetAttrString(function, "__closure__");
        Py_DECREF(function);
        if (closure == NULL) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                PyErr_Clear();
                continue;
            }
            return -1;
        }
        if (closure == Py_None || !PyTuple_Check(closure)) {
            Py_DECREF(closure);
            continue;
        }
        for (Py_ssize_t index = 0; index < PyTuple_GET_SIZE(closure); index++) {
            PyObject *cell = PyTuple_GET_ITEM(closure, index);
            PyObject *candidate = PyCell_Get(cell);
            if (candidate == NULL) {
                if (PyErr_Occurred()) {
                    PyErr_Clear();
                }
                continue;
            }
            if (candidate == state->external || candidate == Py_None) {
                Py_DECREF(candidate);
                continue;
            }
            PickleContinuationState temporary = {0};
            temporary.external = candidate;
            if (pickle_capture_external_state(&temporary) < 0) {
                Py_XDECREF(temporary.external_state);
                Py_DECREF(candidate);
                Py_DECREF(closure);
                return -1;
            }
            if (temporary.external_state_kind != PICKLE_EXTERNAL_NONE) {
                state->external_aux = Py_NewRef(candidate);
                state->external_aux_state = temporary.external_state;
                state->external_aux_position = temporary.external_position;
                state->external_aux_state_kind = temporary.external_state_kind;
                Py_DECREF(candidate);
                Py_DECREF(closure);
                return 0;
            }
            Py_XDECREF(temporary.external_state);
            Py_DECREF(candidate);
        }
        Py_DECREF(closure);
    }
    return 0;
}

static int
pickle_capture_data_state(PickleContinuationState *state)
{
    if (state->data == NULL) {
        return 0;
    }
    state->data_state = PyObject_CallMethod(state->data, "getvalue", NULL);
    if (state->data_state == NULL) {
        return -1;
    }
    PyObject *position = PyObject_CallMethod(state->data, "tell", NULL);
    if (position == NULL) {
        return -1;
    }
    state->data_position = PyLong_AsSsize_t(position);
    Py_DECREF(position);
    return PyErr_Occurred() ? -1 : 0;
}

static int
pickle_capture_proxy_state(PickleContinuationState *state)
{
    PyObject *dictionary = PyObject_GetAttrString(state->worker, "__dict__");
    if (dictionary == NULL || !PyDict_Check(dictionary)) {
        Py_XDECREF(dictionary);
        PyErr_Clear();
        return 0;
    }
    const char *method_name = state->kind == PICKLE_WORKER_PICKLER
        ? "write" : "readline";
    PyObject *method = PyDict_GetItemString(dictionary, method_name);
    PyObject *owner = method == NULL
        ? NULL : PyObject_GetAttrString(method, "__self__");
    Py_DECREF(dictionary);
    if (owner == NULL) {
        PyErr_Clear();
        return 0;
    }
    const char *file_method_name = state->kind == PICKLE_WORKER_PICKLER
        ? "file_write" : "file_readline";
    PyObject *file_method = PyObject_GetAttrString(owner, file_method_name);
    Py_DECREF(owner);
    if (file_method == NULL) {
        PyErr_Clear();
        return 0;
    }
    PyObject *proxy_owner = PyObject_GetAttrString(file_method, "__self__");
    Py_DECREF(file_method);
    if (proxy_owner == NULL) {
        PyErr_Clear();
        return 0;
    }
    if (!Py_IS_TYPE(proxy_owner, &PickleFileProxyType)) {
        Py_DECREF(proxy_owner);
        return 0;
    }
    PickleFileProxy *proxy = (PickleFileProxy *)proxy_owner;
    if (proxy->before_external_state_kind != PICKLE_EXTERNAL_NONE) {
        state->external_aux = Py_NewRef(proxy->before_external);
        state->external_aux_state = Py_NewRef(proxy->before_external_state);
        state->external_aux_position = proxy->before_external_position;
        state->external_aux_state_kind = proxy->before_external_state_kind;
        state->external_aux_readline = proxy->before_external_readline;
    }
    Py_DECREF(proxy_owner);
    return 0;
}

static PyObject *
pickle_copy_external_dict(PyObject *source)
{
    PyObject *copy = PyDict_Copy(source);
    if (copy == NULL) {
        return NULL;
    }
    PyObject *key;
    PyObject *value;
    Py_ssize_t position = 0;
    while (PyDict_Next(source, &position, &key, &value)) {
        if (PyList_Check(value)) {
            PyObject *value_copy = PyList_GetSlice(
                value,
                0,
                PyList_GET_SIZE(value)
            );
            if (value_copy == NULL || PyDict_SetItem(copy, key, value_copy) < 0) {
                Py_XDECREF(value_copy);
                Py_DECREF(copy);
                return NULL;
            }
            Py_DECREF(value_copy);
        }
        else if (PyDict_Check(value)) {
            PyObject *value_copy = PyDict_Copy(value);
            if (value_copy == NULL || PyDict_SetItem(copy, key, value_copy) < 0) {
                Py_XDECREF(value_copy);
                Py_DECREF(copy);
                return NULL;
            }
            Py_DECREF(value_copy);
        }
    }
    return copy;
}

static int
pickle_capture_external_state(PickleContinuationState *state)
{
    if (state->external == NULL) {
        return 0;
    }

    PyObject *value = PyObject_CallMethod(state->external, "getvalue", NULL);
    if (value != NULL) {
        PyObject *position = PyObject_CallMethod(state->external, "tell", NULL);
        if (position == NULL) {
            Py_DECREF(value);
            return -1;
        }
        state->external_position = PyLong_AsSsize_t(position);
        Py_DECREF(position);
        if (PyErr_Occurred()) {
            Py_DECREF(value);
            return -1;
        }
        state->external_state = value;
        state->external_state_kind = PICKLE_EXTERNAL_BYTES;
        return 0;
    }
    if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
        return -1;
    }
    PyErr_Clear();

    PyObject *dictionary = PyObject_GetAttrString(state->external, "__dict__");
    if (dictionary != NULL) {
        if (PyDict_Check(dictionary)) {
            state->external_state = pickle_copy_external_dict(dictionary);
            Py_DECREF(dictionary);
            if (state->external_state == NULL) {
                return -1;
            }
            state->external_state_kind = PICKLE_EXTERNAL_DICT;
            return 0;
        }
        Py_DECREF(dictionary);
    }
    else if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
        return -1;
    }
    PyErr_Clear();

    PyObject *position = PyObject_CallMethod(state->external, "tell", NULL);
    if (position != NULL) {
        state->external_position = PyLong_AsSsize_t(position);
        Py_DECREF(position);
        if (PyErr_Occurred()) {
            return -1;
        }
        state->external_state_kind = PICKLE_EXTERNAL_POSITION;
        return 0;
    }
    if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
        return 0;
    }
    return -1;
}

static void
pickle_clear_state(PickleContinuationState *state)
{
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->data_state);
    Py_XDECREF(state->external_state);
    Py_XDECREF(state->external_aux_state);
    Py_XDECREF(state->worker_state);
    Py_XDECREF(state->data);
    Py_XDECREF(state->external);
    Py_XDECREF(state->external_aux);
    Py_XDECREF(state->owner);
    Py_XDECREF(state->worker);
}

static void
pickle_free_state(void *raw_state)
{
    PickleContinuationState *state = raw_state;
    if (state == NULL) {
        return;
    }
    pickle_clear_state(state);
    PyMem_Free(state);
}

static void *
pickle_copy_state(const void *raw_state)
{
    const PickleContinuationState *source = raw_state;
    PickleContinuationState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->worker = NULL;
    copy->owner = NULL;
    copy->data = NULL;
    copy->external = NULL;
    copy->worker_state = NULL;
    copy->data_state = NULL;
    copy->external_state = NULL;
    copy->external_state_kind = PICKLE_EXTERNAL_NONE;
    copy->external_aux = NULL;
    copy->external_aux_state = NULL;
    copy->external_aux_state_kind = PICKLE_EXTERNAL_NONE;
    copy->external_aux_consumed = 0;
    copy->worker = Py_NewRef(source->worker);
    copy->owner = Py_XNewRef(source->owner);
    copy->data = Py_XNewRef(source->data);
    copy->external = Py_XNewRef(source->external);
    copy->worker_state = pickle_clone_worker_state(source->worker, source->kind);
    if (copy->worker_state == NULL ||
        pickle_capture_data_state(copy) < 0 ||
        pickle_capture_external_state(copy) < 0 ||
        pickle_capture_proxy_state(copy) < 0 ||
        pickle_capture_external_aux(copy) < 0) {
        pickle_free_state(copy);
        return NULL;
    }
    return copy;
}

static int
pickle_proxy_capture_before(
    PickleFileProxy *proxy,
    PyObject *callable,
    int readline
)
{
    PyObject *function = PyObject_GetAttrString(callable, "__func__");
    if (function == NULL) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Clear();
            return 0;
        }
        return -1;
    }
    PyObject *closure = PyObject_GetAttrString(function, "__closure__");
    Py_DECREF(function);
    if (closure == NULL) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Clear();
            return 0;
        }
        return -1;
    }
    if (closure == Py_None || !PyTuple_Check(closure)) {
        Py_DECREF(closure);
        return 0;
    }
    for (Py_ssize_t index = 0; index < PyTuple_GET_SIZE(closure); index++) {
        PyObject *candidate = PyCell_Get(PyTuple_GET_ITEM(closure, index));
        if (candidate == NULL) {
            if (PyErr_Occurred()) {
                PyErr_Clear();
            }
            continue;
        }
        PickleContinuationState temporary = {0};
        temporary.external = candidate;
        if (pickle_capture_external_state(&temporary) < 0) {
            Py_XDECREF(temporary.external_state);
            Py_DECREF(candidate);
            Py_DECREF(closure);
            return -1;
        }
        if (temporary.external_state_kind != PICKLE_EXTERNAL_NONE) {
            Py_XSETREF(proxy->before_external, Py_NewRef(candidate));
            Py_XSETREF(proxy->before_external_state, temporary.external_state);
            proxy->before_external_position = temporary.external_position;
            proxy->before_external_state_kind = temporary.external_state_kind;
            proxy->before_external_readline = readline;
            Py_DECREF(candidate);
            Py_DECREF(closure);
            return 0;
        }
        Py_XDECREF(temporary.external_state);
        Py_DECREF(candidate);
    }
    Py_DECREF(closure);
    return 0;
}

static int
pickle_restore_external_value(
    PyObject *external,
    PyObject *value,
    Py_ssize_t position,
    int state_kind
)
{
    if (external == NULL || state_kind == PICKLE_EXTERNAL_NONE) {
        return 0;
    }
    PyObject *result;
    if (state_kind == PICKLE_EXTERNAL_BYTES) {
        result = PyObject_CallMethod(external, "seek", "i", 0);
        if (result == NULL) {
            return -1;
        }
        Py_DECREF(result);
        result = PyObject_CallMethod(external, "truncate", "i", 0);
        if (result == NULL) {
            return -1;
        }
        Py_DECREF(result);
        result = PyObject_CallMethod(external, "write", "O", value);
    }
    else if (state_kind == PICKLE_EXTERNAL_DICT) {
        PyObject *target = PyObject_GetAttrString(external, "__dict__");
        if (target == NULL || !PyDict_Check(target)) {
            Py_XDECREF(target);
            PyErr_SetString(PyExc_RuntimeError, "pickle external stream has no dictionary state");
            return -1;
        }
        PyDict_Clear(target);
        int failed = PyDict_Update(target, value) < 0;
        Py_DECREF(target);
        if (failed) {
            return -1;
        }
        result = Py_NewRef(Py_None);
    }
    else {
        result = PyObject_CallMethod(external, "seek", "n", position);
    }
    if (result == NULL) {
        return -1;
    }
    Py_DECREF(result);
    if (state_kind == PICKLE_EXTERNAL_BYTES) {
        result = PyObject_CallMethod(external, "seek", "n", position);
        if (result == NULL) {
            return -1;
        }
        Py_DECREF(result);
    }
    return 0;
}

static int
pickle_prepare_resume(void *raw_state)
{
    PickleContinuationState *state = raw_state;
    state->external_aux_consumed = 0;
    if (state->worker_state == NULL || pickle_restore_worker_state(state) < 0) {
        return -1;
    }
    if (state->data != NULL && state->data_state != NULL &&
        pickle_restore_bytesio(state->data, state->data_state, state->data_position) < 0) {
        return -1;
    }
    if (pickle_restore_external_value(
            state->external,
            state->external_state,
            state->external_position,
            state->external_state_kind
        ) < 0 || pickle_restore_external_value(
            state->external_aux,
            state->external_aux_state,
            state->external_aux_position,
            state->external_aux_state_kind
        ) < 0) {
        return -1;
    }
    return 0;
}

static PyObject *
pickle_resume(const void *raw_state, PyObject *value)
{
    const PickleContinuationState *state = raw_state;
    if (value == NULL) {
        return NULL;
    }
    if (state->kind == PICKLE_WORKER_UNPICKLER) {
        PyObject *normalized = pickle_normalize_external_result(state, value);
        if (normalized == NULL) {
            return NULL;
        }
        value = normalized;
    }
    if (state->return_bytes) {
        PyObject *result = PyObject_CallMethod(state->data, "getvalue", NULL);
        if (result != NULL && pickle_sync_owner_memo(state->owner, state->worker) < 0) {
            Py_CLEAR(result);
        }
        Py_DECREF(value);
        return result;
    }
    if (pickle_sync_owner_memo(state->owner, state->worker) < 0) {
        Py_DECREF(value);
        return NULL;
    }
    return value;
}

static const AleffAdapterVTable pickle_vtable = {
    .copy_state = pickle_copy_state,
    .free_state = pickle_free_state,
    .resume = pickle_resume,
    .prepare_resume = pickle_prepare_resume,
};

PyObject *
adapter_pickle_normalize_result(void *raw_snapshot, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    struct AleffAdapterSnapshot *snapshot = raw_snapshot;
    PyObject *current = Py_NewRef(value);
    if (snapshot == NULL) {
        return current;
    }
    for (Py_ssize_t index = 0; index < snapshot->count; index++) {
        PickleAdapterSnapshotItem *item = &snapshot->items[index];
        if (item->vtable != &pickle_vtable) {
            continue;
        }
        PyObject *normalized = pickle_normalize_external_result(
            item->state,
            current
        );
        if (normalized == NULL) {
            Py_DECREF(current);
            return NULL;
        }
        Py_DECREF(current);
        current = normalized;
    }
    return current;
}

static int
pickle_sync_owner_memo(PyObject *owner, PyObject *worker)
{
    if (owner == NULL) {
        return 0;
    }
    PyObject *memo = PyObject_GetAttrString(worker, "memo");
    if (memo == NULL) {
        return -1;
    }
    int result = PyObject_SetAttrString(owner, "memo", memo);
    Py_DECREF(memo);
    return result;
}

static PyObject *
pickle_run_worker(
    PyObject *worker,
    PyObject *data,
    PyObject *external,
    PyObject *object,
    PickleWorkerKind kind,
    int return_bytes,
    PyObject *owner
)
{
    PickleContinuationState state = {
        .kind = kind,
        .worker = Py_NewRef(worker),
        .owner = Py_XNewRef(owner),
        .data = Py_XNewRef(data),
        .external = Py_XNewRef(external),
        .return_bytes = return_bytes,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &pickle_vtable, &state) < 0) {
        pickle_clear_state(&state);
        return NULL;
    }
    PyObject *result = kind == PICKLE_WORKER_PICKLER
        ? pickle_call_dump_method(worker, object)
        : pickle_call_load_method(worker);
    if (result != NULL && return_bytes) {
        PyObject *bytes = PyObject_CallMethod(data, "getvalue", NULL);
        Py_DECREF(result);
        result = bytes;
    }
    if (result != NULL && pickle_sync_owner_memo(owner, worker) < 0) {
        Py_CLEAR(result);
    }
    adapter_leave(&frame);
    pickle_clear_state(&state);
    return result;
}

static void
pickle_normalize_dump_error(void)
{
    PyObject *exception = PyErr_GetRaisedException();
    if (exception == NULL) {
        return;
    }
    PyObject *error_type = PyObject_GetAttrString(
        installed__pickle_module,
        "PicklingError"
    );
    int is_pickling_error = error_type != NULL &&
        PyObject_IsInstance(exception, error_type);
    if (is_pickling_error < 0) {
        Py_XDECREF(error_type);
        PyErr_SetRaisedException(exception);
        return;
    }
    if (is_pickling_error) {
        PyObject *text = PyObject_Str(exception);
        PyObject *suffix = PyUnicode_FromString(" must have two to six elements");
        int matches = text != NULL && suffix != NULL && PyUnicode_Check(text) &&
            PyUnicode_Tailmatch(text, suffix, 0, PY_SSIZE_T_MAX, 1) == 1;
        Py_XDECREF(suffix);
        if (matches) {
            PyObject *message = PyUnicode_FromString(
                "tuple returned by __reduce__ must contain 2 through 6 elements"
            );
            if (message != NULL) {
                PyErr_SetObject(error_type, message);
                Py_DECREF(message);
                Py_DECREF(text);
                Py_DECREF(error_type);
                Py_DECREF(exception);
                return;
            }
        }
        Py_XDECREF(text);
    }
    Py_XDECREF(error_type);
    PyErr_SetRaisedException(exception);
}

static void
pickle_normalize_load_error(void)
{
    PyObject *exception = PyErr_GetRaisedException();
    if (exception == NULL) {
        return;
    }

    int is_eof = PyObject_IsInstance(exception, PyExc_EOFError);
    int is_index = PyObject_IsInstance(exception, PyExc_IndexError);
    int is_key = PyObject_IsInstance(exception, PyExc_KeyError);
    PyObject *struct_module = PyImport_ImportModule("struct");
    PyObject *struct_error = struct_module == NULL
        ? NULL : PyObject_GetAttrString(struct_module, "error");
    Py_XDECREF(struct_module);
    if (struct_error == NULL) {
        PyErr_Clear();
    }
    int is_struct = struct_error == NULL
        ? 0 : PyObject_IsInstance(exception, struct_error);
    Py_XDECREF(struct_error);
    if (is_eof < 0 || is_index < 0 || is_key < 0 || is_struct < 0) {
        PyErr_SetRaisedException(exception);
        return;
    }

    if (is_eof) {
        PyObject *text = PyObject_Str(exception);
        int empty = text != NULL && PyUnicode_GET_LENGTH(text) == 0;
        Py_XDECREF(text);
        if (empty) {
            Py_DECREF(exception);
            PyErr_SetString(PyExc_EOFError, "Ran out of input");
            return;
        }
    }

    PyObject *message = NULL;
    if (is_key) {
        PyObject *args = PyObject_GetAttrString(exception, "args");
        if (args != NULL && PyTuple_Check(args) && PyTuple_GET_SIZE(args) == 1 &&
            PyLong_Check(PyTuple_GET_ITEM(args, 0))) {
            long key = PyLong_AsLong(PyTuple_GET_ITEM(args, 0));
            if (!(key == -1 && PyErr_Occurred()) && key >= 0 && key <= 255) {
                message = PyUnicode_FromFormat(
                    "invalid load key, '%c'.",
                    (int)key
                );
            }
        }
        Py_XDECREF(args);
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
    }
    if (message == NULL && (is_index || is_eof || is_struct)) {
        message = PyUnicode_FromString("pickle data was truncated");
    }
    if (message != NULL) {
        PyObject *error_type = PyObject_GetAttrString(
            installed__pickle_module,
            "UnpicklingError"
        );
        if (error_type != NULL) {
            PyErr_SetObject(error_type, message);
            Py_DECREF(error_type);
            Py_DECREF(message);
            Py_DECREF(exception);
            return;
        }
        PyErr_Clear();
        Py_DECREF(message);
    }
    PyErr_SetRaisedException(exception);
}

static PyObject *
pickle_file_proxy_write(PickleFileProxy *self, PyObject *value)
{
    if (self->write == NULL) {
        PyErr_SetString(PyExc_AttributeError, "write");
        return NULL;
    }
    if (pickle_proxy_capture_before(self, self->write, 0) < 0) {
        return NULL;
    }
    PyObject *bytes = pickle_bytes_from_object(value);
    if (bytes == NULL) {
        return NULL;
    }
    Py_ssize_t length = PyBytes_GET_SIZE(bytes);
    if (self->pending == NULL && length == 2 &&
        (unsigned char)PyBytes_AS_STRING(bytes)[0] == 0x80) {
        int failed = pickle_proxy_append(self, bytes);
        Py_DECREF(bytes);
        return failed < 0 ? NULL : PyLong_FromSsize_t(length);
    }

    Py_ssize_t pending_length;
    if (pickle_proxy_pending_length(self, &pending_length) < 0) {
        Py_DECREF(bytes);
        return NULL;
    }
    if (pending_length == 2 && length == 9 &&
        (unsigned char)PyBytes_AS_STRING(bytes)[0] == 0x95) {
        int failed = pickle_proxy_append(self, bytes);
        Py_DECREF(bytes);
        return failed < 0 ? NULL : PyLong_FromSsize_t(length);
    }
    if (pending_length == 11) {
        PyObject *pending = pickle_proxy_pending_bytes(self);
        if (pending == NULL) {
            Py_DECREF(bytes);
            return NULL;
        }
        Py_ssize_t pending_size = PyBytes_GET_SIZE(pending);
        PyObject *combined = PyBytes_FromStringAndSize(
            NULL,
            pending_size + length
        );
        if (combined == NULL) {
            Py_DECREF(pending);
            Py_DECREF(bytes);
            return NULL;
        }
        memcpy(
            PyBytes_AS_STRING(combined),
            PyBytes_AS_STRING(pending),
            (size_t)pending_size
        );
        memcpy(
            PyBytes_AS_STRING(combined) + pending_size,
            PyBytes_AS_STRING(bytes),
            (size_t)length
        );
        PyObject *result = PyObject_CallOneArg(self->write, combined);
        if (result != NULL) {
            Py_CLEAR(self->pending);
        }
        Py_DECREF(combined);
        Py_DECREF(pending);
        Py_DECREF(bytes);
        return result;
    }

    Py_DECREF(bytes);
    if (self->pending != NULL) {
        PyObject *pending = pickle_proxy_pending_bytes(self);
        if (pending == NULL) {
            return NULL;
        }
        PyObject *result = PyObject_CallOneArg(self->write, pending);
        if (result == NULL) {
            Py_DECREF(pending);
            return NULL;
        }
        Py_CLEAR(self->pending);
        Py_DECREF(pending);
        return PyObject_CallOneArg(self->write, value);
    }
    return PyObject_CallOneArg(self->write, value);
}

static PyObject *
pickle_file_proxy_read(PickleFileProxy *self, PyObject *args)
{
    Py_ssize_t size;
    if (!PyArg_ParseTuple(args, "n:read", &size)) {
        return NULL;
    }
    if (self->read == NULL) {
        PyErr_SetString(PyExc_AttributeError, "read");
        return NULL;
    }
    if (pickle_proxy_capture_before(self, self->read, 0) < 0) {
        return NULL;
    }
    PyObject *size_object = PyLong_FromSsize_t(size);
    if (size_object == NULL) {
        return NULL;
    }
    PyObject *result = PyObject_CallOneArg(self->read, size_object);
    Py_DECREF(size_object);
    if (result == NULL) {
        return NULL;
    }
    PyObject *expected = pickle_proxy_expected_read(self, result, 0, size);
    Py_DECREF(result);
    return expected;
}

static PyObject *
pickle_file_proxy_readline(
    PickleFileProxy *self,
    PyObject *Py_UNUSED(arguments)
)
{
    if (self->readline == NULL) {
        PyErr_SetString(PyExc_AttributeError, "readline");
        return NULL;
    }
    if (pickle_proxy_capture_before(self, self->readline, 1) < 0) {
        return NULL;
    }
    PyObject *result = PyObject_CallNoArgs(self->readline);
    if (result == NULL) {
        return NULL;
    }
    PyObject *expected = pickle_proxy_expected_read(self, result, 1, 0);
    Py_DECREF(result);
    return expected;
}

static void
pickle_file_proxy_dealloc(PickleFileProxy *self)
{
    Py_XDECREF(self->before_external_state);
    Py_XDECREF(self->before_external);
    Py_XDECREF(self->pending);
    Py_XDECREF(self->readline);
    Py_XDECREF(self->read);
    Py_XDECREF(self->write);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMethodDef pickle_file_proxy_methods[] = {
    {
        "write",
        _PyCFunction_CAST(pickle_file_proxy_write),
        METH_O,
        NULL,
    },
    {
        "read",
        _PyCFunction_CAST(pickle_file_proxy_read),
        METH_VARARGS,
        NULL,
    },
    {
        "readline",
        _PyCFunction_CAST(pickle_file_proxy_readline),
        METH_NOARGS,
        NULL,
    },
    {NULL, NULL, 0, NULL},
};

static PyTypeObject PickleFileProxyType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "aleff._pickle_file_proxy",
    .tp_basicsize = sizeof(PickleFileProxy),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)pickle_file_proxy_dealloc,
    .tp_methods = pickle_file_proxy_methods,
};

static PickleFileProxy *
pickle_file_proxy_new(
    PyObject *write,
    PyObject *read,
    PyObject *readline
)
{
    PickleFileProxy *proxy = PyObject_New(
        PickleFileProxy,
        &PickleFileProxyType
    );
    if (proxy == NULL) {
        return NULL;
    }
    proxy->write = Py_XNewRef(write);
    proxy->read = Py_XNewRef(read);
    proxy->readline = Py_XNewRef(readline);
    proxy->pending = NULL;
    proxy->before_external = NULL;
    proxy->before_external_state = NULL;
    proxy->before_external_position = 0;
    proxy->before_external_state_kind = PICKLE_EXTERNAL_NONE;
    proxy->before_external_readline = 0;
    return proxy;
}

static PyObject *
pickle_optional_attr(PyObject *object, const char *name, int *present)
{
    PyObject *result = PyObject_GetAttrString(object, name);
    if (result != NULL) {
        *present = 1;
        return result;
    }
    if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
        *present = 0;
        return NULL;
    }
    return NULL;
}

static PyObject *
pickle_callable_owner(PyObject *callable)
{
    PyObject *owner = PyObject_GetAttrString(callable, "__self__");
    if (owner == NULL && PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
    }
    return owner;
}

static PyObject *
pickle_copy_keywords(
    PyObject *const *values,
    Py_ssize_t positional_count,
    PyObject *keyword_names
)
{
    PyObject *kwargs = PyDict_New();
    if (kwargs == NULL) {
        return NULL;
    }
    if (keyword_names == NULL) {
        return kwargs;
    }
    Py_ssize_t keyword_count = PyTuple_GET_SIZE(keyword_names);
    for (Py_ssize_t index = 0; index < keyword_count; index++) {
        if (PyDict_SetItem(
                kwargs,
                PyTuple_GET_ITEM(keyword_names, index),
                values[positional_count + index]
            ) < 0) {
            Py_DECREF(kwargs);
            return NULL;
        }
    }
    return kwargs;
}

static int
pickle_keyword_index(PyObject *keyword_names, const char *name)
{
    if (keyword_names == NULL) {
        return -1;
    }
    Py_ssize_t count = PyTuple_GET_SIZE(keyword_names);
    for (Py_ssize_t index = 0; index < count; index++) {
        PyObject *key = PyTuple_GET_ITEM(keyword_names, index);
        if (PyUnicode_Check(key) &&
            PyUnicode_CompareWithASCIIString(key, name) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int
pickle_keyword_allowed(PyObject *keyword_names, const char *const *allowed)
{
    if (keyword_names == NULL) {
        return 1;
    }
    Py_ssize_t count = PyTuple_GET_SIZE(keyword_names);
    for (Py_ssize_t index = 0; index < count; index++) {
        PyObject *key = PyTuple_GET_ITEM(keyword_names, index);
        int found = 0;
        for (size_t allowed_index = 0; allowed[allowed_index] != NULL;
             allowed_index++) {
            if (PyUnicode_Check(key) &&
                PyUnicode_CompareWithASCIIString(key, allowed[allowed_index]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return 0;
        }
    }
    return 1;
}

static int
pickle_valid_shape(
    PickleOperation operation,
    PyObject *const *values,
    Py_ssize_t positional_count,
    PyObject *keyword_names
)
{
    static const char *const dump_keywords[] = {
        "protocol", "fix_imports", "buffer_callback", NULL
    };
    static const char *const load_keywords[] = {
        "fix_imports", "encoding", "errors", "buffers", NULL
    };
    Py_ssize_t minimum = operation == PICKLE_DUMP || operation == PICKLE_LOAD
        ? 1 : operation == PICKLE_DUMPS ? 1 : 1;
    Py_ssize_t maximum = operation == PICKLE_DUMP ? 3
        : operation == PICKLE_DUMPS ? 2 : 1;
    if (positional_count < minimum || positional_count > maximum) {
        return 0;
    }
    if (!pickle_keyword_allowed(
            keyword_names,
            operation == PICKLE_DUMP || operation == PICKLE_DUMPS
                ? dump_keywords : load_keywords
        )) {
        return 0;
    }
    if (operation == PICKLE_DUMP && positional_count < 2) {
        return 0;
    }
    if (operation == PICKLE_LOAD && positional_count != 1) {
        return 0;
    }

    int protocol_keyword = pickle_keyword_index(keyword_names, "protocol");
    if ((operation == PICKLE_DUMP || operation == PICKLE_DUMPS) &&
        protocol_keyword >= 0 &&
        positional_count == (operation == PICKLE_DUMP ? 3 : 2)) {
        return 0;
    }
    if (operation == PICKLE_DUMP || operation == PICKLE_DUMPS) {
        PyObject *protocol = protocol_keyword >= 0
            ? values[positional_count + protocol_keyword]
            : positional_count == (operation == PICKLE_DUMP ? 3 : 2)
                ? values[positional_count - 1] : NULL;
        if (protocol != NULL && protocol != Py_None &&
            !PyLong_Check(protocol)) {
            return 0;
        }
    }
    return 1;
}

static PyObject *
pickle_call_constructor(
    PyObject *constructor,
    PyObject *file,
    PyObject *protocol,
    PyObject *kwargs
)
{
    PyObject *arguments = PyTuple_New(protocol == NULL ? 1 : 2);
    if (arguments == NULL) {
        return NULL;
    }
    PyTuple_SET_ITEM(arguments, 0, Py_NewRef(file));
    if (protocol != NULL) {
        PyTuple_SET_ITEM(arguments, 1, Py_NewRef(protocol));
    }
    PyObject *result = PyObject_Call(constructor, arguments, kwargs);
    Py_DECREF(arguments);
    return result;
}

static PyObject *
pickle_call_dump_method(PyObject *pickler, PyObject *object)
{
    return PyObject_CallMethod(pickler, "dump", "O", object);
}

static PyObject *
pickle_call_load_method(PyObject *unpickler)
{
    return PyObject_CallMethod(unpickler, "load", NULL);
}

static PyObject *
pickle_public_dispatch(
    PickleOperation operation,
    PyObject *const *values,
    Py_ssize_t positional_count,
    PyObject *keyword_names
)
{
    PyObject *original = original_functions[operation];
    if (!pickle_valid_shape(
            operation,
            values,
            positional_count,
            keyword_names
        )) {
        return PyObject_Vectorcall(
            original,
            values,
            (size_t)positional_count,
            keyword_names
        );
    }

    PyObject *kwargs = pickle_copy_keywords(
        values,
        positional_count,
        keyword_names
    );
    if (kwargs == NULL) {
        return NULL;
    }

    PyObject *file = NULL;
    PyObject *data = NULL;
    PyObject *proxy = NULL;
    PyObject *worker = NULL;
    PyObject *result = NULL;
    PyObject *protocol = NULL;
    PyObject *write = NULL;
    PyObject *read = NULL;
    PyObject *readline = NULL;

    if (operation == PICKLE_DUMP || operation == PICKLE_LOAD) {
        file = values[1 - (operation == PICKLE_LOAD)];
    }
    else if (operation == PICKLE_DUMPS) {
        data = PyObject_CallNoArgs(bytesio_type);
        if (data == NULL) {
            goto done;
        }
        file = data;
    }
    else {
        data = PyObject_CallOneArg(bytesio_type, values[0]);
        if (data == NULL) {
            goto done;
        }
        file = data;
    }

    if (operation == PICKLE_DUMP || operation == PICKLE_DUMPS) {
        write = PyObject_GetAttrString(file, "write");
        if (write == NULL) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                PyErr_Clear();
                result = PyObject_Vectorcall(
                    original,
                    values,
                    (size_t)positional_count,
                    keyword_names
                );
            }
            goto done;
        }
        proxy = (PyObject *)pickle_file_proxy_new(write, NULL, NULL);
        Py_CLEAR(write);
    }
    else {
        readline = PyObject_GetAttrString(file, "readline");
        if (readline == NULL) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                PyErr_Clear();
                result = PyObject_Vectorcall(
                    original,
                    values,
                    (size_t)positional_count,
                    keyword_names
                );
            }
            goto done;
        }
        read = PyObject_GetAttrString(file, "read");
        if (read == NULL) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                PyErr_Clear();
                result = PyObject_Vectorcall(
                    original,
                    values,
                    (size_t)positional_count,
                    keyword_names
                );
            }
            goto done;
        }
        proxy = (PyObject *)pickle_file_proxy_new(NULL, read, readline);
        Py_CLEAR(read);
        Py_CLEAR(readline);
    }
    if (proxy == NULL) {
        goto done;
    }

    if (operation == PICKLE_DUMP || operation == PICKLE_DUMPS) {
        int protocol_index = pickle_keyword_index(keyword_names, "protocol");
        if (protocol_index >= 0) {
            protocol = Py_NewRef(values[positional_count + protocol_index]);
            if (PyDict_DelItemString(kwargs, "protocol") < 0) {
                goto done;
            }
        }
        else if (positional_count == (operation == PICKLE_DUMP ? 3 : 2)) {
            protocol = Py_NewRef(values[positional_count - 1]);
        }
        worker = pickle_call_constructor(
            pure_pickler,
            proxy,
            protocol,
            kwargs
        );
        if (worker != NULL) {
            result = pickle_run_worker(
                worker,
                data,
                file,
                values[0],
                PICKLE_WORKER_PICKLER,
                operation == PICKLE_DUMPS,
                NULL
            );
        }
    }
    else {
        worker = pickle_call_constructor(
            pure_unpickler,
            proxy,
            NULL,
            kwargs
        );
        if (worker != NULL) {
            result = pickle_run_worker(
                worker,
                data,
                file,
                NULL,
                PICKLE_WORKER_UNPICKLER,
                0,
                NULL
            );
        }
    }

done:
    if (result == NULL &&
        (operation == PICKLE_DUMP || operation == PICKLE_DUMPS) &&
        PyErr_Occurred()) {
        pickle_normalize_dump_error();
    }
    if (result == NULL &&
        (operation == PICKLE_LOAD || operation == PICKLE_LOADS) &&
        PyErr_Occurred()) {
        pickle_normalize_load_error();
    }
    Py_XDECREF(worker);
    Py_XDECREF(proxy);
    Py_XDECREF(data);
    Py_XDECREF(kwargs);
    Py_XDECREF(protocol);
    Py_XDECREF(write);
    Py_XDECREF(read);
    Py_XDECREF(readline);
    return result;
}

static PyObject *
pickle_original_method_vectorcall(
    PyObject *descriptor,
    PyObject *self,
    PyObject *const *values,
    Py_ssize_t nargs,
    PyObject *keyword_names
)
{
    Py_ssize_t keyword_count = keyword_names == NULL
        ? 0 : PyTuple_GET_SIZE(keyword_names);
    if (nargs > PY_SSIZE_T_MAX - keyword_count - 1) {
        PyErr_NoMemory();
        return NULL;
    }
    Py_ssize_t count = nargs + keyword_count + 1;
    PyObject **arguments = PyMem_Malloc((size_t)count * sizeof(*arguments));
    if (arguments == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    arguments[0] = self;
    for (Py_ssize_t index = 0; index < nargs + keyword_count; index++) {
        arguments[index + 1] = values[index];
    }
    PyObject *result = PyObject_Vectorcall(
        descriptor,
        arguments,
        (size_t)nargs + 1,
        keyword_names
    );
    PyMem_Free(arguments);
    return result;
}

static PyObject *
pickle_pickler_dump_wrapper(
    PyObject *self,
    PyTypeObject *Py_UNUSED(defining_class),
    PyObject *const *values,
    Py_ssize_t nargs,
    PyObject *keyword_names
)
{
    if (nargs != 1 ||
        (keyword_names != NULL && PyTuple_GET_SIZE(keyword_names) != 0)) {
        return pickle_original_method_vectorcall(
            original_pickler_dump,
            self,
            values,
            nargs,
            keyword_names
        );
    }
    PyObject *object = values[0];
    PicklePicklerObject *native = (PicklePicklerObject *)self;
    if (native->write == NULL) {
        return pickle_original_method_vectorcall(
            original_pickler_dump,
            self,
            values,
            nargs,
            keyword_names
        );
    }
    PickleFileProxy *proxy = pickle_file_proxy_new(native->write, NULL, NULL);
    if (proxy == NULL) {
        return NULL;
    }

    PyObject *kwargs = PyDict_New();
    if (kwargs == NULL) {
        Py_DECREF(proxy);
        return NULL;
    }
    if (PyDict_SetItemString(
            kwargs,
            "fix_imports",
            native->fix_imports ? Py_True : Py_False
        ) < 0) {
        Py_DECREF(proxy);
        Py_DECREF(kwargs);
        return NULL;
    }
    if (native->buffer_callback != NULL &&
        PyDict_SetItemString(
            kwargs,
            "buffer_callback",
            native->buffer_callback
        ) < 0) {
        Py_DECREF(proxy);
        Py_DECREF(kwargs);
        return NULL;
    }
    PyObject *protocol = PyLong_FromLong(native->proto);
    if (protocol == NULL) {
        Py_DECREF(proxy);
        Py_DECREF(kwargs);
        return NULL;
    }
    PyObject *worker = pickle_call_constructor(
        pure_pickler,
        (PyObject *)proxy,
        protocol,
        kwargs
    );
    Py_DECREF(protocol);
    Py_DECREF(kwargs);
    if (worker == NULL) {
        Py_DECREF(proxy);
        return NULL;
    }

    int present = 0;
    PyObject *hook = pickle_optional_attr(self, "persistent_id", &present);
    if (hook == NULL && PyErr_Occurred()) {
        Py_DECREF(worker);
        Py_DECREF(proxy);
        return NULL;
    }
    if (present && PyObject_SetAttrString(worker, "persistent_id", hook) < 0) {
        Py_DECREF(hook);
        Py_DECREF(worker);
        Py_DECREF(proxy);
        return NULL;
    }
    Py_XDECREF(hook);

    hook = pickle_optional_attr(self, "reducer_override", &present);
    if (hook == NULL && PyErr_Occurred()) {
        Py_DECREF(worker);
        Py_DECREF(proxy);
        return NULL;
    }
    if (present && PyObject_SetAttrString(worker, "reducer_override", hook) < 0) {
        Py_DECREF(hook);
        Py_DECREF(worker);
        Py_DECREF(proxy);
        return NULL;
    }
    Py_XDECREF(hook);

    hook = pickle_optional_attr(self, "dispatch_table", &present);
    if (hook == NULL && PyErr_Occurred()) {
        Py_DECREF(worker);
        Py_DECREF(proxy);
        return NULL;
    }
    if (present && PyObject_SetAttrString(worker, "dispatch_table", hook) < 0) {
        Py_DECREF(hook);
        Py_DECREF(worker);
        Py_DECREF(proxy);
        return NULL;
    }
    Py_XDECREF(hook);

    PyObject *memo = PyObject_GetAttrString(self, "memo");
    if (memo != NULL) {
        PyObject *copy = PyObject_CallMethod(memo, "copy", NULL);
        Py_DECREF(memo);
        if (copy == NULL || PyObject_SetAttrString(worker, "memo", copy) < 0) {
            Py_XDECREF(copy);
            Py_DECREF(worker);
            Py_DECREF(proxy);
            return NULL;
        }
        Py_DECREF(copy);
    }
    else if (PyErr_Occurred()) {
        Py_DECREF(worker);
        Py_DECREF(proxy);
        return NULL;
    }

    PyObject *fast = PyObject_GetAttrString(self, "fast");
    if (fast != NULL) {
        if (PyObject_SetAttrString(worker, "fast", fast) < 0) {
            Py_DECREF(fast);
            Py_DECREF(worker);
            Py_DECREF(proxy);
            return NULL;
        }
        Py_DECREF(fast);
    }
    else if (PyErr_Occurred()) {
        Py_DECREF(worker);
        Py_DECREF(proxy);
        return NULL;
    }

    PyObject *external = pickle_callable_owner(native->write);
    if (external == NULL && PyErr_Occurred()) {
        Py_DECREF(worker);
        Py_DECREF(proxy);
        return NULL;
    }
    PyObject *result = pickle_run_worker(
        worker,
        NULL,
        external,
        object,
        PICKLE_WORKER_PICKLER,
        0,
        self
    );
    if (result == NULL && PyErr_Occurred()) {
        pickle_normalize_dump_error();
    }
    Py_XDECREF(external);
    Py_DECREF(worker);
    Py_DECREF(proxy);
    return result;
}

static PyObject *
pickle_unpickler_load_wrapper(
    PyObject *self,
    PyTypeObject *Py_UNUSED(defining_class),
    PyObject *const *values,
    Py_ssize_t nargs,
    PyObject *keyword_names
)
{
    if (nargs != 0 ||
        (keyword_names != NULL && PyTuple_GET_SIZE(keyword_names) != 0)) {
        return pickle_original_method_vectorcall(
            original_unpickler_load,
            self,
            values,
            nargs,
            keyword_names
        );
    }
    PickleUnpicklerObject *native = (PickleUnpicklerObject *)self;
    if (native->read == NULL || native->readline == NULL) {
        return pickle_original_method_vectorcall(
            original_unpickler_load,
            self,
            values,
            nargs,
            keyword_names
        );
    }
    PickleFileProxy *proxy = pickle_file_proxy_new(
        NULL,
        native->read,
        native->readline
    );
    if (proxy == NULL) {
        return NULL;
    }
    PyObject *kwargs = PyDict_New();
    if (kwargs == NULL) {
        Py_DECREF(proxy);
        return NULL;
    }
    if (PyDict_SetItemString(
            kwargs,
            "fix_imports",
            native->fix_imports ? Py_True : Py_False
        ) < 0) {
        Py_DECREF(proxy);
        Py_DECREF(kwargs);
        return NULL;
    }
    PyObject *encoding = PyUnicode_FromString(
        native->encoding == NULL ? "ASCII" : native->encoding
    );
    if (encoding == NULL || PyDict_SetItemString(kwargs, "encoding", encoding) < 0) {
        Py_XDECREF(encoding);
        Py_DECREF(proxy);
        Py_DECREF(kwargs);
        return NULL;
    }
    Py_DECREF(encoding);
    PyObject *errors = PyUnicode_FromString(
        native->errors == NULL ? "strict" : native->errors
    );
    if (errors == NULL || PyDict_SetItemString(kwargs, "errors", errors) < 0) {
        Py_XDECREF(errors);
        Py_DECREF(proxy);
        Py_DECREF(kwargs);
        return NULL;
    }
    Py_DECREF(errors);
    if (native->buffers != NULL &&
        PyDict_SetItemString(kwargs, "buffers", native->buffers) < 0) {
        Py_DECREF(proxy);
        Py_DECREF(kwargs);
        return NULL;
    }

    PyObject *worker = pickle_call_constructor(
        pure_unpickler,
        (PyObject *)proxy,
        NULL,
        kwargs
    );
    Py_DECREF(kwargs);
    if (worker == NULL) {
        Py_DECREF(proxy);
        return NULL;
    }

    PyObject *hook = PyObject_GetAttrString(self, "find_class");
    if (hook == NULL || PyObject_SetAttrString(worker, "find_class", hook) < 0) {
        Py_XDECREF(hook);
        Py_DECREF(worker);
        Py_DECREF(proxy);
        return NULL;
    }
    Py_DECREF(hook);

    int present = 0;
    hook = pickle_optional_attr(self, "persistent_load", &present);
    if (hook == NULL && PyErr_Occurred()) {
        Py_DECREF(worker);
        Py_DECREF(proxy);
        return NULL;
    }
    if (present && PyObject_SetAttrString(worker, "persistent_load", hook) < 0) {
        Py_DECREF(hook);
        Py_DECREF(worker);
        Py_DECREF(proxy);
        return NULL;
    }
    Py_XDECREF(hook);

    PyObject *memo = PyObject_GetAttrString(self, "memo");
    if (memo != NULL) {
        PyObject *copy = PyObject_CallMethod(memo, "copy", NULL);
        Py_DECREF(memo);
        if (copy == NULL || PyObject_SetAttrString(worker, "memo", copy) < 0) {
            Py_XDECREF(copy);
            Py_DECREF(worker);
            Py_DECREF(proxy);
            return NULL;
        }
        Py_DECREF(copy);
    }
    else if (PyErr_Occurred()) {
        Py_DECREF(worker);
        Py_DECREF(proxy);
        return NULL;
    }

    PyObject *external = pickle_callable_owner(native->read);
    if (external == NULL && PyErr_Occurred()) {
        Py_DECREF(worker);
        Py_DECREF(proxy);
        return NULL;
    }
    PyObject *result = pickle_run_worker(
        worker,
        NULL,
        external,
        NULL,
        PICKLE_WORKER_UNPICKLER,
        0,
        self
    );
    if (result == NULL && PyErr_Occurred()) {
        pickle_normalize_load_error();
    }
    Py_XDECREF(external);
    Py_DECREF(worker);
    Py_DECREF(proxy);
    return result;
}

static PyObject *
pickle_dump_wrapper(
    PyObject *self,
    PyObject *const *values,
    Py_ssize_t nargs,
    PyObject *keyword_names
)
{
    (void)self;
    return pickle_public_dispatch(PICKLE_DUMP, values, nargs, keyword_names);
}

static PyObject *
pickle_dumps_wrapper(
    PyObject *self,
    PyObject *const *values,
    Py_ssize_t nargs,
    PyObject *keyword_names
)
{
    (void)self;
    return pickle_public_dispatch(PICKLE_DUMPS, values, nargs, keyword_names);
}

static PyObject *
pickle_load_wrapper(
    PyObject *self,
    PyObject *const *values,
    Py_ssize_t nargs,
    PyObject *keyword_names
)
{
    (void)self;
    return pickle_public_dispatch(PICKLE_LOAD, values, nargs, keyword_names);
}

static PyObject *
pickle_loads_wrapper(
    PyObject *self,
    PyObject *const *values,
    Py_ssize_t nargs,
    PyObject *keyword_names
)
{
    (void)self;
    return pickle_public_dispatch(PICKLE_LOADS, values, nargs, keyword_names);
}

static PyCFunction const pickle_wrappers[] = {
    _PyCFunction_CAST(pickle_dump_wrapper),
    _PyCFunction_CAST(pickle_dumps_wrapper),
    _PyCFunction_CAST(pickle_load_wrapper),
    _PyCFunction_CAST(pickle_loads_wrapper),
};

static int
pickle_install_method(
    PyTypeObject *type,
    const char *name,
    PyObject **original,
    PyCFunction wrapper
)
{
    PyObject *type_dict = PyType_GetDict(type);
    if (type_dict == NULL) {
        return -1;
    }
    PyObject *descriptor = PyDict_GetItemString(type_dict, name);
    if (descriptor == NULL || !Py_IS_TYPE(descriptor, &PyMethodDescr_Type)) {
        Py_DECREF(type_dict);
        PyErr_Format(PyExc_RuntimeError, "cannot access _pickle.%s", name);
        return -1;
    }
    *original = Py_NewRef(descriptor);
    PyMethodDef *method = type == pickler_type
        ? &pickle_pickler_dump_method
        : &pickle_unpickler_load_method;
    *method = *((PyMethodDescrObject *)descriptor)->d_method;
    method->ml_meth = wrapper;
    PyObject *replacement = PyDescr_NewMethod(type, method);
    if (replacement == NULL) {
        Py_DECREF(type_dict);
        return -1;
    }
    int result = PyDict_SetItemString(type_dict, name, replacement);
    Py_DECREF(replacement);
    Py_DECREF(type_dict);
    if (result == 0) {
        PyType_Modified(type);
    }
    return result;
}

int
adapter_pickle_install(PyObject *pickle_module)
{
    PyObject *replacements[4] = {NULL, NULL, NULL, NULL};
    const char *names[4] = {"dump", "dumps", "load", "loads"};
    if (pickle_installed) {
        return 0;
    }
    if (PyType_Ready(&PickleFileProxyType) < 0) {
        return -1;
    }
    PyObject *accelerator = PyImport_ImportModule("_pickle");
    if (accelerator == NULL) {
        return -1;
    }
    installed_pickle_module = Py_NewRef(pickle_module);
    installed__pickle_module = accelerator;
    pure_pickler = PyObject_GetAttrString(pickle_module, "_Pickler");
    pure_unpickler = PyObject_GetAttrString(pickle_module, "_Unpickler");
    PyObject *io = PyImport_ImportModule("io");
    bytesio_type = io == NULL ? NULL : PyObject_GetAttrString(io, "BytesIO");
    Py_XDECREF(io);
    if (pure_pickler == NULL || pure_unpickler == NULL || bytesio_type == NULL) {
        goto error;
    }

    for (int index = 0; index < 4; index++) {
        PyObject *original = PyObject_GetAttrString(
            accelerator,
            names[index]
        );
        if (original == NULL || !PyCFunction_Check(original)) {
            Py_XDECREF(original);
            PyErr_Format(PyExc_RuntimeError, "_pickle.%s is not a C function", names[index]);
            goto error;
        }
        original_functions[index] = original;
        pickle_replacement_methods[index] = *((PyCFunctionObject *)original)->m_ml;
        pickle_replacement_methods[index].ml_meth = pickle_wrappers[index];
        PyObject *module_name = PyObject_GetAttrString(original, "__module__");
        replacements[index] = module_name == NULL
            ? NULL
            : PyCFunction_NewEx(
                &pickle_replacement_methods[index],
                PyCFunction_GET_SELF(original),
                module_name
            );
        Py_XDECREF(module_name);
        if (replacements[index] == NULL) {
            goto error;
        }
    }

    PyObject *pickler_object = PyObject_GetAttrString(accelerator, "Pickler");
    PyObject *unpickler_object = PyObject_GetAttrString(accelerator, "Unpickler");
    if (pickler_object == NULL || unpickler_object == NULL ||
        !PyType_Check(pickler_object) || !PyType_Check(unpickler_object)) {
        Py_XDECREF(pickler_object);
        Py_XDECREF(unpickler_object);
        goto error;
    }
    pickler_type = (PyTypeObject *)pickler_object;
    unpickler_type = (PyTypeObject *)unpickler_object;
    if (pickle_install_method(
            pickler_type,
            "dump",
            &original_pickler_dump,
            _PyCFunction_CAST(pickle_pickler_dump_wrapper)
        ) < 0 ||
        pickle_install_method(
            unpickler_type,
            "load",
            &original_unpickler_load,
            _PyCFunction_CAST(pickle_unpickler_load_wrapper)
        ) < 0) {
        Py_DECREF(pickler_object);
        Py_DECREF(unpickler_object);
        goto error;
    }
    Py_DECREF(pickler_object);
    Py_DECREF(unpickler_object);

    for (int index = 0; index < 4; index++) {
        if (PyObject_SetAttrString(
                pickle_module,
                names[index],
                replacements[index]
            ) < 0 ||
            PyObject_SetAttrString(
                accelerator,
                names[index],
                replacements[index]
            ) < 0) {
            goto error;
        }
    }
    for (int index = 0; index < 4; index++) {
        Py_DECREF(replacements[index]);
    }
    pickle_installed = 1;
    return 0;

error:
    for (int index = 0; index < 4; index++) {
        Py_XDECREF(replacements[index]);
    }
    adapter_pickle_rollback();
    return -1;
}

void
adapter_pickle_rollback(void)
{
    const char *names[4] = {"dump", "dumps", "load", "loads"};
    if (installed_pickle_module != NULL && installed__pickle_module != NULL) {
        for (int index = 0; index < 4; index++) {
            if (original_functions[index] != NULL) {
                if (PyObject_SetAttrString(
                        installed_pickle_module,
                        names[index],
                        original_functions[index]
                    ) < 0) {
                    PyErr_Clear();
                }
                if (PyObject_SetAttrString(
                        installed__pickle_module,
                        names[index],
                        original_functions[index]
                    ) < 0) {
                    PyErr_Clear();
                }
                Py_CLEAR(original_functions[index]);
            }
        }
    }
    if (pickler_type != NULL && original_pickler_dump != NULL) {
        PyObject *type_dict = PyType_GetDict(pickler_type);
        if (type_dict != NULL && PyDict_SetItemString(
                type_dict,
                "dump",
                original_pickler_dump
            ) < 0) {
            PyErr_Clear();
        }
        Py_XDECREF(type_dict);
        PyType_Modified(pickler_type);
    }
    if (unpickler_type != NULL && original_unpickler_load != NULL) {
        PyObject *type_dict = PyType_GetDict(unpickler_type);
        if (type_dict != NULL && PyDict_SetItemString(
                type_dict,
                "load",
                original_unpickler_load
            ) < 0) {
            PyErr_Clear();
        }
        Py_XDECREF(type_dict);
        PyType_Modified(unpickler_type);
    }
    Py_CLEAR(original_pickler_dump);
    Py_CLEAR(original_unpickler_load);
    pickler_type = NULL;
    unpickler_type = NULL;
    Py_CLEAR(pure_pickler);
    Py_CLEAR(pure_unpickler);
    Py_CLEAR(bytesio_type);
    Py_CLEAR(installed_pickle_module);
    Py_CLEAR(installed__pickle_module);
    pickle_installed = 0;
}
