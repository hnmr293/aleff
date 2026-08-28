typedef struct {
    PyObject *source;
    PyObject *encoding;
    PyObject *errors;
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
    PyMem_Free(state);
}

static const char *
codec_name(PyObject *object)
{
    if (object == NULL || object == Py_None) {
        return NULL;
    }
    return PyUnicode_AsUTF8(object);
}

static PyObject *
codec_resume(const void *raw_state, PyObject *value)
{
    const CodecState *state = raw_state;
    if (value == NULL) {
        return NULL;
    }
    if (!PyTuple_Check(value) || PyTuple_GET_SIZE(value) != 2) {
        PyErr_SetString(PyExc_TypeError, "codec error handler must return (replacement, position)");
        return NULL;
    }
    PyObject *replacement = PyTuple_GET_ITEM(value, 0);
    PyObject *position = PyTuple_GET_ITEM(value, 1);
    if (!PyUnicode_Check(replacement) || !PyLong_Check(position)) {
        PyErr_SetString(PyExc_TypeError, "codec error handler returned invalid replacement");
        return NULL;
    }
    const char *encoding = codec_name(state->encoding);
    const char *errors = "strict";
    if (state->kind == 0) {
        PyObject *result = PyUnicode_AsEncodedString(replacement, encoding, errors);
        if (result == NULL) {
            return NULL;
        }
        return result;
    }
    const char *replacement_text = PyUnicode_AsUTF8(replacement);
    if (replacement_text == NULL) {
        return NULL;
    }
    Py_ssize_t replacement_size = PyUnicode_GET_LENGTH(replacement);
    (void)position;
    if (state->kind == 1) {
        return PyUnicode_DecodeUTF8(
            replacement_text,
            (Py_ssize_t)strlen(replacement_text),
            errors
        );
    }
    return PyUnicode_DecodeUTF8(
        replacement_text,
        replacement_size,
        errors
    );
}

static const AleffAdapterVTable codec_vtable = {
    .copy_state = codec_copy_state,
    .free_state = codec_free_state,
    .resume = codec_resume,
};

static PyObject *
adapter_str_encode(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *encoding_object = Py_None;
    PyObject *errors_object = Py_None;
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
    if (encoding_object != Py_None && encoding == NULL) {
        return NULL;
    }
    const char *errors = codec_name(errors_object);
    if (errors_object != Py_None && errors == NULL) {
        return NULL;
    }
    CodecState state = {
        .source = self,
        .encoding = encoding_object == Py_None ? NULL : encoding_object,
        .errors = errors_object == Py_None ? NULL : errors_object,
        .kind = 0,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &codec_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = PyUnicode_AsEncodedString(
        self,
        encoding,
        errors
    );
    adapter_leave(&frame);
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
    PyObject *encoding_object = Py_None;
    PyObject *errors_object = Py_None;
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
    if (encoding_object != Py_None && encoding == NULL) {
        return NULL;
    }
    const char *errors = codec_name(errors_object);
    if (errors_object != Py_None && errors == NULL) {
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
        .encoding = encoding_object == Py_None ? NULL : encoding_object,
        .errors = errors_object == Py_None ? NULL : errors_object,
        .kind = kind,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &codec_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = PyUnicode_Decode(
        PyBytes_AS_STRING(source),
        PyBytes_GET_SIZE(source),
        encoding,
        errors
    );
    adapter_leave(&frame);
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

static PyObject *
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
        PyObject *descriptor = PyDict_GetItemString(dictionary, name);
        if (descriptor != NULL) {
            return Py_NewRef(descriptor);
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
    PyObject *released = PyObject_CallMethod(
        state->input,
        "__release_buffer__",
        "O",
        state->view
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
    PyObject *view = PyObject_CallMethod(input, "__buffer__", "i", PyBUF_FULL_RO);
    PyObject *result = view == NULL ? NULL : bytes_buffer_continue(&state, view);
    Py_XDECREF(view);
    adapter_leave(&frame);
    Py_DECREF(state.input);
    Py_XDECREF(state.view);
    Py_XDECREF(state.result);
    return result;
}

static PyObject *
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
    return collect_iterable(input, COLLECT_BYTES);
}

static int
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
        result = collect_iterable(input, COLLECT_BYTEARRAY);
    }
    if (result == NULL) {
        return -1;
    }
    int status = bytearray_replace_buffer(self, result);
    Py_DECREF(result);
    return status;
}

static PyObject *
bytearray_join_items(PyObject *self, PyObject *items)
{
    PyObject *result = PyByteArray_FromStringAndSize(NULL, 0);
    PyObject *separator = result == NULL ? NULL : PyByteArray_FromObject(self);
    if (separator == NULL) {
        Py_XDECREF(result);
        return NULL;
    }
    Py_ssize_t size = PyList_GET_SIZE(items);
    for (Py_ssize_t index = 0; result != NULL && index < size; index++) {
        if (index != 0) {
            PyObject *joined = PyByteArray_Concat(result, separator);
            Py_DECREF(result);
            result = joined;
        }
        Py_buffer view;
        if (PyObject_GetBuffer(PyList_GET_ITEM(items, index), &view, PyBUF_SIMPLE) < 0) {
            PyErr_Clear();
            PyErr_Format(
                PyExc_TypeError,
                "sequence item %zd: expected a bytes-like object, %.80s found",
                index,
                Py_TYPE(PyList_GET_ITEM(items, index))->tp_name
            );
            Py_CLEAR(result);
            break;
        }
        PyObject *item = PyByteArray_FromStringAndSize(view.buf, view.len);
        PyBuffer_Release(&view);
        if (item == NULL) {
            Py_CLEAR(result);
            break;
        }
        PyObject *joined = PyByteArray_Concat(result, item);
        Py_DECREF(item);
        Py_DECREF(result);
        result = joined;
    }
    Py_DECREF(separator);
    return result;
}

#if PY_VERSION_HEX < 0x030e0000
static PyObject *
bytes_join_items(PyObject *self, PyObject *items)
{
    PyObject *result = PyBytes_FromStringAndSize(NULL, 0);
    PyObject *separator = result == NULL ? NULL : PyBytes_FromObject(self);
    if (separator == NULL) {
        Py_XDECREF(result);
        return NULL;
    }
    Py_ssize_t size = PyList_GET_SIZE(items);
    for (Py_ssize_t index = 0; result != NULL && index < size; index++) {
        if (index != 0) {
            PyBytes_Concat(&result, separator);
        }
        if (result == NULL) {
            break;
        }
        PyObject *item = PyBytes_FromObject(PyList_GET_ITEM(items, index));
        if (item == NULL) {
            Py_CLEAR(result);
            break;
        }
        PyBytes_Concat(&result, item);
        Py_DECREF(item);
    }
    Py_DECREF(separator);
    return result;
}
#endif

typedef struct {
    PyObject *separator;
    int kind;
} JoinState;

static const AleffAdapterVTable join_vtable;

static void *
join_copy_state(const void *raw_state)
{
    const JoinState *state = raw_state;
    JoinState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->separator = Py_NewRef(state->separator);
    copy->kind = state->kind;
    return copy;
}

static void
join_free_state(void *raw_state)
{
    JoinState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->separator);
    PyMem_Free(state);
}

static PyObject *
join_apply(JoinState *state, PyObject *items)
{
    if (state->kind == 0) {
        return PyUnicode_Join(state->separator, items);
    }
    if (state->kind == 1) {
#if PY_VERSION_HEX >= 0x030e0000
        return PyBytes_Join(state->separator, items);
#else
        return bytes_join_items(state->separator, items);
#endif
    }
    return bytearray_join_items(state->separator, items);
}

static PyObject *
join_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    return join_apply((JoinState *)raw_state, value);
}

static const AleffAdapterVTable join_vtable = {
    .copy_state = join_copy_state,
    .free_state = join_free_state,
    .resume = join_resume,
};

static PyObject *
adapter_join(PyObject *self, PyObject *iterable, int kind)
{
    JoinState state = {
        .separator = self,
        .kind = kind,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &join_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *items = collect_iterable(iterable, COLLECT_LIST);
    PyObject *result = items == NULL ? NULL : join_apply(&state, items);
    Py_XDECREF(items);
    adapter_leave(&frame);
    return result;
}

static PyObject *
adapter_str_join(PyObject *self, PyObject *iterable)
{
    return adapter_join(self, iterable, 0);
}

static PyObject *
adapter_bytes_join(PyObject *self, PyObject *iterable)
{
    return adapter_join(self, iterable, 1);
}

static PyObject *
adapter_bytearray_join(PyObject *self, PyObject *iterable)
{
    return adapter_join(self, iterable, 2);
}

static PyMethodDef containers_str_encode_method = {
    .ml_name = "encode",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_str_encode,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Encode the string.",
};

static PyMethodDef containers_bytes_decode_method = {
    .ml_name = "decode",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_bytes_decode,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Decode the bytes.",
};

static PyMethodDef containers_bytearray_decode_method = {
    .ml_name = "decode",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_bytearray_decode,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Decode the bytearray.",
};

static PyMethodDef containers_str_join_method = {
    .ml_name = "join",
    .ml_meth = adapter_str_join,
    .ml_flags = METH_O,
    .ml_doc = "Concatenate any number of strings.",
};

static PyMethodDef containers_bytes_join_method = {
    .ml_name = "join",
    .ml_meth = adapter_bytes_join,
    .ml_flags = METH_O,
    .ml_doc = "Concatenate any number of bytes-like objects.",
};

static PyMethodDef containers_bytearray_join_method = {
    .ml_name = "join",
    .ml_meth = adapter_bytearray_join,
    .ml_flags = METH_O,
    .ml_doc = "Concatenate any number of bytes-like objects.",
};
