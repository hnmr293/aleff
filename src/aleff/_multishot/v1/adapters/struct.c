#include "api.h"
#include "struct.h"

typedef struct {
    PyObject_HEAD
    Py_ssize_t size;
    Py_ssize_t length;
    void *codes;
    PyObject *format;
} StructObjectPrefix;

typedef struct {
    PyObject_HEAD
    PyObject *owner;
    PyObject *view;
    int release_requested;
    int release_started;
} StructBufferProxy;

typedef struct {
    Py_buffer inner;
} StructBufferLease;

typedef enum {
    STRUCT_PACK,
    STRUCT_PACK_INTO,
    STRUCT_UNPACK,
    STRUCT_UNPACK_FROM,
    STRUCT_ITER_UNPACK,
    STRUCT_OPERATION_COUNT,
} StructOperation;

typedef enum {
    STRUCT_VALUE_BYTES,
    STRUCT_VALUE_CHAR,
    STRUCT_VALUE_INDEX,
    STRUCT_VALUE_FLOAT,
    STRUCT_VALUE_BOOL,
#if PY_VERSION_HEX >= 0x030e0000
    STRUCT_VALUE_COMPLEX,
#endif
} StructValueKind;

typedef enum {
    STRUCT_PENDING_NONE,
    STRUCT_PENDING_READ_BUFFER,
    STRUCT_PENDING_WRITE_BUFFER,
    STRUCT_PENDING_OFFSET_INDEX,
    STRUCT_PENDING_VALUE_INDEX,
    STRUCT_PENDING_VALUE_FLOAT,
    STRUCT_PENDING_VALUE_FLOAT_INDEX,
    STRUCT_PENDING_VALUE_BOOL,
    STRUCT_PENDING_VALUE_LENGTH,
    STRUCT_PENDING_RELEASE_BUFFER,
#if PY_VERSION_HEX >= 0x030e0000
    STRUCT_PENDING_VALUE_COMPLEX,
    STRUCT_PENDING_VALUE_COMPLEX_FLOAT,
    STRUCT_PENDING_VALUE_COMPLEX_INDEX,
#endif
} StructPending;

typedef enum {
    STRUCT_STAGE_BUFFER,
    STRUCT_STAGE_OFFSET,
    STRUCT_STAGE_VALUES,
    STRUCT_STAGE_CALL,
} StructStage;

typedef struct {
    PyObject *function;
    PyObject *receiver;
    PyObject *args;
    PyObject *converted;
    PyObject *kinds;
    PyObject *validation_formats;
    PyObject *release_owner;
    PyObject *release_view;
    PyObject *completion_result;
    PyObject *completion_exception;
    Py_ssize_t buffer_index;
    Py_ssize_t offset_index;
    Py_ssize_t value_start;
    Py_ssize_t value_index;
    Py_ssize_t pending_index;
    Py_ssize_t struct_size;
    StructOperation operation;
    StructStage stage;
    StructPending pending;
    int target_validated;
} StructState;

static const char *const operation_names[STRUCT_OPERATION_COUNT] = {
    "pack", "pack_into", "unpack", "unpack_from", "iter_unpack",
};
static PyObject *struct_module;
static PyObject *struct_error;
static PyTypeObject *struct_type;
static PyObject *original_module[STRUCT_OPERATION_COUNT];
static PyObject *original_methods[STRUCT_OPERATION_COUNT];
static PyMethodDef module_methods[STRUCT_OPERATION_COUNT];
static PyMethodDef type_methods[STRUCT_OPERATION_COUNT];
static int struct_installed;

static int struct_buffer_proxy_getbuffer(PyObject *, Py_buffer *, int);
static void struct_buffer_proxy_releasebuffer(PyObject *, Py_buffer *);
static PyBufferProcs struct_buffer_proxy_as_buffer = {
    .bf_getbuffer = struct_buffer_proxy_getbuffer,
    .bf_releasebuffer = struct_buffer_proxy_releasebuffer,
};

static void
struct_buffer_proxy_dealloc(PyObject *object)
{
    StructBufferProxy *proxy = (StructBufferProxy *)object;
    Py_XDECREF(proxy->owner);
    Py_XDECREF(proxy->view);
    Py_TYPE(object)->tp_free(object);
}

static PyTypeObject StructBufferProxyType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "aleff._struct_buffer_proxy",
    .tp_basicsize = sizeof(StructBufferProxy),
    .tp_dealloc = struct_buffer_proxy_dealloc,
    .tp_as_buffer = &struct_buffer_proxy_as_buffer,
    .tp_flags = Py_TPFLAGS_DEFAULT,
};

static PyObject *struct_resume(const void *, PyObject *);
static PyObject *call_raw_special_onearg(PyObject *, const char *, PyObject *);

static int
struct_buffer_proxy_getbuffer(PyObject *object, Py_buffer *view, int flags)
{
    StructBufferProxy *proxy = (StructBufferProxy *)object;
    StructBufferLease *lease = PyMem_Malloc(sizeof(*lease));
    if (lease == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    if (PyObject_GetBuffer(proxy->view, &lease->inner, flags) < 0) {
        PyMem_Free(lease);
        return -1;
    }
    *view = lease->inner;
    view->obj = Py_NewRef(object);
    view->internal = lease;
    return 0;
}

static void
struct_buffer_proxy_releasebuffer(PyObject *object, Py_buffer *view)
{
    StructBufferProxy *proxy = (StructBufferProxy *)object;
    PyObject *raised = PyErr_GetRaisedException();
    StructBufferLease *lease = view->internal;
    if (lease != NULL) {
        PyBuffer_Release(&lease->inner);
        PyMem_Free(lease);
        view->internal = NULL;
    }
    proxy->release_requested = 1;
    PyErr_SetRaisedException(raised);
}

static PyObject *
struct_buffer_proxy_new(PyObject *owner, PyObject *view)
{
    StructBufferProxy *proxy = PyObject_New(
        StructBufferProxy,
        &StructBufferProxyType
    );
    if (proxy == NULL) {
        return NULL;
    }
    proxy->owner = Py_NewRef(owner);
    proxy->view = Py_NewRef(view);
    proxy->release_requested = 0;
    proxy->release_started = 0;
    return (PyObject *)proxy;
}

static void *
struct_copy_state(const void *raw_state)
{
    const StructState *source = raw_state;
    StructState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->function = Py_NewRef(source->function);
    copy->receiver = Py_XNewRef(source->receiver);
    copy->args = Py_NewRef(source->args);
    copy->converted = PyList_GetSlice(source->converted, 0, PyList_GET_SIZE(source->converted));
    copy->kinds = Py_NewRef(source->kinds);
    copy->validation_formats = Py_NewRef(source->validation_formats);
    copy->release_owner = Py_XNewRef(source->release_owner);
    copy->release_view = Py_XNewRef(source->release_view);
    copy->completion_result = Py_XNewRef(source->completion_result);
    copy->completion_exception = Py_XNewRef(source->completion_exception);
    if (copy->converted == NULL) {
        Py_DECREF(copy->function);
        Py_XDECREF(copy->receiver);
        Py_DECREF(copy->args);
        Py_DECREF(copy->kinds);
        Py_DECREF(copy->validation_formats);
        Py_XDECREF(copy->release_owner);
        Py_XDECREF(copy->release_view);
        Py_XDECREF(copy->completion_result);
        Py_XDECREF(copy->completion_exception);
        PyMem_Free(copy);
        return NULL;
    }
    return copy;
}

static void
struct_free_state(void *raw_state)
{
    StructState *state = raw_state;
    if (state == NULL) return;
    PyObject *raised = PyErr_GetRaisedException();
    Py_XDECREF(state->function);
    Py_XDECREF(state->receiver);
    Py_XDECREF(state->args);
    Py_XDECREF(state->converted);
    Py_XDECREF(state->kinds);
    Py_XDECREF(state->validation_formats);
    Py_XDECREF(state->release_owner);
    Py_XDECREF(state->release_view);
    Py_XDECREF(state->completion_result);
    Py_XDECREF(state->completion_exception);
    PyMem_Free(state);
    PyErr_SetRaisedException(raised);
}

static const AleffAdapterVTable struct_vtable = {
    .copy_state = struct_copy_state,
    .free_state = struct_free_state,
    .resume = struct_resume,
    .prepare_resume = NULL,
};

static int
has_raw_special(PyObject *object, const char *name)
{
    PyObject *descriptor = lookup_raw_special(object, name);
    if (descriptor == NULL) return 0;
    Py_DECREF(descriptor);
    return 1;
}

static int
has_python_buffer_special(PyObject *object)
{
    PyObject *descriptor = lookup_raw_special(object, "__buffer__");
    if (descriptor == NULL) return 0;
    int result = PyFunction_Check(descriptor);
    Py_DECREF(descriptor);
    return result;
}

static PyObject *
call_raw_special_onearg(PyObject *object, const char *name, PyObject *argument)
{
    PyObject *descriptor = lookup_raw_special(object, name);
    if (descriptor == NULL) return NULL;
    descrgetfunc get = Py_TYPE(descriptor)->tp_descr_get;
    PyObject *callable = get == NULL
        ? Py_NewRef(descriptor)
        : get(descriptor, object, (PyObject *)Py_TYPE(object));
    Py_DECREF(descriptor);
    if (callable == NULL) return NULL;
    PyObject *result = PyObject_CallOneArg(callable, argument);
    Py_DECREF(callable);
    return result;
}

static int
append_schema(PyObject *kinds, PyObject *formats, StructValueKind kind,
              Py_ssize_t repeat, char prefix, Py_ssize_t field_repeat,
              char code)
{
    for (Py_ssize_t index = 0; index < repeat; index++) {
        PyObject *item = PyLong_FromLong((long)kind);
        if (item == NULL) return -1;
        int status = PyList_Append(kinds, item);
        Py_DECREF(item);
        if (status < 0) return -1;
        PyObject *format = (code == 's' || code == 'p')
            ? PyUnicode_FromFormat("%c%zd%c", prefix, field_repeat, code)
            : PyUnicode_FromFormat("%c%c", prefix, code);
        if (format == NULL) return -1;
        status = PyList_Append(formats, format);
        Py_DECREF(format);
        if (status < 0) return -1;
    }
    return 0;
}

static PyObject *
build_kinds(PyObject *format, PyObject **validation_formats)
{
    Py_ssize_t length;
    const char *text;
    if (PyUnicode_Check(format)) {
        text = PyUnicode_AsUTF8AndSize(format, &length);
        if (text == NULL) return NULL;
    }
    else if (PyBytes_Check(format)) {
        text = PyBytes_AS_STRING(format);
        length = PyBytes_GET_SIZE(format);
    }
    else return NULL;

    PyObject *kinds = PyList_New(0);
    if (kinds == NULL) return NULL;
    PyObject *formats = PyList_New(0);
    if (formats == NULL) {
        Py_DECREF(kinds);
        return NULL;
    }
    Py_ssize_t index = 0;
    int at_start = 1;
    char prefix = '@';
    while (index < length) {
        unsigned char current = (unsigned char)text[index];
        if (Py_ISSPACE(current)) {
            index++;
            continue;
        }
        if (current == '@' || current == '=' || current == '<' || current == '>' || current == '!') {
            if (!at_start) goto unsupported;
            prefix = (char)current;
            index++;
            at_start = 0;
            continue;
        }
        Py_ssize_t repeat = 0;
        int has_repeat = 0;
        while (index < length && text[index] >= '0' && text[index] <= '9') {
            int digit = text[index++] - '0';
            has_repeat = 1;
            if (repeat > (PY_SSIZE_T_MAX - digit) / 10) goto unsupported;
            repeat = repeat * 10 + digit;
        }
        if (index >= length) goto unsupported;
        if (!has_repeat) repeat = 1;
        unsigned char code = (unsigned char)text[index++];
        at_start = 0;
        StructValueKind kind;
        switch (code) {
            case 'b': case 'B': case 'h': case 'H':
            case 'i': case 'I': case 'l': case 'L':
            case 'q': case 'Q': case 'n': case 'N': case 'P':
                kind = STRUCT_VALUE_INDEX;
                break;
            case 'e': case 'f': case 'd':
                kind = STRUCT_VALUE_FLOAT;
                break;
            case '?':
                kind = STRUCT_VALUE_BOOL;
                break;
#if PY_VERSION_HEX >= 0x030e0000
            case 'F': case 'D':
                kind = STRUCT_VALUE_COMPLEX;
                break;
#endif
            case 'x':
                continue;
            case 's': case 'p':
                kind = STRUCT_VALUE_BYTES;
                if (append_schema(
                        kinds, formats, kind, 1, prefix, repeat, (char)code
                    ) < 0) {
                    goto error;
                }
                continue;
            case 'c':
                kind = STRUCT_VALUE_CHAR;
                break;
            default:
                goto unsupported;
        }
        if (append_schema(
                kinds, formats, kind, repeat, prefix, 1, (char)code
            ) < 0) goto error;
    }
    *validation_formats = formats;
    return kinds;

unsupported:
    Py_DECREF(kinds);
    Py_DECREF(formats);
    return NULL;
error:
    Py_DECREF(kinds);
    Py_DECREF(formats);
    return NULL;
}

static PyObject *
validate_index(PyObject *value)
{
    if (!PyLong_Check(value)) {
        PyErr_Format(PyExc_TypeError, "__index__ returned non-int (type %.200s)", Py_TYPE(value)->tp_name);
        return NULL;
    }
    return PyNumber_Index(value);
}

static PyObject *
validate_float(PyObject *value)
{
    if (!PyFloat_Check(value)) {
        PyErr_SetString(struct_error, "required argument is not a float");
        return NULL;
    }
    if (PyFloat_CheckExact(value)) return Py_NewRef(value);
    if (PyErr_WarnFormat(
            PyExc_DeprecationWarning, 1,
            "__float__ returned non-float (type %.200s).  The ability to return an "
            "instance of a strict subclass of float is deprecated, and may be "
            "removed in a future version of Python.", Py_TYPE(value)->tp_name)) return NULL;
    return PyFloat_FromDouble(PyFloat_AS_DOUBLE(value));
}

#if PY_VERSION_HEX >= 0x030e0000
static PyObject *
validate_complex(PyObject *value)
{
    if (!PyComplex_Check(value)) {
        PyErr_SetString(struct_error, "required argument is not a complex");
        return NULL;
    }
    Py_complex number = PyComplex_AsCComplex(value);
    if (PyErr_Occurred()) return NULL;
    return PyComplex_FromCComplex(number);
}
#endif

static PyObject *
validate_resumed(PyObject *value, StructPending pending)
{
    if (value == NULL) {
        if (pending == STRUCT_PENDING_VALUE_FLOAT ||
            pending == STRUCT_PENDING_VALUE_FLOAT_INDEX) {
            PyErr_Clear();
            PyErr_SetString(struct_error, "required argument is not a float");
        }
#if PY_VERSION_HEX >= 0x030e0000
        else if (pending == STRUCT_PENDING_VALUE_COMPLEX ||
                 pending == STRUCT_PENDING_VALUE_COMPLEX_FLOAT ||
                 pending == STRUCT_PENDING_VALUE_COMPLEX_INDEX) {
            PyErr_Clear();
            PyErr_SetString(struct_error, "required argument is not a complex");
        }
#endif
        return NULL;
    }
    switch (pending) {
        case STRUCT_PENDING_READ_BUFFER:
        case STRUCT_PENDING_WRITE_BUFFER: {
            if (!PyMemoryView_Check(value)) {
                PyErr_SetString(PyExc_TypeError, "__buffer__ returned non-memoryview object");
                return NULL;
            }
            if (pending == STRUCT_PENDING_WRITE_BUFFER &&
                PyMemoryView_GET_BUFFER(value)->readonly) {
                PyErr_SetString(PyExc_BufferError, "Object is not writable.");
                return NULL;
            }
            return Py_NewRef(value);
        }
        case STRUCT_PENDING_OFFSET_INDEX:
        case STRUCT_PENDING_VALUE_INDEX:
            return validate_index(value);
        case STRUCT_PENDING_VALUE_FLOAT:
            return validate_float(value);
        case STRUCT_PENDING_VALUE_FLOAT_INDEX: {
            PyObject *index = validate_index(value);
            if (index == NULL) {
                PyErr_Clear();
                PyErr_SetString(struct_error, "required argument is not a float");
                return NULL;
            }
            PyObject *result = PyNumber_Float(index);
            Py_DECREF(index);
            return result;
        }
        case STRUCT_PENDING_VALUE_BOOL:
            if (!PyBool_Check(value)) {
                PyErr_Format(PyExc_TypeError, "__bool__ should return bool, returned %.200s", Py_TYPE(value)->tp_name);
                return NULL;
            }
            return Py_NewRef(value);
        case STRUCT_PENDING_VALUE_LENGTH: {
            Py_ssize_t length = PyNumber_AsSsize_t(value, PyExc_OverflowError);
            if (length < 0) {
                if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, "__len__() should return >= 0");
                return NULL;
            }
            return PyBool_FromLong(length != 0);
        }
        case STRUCT_PENDING_RELEASE_BUFFER:
            break;
#if PY_VERSION_HEX >= 0x030e0000
        case STRUCT_PENDING_VALUE_COMPLEX:
            return validate_complex(value);
        case STRUCT_PENDING_VALUE_COMPLEX_FLOAT: {
            PyObject *real = validate_float(value);
            if (real == NULL) {
                PyErr_Clear();
                PyErr_SetString(struct_error, "required argument is not a complex");
                return NULL;
            }
            PyObject *result = PyComplex_FromDoubles(PyFloat_AS_DOUBLE(real), 0.0);
            Py_DECREF(real);
            return result;
        }
        case STRUCT_PENDING_VALUE_COMPLEX_INDEX: {
            PyObject *index = validate_index(value);
            if (index == NULL) {
                PyErr_Clear();
                PyErr_SetString(struct_error, "required argument is not a complex");
                return NULL;
            }
            double real = PyLong_AsDouble(index);
            Py_DECREF(index);
            if (real == -1.0 && PyErr_Occurred()) return NULL;
            return PyComplex_FromDoubles(real, 0.0);
        }
#endif
        case STRUCT_PENDING_NONE:
            break;
    }
    PyErr_SetString(PyExc_RuntimeError, "invalid struct resume role");
    return NULL;
}

static PyObject *
normalize_python_buffer(StructState *state, PyObject *owner, PyObject *view)
{
    (void)state;
    return struct_buffer_proxy_new(owner, view);
}

static PyObject *
convert_buffer(StructState *state, PyObject *value, int writable)
{
    if (!has_python_buffer_special(value)) {
        PyObject *memoryview = PyMemoryView_FromObject(value);
        if (memoryview == NULL) return NULL;
        if (writable && PyMemoryView_GET_BUFFER(memoryview)->readonly) {
            Py_DECREF(memoryview);
            PyErr_Format(
                PyExc_TypeError,
                "argument must be read-write bytes-like object, not %.200s",
                Py_TYPE(value)->tp_name
            );
            return NULL;
        }
        return memoryview;
    }
    PyObject *flags = PyLong_FromLong(writable ? PyBUF_WRITABLE : PyBUF_SIMPLE);
    if (flags == NULL) return NULL;
    state->pending = writable ? STRUCT_PENDING_WRITE_BUFFER : STRUCT_PENDING_READ_BUFFER;
    PyObject *result = call_raw_special_onearg(value, "__buffer__", flags);
    Py_DECREF(flags);
    if (result == NULL) return NULL;
    PyObject *converted = validate_resumed(result, state->pending);
    Py_DECREF(result);
    if (converted == NULL) return NULL;
    PyObject *memoryview = normalize_python_buffer(state, value, converted);
    Py_DECREF(converted);
    return memoryview;
}

static PyObject *
convert_index(StructState *state, PyObject *value, StructPending pending)
{
    if (PyLong_Check(value) || !has_raw_special(value, "__index__")) return Py_NewRef(value);
    state->pending = pending;
    return PyNumber_Index(value);
}

static PyObject *
convert_value(StructState *state, PyObject *value, StructValueKind kind)
{
    switch (kind) {
        case STRUCT_VALUE_BYTES:
        case STRUCT_VALUE_CHAR:
            return Py_NewRef(value);
        case STRUCT_VALUE_INDEX:
            return convert_index(state, value, STRUCT_PENDING_VALUE_INDEX);
        case STRUCT_VALUE_FLOAT:
            if (PyFloat_Check(value)) return Py_NewRef(value);
            if (has_raw_special(value, "__float__")) state->pending = STRUCT_PENDING_VALUE_FLOAT;
            else if (has_raw_special(value, "__index__")) state->pending = STRUCT_PENDING_VALUE_FLOAT_INDEX;
            else return Py_NewRef(value);
            {
                PyObject *converted = PyNumber_Float(value);
                if (converted == NULL) {
                    PyErr_Clear();
                    PyErr_SetString(
                        struct_error,
                        "required argument is not a float"
                    );
                }
                return converted;
            }
        case STRUCT_VALUE_BOOL: {
            if (PyBool_Check(value)) return Py_NewRef(value);
            if (has_raw_special(value, "__bool__")) state->pending = STRUCT_PENDING_VALUE_BOOL;
            else if (has_raw_special(value, "__len__")) state->pending = STRUCT_PENDING_VALUE_LENGTH;
            else return Py_NewRef(value);
            int truth = PyObject_IsTrue(value);
            return truth < 0 ? NULL : PyBool_FromLong(truth);
        }
#if PY_VERSION_HEX >= 0x030e0000
        case STRUCT_VALUE_COMPLEX:
            if (PyComplex_Check(value) || PyFloat_Check(value)) return Py_NewRef(value);
            if (has_raw_special(value, "__complex__")) state->pending = STRUCT_PENDING_VALUE_COMPLEX;
            else if (has_raw_special(value, "__float__")) state->pending = STRUCT_PENDING_VALUE_COMPLEX_FLOAT;
            else if (has_raw_special(value, "__index__")) state->pending = STRUCT_PENDING_VALUE_COMPLEX_INDEX;
            else return Py_NewRef(value);
            {
                PyObject *converted;
                if (state->pending == STRUCT_PENDING_VALUE_COMPLEX) {
                    converted = PyObject_CallOneArg(
                        (PyObject *)&PyComplex_Type,
                        value
                    );
                }
                else if (state->pending == STRUCT_PENDING_VALUE_COMPLEX_FLOAT) {
                    converted = PyNumber_Float(value);
                }
                else {
                    converted = PyNumber_Index(value);
                }
                if (converted == NULL) {
                    return validate_resumed(NULL, state->pending);
                }
                PyObject *result = validate_resumed(converted, state->pending);
                Py_DECREF(converted);
                return result;
            }
#endif
    }
    PyErr_SetString(PyExc_RuntimeError, "invalid struct value kind");
    return NULL;
}

static int
replace_argument(StructState *state, Py_ssize_t index, PyObject *value)
{
    if (PyList_SetItem(state->converted, index, value) < 0) {
        return -1;
    }
    return 0;
}

static PyObject *
call_with_receiver(PyObject *function, PyObject *receiver, PyObject *args)
{
    if (receiver == NULL) return PyObject_Call(function, args, NULL);
    Py_ssize_t count = PyTuple_GET_SIZE(args);
    PyObject *call_args = PyTuple_New(count + 1);
    if (call_args == NULL) return NULL;
    PyTuple_SET_ITEM(call_args, 0, Py_NewRef(receiver));
    for (Py_ssize_t index = 0; index < count; index++)
        PyTuple_SET_ITEM(call_args, index + 1, Py_NewRef(PyTuple_GET_ITEM(args, index)));
    PyObject *result = PyObject_Call(function, call_args, NULL);
    Py_DECREF(call_args);
    return result;
}

static PyObject *
call_original(StructState *state)
{
    PyObject *args = PyList_AsTuple(state->converted);
    if (args == NULL) return NULL;
    PyObject *result = call_with_receiver(state->function, state->receiver, args);
    Py_DECREF(args);
    return result;
}

static PyObject *
struct_finish_operation_release(StructState *state, PyObject *value)
{
    if (value == NULL && PyErr_Occurred()) {
        PyErr_WriteUnraisable(state->release_owner);
    }
    if (state->completion_exception != NULL) {
        PyErr_SetRaisedException(Py_NewRef(state->completion_exception));
        return NULL;
    }
    return Py_XNewRef(state->completion_result);
}

static PyObject *
struct_invoke_operation_release(
    StructState *state,
    StructBufferProxy *proxy,
    PyObject *result,
    PyObject *completion_exception
)
{
    proxy->release_started = 1;
    state->pending = STRUCT_PENDING_RELEASE_BUFFER;
    Py_XSETREF(state->release_owner, Py_NewRef(proxy->owner));
    Py_XSETREF(state->release_view, Py_NewRef(proxy->view));
    Py_XSETREF(state->completion_result, result);
    Py_XSETREF(state->completion_exception, completion_exception);
    PyObject *descriptor = lookup_raw_special(
        state->release_owner,
        "__release_buffer__"
    );
    if (descriptor == NULL) {
        PyErr_Clear();
        return struct_finish_operation_release(state, Py_None);
    }
    Py_DECREF(descriptor);
    PyObject *released = call_raw_special_onearg(
        state->release_owner,
        "__release_buffer__",
        state->release_view
    );
    PyObject *completed = struct_finish_operation_release(state, released);
    Py_XDECREF(released);
    return completed;
}

static PyObject *
struct_call_original_and_release(StructState *state)
{
    PyObject *result = call_original(state);
    PyObject *completion_exception = result == NULL
        ? PyErr_GetRaisedException()
        : NULL;
    StructBufferProxy *proxy = NULL;
    if (state->buffer_index >= 0) {
        PyObject *buffer = PyList_GET_ITEM(
            state->converted,
            state->buffer_index
        );
        if (Py_IS_TYPE(buffer, &StructBufferProxyType)) {
            proxy = (StructBufferProxy *)buffer;
        }
    }
    if (proxy != NULL && proxy->release_requested && !proxy->release_started) {
        return struct_invoke_operation_release(
            state,
            proxy,
            result,
            completion_exception
        );
    }
    if (completion_exception != NULL) {
        PyErr_SetRaisedException(completion_exception);
        return NULL;
    }
    return result;
}

static int
validate_field(StructState *state, Py_ssize_t value_index)
{
    PyObject *format = PyList_GET_ITEM(
        state->validation_formats,
        value_index
    );
    PyObject *value = PyList_GET_ITEM(
        state->converted,
        state->value_start + value_index
    );
    PyObject *args = PyTuple_Pack(2, format, value);
    if (args == NULL) return -1;
    PyObject *result = PyObject_Call(original_module[STRUCT_PACK], args, NULL);
    Py_DECREF(args);
    if (result == NULL) return -1;
    Py_DECREF(result);
    return 0;
}

static int
validate_pack_into_target(StructState *state)
{
    if (state->target_validated) return 0;
    PyObject *buffer_object = PyList_GET_ITEM(
        state->converted,
        state->buffer_index
    );
    Py_buffer *buffer;
    if (PyMemoryView_Check(buffer_object)) {
        buffer = PyMemoryView_GET_BUFFER(buffer_object);
    }
    else if (PyObject_TypeCheck(buffer_object, &StructBufferProxyType)) {
        buffer = PyMemoryView_GET_BUFFER(
            ((StructBufferProxy *)buffer_object)->view
        );
    }
    else {
        PyErr_SetString(PyExc_RuntimeError, "invalid struct buffer state");
        return -1;
    }
    Py_ssize_t offset = PyNumber_AsSsize_t(
        PyList_GET_ITEM(state->converted, state->offset_index),
        PyExc_IndexError
    );
    if (offset == -1 && PyErr_Occurred()) return -1;
    if (offset < 0) {
        if (offset + state->struct_size > 0) {
            PyErr_Format(
                struct_error,
                "no space to pack %zd bytes at offset %zd",
                state->struct_size,
                offset
            );
            return -1;
        }
        if (offset + buffer->len < 0) {
            PyErr_Format(
                struct_error,
                "offset %zd out of range for %zd-byte buffer",
                offset,
                buffer->len
            );
            return -1;
        }
        offset += buffer->len;
    }
    if (offset > buffer->len ||
        buffer->len - offset < state->struct_size) {
        PyErr_Format(
            struct_error,
            "pack_into requires a buffer of at least %zu bytes for packing "
            "%zd bytes at offset %zd (actual buffer size is %zd)",
            (size_t)state->struct_size + (size_t)offset,
            state->struct_size,
            offset,
            buffer->len
        );
        return -1;
    }
    PyObject *normalized = PyLong_FromSsize_t(offset);
    if (normalized == NULL) return -1;
    if (replace_argument(state, state->offset_index, normalized) < 0) return -1;
    state->target_validated = 1;
    return 0;
}

static PyObject *
struct_continue(StructState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed) {
        if (state->pending == STRUCT_PENDING_RELEASE_BUFFER) {
            return struct_finish_operation_release(state, resumed_value);
        }
        PyObject *buffer_owner = NULL;
        if (state->pending == STRUCT_PENDING_READ_BUFFER ||
            state->pending == STRUCT_PENDING_WRITE_BUFFER) {
            buffer_owner = PyList_GET_ITEM(
                state->converted,
                state->pending_index
            );
        }
        PyObject *converted = validate_resumed(resumed_value, state->pending);
        if (converted == NULL) return NULL;
        if (buffer_owner != NULL) {
            PyObject *memoryview = normalize_python_buffer(
                state,
                buffer_owner,
                converted
            );
            Py_DECREF(converted);
            if (memoryview == NULL) return NULL;
            converted = memoryview;
        }
        if (replace_argument(state, state->pending_index, converted) < 0) return NULL;
        if (state->stage == STRUCT_STAGE_BUFFER) state->stage = STRUCT_STAGE_OFFSET;
        else if (state->stage == STRUCT_STAGE_OFFSET) state->stage = STRUCT_STAGE_VALUES;
        else if (state->stage == STRUCT_STAGE_VALUES) {
            if (validate_field(state, state->value_index) < 0) return NULL;
            state->value_index++;
        }
        state->pending = STRUCT_PENDING_NONE;
    }
    while (1) {
        if (state->stage == STRUCT_STAGE_BUFFER) {
            if (state->buffer_index < 0) {
                state->stage = STRUCT_STAGE_OFFSET;
                continue;
            }
            state->pending_index = state->buffer_index;
            PyObject *converted = convert_buffer(
                state, PyList_GET_ITEM(state->converted, state->buffer_index),
                state->operation == STRUCT_PACK_INTO);
            if (converted == NULL) return NULL;
            if (replace_argument(state, state->buffer_index, converted) < 0) return NULL;
            state->stage = STRUCT_STAGE_OFFSET;
            state->pending = STRUCT_PENDING_NONE;
            continue;
        }
        if (state->stage == STRUCT_STAGE_OFFSET) {
            if (state->offset_index < 0) {
                state->stage = STRUCT_STAGE_VALUES;
                continue;
            }
            state->pending_index = state->offset_index;
            PyObject *converted = convert_index(
                state, PyList_GET_ITEM(state->converted, state->offset_index),
                STRUCT_PENDING_OFFSET_INDEX);
            if (converted == NULL) return NULL;
            if (replace_argument(state, state->offset_index, converted) < 0) return NULL;
            state->stage = STRUCT_STAGE_VALUES;
            state->pending = STRUCT_PENDING_NONE;
            continue;
        }
        if (state->stage == STRUCT_STAGE_VALUES) {
            if (state->operation == STRUCT_PACK_INTO &&
                validate_pack_into_target(state) < 0) return NULL;
            if (state->value_index >= PyList_GET_SIZE(state->kinds)) {
                state->stage = STRUCT_STAGE_CALL;
                continue;
            }
            long kind = PyLong_AsLong(PyList_GET_ITEM(state->kinds, state->value_index));
            if (kind == -1 && PyErr_Occurred()) return NULL;
            Py_ssize_t argument_index = state->value_start + state->value_index;
            PyObject *argument = PyList_GET_ITEM(state->converted, argument_index);
            state->pending_index = argument_index;
            PyObject *converted = convert_value(
                state, argument,
                (StructValueKind)kind);
            if (converted == NULL) return NULL;
            if (replace_argument(state, argument_index, converted) < 0) return NULL;
            if (validate_field(state, state->value_index) < 0) return NULL;
            state->value_index++;
            state->pending = STRUCT_PENDING_NONE;
            continue;
        }
        return struct_call_original_and_release(state);
    }
}

static PyObject *
struct_resume(const void *raw_state, PyObject *value)
{
    StructState *state = struct_copy_state(raw_state);
    if (state == NULL) return NULL;
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &struct_vtable, state) < 0) {
        struct_free_state(state);
        return NULL;
    }
    PyObject *result = struct_continue(state, value, 1);
    adapter_leave(&frame);
    struct_free_state(state);
    return result;
}

static int
argument_layout(StructOperation operation, int method, Py_ssize_t count,
                Py_ssize_t *buffer, Py_ssize_t *offset, Py_ssize_t *values)
{
    Py_ssize_t base = method ? 0 : 1;
    *buffer = -1;
    *offset = -1;
    *values = count;
    switch (operation) {
        case STRUCT_PACK:
            if (count < base) return -1;
            *values = base;
            return 0;
        case STRUCT_PACK_INTO:
            if (count < base + 2) return -1;
            *buffer = base;
            *offset = base + 1;
            *values = base + 2;
            return 0;
        case STRUCT_UNPACK:
        case STRUCT_ITER_UNPACK:
            if (count != base + 1) return -1;
            *buffer = base;
            return 0;
        case STRUCT_UNPACK_FROM:
            if (count != base + 1 && count != base + 2) return -1;
            *buffer = base;
            if (count == base + 2) *offset = base + 1;
            return 0;
        case STRUCT_OPERATION_COUNT:
            break;
    }
    return -1;
}

static PyObject *
struct_start(PyObject *function, PyObject *receiver, PyObject *args,
             PyObject *format, StructOperation operation)
{
    Py_ssize_t buffer_index, offset_index, value_start;
    if (argument_layout(operation, receiver != NULL, PyTuple_GET_SIZE(args),
                        &buffer_index, &offset_index, &value_start) < 0)
        return call_with_receiver(function, receiver, args);

    PyObject *normalized_format;
    Py_ssize_t struct_size;
    if (receiver == NULL) {
        PyObject *format_args = PyTuple_Pack(1, format);
        if (format_args == NULL) return NULL;
        PyObject *descriptor = PyObject_Call((PyObject *)struct_type, format_args, NULL);
        Py_DECREF(format_args);
        if (descriptor == NULL) return NULL;
        StructObjectPrefix *prefix = (StructObjectPrefix *)descriptor;
        struct_size = prefix->size;
        normalized_format = Py_NewRef(prefix->format);
        Py_DECREF(descriptor);
    }
    else {
        StructObjectPrefix *prefix = (StructObjectPrefix *)receiver;
        struct_size = prefix->size;
        normalized_format = Py_NewRef(prefix->format);
    }
    if (normalized_format == NULL) return NULL;
    PyObject *validation_formats = NULL;
    PyObject *kinds = operation == STRUCT_PACK || operation == STRUCT_PACK_INTO
        ? build_kinds(normalized_format, &validation_formats) : PyList_New(0);
    Py_DECREF(normalized_format);
    if (kinds == NULL) {
        if (PyErr_Occurred()) return NULL;
        return call_with_receiver(function, receiver, args);
    }
    if (validation_formats == NULL) {
        validation_formats = PyList_New(0);
        if (validation_formats == NULL) {
            Py_DECREF(kinds);
            return NULL;
        }
    }
    if ((operation == STRUCT_PACK || operation == STRUCT_PACK_INTO) &&
        PyTuple_GET_SIZE(args) - value_start != PyList_GET_SIZE(kinds)) {
        Py_DECREF(kinds);
        Py_DECREF(validation_formats);
        return call_with_receiver(function, receiver, args);
    }
    if (operation == STRUCT_ITER_UNPACK && struct_size == 0) {
        Py_DECREF(kinds);
        Py_DECREF(validation_formats);
        return call_with_receiver(function, receiver, args);
    }
    StructState state = {
        .function = function,
        .receiver = receiver,
        .args = args,
        .converted = PySequence_List(args),
        .kinds = kinds,
        .validation_formats = validation_formats,
        .buffer_index = buffer_index,
        .offset_index = offset_index,
        .value_start = value_start,
        .operation = operation,
        .stage = STRUCT_STAGE_BUFFER,
        .struct_size = struct_size,
    };
    if (state.converted == NULL) {
        Py_DECREF(kinds);
        Py_DECREF(validation_formats);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &struct_vtable, &state) < 0) {
        Py_DECREF(state.converted);
        Py_DECREF(kinds);
        Py_DECREF(validation_formats);
        return NULL;
    }
    PyObject *result = struct_continue(&state, NULL, 0);
    adapter_leave(&frame);
    PyObject *raised = PyErr_GetRaisedException();
    Py_DECREF(state.converted);
    Py_DECREF(kinds);
    Py_DECREF(validation_formats);
    Py_XDECREF(state.release_owner);
    Py_XDECREF(state.release_view);
    Py_XDECREF(state.completion_result);
    Py_XDECREF(state.completion_exception);
    PyErr_SetRaisedException(raised);
    return result;
}

static PyObject *
module_wrapper(PyObject *self, PyObject *args)
{
    if (!PyTuple_Check(self) || PyTuple_GET_SIZE(self) != 2) {
        PyErr_SetString(PyExc_RuntimeError, "invalid struct adapter state");
        return NULL;
    }
    long operation = PyLong_AsLong(PyTuple_GET_ITEM(self, 1));
    if (operation < 0 || operation >= STRUCT_OPERATION_COUNT) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_RuntimeError, "invalid struct operation");
        return NULL;
    }
    PyObject *function = PyTuple_GET_ITEM(self, 0);
    if (PyTuple_GET_SIZE(args) < 1) return PyObject_Call(function, args, NULL);
    return struct_start(function, NULL, args, PyTuple_GET_ITEM(args, 0), (StructOperation)operation);
}

static PyObject *
method_start(PyObject *self, PyObject *args, StructOperation operation)
{
    return struct_start(
        original_methods[operation],
        self,
        args,
        ((StructObjectPrefix *)self)->format,
        operation
    );
}

static PyObject *pack_method(PyObject *self, PyObject *args) { return method_start(self, args, STRUCT_PACK); }
static PyObject *pack_into_method(PyObject *self, PyObject *args) { return method_start(self, args, STRUCT_PACK_INTO); }
static PyObject *unpack_method(PyObject *self, PyObject *args) { return method_start(self, args, STRUCT_UNPACK); }
static PyObject *
unpack_from_method(PyObject *self, PyObject *args)
{
    return method_start(self, args, STRUCT_UNPACK_FROM);
}

static PyObject *
iter_unpack_method(PyObject *self, PyObject *args)
{
    return method_start(self, args, STRUCT_ITER_UNPACK);
}

static PyCFunction const method_wrappers[STRUCT_OPERATION_COUNT] = {
    (PyCFunction)pack_method,
    (PyCFunction)pack_into_method,
    (PyCFunction)unpack_method,
    (PyCFunction)unpack_from_method,
    (PyCFunction)iter_unpack_method,
};

static int
replace_module(StructOperation operation)
{
    PyObject *kind = PyLong_FromLong((long)operation);
    PyObject *tag = kind == NULL ? NULL : PyTuple_Pack(2, original_module[operation], kind);
    Py_XDECREF(kind);
    if (tag == NULL) return -1;
    PyObject *module_name = PyObject_GetAttrString(original_module[operation], "__module__");
    if (module_name == NULL) {
        Py_DECREF(tag);
        return -1;
    }
    PyMethodDef *method = &module_methods[operation];
    *method = (PyMethodDef){
        .ml_name = operation_names[operation],
        .ml_meth = (PyCFunction)module_wrapper,
        .ml_flags = METH_VARARGS,
        .ml_doc = PyCFunction_Check(original_module[operation])
            ? ((PyCFunctionObject *)original_module[operation])->m_ml->ml_doc : NULL,
    };
    PyObject *replacement = PyCFunction_NewEx(method, tag, module_name);
    Py_DECREF(module_name);
    Py_DECREF(tag);
    if (replacement == NULL) return -1;
    int status = aleff_adapter_register_callable(replacement);
    if (status == 0) {
        status = PyObject_SetAttrString(struct_module, operation_names[operation], replacement);
    }
    Py_DECREF(replacement);
    return status;
}

static int
replace_method(StructOperation operation)
{
    PyMethodDef *method = &type_methods[operation];
    *method = (PyMethodDef){
        .ml_name = operation_names[operation],
        .ml_meth = method_wrappers[operation],
        .ml_flags = METH_VARARGS,
        .ml_doc = ((PyMethodDescrObject *)original_methods[operation])->d_method->ml_doc,
    };
    PyObject *descriptor = PyDescr_NewMethod(struct_type, method);
    if (descriptor == NULL) return -1;
    PyObject *dict = PyType_GetDict(struct_type);
    int status = dict == NULL
        ? -1
        : aleff_adapter_register_callable(descriptor);
    if (status == 0) {
        status = PyDict_SetItemString(dict, operation_names[operation], descriptor);
    }
    Py_XDECREF(dict);
    Py_DECREF(descriptor);
    if (status == 0) PyType_Modified(struct_type);
    return status;
}

int
adapter_struct_install(PyObject *module)
{
    if (struct_installed) return 0;
    if (PyType_Ready(&StructBufferProxyType) < 0) return -1;
    struct_module = Py_NewRef(module);
    struct_error = PyObject_GetAttrString(module, "error");
    if (struct_error == NULL) {
        adapter_struct_rollback();
        return -1;
    }
    PyObject *type_object = PyObject_GetAttrString(module, "Struct");
    if (type_object == NULL || !PyType_Check(type_object)) {
        Py_XDECREF(type_object);
        PyErr_SetString(PyExc_RuntimeError, "cannot access struct.Struct");
        adapter_struct_rollback();
        return -1;
    }
    struct_type = (PyTypeObject *)type_object;
    PyObject *dict = PyType_GetDict(struct_type);
    if (dict == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "cannot access struct.Struct methods");
        adapter_struct_rollback();
        return -1;
    }
    for (int index = 0; index < STRUCT_OPERATION_COUNT; index++) {
        original_module[index] = PyObject_GetAttrString(module, operation_names[index]);
        original_methods[index] = Py_XNewRef(PyDict_GetItemString(dict, operation_names[index]));
        if (original_module[index] == NULL || original_methods[index] == NULL) {
            Py_DECREF(dict);
            if (!PyErr_Occurred()) PyErr_SetString(PyExc_RuntimeError, "cannot access struct APIs");
            adapter_struct_rollback();
            return -1;
        }
    }
    Py_DECREF(dict);
    for (int index = 0; index < STRUCT_OPERATION_COUNT; index++) {
        if (replace_module((StructOperation)index) < 0 || replace_method((StructOperation)index) < 0) {
            adapter_struct_rollback();
            return -1;
        }
    }
    struct_installed = 1;
    return 0;
}

void
adapter_struct_rollback(void)
{
    PyObject *error_type, *error_value, *error_traceback;
    PyErr_Fetch(&error_type, &error_value, &error_traceback);
    if (struct_module != NULL) {
        for (int index = 0; index < STRUCT_OPERATION_COUNT; index++) {
            if (original_module[index] != NULL &&
                PyObject_SetAttrString(struct_module, operation_names[index], original_module[index]) < 0)
                PyErr_Clear();
        }
    }
    if (struct_type != NULL) {
        PyObject *dict = PyType_GetDict(struct_type);
        if (dict != NULL) {
            for (int index = 0; index < STRUCT_OPERATION_COUNT; index++) {
                if (original_methods[index] != NULL &&
                    PyDict_SetItemString(dict, operation_names[index], original_methods[index]) < 0)
                    PyErr_Clear();
            }
            PyType_Modified(struct_type);
            Py_DECREF(dict);
        }
    }
    Py_CLEAR(struct_module);
    Py_CLEAR(struct_type);
    Py_CLEAR(struct_error);
    for (int index = 0; index < STRUCT_OPERATION_COUNT; index++) {
        Py_CLEAR(original_module[index]);
        Py_CLEAR(original_methods[index]);
    }
    struct_installed = 0;
    PyErr_Restore(error_type, error_value, error_traceback);
}
