#include "internal.h"
#include "containers.h"
#include "text.h"

typedef struct {
    PyObject *source;
    PyObject *encoding;
    PyObject *errors;
    PyObject *prefix;
    int kind;
} CodecState;

static const AleffAdapterVTable codec_vtable;

static void *
codec_copy_state(const void *raw_state)
{
    const CodecState *state = raw_state;
    CodecState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->source = Py_NewRef(state->source);
    copy->encoding = Py_XNewRef(state->encoding);
    copy->errors = Py_XNewRef(state->errors);
    copy->prefix = Py_NewRef(state->prefix);
    copy->kind = state->kind;
    return copy;
}

static void
codec_free_state(void *raw_state)
{
    CodecState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->source);
    Py_XDECREF(state->encoding);
    Py_XDECREF(state->errors);
    Py_DECREF(state->prefix);
    PyMem_Free(state);
}

static const char *
codec_name(PyObject *object)
{
    if (object == NULL) {
        return NULL;
    }
    return PyUnicode_AsUTF8(object);
}

static PyObject *
codec_concat(PyObject *left, PyObject *right, int encode)
{
    if (encode) {
        PyObject *result = Py_NewRef(left);
        PyBytes_Concat(&result, right);
        return result;
    }
    return PyUnicode_Concat(left, right);
}

static int
codec_error_bounds(
    CodecState *state,
    Py_ssize_t *start,
    Py_ssize_t *end
)
{
    const char *encoding = codec_name(state->encoding);
    if (state->encoding != NULL && encoding == NULL) {
        return -1;
    }
    PyObject *strict_result = state->kind == 0
        ? PyUnicode_AsEncodedString(state->source, encoding, "strict")
        : PyUnicode_Decode(
            PyBytes_AS_STRING(state->source),
            PyBytes_GET_SIZE(state->source),
            encoding,
            "strict"
        );
    if (strict_result != NULL) {
        Py_DECREF(strict_result);
        PyErr_SetString(PyExc_RuntimeError, "codec handler resumed without an encoding error");
        return -1;
    }
    PyObject *exception = PyErr_GetRaisedException();
    if (exception == NULL) {
        return -1;
    }
    int status;
    if (state->kind == 0) {
        status = PyUnicodeEncodeError_GetStart(exception, start);
        if (status == 0) {
            status = PyUnicodeEncodeError_GetEnd(exception, end);
        }
    }
    else {
        status = PyUnicodeDecodeError_GetStart(exception, start);
        if (status == 0) {
            status = PyUnicodeDecodeError_GetEnd(exception, end);
        }
    }
    Py_DECREF(exception);
    return status;
}

static int
codec_position(PyObject *position, Py_ssize_t length, Py_ssize_t *result)
{
    if (!PyLong_Check(position)) {
        PyErr_SetString(PyExc_TypeError, "position from error handler must be an integer");
        return -1;
    }
    Py_ssize_t value = PyLong_AsSsize_t(position);
    if (value == -1 && PyErr_Occurred()) {
        return -1;
    }
    Py_ssize_t original = value;
    if (value < 0) {
        value += length;
    }
    if (value < 0 || value > length) {
        PyErr_Format(
            PyExc_IndexError,
            "position %zd from error handler out of bounds",
            original
        );
        return -1;
    }
    *result = value;
    return 0;
}

static PyObject *
codec_convert_remaining(CodecState *state)
{
    const char *encoding = codec_name(state->encoding);
    if (state->encoding != NULL && encoding == NULL) {
        return NULL;
    }
    const char *errors = codec_name(state->errors);
    if (state->errors != NULL && errors == NULL) {
        return NULL;
    }
    PyObject *tail = state->kind == 0
        ? PyUnicode_AsEncodedString(state->source, encoding, errors)
        : PyUnicode_Decode(
            PyBytes_AS_STRING(state->source),
            PyBytes_GET_SIZE(state->source),
            encoding,
            errors
        );
    if (tail == NULL) {
        return NULL;
    }
    PyObject *result = codec_concat(state->prefix, tail, state->kind == 0);
    Py_DECREF(tail);
    return result;
}

static PyObject *
codec_continue(CodecState *state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    if (!PyTuple_Check(value) || PyTuple_GET_SIZE(value) != 2) {
        PyErr_SetString(PyExc_TypeError, "codec error handler must return (replacement, position)");
        return NULL;
    }
    PyObject *replacement = PyTuple_GET_ITEM(value, 0);
    PyObject *position = PyTuple_GET_ITEM(value, 1);
    if ((state->kind == 0 &&
            !PyUnicode_Check(replacement) && !PyBytes_Check(replacement)) ||
        (state->kind != 0 && !PyUnicode_Check(replacement))) {
        PyErr_SetString(
            PyExc_TypeError,
            "codec error handler returned an invalid replacement"
        );
        return NULL;
    }

    Py_ssize_t error_start;
    Py_ssize_t error_end;
    if (codec_error_bounds(state, &error_start, &error_end) < 0) {
        return NULL;
    }
    (void)error_end;
    const char *encoding = codec_name(state->encoding);
    if (state->encoding != NULL && encoding == NULL) {
        return NULL;
    }
    Py_ssize_t source_length = state->kind == 0
        ? PyUnicode_GET_LENGTH(state->source)
        : PyBytes_GET_SIZE(state->source);
    Py_ssize_t next_position;
    if (codec_position(position, source_length, &next_position) < 0) {
        return NULL;
    }

    PyObject *valid_prefix;
    PyObject *replacement_result;
    if (state->kind == 0) {
        PyObject *prefix_text = PyUnicode_Substring(
            state->source, 0, error_start
        );
        if (prefix_text == NULL) {
            return NULL;
        }
        valid_prefix = PyUnicode_AsEncodedString(
            prefix_text, encoding, "strict"
        );
        Py_DECREF(prefix_text);
        if (valid_prefix == NULL) {
            return NULL;
        }
        replacement_result = PyBytes_Check(replacement)
            ? Py_NewRef(replacement)
            : PyUnicode_AsEncodedString(replacement, encoding, "strict");
    }
    else {
        valid_prefix = PyUnicode_Decode(
            PyBytes_AS_STRING(state->source),
            error_start,
            encoding,
            "strict"
        );
        replacement_result = Py_NewRef(replacement);
    }
    if (replacement_result == NULL) {
        Py_DECREF(valid_prefix);
        return NULL;
    }
    PyObject *with_prefix = codec_concat(
        state->prefix, valid_prefix, state->kind == 0
    );
    Py_DECREF(valid_prefix);
    if (with_prefix == NULL) {
        Py_DECREF(replacement_result);
        return NULL;
    }
    PyObject *accumulated = codec_concat(
        with_prefix, replacement_result, state->kind == 0
    );
    Py_DECREF(with_prefix);
    Py_DECREF(replacement_result);
    if (accumulated == NULL) {
        return NULL;
    }

    PyObject *remaining = state->kind == 0
        ? PyUnicode_Substring(state->source, next_position, source_length)
        : PyBytes_FromStringAndSize(
            PyBytes_AS_STRING(state->source) + next_position,
            source_length - next_position
        );
    if (remaining == NULL) {
        Py_DECREF(accumulated);
        return NULL;
    }
    Py_SETREF(state->source, remaining);
    Py_SETREF(state->prefix, accumulated);
    return codec_convert_remaining(state);
}

static PyObject *
codec_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    CodecState *state = codec_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &codec_vtable, state) < 0) {
        codec_free_state(state);
        return NULL;
    }
    PyObject *result = codec_continue(state, value);
    adapter_leave(&frame);
    codec_free_state(state);
    return result;
}

static const AleffAdapterVTable codec_vtable = {
    .copy_state = codec_copy_state,
    .free_state = codec_free_state,
    .resume = codec_resume,
};

static PyObject *
adapter_str_encode(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *encoding_object = NULL;
    PyObject *errors_object = NULL;
    static char *keywords[] = {"encoding", "errors", NULL};
    if (!PyArg_ParseTupleAndKeywords(
        args,
        kwargs,
        "|OO:encode",
        keywords,
        &encoding_object,
        &errors_object
    )) {
        return NULL;
    }
    const char *encoding = codec_name(encoding_object);
    if (encoding_object != NULL && encoding == NULL) {
        return NULL;
    }
    const char *errors = codec_name(errors_object);
    if (errors_object != NULL && errors == NULL) {
        return NULL;
    }
    CodecState state = {
        .source = self,
        .encoding = encoding_object,
        .errors = errors_object,
        .prefix = PyBytes_FromStringAndSize("", 0),
        .kind = 0,
    };
    if (state.prefix == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &codec_vtable, &state) < 0) {
        Py_DECREF(state.prefix);
        return NULL;
    }
    PyObject *result = PyUnicode_AsEncodedString(
        self,
        encoding,
        errors
    );
    adapter_leave(&frame);
    Py_DECREF(state.prefix);
    return result;
}

static PyObject *
adapter_bytes_decode_common(
    PyObject *self,
    PyObject *args,
    PyObject *kwargs,
    int kind
)
{
    PyObject *encoding_object = NULL;
    PyObject *errors_object = NULL;
    static char *keywords[] = {"encoding", "errors", NULL};
    if (!PyArg_ParseTupleAndKeywords(
        args,
        kwargs,
        "|OO:decode",
        keywords,
        &encoding_object,
        &errors_object
    )) {
        return NULL;
    }
    const char *encoding = codec_name(encoding_object);
    if (encoding_object != NULL && encoding == NULL) {
        return NULL;
    }
    const char *errors = codec_name(errors_object);
    if (errors_object != NULL && errors == NULL) {
        return NULL;
    }
    PyObject *source = kind == 1
        ? PyBytes_FromObject(self)
        : PyBytes_FromStringAndSize(
            PyByteArray_AS_STRING(self),
            PyByteArray_GET_SIZE(self)
        );
    if (source == NULL) {
        return NULL;
    }
    CodecState state = {
        .source = source,
        .encoding = encoding_object,
        .errors = errors_object,
        .prefix = PyUnicode_New(0, 0),
        .kind = kind,
    };
    if (state.prefix == NULL) {
        Py_DECREF(source);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &codec_vtable, &state) < 0) {
        Py_DECREF(state.prefix);
        Py_DECREF(source);
        return NULL;
    }
    PyObject *result = PyUnicode_Decode(
        PyBytes_AS_STRING(source),
        PyBytes_GET_SIZE(source),
        encoding,
        errors
    );
    adapter_leave(&frame);
    Py_DECREF(state.prefix);
    Py_DECREF(source);
    return result;
}

static PyObject *adapter_bytes_decode(
    PyObject *self,
    PyObject *args,
    PyObject *kwargs
)
{
    return adapter_bytes_decode_common(self, args, kwargs, 1);
}

static PyObject *adapter_bytearray_decode(
    PyObject *self,
    PyObject *args,
    PyObject *kwargs
)
{
    return adapter_bytes_decode_common(self, args, kwargs, 2);
}

typedef enum {
    BYTES_WAIT_BYTES,
    BYTES_WAIT_INDEX,
    BYTES_WAIT_BUFFER_ACQUIRE,
    BYTES_WAIT_BUFFER_RELEASE,
} BytesPhase;

typedef struct {
    BytesPhase phase;
    int make_bytearray;
    PyObject *input;
    PyObject *view;
    PyObject *result;
} BytesState;

static void *
bytes_copy_state(const void *raw_state)
{
    const BytesState *state = raw_state;
    BytesState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->phase = state->phase;
    copy->make_bytearray = state->make_bytearray;
    copy->input = Py_XNewRef(state->input);
    copy->view = Py_XNewRef(state->view);
    copy->result = Py_XNewRef(state->result);
    return copy;
}

static void
bytes_free_state(void *raw_state)
{
    BytesState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->input);
    Py_XDECREF(state->view);
    Py_XDECREF(state->result);
    PyMem_Free(state);
}

PyObject *
lookup_raw_special(PyObject *object, const char *name)
{
    PyObject *mro = Py_TYPE(object)->tp_mro;
    if (mro == NULL || !PyTuple_Check(mro)) {
        return NULL;
    }
    Py_ssize_t size = PyTuple_GET_SIZE(mro);
    for (Py_ssize_t index = 0; index < size; index++) {
        PyObject *base = PyTuple_GET_ITEM(mro, index);
        if (!PyType_Check(base)) {
            continue;
        }
        PyObject *dictionary = PyType_GetDict((PyTypeObject *)base);
        if (dictionary == NULL) {
            return NULL;
        }
        PyObject *descriptor = Py_XNewRef(
            PyDict_GetItemString(dictionary, name)
        );
        Py_DECREF(dictionary);
        if (descriptor != NULL) {
            return descriptor;
        }
    }
    return NULL;
}

static PyObject *
bytes_from_index_result(PyObject *value, int make_bytearray)
{
    PyObject *index = PyNumber_Index(value);
    if (index == NULL) {
        return NULL;
    }
    PyObject *args = PyTuple_Pack(1, index);
    Py_DECREF(index);
    if (args == NULL) {
        return NULL;
    }
    PyObject *result;
    if (make_bytearray) {
        result = PyByteArray_FromStringAndSize(NULL, 0);
        if (result != NULL && original_bytearray_init(result, args, NULL) < 0) {
            Py_CLEAR(result);
        }
    }
    else {
        result = original_bytes_new(&PyBytes_Type, args, NULL);
    }
    Py_DECREF(args);
    return result;
}

static PyObject *
bytearray_copy_buffer(PyObject *source)
{
    Py_buffer view;
    if (PyObject_GetBuffer(source, &view, PyBUF_FULL_RO) < 0) {
        return NULL;
    }
    PyObject *result = PyByteArray_FromStringAndSize(NULL, view.len);
    if (
        result != NULL &&
        PyBuffer_ToContiguous(PyByteArray_AS_STRING(result), &view, view.len, 'C') < 0
    ) {
        Py_CLEAR(result);
    }
    PyBuffer_Release(&view);
    return result;
}

static int
bytearray_replace_buffer(PyObject *target, PyObject *source)
{
    Py_buffer view;
    if (PyObject_GetBuffer(source, &view, PyBUF_FULL_RO) < 0) {
        return -1;
    }
    int status = PyByteArray_Resize(target, view.len);
    if (
        status == 0 &&
        PyBuffer_ToContiguous(PyByteArray_AS_STRING(target), &view, view.len, 'C') < 0
    ) {
        status = -1;
    }
    PyBuffer_Release(&view);
    return status;
}

static PyObject *
call_raw_special_onearg(PyObject *object, const char *name, PyObject *argument)
{
    PyObject *descriptor = lookup_raw_special(object, name);
    if (descriptor == NULL) {
        return NULL;
    }
    descrgetfunc get = Py_TYPE(descriptor)->tp_descr_get;
    PyObject *callable = get == NULL
        ? Py_NewRef(descriptor)
        : get(descriptor, object, (PyObject *)Py_TYPE(object));
    Py_DECREF(descriptor);
    if (callable == NULL) {
        return NULL;
    }
    PyObject *result = PyObject_CallOneArg(callable, argument);
    Py_DECREF(callable);
    return result;
}

static PyObject *
bytes_buffer_continue(BytesState *state, PyObject *value)
{
    if (!PyMemoryView_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "__buffer__ returned non-memoryview object");
        return NULL;
    }
    Py_XSETREF(state->view, Py_NewRef(value));
    PyObject *converted = state->make_bytearray
        ? bytearray_copy_buffer(state->view)
        : PyBytes_FromObject(state->view);
    if (converted == NULL) {
        return NULL;
    }
    Py_XSETREF(state->result, converted);

    PyObject *release_descriptor = lookup_raw_special(state->input, "__release_buffer__");
    if (release_descriptor == NULL) {
        return Py_NewRef(state->result);
    }
    Py_DECREF(release_descriptor);

    state->phase = BYTES_WAIT_BUFFER_RELEASE;
    PyObject *released = call_raw_special_onearg(
        state->input, "__release_buffer__", state->view
    );
    if (released == NULL) {
        PyErr_WriteUnraisable(state->input);
    }
    else {
        Py_DECREF(released);
    }
    return Py_NewRef(state->result);
}

static const AleffAdapterVTable bytes_vtable;

static PyObject *
bytes_resume(const void *raw_state, PyObject *value)
{
    const BytesState *state = raw_state;
    if (value == NULL && state->phase != BYTES_WAIT_BUFFER_RELEASE) {
        return NULL;
    }
    switch (state->phase) {
        case BYTES_WAIT_BYTES:
            if (!PyBytes_Check(value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "__bytes__ returned non-bytes (type %.200s)",
                    Py_TYPE(value)->tp_name
                );
                return NULL;
            }
            return Py_NewRef(value);
        case BYTES_WAIT_INDEX:
            return bytes_from_index_result(value, state->make_bytearray);
        case BYTES_WAIT_BUFFER_ACQUIRE: {
            BytesState *copy = bytes_copy_state(state);
            if (copy == NULL) {
                return NULL;
            }
            AleffAdapterFrame frame;
            if (adapter_enter(&frame, &bytes_vtable, copy) < 0) {
                return NULL;
            }
            PyObject *result = bytes_buffer_continue(copy, value);
            adapter_leave(&frame);
            bytes_free_state(copy);
            return result;
        }
        case BYTES_WAIT_BUFFER_RELEASE:
            if (value == NULL && PyErr_Occurred()) {
                PyErr_WriteUnraisable(state->input);
            }
            return Py_NewRef(state->result);
    }
    PyErr_SetString(PyExc_RuntimeError, "unknown bytes conversion phase");
    return NULL;
}

static const AleffAdapterVTable bytes_vtable = {
    .copy_state = bytes_copy_state,
    .free_state = bytes_free_state,
    .resume = bytes_resume,
};

static PyObject *
convert_python_buffer(PyObject *input, int make_bytearray)
{
    BytesState state = {
        .phase = BYTES_WAIT_BUFFER_ACQUIRE,
        .make_bytearray = make_bytearray,
        .input = Py_NewRef(input),
        .view = NULL,
        .result = NULL,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &bytes_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *flags = PyLong_FromLong(PyBUF_FULL_RO);
    PyObject *view = flags == NULL
        ? NULL
        : call_raw_special_onearg(input, "__buffer__", flags);
    Py_XDECREF(flags);
    PyObject *result = view == NULL ? NULL : bytes_buffer_continue(&state, view);
    Py_XDECREF(view);
    adapter_leave(&frame);
    Py_DECREF(state.input);
    Py_XDECREF(state.view);
    Py_XDECREF(state.result);
    return result;
}

PyObject *
adapter_bytes_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    if (
        type != &PyBytes_Type || PyTuple_GET_SIZE(args) != 1 ||
        (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0)
    ) {
        return original_bytes_new(type, args, kwargs);
    }
    PyObject *input = PyTuple_GET_ITEM(args, 0);
    if (PyBytes_CheckExact(input)) {
        return original_bytes_new(type, args, kwargs);
    }

    PyObject *bytes_descriptor = lookup_raw_special(input, "__bytes__");
    if (bytes_descriptor != NULL) {
        Py_DECREF(bytes_descriptor);
        BytesState state = {
            .phase = BYTES_WAIT_BYTES,
            .make_bytearray = 0,
            .input = NULL,
            .view = NULL,
            .result = NULL,
        };
        AleffAdapterFrame frame;
        if (adapter_enter(&frame, &bytes_vtable, &state) < 0) {
            return NULL;
        }
        PyObject *result = original_bytes_new(type, args, kwargs);
        adapter_leave(&frame);
        return result;
    }
    if (PyUnicode_Check(input)) {
        return original_bytes_new(type, args, kwargs);
    }
    if (PyIndex_Check(input)) {
        BytesState state = {
            .phase = BYTES_WAIT_INDEX,
            .make_bytearray = 0,
            .input = NULL,
            .view = NULL,
            .result = NULL,
        };
        AleffAdapterFrame frame;
        if (adapter_enter(&frame, &bytes_vtable, &state) < 0) {
            return NULL;
        }
        PyObject *result = original_bytes_new(type, args, kwargs);
        adapter_leave(&frame);
        return result;
    }
    if (PyObject_CheckBuffer(input)) {
        PyObject *buffer_descriptor = lookup_raw_special(input, "__buffer__");
        if (
            buffer_descriptor == NULL ||
            Py_IS_TYPE(buffer_descriptor, &PyWrapperDescr_Type)
        ) {
            Py_XDECREF(buffer_descriptor);
            return original_bytes_new(type, args, kwargs);
        }
        Py_DECREF(buffer_descriptor);
        return convert_python_buffer(input, 0);
    }
    if (Py_TYPE(input)->tp_iter == NULL && !PySequence_Check(input)) {
        return original_bytes_new(type, args, kwargs);
    }
    return collect_iterable(input, COLLECT_BYTES);
}

int
adapter_bytearray_init(PyObject *self, PyObject *args, PyObject *kwargs)
{
    if (
        PyTuple_GET_SIZE(args) != 1 ||
        (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0) ||
        PyUnicode_Check(PyTuple_GET_ITEM(args, 0))
    ) {
        return original_bytearray_init(self, args, kwargs);
    }
    PyObject *input = PyTuple_GET_ITEM(args, 0);
    if (PyIndex_Check(input)) {
        BytesState state = {
            .phase = BYTES_WAIT_INDEX,
            .make_bytearray = 1,
            .input = NULL,
            .view = NULL,
            .result = NULL,
        };
        AleffAdapterFrame frame;
        if (adapter_enter(&frame, &bytes_vtable, &state) < 0) {
            return -1;
        }
        int status = original_bytearray_init(self, args, kwargs);
        adapter_leave(&frame);
        return status;
    }

    PyObject *result;
    if (PyObject_CheckBuffer(input)) {
        PyObject *buffer_descriptor = lookup_raw_special(input, "__buffer__");
        if (
            buffer_descriptor == NULL ||
            Py_IS_TYPE(buffer_descriptor, &PyWrapperDescr_Type)
        ) {
            Py_XDECREF(buffer_descriptor);
            return original_bytearray_init(self, args, kwargs);
        }
        Py_DECREF(buffer_descriptor);
        result = convert_python_buffer(input, 1);
    }
    else {
        if (Py_TYPE(input)->tp_iter == NULL && !PySequence_Check(input)) {
            return original_bytearray_init(self, args, kwargs);
        }
        result = collect_iterable(input, COLLECT_BYTEARRAY);
    }
    if (result == NULL) {
        return -1;
    }
    int status = bytearray_replace_buffer(self, result);
    Py_DECREF(result);
    return status;
}

typedef struct {
    PyObject *separator;
} UnicodeJoinState;

static const AleffAdapterVTable unicode_join_vtable;

static void *
unicode_join_copy_state(const void *raw_state)
{
    const UnicodeJoinState *state = raw_state;
    UnicodeJoinState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->separator = Py_NewRef(state->separator);
    return copy;
}

static void
unicode_join_free_state(void *raw_state)
{
    UnicodeJoinState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->separator);
    PyMem_Free(state);
}

static PyObject *
unicode_join_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    const UnicodeJoinState *state = raw_state;
    return PyUnicode_Join(state->separator, value);
}

static const AleffAdapterVTable unicode_join_vtable = {
    .copy_state = unicode_join_copy_state,
    .free_state = unicode_join_free_state,
    .resume = unicode_join_resume,
};

static PyObject *
adapter_unicode_join(PyObject *self, PyObject *iterable)
{
    UnicodeJoinState state = {.separator = self};
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &unicode_join_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *items = NULL;
    PyObject *result;
    if (PyList_CheckExact(iterable) || PyTuple_CheckExact(iterable)) {
        result = PyUnicode_Join(self, iterable);
    }
    else {
        items = collect_iterable(iterable, COLLECT_LIST);
        result = items == NULL ? NULL : PyUnicode_Join(self, items);
    }
    Py_XDECREF(items);
    adapter_leave(&frame);
    return result;
}

typedef enum {
    BINARY_JOIN_WAIT_ITEMS,
    BINARY_JOIN_WAIT_ITEM_BUFFER,
    BINARY_JOIN_WAIT_ITEM_RELEASE,
} BinaryJoinPhase;

typedef struct {
    PyObject *separator;
    PyObject *separator_view;
    PyObject *items;
    PyObject *views;
    PyObject *release_items;
    PyObject *current_item;
    PyObject *result;
    Py_ssize_t source_size;
    Py_ssize_t index;
    Py_ssize_t release_index;
    BinaryJoinPhase phase;
    int make_bytearray;
    int source_is_exact_list;
} BinaryJoinState;

static const AleffAdapterVTable binary_join_vtable;

static void *
binary_join_copy_state(const void *raw_state)
{
    const BinaryJoinState *state = raw_state;
    BinaryJoinState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->separator = Py_NewRef(state->separator);
    copy->separator_view = Py_NewRef(state->separator_view);
    copy->items = Py_XNewRef(state->items);
    copy->views = PyList_GetSlice(
        state->views, 0, PyList_GET_SIZE(state->views)
    );
    copy->release_items = copy->views == NULL
        ? NULL
        : PyList_GetSlice(
            state->release_items,
            0,
            PyList_GET_SIZE(state->release_items)
        );
    copy->current_item = Py_XNewRef(state->current_item);
    copy->result = Py_XNewRef(state->result);
    if (copy->views == NULL || copy->release_items == NULL) {
        Py_DECREF(copy->separator);
        Py_DECREF(copy->separator_view);
        Py_XDECREF(copy->items);
        Py_XDECREF(copy->views);
        Py_XDECREF(copy->release_items);
        Py_XDECREF(copy->current_item);
        Py_XDECREF(copy->result);
        PyMem_Free(copy);
        return NULL;
    }
    return copy;
}

static void
binary_join_free_state(void *raw_state)
{
    BinaryJoinState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->separator);
    Py_DECREF(state->separator_view);
    Py_XDECREF(state->items);
    Py_DECREF(state->views);
    Py_DECREF(state->release_items);
    Py_XDECREF(state->current_item);
    Py_XDECREF(state->result);
    PyMem_Free(state);
}

static int
binary_join_source_unchanged(BinaryJoinState *state)
{
    if (
        state->source_is_exact_list &&
        PyList_GET_SIZE(state->items) != state->source_size
    ) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "sequence changed size during iteration"
        );
        return 0;
    }
    return 1;
}

static void
binary_join_item_type_error(BinaryJoinState *state)
{
    PyErr_Clear();
    PyErr_Format(
        PyExc_TypeError,
        "sequence item %zd: expected a bytes-like object, %.80s found",
        state->index,
        Py_TYPE(state->current_item)->tp_name
    );
}

static int
binary_join_store_view(
    BinaryJoinState *state,
    PyObject *view,
    int needs_release
)
{
    if (!PyMemoryView_Check(view)) {
        binary_join_item_type_error(state);
        return -1;
    }
    Py_buffer buffer;
    if (PyObject_GetBuffer(view, &buffer, PyBUF_SIMPLE) < 0) {
        binary_join_item_type_error(state);
        return -1;
    }
    PyBuffer_Release(&buffer);
    if (!binary_join_source_unchanged(state)) {
        return -1;
    }
    if (PyList_Append(state->views, view) < 0) {
        return -1;
    }
    if (PyList_Append(
            state->release_items,
            needs_release ? state->current_item : Py_None
        ) < 0) {
        if (PySequence_DelItem(
                state->views, PyList_GET_SIZE(state->views) - 1
            ) < 0) {
            PyErr_Clear();
            PyErr_NoMemory();
        }
        return -1;
    }
    Py_CLEAR(state->current_item);
    state->index++;
    return 0;
}

static PyObject *binary_join_continue(
    BinaryJoinState *state,
    PyObject *resumed_value,
    int is_resumed
);

static PyObject *
binary_join_release_continue(
    BinaryJoinState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        PyObject *item = PyList_GET_ITEM(
            state->release_items, state->release_index
        );
        if (resumed_value == NULL && PyErr_Occurred()) {
            PyErr_WriteUnraisable(item);
        }
        state->release_index++;
    }
    Py_ssize_t count = PyList_GET_SIZE(state->release_items);
    while (state->release_index < count) {
        PyObject *item = PyList_GET_ITEM(
            state->release_items, state->release_index
        );
        if (item == Py_None) {
            state->release_index++;
            continue;
        }
        PyObject *descriptor = lookup_raw_special(item, "__release_buffer__");
        if (descriptor == NULL) {
            state->release_index++;
            continue;
        }
        Py_DECREF(descriptor);
        PyObject *view = PyList_GET_ITEM(state->views, state->release_index);
        state->phase = BINARY_JOIN_WAIT_ITEM_RELEASE;
        PyObject *released = call_raw_special_onearg(
            item, "__release_buffer__", view
        );
        if (released == NULL) {
            PyErr_WriteUnraisable(item);
        }
        else {
            Py_DECREF(released);
        }
        state->release_index++;
    }
    return Py_NewRef(state->result);
}

static PyObject *
binary_join_build_result(BinaryJoinState *state)
{
    if (!binary_join_source_unchanged(state)) {
        return NULL;
    }
    Py_buffer separator;
    if (PyObject_GetBuffer(state->separator_view, &separator, PyBUF_SIMPLE) < 0) {
        return NULL;
    }
    Py_ssize_t count = PyList_GET_SIZE(state->views);
    Py_ssize_t total = 0;
    for (Py_ssize_t index = 0; index < count; index++) {
        Py_buffer view;
        if (PyObject_GetBuffer(
                PyList_GET_ITEM(state->views, index),
                &view,
                PyBUF_SIMPLE
            ) < 0) {
            PyBuffer_Release(&separator);
            return NULL;
        }
        if (view.len > PY_SSIZE_T_MAX - total) {
            PyBuffer_Release(&view);
            PyBuffer_Release(&separator);
            PyErr_NoMemory();
            return NULL;
        }
        total += view.len;
        PyBuffer_Release(&view);
    }
    if (count > 1) {
        if (separator.len > (PY_SSIZE_T_MAX - total) / (count - 1)) {
            PyBuffer_Release(&separator);
            PyErr_NoMemory();
            return NULL;
        }
        total += separator.len * (count - 1);
    }

    PyObject *result;
    if (
        !state->make_bytearray && count == 1 &&
        PyBytes_CheckExact(PyList_GET_ITEM(state->items, 0))
    ) {
        result = Py_NewRef(PyList_GET_ITEM(state->items, 0));
    }
    else {
        result = state->make_bytearray
            ? PyByteArray_FromStringAndSize(NULL, total)
            : PyBytes_FromStringAndSize(NULL, total);
        if (result != NULL) {
            char *output = state->make_bytearray
                ? PyByteArray_AS_STRING(result)
                : PyBytes_AS_STRING(result);
            Py_ssize_t offset = 0;
            for (Py_ssize_t index = 0; index < count; index++) {
                if (index != 0 && separator.len != 0) {
                    memcpy(
                        output + offset,
                        separator.buf,
                        (size_t)separator.len
                    );
                    offset += separator.len;
                }
                Py_buffer view;
                if (PyObject_GetBuffer(
                        PyList_GET_ITEM(state->views, index),
                        &view,
                        PyBUF_SIMPLE
                    ) < 0) {
                    Py_CLEAR(result);
                    break;
                }
                if (view.len != 0) {
                    memcpy(output + offset, view.buf, (size_t)view.len);
                    offset += view.len;
                }
                PyBuffer_Release(&view);
            }
        }
    }
    PyBuffer_Release(&separator);
    if (result == NULL) {
        return NULL;
    }
    Py_XSETREF(state->result, result);
    state->release_index = 0;
    return binary_join_release_continue(state, NULL, 0);
}

static PyObject *
binary_join_continue(
    BinaryJoinState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (state->phase == BINARY_JOIN_WAIT_ITEMS) {
            if (!PyList_Check(resumed_value)) {
                PyErr_SetString(
                    PyExc_RuntimeError,
                    "join collector returned a non-list"
                );
                return NULL;
            }
            Py_XSETREF(state->items, Py_NewRef(resumed_value));
            state->source_size = PyList_GET_SIZE(state->items);
        }
        else if (state->phase == BINARY_JOIN_WAIT_ITEM_BUFFER) {
            if (binary_join_store_view(state, resumed_value, 1) < 0) {
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "invalid binary join phase");
            return NULL;
        }
    }

    while (state->index < state->source_size) {
        if (!binary_join_source_unchanged(state)) {
            return NULL;
        }
        state->current_item = Py_NewRef(
            PyList_GET_ITEM(state->items, state->index)
        );
        PyObject *descriptor = lookup_raw_special(
            state->current_item, "__buffer__"
        );
        int has_python_buffer = descriptor != NULL &&
            !Py_IS_TYPE(descriptor, &PyWrapperDescr_Type);
        Py_XDECREF(descriptor);
        if (has_python_buffer) {
            state->phase = BINARY_JOIN_WAIT_ITEM_BUFFER;
            PyObject *flags = PyLong_FromLong(PyBUF_SIMPLE);
            PyObject *view = flags == NULL
                ? NULL
                : call_raw_special_onearg(
                    state->current_item, "__buffer__", flags
                );
            Py_XDECREF(flags);
            if (view == NULL) {
                if (PyErr_ExceptionMatches(PyExc_BufferError)) {
                    binary_join_item_type_error(state);
                }
                return NULL;
            }
            int stored = binary_join_store_view(state, view, 1);
            Py_DECREF(view);
            if (stored < 0) {
                return NULL;
            }
        }
        else {
            PyObject *view = PyMemoryView_FromObject(state->current_item);
            if (view == NULL) {
                binary_join_item_type_error(state);
                return NULL;
            }
            int stored = binary_join_store_view(state, view, 0);
            Py_DECREF(view);
            if (stored < 0) {
                return NULL;
            }
        }
    }
    return binary_join_build_result(state);
}

static PyObject *
binary_join_resume(const void *raw_state, PyObject *value)
{
    const BinaryJoinState *source = raw_state;
    if (
        value == NULL &&
        source->phase != BINARY_JOIN_WAIT_ITEM_RELEASE
    ) {
        return NULL;
    }
    BinaryJoinState *state = binary_join_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &binary_join_vtable, state) < 0) {
        binary_join_free_state(state);
        return NULL;
    }
    PyObject *result = state->phase == BINARY_JOIN_WAIT_ITEM_RELEASE
        ? binary_join_release_continue(state, value, 1)
        : binary_join_continue(state, value, 1);
    adapter_leave(&frame);
    binary_join_free_state(state);
    return result;
}

static const AleffAdapterVTable binary_join_vtable = {
    .copy_state = binary_join_copy_state,
    .free_state = binary_join_free_state,
    .resume = binary_join_resume,
};

static PyObject *
adapter_binary_join(PyObject *self, PyObject *iterable, int make_bytearray)
{
    BinaryJoinState state = {
        .separator = self,
        .separator_view = PyMemoryView_FromObject(self),
        .items = NULL,
        .views = PyList_New(0),
        .release_items = PyList_New(0),
        .current_item = NULL,
        .result = NULL,
        .source_size = 0,
        .index = 0,
        .release_index = 0,
        .phase = BINARY_JOIN_WAIT_ITEMS,
        .make_bytearray = make_bytearray,
        .source_is_exact_list = 0,
    };
    if (
        state.separator_view == NULL || state.views == NULL ||
        state.release_items == NULL
    ) {
        Py_XDECREF(state.separator_view);
        Py_XDECREF(state.views);
        Py_XDECREF(state.release_items);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &binary_join_vtable, &state) < 0) {
        Py_DECREF(state.separator_view);
        Py_DECREF(state.views);
        Py_DECREF(state.release_items);
        return NULL;
    }
    PyObject *result;
    if (PyList_CheckExact(iterable)) {
        state.items = Py_NewRef(iterable);
        state.source_size = PyList_GET_SIZE(iterable);
        state.source_is_exact_list = 1;
        result = binary_join_continue(&state, NULL, 0);
    }
    else if (PyTuple_CheckExact(iterable)) {
        state.items = PySequence_List(iterable);
        state.source_size = state.items == NULL
            ? 0
            : PyList_GET_SIZE(state.items);
        result = state.items == NULL
            ? NULL
            : binary_join_continue(&state, NULL, 0);
    }
    else {
        state.phase = BINARY_JOIN_WAIT_ITEMS;
        state.items = collect_iterable(iterable, COLLECT_LIST);
        state.source_size = state.items == NULL
            ? 0
            : PyList_GET_SIZE(state.items);
        result = state.items == NULL
            ? NULL
            : binary_join_continue(&state, NULL, 0);
    }
    adapter_leave(&frame);
    Py_DECREF(state.separator_view);
    Py_XDECREF(state.items);
    Py_DECREF(state.views);
    Py_DECREF(state.release_items);
    Py_XDECREF(state.current_item);
    Py_XDECREF(state.result);
    return result;
}

static PyObject *
adapter_str_join(PyObject *self, PyObject *iterable)
{
    return adapter_unicode_join(self, iterable);
}

static PyObject *
adapter_bytes_join(PyObject *self, PyObject *iterable)
{
    return adapter_binary_join(self, iterable, 0);
}

static PyObject *
adapter_bytearray_join(PyObject *self, PyObject *iterable)
{
    return adapter_binary_join(self, iterable, 1);
}

PyMethodDef containers_str_encode_method = {
    .ml_name = "encode",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_str_encode,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Encode the string.",
};

PyMethodDef containers_bytes_decode_method = {
    .ml_name = "decode",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_bytes_decode,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Decode the bytes.",
};

PyMethodDef containers_bytearray_decode_method = {
    .ml_name = "decode",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_bytearray_decode,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Decode the bytearray.",
};

PyMethodDef containers_str_join_method = {
    .ml_name = "join",
    .ml_meth = adapter_str_join,
    .ml_flags = METH_O,
    .ml_doc = "Concatenate any number of strings.",
};

PyMethodDef containers_bytes_join_method = {
    .ml_name = "join",
    .ml_meth = adapter_bytes_join,
    .ml_flags = METH_O,
    .ml_doc = "Concatenate any number of bytes-like objects.",
};

PyMethodDef containers_bytearray_join_method = {
    .ml_name = "join",
    .ml_meth = adapter_bytearray_join,
    .ml_flags = METH_O,
    .ml_doc = "Concatenate any number of bytes-like objects.",
};
