typedef struct {
    PyObject *default_value;
} NextState;

static void *
next_copy_state(const void *raw_state)
{
    const NextState *state = raw_state;
    NextState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->default_value = Py_XNewRef(state->default_value);
    return copy;
}

static void
next_free_state(void *raw_state)
{
    NextState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->default_value);
    PyMem_Free(state);
}

static PyObject *
next_resume(const void *raw_state, PyObject *value)
{
    const NextState *state = raw_state;
    if (value != NULL) {
        return Py_NewRef(value);
    }
    if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
        return NULL;
    }
    if (state->default_value == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetNone(PyExc_StopIteration);
        }
        return NULL;
    }
    if (PyErr_Occurred()) {
        PyErr_Clear();
    }
    return Py_NewRef(state->default_value);
}

static const AleffAdapterVTable next_vtable = {
    .copy_state = next_copy_state,
    .free_state = next_free_state,
    .resume = next_resume,
};

static PyObject *
adapter_next(PyObject *Py_UNUSED(self), PyObject *args)
{
    PyObject *iterator;
    PyObject *default_value = NULL;
    if (!PyArg_ParseTuple(args, "O|O:next", &iterator, &default_value)) {
        return NULL;
    }
    if (!PyIter_Check(iterator)) {
        PyErr_Format(
            PyExc_TypeError,
            "'%s' object is not an iterator",
            Py_TYPE(iterator)->tp_name
        );
        return NULL;
    }

    NextState state = {.default_value = default_value};
    AleffAdapterFrame frame;
    adapter_enter(&frame, &next_vtable, &state);
    PyObject *result = Py_TYPE(iterator)->tp_iternext(iterator);
    adapter_leave(&frame);

    if (result == NULL) {
        if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
            return NULL;
        }
        if (default_value != NULL) {
            if (PyErr_Occurred()) {
                PyErr_Clear();
            }
            return Py_NewRef(default_value);
        }
        if (!PyErr_Occurred()) {
            PyErr_SetNone(PyExc_StopIteration);
        }
    }
    return result;
}

static PyObject *
len_resume(const void *Py_UNUSED(state), PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    Py_ssize_t length = PyNumber_AsSsize_t(value, PyExc_OverflowError);
    if (length < 0) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_ValueError, "__len__() should return >= 0");
        }
        return NULL;
    }
    return PyLong_FromSsize_t(length);
}

static const AleffAdapterVTable len_vtable = {
    .copy_state = empty_copy_state,
    .free_state = empty_free_state,
    .resume = len_resume,
};

static PyObject *
adapter_len(PyObject *Py_UNUSED(self), PyObject *object)
{
    AleffAdapterFrame frame;
    adapter_enter(&frame, &len_vtable, NULL);
    Py_ssize_t length = PyObject_Size(object);
    adapter_leave(&frame);
    if (length < 0) {
        return NULL;
    }
    return PyLong_FromSsize_t(length);
}

static PyObject *
ascii_from_repr(PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    if (!PyUnicode_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "__repr__ returned non-string (type %.200s)",
            Py_TYPE(value)->tp_name
        );
        return NULL;
    }
    PyObject *encoded = PyUnicode_AsEncodedString(
        value,
        "ascii",
        "backslashreplace"
    );
    if (encoded == NULL) {
        return NULL;
    }
    PyObject *result = PyUnicode_DecodeASCII(
        PyBytes_AS_STRING(encoded),
        PyBytes_GET_SIZE(encoded),
        NULL
    );
    Py_DECREF(encoded);
    return result;
}

static PyObject *
ascii_resume(const void *Py_UNUSED(state), PyObject *value)
{
    return ascii_from_repr(value);
}

static const AleffAdapterVTable ascii_vtable = {
    .copy_state = empty_copy_state,
    .free_state = empty_free_state,
    .resume = ascii_resume,
};

static PyObject *
adapter_ascii(PyObject *Py_UNUSED(self), PyObject *object)
{
    AleffAdapterFrame frame;
    adapter_enter(&frame, &ascii_vtable, NULL);
    PyObject *result = PyObject_ASCII(object);
    adapter_leave(&frame);
    return result;
}

static PyObject *
hasattr_result(PyObject *value)
{
    if (value != NULL) {
        Py_DECREF(value);
        return Py_NewRef(Py_True);
    }
    if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
        return Py_NewRef(Py_False);
    }
    return NULL;
}

static PyObject *
hasattr_resume(const void *Py_UNUSED(state), PyObject *value)
{
    return hasattr_result(Py_XNewRef(value));
}

static const AleffAdapterVTable hasattr_vtable = {
    .copy_state = empty_copy_state,
    .free_state = empty_free_state,
    .resume = hasattr_resume,
};

static PyObject *
adapter_hasattr(PyObject *Py_UNUSED(self), PyObject *args)
{
    PyObject *object;
    PyObject *name;
    if (!PyArg_ParseTuple(args, "OO:hasattr", &object, &name)) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &hasattr_vtable, NULL);
    PyObject *value = PyObject_GetAttr(object, name);
    adapter_leave(&frame);
    return hasattr_result(value);
}

typedef struct {
    PyObject *default_value;
} GetattrState;

static void *
getattr_copy_state(const void *raw_state)
{
    const GetattrState *state = raw_state;
    GetattrState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->default_value = Py_XNewRef(state->default_value);
    return copy;
}

static void
getattr_free_state(void *raw_state)
{
    GetattrState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->default_value);
    PyMem_Free(state);
}

static PyObject *
getattr_resume(const void *raw_state, PyObject *value)
{
    const GetattrState *state = raw_state;
    if (value != NULL) {
        return Py_NewRef(value);
    }
    if (
        state->default_value != NULL &&
        PyErr_ExceptionMatches(PyExc_AttributeError)
    ) {
        PyErr_Clear();
        return Py_NewRef(state->default_value);
    }
    return NULL;
}

static const AleffAdapterVTable getattr_vtable = {
    .copy_state = getattr_copy_state,
    .free_state = getattr_free_state,
    .resume = getattr_resume,
};

static PyObject *
adapter_getattr(PyObject *Py_UNUSED(self), PyObject *args)
{
    PyObject *object;
    PyObject *name;
    PyObject *default_value = NULL;
    if (!PyArg_ParseTuple(args, "OO|O:getattr", &object, &name, &default_value)) {
        return NULL;
    }
    GetattrState state = {.default_value = default_value};
    AleffAdapterFrame frame;
    adapter_enter(&frame, &getattr_vtable, &state);
    PyObject *result = PyObject_GetAttr(object, name);
    adapter_leave(&frame);
    if (
        result == NULL && default_value != NULL &&
        PyErr_ExceptionMatches(PyExc_AttributeError)
    ) {
        PyErr_Clear();
        return Py_NewRef(default_value);
    }
    return result;
}

static PyObject *original_input = NULL;
static PyObject *original_anext = NULL;
static PyObject *adapter_print(PyObject *self, PyObject *args, PyObject *kwargs);

typedef struct {
    PyObject_HEAD
    PyObject *awaitable;
    PyObject *iterator;
    PyObject *default_value;
} AleffAnextAwaitable;

typedef struct {
    PyObject *default_value;
} AnextAwaitState;

static void *
anext_await_copy_state(const void *raw_state)
{
    const AnextAwaitState *state = raw_state;
    AnextAwaitState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->default_value = Py_XNewRef(state->default_value);
    return copy;
}

static void
anext_await_free_state(void *raw_state)
{
    AnextAwaitState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->default_value);
    PyMem_Free(state);
}

static PyObject *
anext_await_resume(const void *raw_state, PyObject *value)
{
    const AnextAwaitState *state = raw_state;
    if (value != NULL) {
        return Py_NewRef(value);
    }
    if (
        state->default_value != NULL &&
        PyErr_ExceptionMatches(PyExc_StopAsyncIteration)
    ) {
        PyErr_Clear();
        return Py_NewRef(state->default_value);
    }
    return NULL;
}

static const AleffAdapterVTable anext_await_vtable = {
    .copy_state = anext_await_copy_state,
    .free_state = anext_await_free_state,
    .resume = anext_await_resume,
};

static int
anext_awaitable_ensure_iterator(AleffAnextAwaitable *self)
{
    if (self->iterator != NULL) {
        return 0;
    }
    PyObject *iterator = PyObject_CallMethod(self->awaitable, "__await__", NULL);
    if (iterator == NULL) {
        return -1;
    }
    if (!PyIter_Check(iterator)) {
        PyErr_Format(
            PyExc_TypeError,
            "__await__() returned non-iterator of type '%.100s'",
            Py_TYPE(iterator)->tp_name
        );
        Py_DECREF(iterator);
        return -1;
    }
    self->iterator = iterator;
    return 0;
}

static PyObject *
anext_awaitable_await(PyObject *object)
{
    AleffAnextAwaitable *self = (AleffAnextAwaitable *)object;
    if (anext_awaitable_ensure_iterator(self) < 0) {
        return NULL;
    }
    return Py_NewRef(object);
}

static PyObject *
anext_awaitable_next(PyObject *object)
{
    AleffAnextAwaitable *self = (AleffAnextAwaitable *)object;
    if (anext_awaitable_ensure_iterator(self) < 0) {
        return NULL;
    }
    AnextAwaitState state = {.default_value = self->default_value};
    AleffAdapterFrame frame;
    adapter_enter(&frame, &anext_await_vtable, &state);
    PyObject *result = Py_TYPE(self->iterator)->tp_iternext(self->iterator);
    adapter_leave(&frame);
    if (
        result == NULL && self->default_value != NULL &&
        PyErr_ExceptionMatches(PyExc_StopAsyncIteration)
    ) {
        PyErr_Clear();
        PyErr_SetObject(PyExc_StopIteration, self->default_value);
    }
    return result;
}

static int
anext_awaitable_traverse(PyObject *object, visitproc visit, void *arg)
{
    AleffAnextAwaitable *self = (AleffAnextAwaitable *)object;
    Py_VISIT(self->awaitable);
    Py_VISIT(self->iterator);
    Py_VISIT(self->default_value);
    return 0;
}

static int
anext_awaitable_clear(PyObject *object)
{
    AleffAnextAwaitable *self = (AleffAnextAwaitable *)object;
    Py_CLEAR(self->awaitable);
    Py_CLEAR(self->iterator);
    Py_CLEAR(self->default_value);
    return 0;
}

static void
anext_awaitable_dealloc(PyObject *object)
{
    PyObject_GC_UnTrack(object);
    anext_awaitable_clear(object);
    Py_TYPE(object)->tp_free(object);
}

static PyAsyncMethods anext_awaitable_as_async = {
    .am_await = anext_awaitable_await,
};

static PyTypeObject AleffAnextAwaitable_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "aleff._continuation_anext_awaitable",
    .tp_basicsize = sizeof(AleffAnextAwaitable),
    .tp_dealloc = anext_awaitable_dealloc,
    .tp_as_async = &anext_awaitable_as_async,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = anext_awaitable_traverse,
    .tp_clear = anext_awaitable_clear,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = anext_awaitable_next,
};

static PyObject *
anext_awaitable_new(PyObject *awaitable, PyObject *default_value)
{
    AleffAnextAwaitable *self = PyObject_GC_New(
        AleffAnextAwaitable,
        &AleffAnextAwaitable_Type
    );
    if (self == NULL) {
        return NULL;
    }
    self->awaitable = Py_NewRef(awaitable);
    self->iterator = NULL;
    self->default_value = Py_XNewRef(default_value);
    PyObject_GC_Track(self);
    return (PyObject *)self;
}

static PyObject *
adapter_anext(PyObject *Py_UNUSED(self), PyObject *args)
{
    PyObject *iterator;
    PyObject *default_value = NULL;
    if (!PyArg_ParseTuple(args, "O|O:anext", &iterator, &default_value)) {
        return NULL;
    }
    PyObject *awaitable = PyObject_Call(original_anext, args, NULL);
    if (awaitable == NULL) {
        return NULL;
    }
    PyObject *result = anext_awaitable_new(awaitable, default_value);
    Py_DECREF(awaitable);
    return result;
}

static PyObject *
input_normalize_line(PyObject *line)
{
    if (line == NULL) {
        return NULL;
    }
    if (PyUnicode_Check(line)) {
        Py_ssize_t length = PyUnicode_GetLength(line);
        if (length < 0) {
            return NULL;
        }
        if (length == 0) {
            PyErr_SetNone(PyExc_EOFError);
            return NULL;
        }
        Py_ssize_t end = length;
        if (PyUnicode_ReadChar(line, end - 1) == '\n') {
            end--;
            if (end > 0 && PyUnicode_ReadChar(line, end - 1) == '\r') {
                end--;
            }
        }
        return PyUnicode_Substring(line, 0, end);
    }
    if (PyBytes_Check(line)) {
        Py_ssize_t length = PyBytes_GET_SIZE(line);
        if (length == 0) {
            PyErr_SetNone(PyExc_EOFError);
            return NULL;
        }
        const char *data = PyBytes_AS_STRING(line);
        Py_ssize_t end = length;
        if (data[end - 1] == '\n') {
            end--;
            if (end > 0 && data[end - 1] == '\r') {
                end--;
            }
        }
        return PyBytes_FromStringAndSize(data, end);
    }
    PyErr_Format(
        PyExc_TypeError,
        "object.readline() returned non-string (type %.200s)",
        Py_TYPE(line)->tp_name
    );
    return NULL;
}

typedef enum {
    INPUT_WAIT_PROMPT,
    INPUT_WAIT_LINE,
} InputPhase;

typedef struct {
    InputPhase phase;
    PyObject *stdin_object;
} InputState;

static void *
input_copy_state(const void *raw_state)
{
    const InputState *state = raw_state;
    InputState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->stdin_object = Py_NewRef(state->stdin_object);
    return copy;
}

static void
input_free_state(void *state)
{
    InputState *input_state = state;
    if (input_state != NULL) {
        Py_DECREF(input_state->stdin_object);
        PyMem_Free(input_state);
    }
}

static const AleffAdapterVTable input_vtable;

static PyObject *
input_read_line(InputState *state)
{
    state->phase = INPUT_WAIT_LINE;
    PyObject *empty = PyTuple_New(0);
    if (empty == NULL) {
        return NULL;
    }
    PyObject *current_stdin = PySys_GetObject("stdin");
    Py_XINCREF(current_stdin);
    if (PySys_SetObject("stdin", state->stdin_object) < 0) {
        Py_DECREF(empty);
        Py_XDECREF(current_stdin);
        return NULL;
    }
    PyObject *line = PyObject_Call(original_input, empty, NULL);
    PyObject *exception = line == NULL ? PyErr_GetRaisedException() : NULL;
    int restored = PySys_SetObject("stdin", current_stdin);
    Py_XDECREF(current_stdin);
    Py_DECREF(empty);
    if (restored < 0) {
        Py_XDECREF(line);
        Py_XDECREF(exception);
        return NULL;
    }
    if (exception != NULL) {
        PyErr_SetRaisedException(exception);
    }
    return line;
}

static PyObject *
input_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    const InputState *source = raw_state;
    if (source->phase == INPUT_WAIT_LINE) {
        return input_normalize_line(value);
    }
    InputState *state = input_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &input_vtable, state);
    PyObject *result = input_read_line(state);
    adapter_leave(&frame);
    input_free_state(state);
    return result;
}

static const AleffAdapterVTable input_vtable = {
    .copy_state = input_copy_state,
    .free_state = input_free_state,
    .resume = input_resume,
};

static PyObject *
adapter_input(PyObject *Py_UNUSED(self), PyObject *args)
{
    Py_ssize_t count = PyTuple_GET_SIZE(args);
    if (count > 1) {
        return PyObject_Call(original_input, args, NULL);
    }
    InputState state = {
        .phase = count == 0 ? INPUT_WAIT_LINE : INPUT_WAIT_PROMPT,
        .stdin_object = PySys_GetObject("stdin"),
    };
    if (state.stdin_object == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "lost sys.stdin");
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &input_vtable, &state);
    PyObject *result = NULL;
    if (count == 1) {
        PyObject *prompt_args = PyTuple_Pack(1, PyTuple_GET_ITEM(args, 0));
        PyObject *prompt_kwargs = Py_BuildValue(
            "{s:s,s:O}",
            "end",
            "",
            "flush",
            Py_True
        );
        if (prompt_args != NULL && prompt_kwargs != NULL) {
            PyObject *printed = adapter_print(NULL, prompt_args, prompt_kwargs);
            if (printed != NULL) {
                Py_DECREF(printed);
                result = input_read_line(&state);
            }
        }
        Py_XDECREF(prompt_args);
        Py_XDECREF(prompt_kwargs);
    }
    else {
        result = input_read_line(&state);
    }
    adapter_leave(&frame);
    return result;
}

typedef enum {
    OPEN_WAIT_PATH,
    OPEN_WAIT_OPEN,
} OpenPhase;

typedef struct {
    PyObject *args;
    PyObject *kwargs;
    OpenPhase phase;
} OpenState;

static PyObject *original_open = NULL;
static PyObject *original_import = NULL;
static PyObject *import_get_module_lock = NULL;
static PyObject *import_global_lock_held = NULL;
static PyObject *import_global_lock_acquire = NULL;

static void *
open_copy_state(const void *raw_state)
{
    const OpenState *state = raw_state;
    OpenState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->args = Py_NewRef(state->args);
    copy->kwargs = Py_XNewRef(state->kwargs);
    copy->phase = state->phase;
    return copy;
}

static void
open_free_state(void *raw_state)
{
    OpenState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->args);
    Py_XDECREF(state->kwargs);
    PyMem_Free(state);
}

static PyObject *
open_with_path(OpenState *state, PyObject *path)
{
    if (!PyUnicode_Check(path) && !PyBytes_Check(path)) {
        PyErr_Format(
            PyExc_TypeError,
            "expected %.200s.__fspath__() to return str or bytes, not %.200s",
            Py_TYPE(PyTuple_GET_ITEM(state->args, 0))->tp_name,
            Py_TYPE(path)->tp_name
        );
        return NULL;
    }
    Py_ssize_t count = PyTuple_GET_SIZE(state->args);
    PyObject *normalized_args = PyTuple_New(count);
    if (normalized_args == NULL) {
        return NULL;
    }
    PyTuple_SET_ITEM(normalized_args, 0, Py_NewRef(path));
    for (Py_ssize_t i = 1; i < count; i++) {
        PyTuple_SET_ITEM(
            normalized_args,
            i,
            Py_NewRef(PyTuple_GET_ITEM(state->args, i))
        );
    }
    state->phase = OPEN_WAIT_OPEN;
    PyObject *result = PyObject_Call(original_open, normalized_args, state->kwargs);
    Py_DECREF(normalized_args);
    return result;
}

static PyObject *
open_with_file_descriptor(OpenState *state, PyObject *descriptor)
{
    Py_ssize_t count = PyTuple_GET_SIZE(state->args);
    PyObject *descriptor_args = PyTuple_New(count);
    if (descriptor_args == NULL) {
        return NULL;
    }
    PyTuple_SET_ITEM(descriptor_args, 0, Py_NewRef(descriptor));
    for (Py_ssize_t i = 1; i < count; i++) {
        PyTuple_SET_ITEM(
            descriptor_args,
            i,
            Py_NewRef(PyTuple_GET_ITEM(state->args, i))
        );
    }
    PyObject *kwargs = state->kwargs == NULL ? NULL : PyDict_Copy(state->kwargs);
    if (state->kwargs != NULL && kwargs == NULL) {
        Py_DECREF(descriptor_args);
        return NULL;
    }
    if (kwargs != NULL && PyDict_DelItemString(kwargs, "opener") < 0) {
        if (!PyErr_ExceptionMatches(PyExc_KeyError)) {
            Py_DECREF(descriptor_args);
            Py_DECREF(kwargs);
            return NULL;
        }
        PyErr_Clear();
    }
    PyObject *result = PyObject_Call(original_open, descriptor_args, kwargs);
    Py_DECREF(descriptor_args);
    Py_XDECREF(kwargs);
    return result;
}

static PyObject *
open_resume(const void *raw_state, PyObject *value)
{
    OpenState *state = (OpenState *)raw_state;
    if (value == NULL) {
        return NULL;
    }
    if (state->phase == OPEN_WAIT_PATH) {
        return open_with_path(state, value);
    }
    if (PyLong_Check(value)) {
        return open_with_file_descriptor(state, value);
    }
    return Py_NewRef(value);
}

static const AleffAdapterVTable open_vtable = {
    .copy_state = open_copy_state,
    .free_state = open_free_state,
    .resume = open_resume,
};

static PyObject *
adapter_open(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    if (PyTuple_GET_SIZE(args) == 0) {
        return PyObject_Call(original_open, args, kwargs);
    }
    PyObject *file = PyTuple_GET_ITEM(args, 0);
    OpenState state = {
        .args = args,
        .kwargs = kwargs,
        .phase = OPEN_WAIT_PATH,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &open_vtable, &state);
    PyObject *result;
    if (PyLong_Check(file)) {
        state.phase = OPEN_WAIT_OPEN;
        result = PyObject_Call(original_open, args, kwargs);
    }
    else {
        PyObject *path = PyOS_FSPath(file);
        result = path == NULL ? NULL : open_with_path(&state, path);
        Py_XDECREF(path);
    }
    adapter_leave(&frame);
    return result;
}

typedef struct {
    PyObject *module_lock;
    PyObject *name;
    PyObject *module;
    PyObject *spec;
    int spec_initializing;
    int module_lock_held;
    int global_lock_held;
} ImportState;

static int
import_module_lock_is_held(PyObject *module_lock)
{
    PyObject *owner = PyObject_GetAttrString(module_lock, "owner");
    if (owner == NULL) {
        return -1;
    }
    if (owner == Py_None) {
        Py_DECREF(owner);
        return 0;
    }
    unsigned long long owner_id = PyLong_AsUnsignedLongLong(owner);
    Py_DECREF(owner);
    if (owner_id == (unsigned long long)-1 && PyErr_Occurred()) {
        return -1;
    }
    return owner_id == (unsigned long long)PyThread_get_thread_ident();
}

static void *
import_copy_state(const void *raw_state)
{
    const ImportState *state = raw_state;
    ImportState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->module_lock = Py_NewRef(state->module_lock);
    copy->name = Py_NewRef(state->name);
    PyObject *modules = PyImport_GetModuleDict();
    PyObject *module = PyDict_GetItemWithError(modules, state->name);
    if (module == NULL && PyErr_Occurred()) {
        Py_DECREF(copy->module_lock);
        Py_DECREF(copy->name);
        PyMem_Free(copy);
        return NULL;
    }
    copy->module = Py_XNewRef(module);
    copy->spec = NULL;
    copy->spec_initializing = 0;
    if (module != NULL) {
        copy->spec = PyObject_GetAttrString(module, "__spec__");
        if (copy->spec == NULL) {
            Py_DECREF(copy->module_lock);
            Py_DECREF(copy->name);
            Py_DECREF(copy->module);
            PyMem_Free(copy);
            return NULL;
        }
        PyObject *initializing = PyObject_GetAttrString(
            copy->spec,
            "_initializing"
        );
        if (initializing == NULL) {
            Py_DECREF(copy->module_lock);
            Py_DECREF(copy->name);
            Py_DECREF(copy->module);
            Py_DECREF(copy->spec);
            PyMem_Free(copy);
            return NULL;
        }
        copy->spec_initializing = PyObject_IsTrue(initializing);
        Py_DECREF(initializing);
        if (copy->spec_initializing < 0) {
            Py_DECREF(copy->module_lock);
            Py_DECREF(copy->name);
            Py_DECREF(copy->module);
            Py_DECREF(copy->spec);
            PyMem_Free(copy);
            return NULL;
        }
    }
    copy->module_lock_held = import_module_lock_is_held(state->module_lock);
    if (copy->module_lock_held < 0) {
        Py_DECREF(copy->module_lock);
        Py_DECREF(copy->name);
        Py_XDECREF(copy->module);
        Py_XDECREF(copy->spec);
        PyMem_Free(copy);
        return NULL;
    }
    PyObject *held = PyObject_CallNoArgs(import_global_lock_held);
    if (held == NULL) {
        Py_DECREF(copy->module_lock);
        Py_DECREF(copy->name);
        Py_XDECREF(copy->module);
        Py_XDECREF(copy->spec);
        PyMem_Free(copy);
        return NULL;
    }
    copy->global_lock_held = PyObject_IsTrue(held);
    Py_DECREF(held);
    if (copy->global_lock_held < 0) {
        Py_DECREF(copy->module_lock);
        Py_DECREF(copy->name);
        Py_XDECREF(copy->module);
        Py_XDECREF(copy->spec);
        PyMem_Free(copy);
        return NULL;
    }
    return copy;
}

static void
import_free_state(void *raw_state)
{
    ImportState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->module_lock);
    Py_DECREF(state->name);
    Py_XDECREF(state->module);
    Py_XDECREF(state->spec);
    PyMem_Free(state);
}

static int
import_prepare_resume(void *raw_state)
{
    ImportState *state = raw_state;
    PyObject *modules = PyImport_GetModuleDict();
    if (state->module != NULL) {
        if (PyDict_SetItem(modules, state->name, state->module) < 0) {
            return -1;
        }
    }
    else if (PyDict_DelItem(modules, state->name) < 0) {
        if (!PyErr_ExceptionMatches(PyExc_KeyError)) {
            return -1;
        }
        PyErr_Clear();
    }
    if (state->spec != NULL) {
        if (
            PyObject_SetAttrString(
                state->spec,
                "_initializing",
                state->spec_initializing ? Py_True : Py_False
            ) < 0
        ) {
            return -1;
        }
    }
    if (state->module_lock_held) {
        PyObject *acquired = PyObject_CallMethod(
            state->module_lock,
            "acquire",
            NULL
        );
        if (acquired == NULL) {
            return -1;
        }
        Py_DECREF(acquired);
    }
    if (state->global_lock_held) {
        PyObject *acquired = PyObject_CallNoArgs(import_global_lock_acquire);
        if (acquired == NULL) {
            if (state->module_lock_held) {
                PyObject *released = PyObject_CallMethod(
                    state->module_lock,
                    "release",
                    NULL
                );
                Py_XDECREF(released);
            }
            return -1;
        }
        Py_DECREF(acquired);
    }
    return 0;
}

static PyObject *
import_resume(const void *Py_UNUSED(state), PyObject *value)
{
    return Py_XNewRef(value);
}

static const AleffAdapterVTable import_vtable = {
    .copy_state = import_copy_state,
    .free_state = import_free_state,
    .resume = import_resume,
    .prepare_resume = import_prepare_resume,
};

static PyObject *
adapter_import(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    if (PyTuple_GET_SIZE(args) == 0) {
        return PyObject_Call(original_import, args, kwargs);
    }
    PyObject *name = PyTuple_GET_ITEM(args, 0);
    if (!PyUnicode_Check(name)) {
        return PyObject_Call(original_import, args, kwargs);
    }
    PyObject *module_lock = PyObject_CallOneArg(import_get_module_lock, name);
    if (module_lock == NULL) {
        return NULL;
    }
    ImportState state = {
        .module_lock = module_lock,
        .name = name,
        .module = NULL,
        .spec = NULL,
        .spec_initializing = 0,
        .module_lock_held = 0,
        .global_lock_held = 0,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &import_vtable, &state);
    PyObject *result = PyObject_Call(original_import, args, kwargs);
    adapter_leave(&frame);
    Py_DECREF(module_lock);
    return result;
}

typedef enum {
    PRINT_WAIT_FLUSH_BOOL,
    PRINT_WAIT_SEPARATOR,
    PRINT_WAIT_STRING,
    PRINT_WAIT_ARGUMENT,
    PRINT_WAIT_END,
    PRINT_WAIT_FLUSH,
} PrintPhase;

typedef struct {
    PyObject *args;
    PyObject *file;
    PyObject *separator;
    PyObject *end;
    PyObject *text;
    Py_ssize_t index;
    int flush;
    PrintPhase phase;
} PrintState;

static void *
print_copy_state(const void *raw_state)
{
    const PrintState *state = raw_state;
    PrintState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->args = Py_NewRef(state->args);
    copy->file = Py_NewRef(state->file);
    copy->separator = Py_NewRef(state->separator);
    copy->end = Py_NewRef(state->end);
    copy->text = Py_XNewRef(state->text);
    return copy;
}

static void
print_free_state(void *raw_state)
{
    PrintState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->args);
    Py_DECREF(state->file);
    Py_DECREF(state->separator);
    Py_DECREF(state->end);
    Py_XDECREF(state->text);
    PyMem_Free(state);
}

static PyObject *print_continue(PrintState *state, PyObject *value, int resuming);

static PyObject *
print_write(PrintState *state, PyObject *text, PrintPhase phase)
{
    state->phase = phase;
    PyObject *result = PyObject_CallMethod(state->file, "write", "O", text);
    if (result == NULL) {
        return NULL;
    }
    Py_DECREF(result);
    return print_continue(state, NULL, 0);
}

static PyObject *
print_next_argument(PrintState *state)
{
    Py_ssize_t count = PyTuple_GET_SIZE(state->args);
    if (state->index >= count) {
        return print_write(state, state->end, PRINT_WAIT_END);
    }
    if (state->index > 0) {
        return print_write(state, state->separator, PRINT_WAIT_SEPARATOR);
    }
    state->phase = PRINT_WAIT_STRING;
    PyObject *text = PyObject_Str(PyTuple_GET_ITEM(state->args, state->index));
    if (text == NULL) {
        return NULL;
    }
    Py_XSETREF(state->text, text);
    return print_write(state, state->text, PRINT_WAIT_ARGUMENT);
}

static PyObject *
print_continue(PrintState *state, PyObject *value, int resuming)
{
    if (resuming && value == NULL) {
        return NULL;
    }
    if (resuming) {
        switch (state->phase) {
            case PRINT_WAIT_FLUSH_BOOL: {
                int truth = PyObject_IsTrue(value);
                if (truth < 0) {
                    return NULL;
                }
                state->flush = truth;
                return print_next_argument(state);
            }
            case PRINT_WAIT_SEPARATOR:
                state->phase = PRINT_WAIT_STRING;
                break;
            case PRINT_WAIT_STRING:
                if (!PyUnicode_Check(value)) {
                    PyErr_Format(
                        PyExc_TypeError,
                        "__str__ returned non-string (type %.200s)",
                        Py_TYPE(value)->tp_name
                    );
                    return NULL;
                }
                Py_XSETREF(state->text, Py_NewRef(value));
                return print_write(state, state->text, PRINT_WAIT_ARGUMENT);
            case PRINT_WAIT_ARGUMENT:
                state->index++;
                return print_next_argument(state);
            case PRINT_WAIT_END:
                if (!state->flush) {
                    return Py_NewRef(Py_None);
                }
                state->phase = PRINT_WAIT_FLUSH;
                PyObject *flushed = PyObject_CallMethod(state->file, "flush", NULL);
                if (flushed == NULL) {
                    return NULL;
                }
                Py_DECREF(flushed);
                return Py_NewRef(Py_None);
            case PRINT_WAIT_FLUSH:
                return Py_NewRef(Py_None);
        }
    }
    if (state->phase == PRINT_WAIT_SEPARATOR) {
        state->phase = PRINT_WAIT_STRING;
    }
    if (state->phase == PRINT_WAIT_STRING) {
        PyObject *text = PyObject_Str(PyTuple_GET_ITEM(state->args, state->index));
        if (text == NULL) {
            return NULL;
        }
        Py_XSETREF(state->text, text);
        return print_write(state, state->text, PRINT_WAIT_ARGUMENT);
    }
    if (state->phase == PRINT_WAIT_ARGUMENT) {
        state->index++;
        return print_next_argument(state);
    }
    if (state->phase == PRINT_WAIT_END) {
        if (!state->flush) {
            return Py_NewRef(Py_None);
        }
        state->phase = PRINT_WAIT_FLUSH;
        PyObject *flushed = PyObject_CallMethod(state->file, "flush", NULL);
        if (flushed == NULL) {
            return NULL;
        }
        Py_DECREF(flushed);
        return Py_NewRef(Py_None);
    }
    return Py_NewRef(Py_None);
}

static PyObject *
print_resume(const void *raw_state, PyObject *value)
{
    return print_continue((PrintState *)raw_state, value, 1);
}

static const AleffAdapterVTable print_vtable = {
    .copy_state = print_copy_state,
    .free_state = print_free_state,
    .resume = print_resume,
};

static PyObject *
adapter_print(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    PyObject *separator = Py_None;
    PyObject *end = Py_None;
    PyObject *file = Py_None;
    PyObject *flush_object = Py_False;
    if (kwargs != NULL) {
        Py_ssize_t position = 0;
        PyObject *key;
        PyObject *item;
        while (PyDict_Next(kwargs, &position, &key, &item)) {
            if (!PyUnicode_Check(key)) {
                PyErr_SetString(PyExc_TypeError, "keywords must be strings");
                return NULL;
            }
            const char *name = PyUnicode_AsUTF8(key);
            if (name == NULL) {
                return NULL;
            }
            if (strcmp(name, "sep") == 0) {
                separator = item;
            }
            else if (strcmp(name, "end") == 0) {
                end = item;
            }
            else if (strcmp(name, "file") == 0) {
                file = item;
            }
            else if (strcmp(name, "flush") == 0) {
                flush_object = item;
            }
            else {
                PyErr_Format(PyExc_TypeError, "'%.200s' is an invalid keyword argument for print()", name);
                return NULL;
            }
        }
    }
    if (separator != Py_None && !PyUnicode_Check(separator)) {
        PyErr_Format(
            PyExc_TypeError,
            "sep must be None or a string, not %.200s",
            Py_TYPE(separator)->tp_name
        );
        return NULL;
    }
    if (end != Py_None && !PyUnicode_Check(end)) {
        PyErr_Format(
            PyExc_TypeError,
            "end must be None or a string, not %.200s",
            Py_TYPE(end)->tp_name
        );
        return NULL;
    }
    PyObject *default_separator = NULL;
    PyObject *default_end = NULL;
    if (separator == Py_None) {
        default_separator = PyUnicode_FromString(" ");
        if (default_separator == NULL) {
            return NULL;
        }
        separator = default_separator;
    }
    if (end == Py_None) {
        default_end = PyUnicode_FromString("\n");
        if (default_end == NULL) {
            Py_XDECREF(default_separator);
            return NULL;
        }
        end = default_end;
    }
    if (file == Py_None) {
        file = PySys_GetObject("stdout");
        if (file == NULL || file == Py_None) {
            Py_XDECREF(default_separator);
            Py_XDECREF(default_end);
            return Py_NewRef(Py_None);
        }
    }
    PrintState state = {
        .args = args,
        .file = file,
        .separator = separator,
        .end = end,
        .text = NULL,
        .index = 0,
        .flush = 0,
        .phase = PRINT_WAIT_FLUSH_BOOL,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &print_vtable, &state);
    int flush = PyObject_IsTrue(flush_object);
    PyObject *result = NULL;
    if (flush >= 0) {
        state.flush = flush;
        result = print_next_argument(&state);
    }
    adapter_leave(&frame);
    Py_XDECREF(state.text);
    Py_XDECREF(default_separator);
    Py_XDECREF(default_end);
    return result;
}

typedef enum {
    TYPE_CONSTRUCTION_UNKNOWN,
    TYPE_CONSTRUCTION_BODY,
    TYPE_CONSTRUCTION_SET_NAME,
    TYPE_CONSTRUCTION_INIT_SUBCLASS,
} TypeConstructionPhase;

typedef struct {
    PyObject *build_args;
    PyObject *build_kwargs;
    PyObject *body_code;
    PyObject *namespace;
    PyObject *owner;
    PyObject *current_name;
    TypeConstructionPhase phase;
} TypeConstructionState;

static PyObject *original_build_class = NULL;
static vectorcallfunc original_type_vectorcall = NULL;

static int
type_construction_capture_frame(TypeConstructionState *state)
{
    PyFrameObject *frame = PyThreadState_GetFrame(PyThreadState_Get());
    while (frame != NULL) {
        PyCodeObject *code = PyFrame_GetCode(frame);
        if (code == NULL) {
            Py_DECREF(frame);
            return -1;
        }
        PyObject *name = PyObject_GetAttrString((PyObject *)code, "co_name");
        if (name == NULL) {
            Py_DECREF(code);
            Py_DECREF(frame);
            return -1;
        }
        int is_set_name = PyUnicode_Check(name) &&
            PyUnicode_CompareWithASCIIString(name, "__set_name__") == 0;
        int is_init_subclass = PyUnicode_Check(name) &&
            PyUnicode_CompareWithASCIIString(name, "__init_subclass__") == 0;
        Py_DECREF(name);
        if (is_set_name || is_init_subclass) {
            PyObject *varnames = PyCode_GetVarnames(code);
            PyObject *locals = PyFrame_GetLocals(frame);
            if (varnames == NULL || locals == NULL) {
                Py_XDECREF(varnames);
                Py_XDECREF(locals);
                Py_DECREF(code);
                Py_DECREF(frame);
                return -1;
            }
            Py_ssize_t owner_index = is_set_name ? 1 : 0;
            if (PyTuple_GET_SIZE(varnames) > owner_index) {
                PyObject *owner_name = PyTuple_GET_ITEM(varnames, owner_index);
                PyObject *owner = PyObject_GetItem(locals, owner_name);
                if (owner != NULL && PyType_Check(owner)) {
                    Py_XSETREF(state->owner, owner);
                    owner = NULL;
                    state->phase = is_set_name
                        ? TYPE_CONSTRUCTION_SET_NAME
                        : TYPE_CONSTRUCTION_INIT_SUBCLASS;
                    if (is_set_name && PyTuple_GET_SIZE(varnames) > 2) {
                        PyObject *name_name = PyTuple_GET_ITEM(varnames, 2);
                        PyObject *current_name = PyObject_GetItem(locals, name_name);
                        if (current_name != NULL) {
                            Py_XSETREF(state->current_name, current_name);
                        }
                        else {
                            PyErr_Clear();
                        }
                    }
                    Py_XDECREF(owner);
                    Py_DECREF(locals);
                    Py_DECREF(varnames);
                    Py_DECREF(code);
                    Py_DECREF(frame);
                    return 0;
                }
                Py_XDECREF(owner);
                PyErr_Clear();
            }
            Py_DECREF(locals);
            Py_DECREF(varnames);
        }
        if (state->body_code != NULL && (PyObject *)code == state->body_code) {
            PyObject *namespace = PyFrame_GetLocals(frame);
            if (namespace == NULL) {
                Py_DECREF(code);
                Py_DECREF(frame);
                return -1;
            }
            Py_XSETREF(state->namespace, namespace);
            state->phase = TYPE_CONSTRUCTION_BODY;
            Py_DECREF(code);
            Py_DECREF(frame);
            return 0;
        }
        PyFrameObject *back = PyFrame_GetBack(frame);
        Py_DECREF(code);
        Py_DECREF(frame);
        frame = back;
    }
    return 0;
}

static void *
type_construction_copy_state(const void *raw_state)
{
    const TypeConstructionState *state = raw_state;
    TypeConstructionState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->build_args = Py_XNewRef(state->build_args);
    copy->build_kwargs = Py_XNewRef(state->build_kwargs);
    copy->body_code = Py_XNewRef(state->body_code);
    copy->namespace = Py_XNewRef(state->namespace);
    copy->owner = Py_XNewRef(state->owner);
    copy->current_name = Py_XNewRef(state->current_name);
    if (type_construction_capture_frame(copy) < 0) {
        Py_XDECREF(copy->build_args);
        Py_XDECREF(copy->build_kwargs);
        Py_XDECREF(copy->body_code);
        Py_XDECREF(copy->namespace);
        Py_XDECREF(copy->owner);
        Py_XDECREF(copy->current_name);
        PyMem_Free(copy);
        return NULL;
    }
    return copy;
}

static void
type_construction_free_state(void *raw_state)
{
    TypeConstructionState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->build_args);
    Py_XDECREF(state->build_kwargs);
    Py_XDECREF(state->body_code);
    Py_XDECREF(state->namespace);
    Py_XDECREF(state->owner);
    Py_XDECREF(state->current_name);
    PyMem_Free(state);
}

static PyObject *type_construction_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable type_construction_vtable = {
    .copy_state = type_construction_copy_state,
    .free_state = type_construction_free_state,
    .resume = type_construction_resume,
};

static PyObject *
type_construction_init_subclass(TypeConstructionState *state)
{
    PyObject *super = PyObject_CallFunctionObjArgs(
        (PyObject *)&PySuper_Type,
        state->owner,
        state->owner,
        NULL
    );
    if (super == NULL) {
        return NULL;
    }
    PyObject *method = PyObject_GetAttrString(super, "__init_subclass__");
    Py_DECREF(super);
    if (method == NULL) {
        return NULL;
    }
    PyObject *empty = PyTuple_New(0);
    if (empty == NULL) {
        Py_DECREF(method);
        return NULL;
    }
    state->phase = TYPE_CONSTRUCTION_INIT_SUBCLASS;
    PyObject *result = PyObject_Call(method, empty, NULL);
    Py_DECREF(empty);
    Py_DECREF(method);
    if (result == NULL) {
        return NULL;
    }
    Py_DECREF(result);
    return Py_NewRef(state->owner);
}

static PyObject *
type_construction_continue_set_names(TypeConstructionState *state)
{
    PyObject *dictionary = PyType_GetDict((PyTypeObject *)state->owner);
    PyObject *keys = PyDict_Keys(dictionary);
    if (keys == NULL) {
        return NULL;
    }
    Py_ssize_t start = 0;
    if (state->current_name != NULL) {
        Py_ssize_t count = PyList_GET_SIZE(keys);
        for (Py_ssize_t i = 0; i < count; i++) {
            int equal = PyObject_RichCompareBool(
                PyList_GET_ITEM(keys, i),
                state->current_name,
                Py_EQ
            );
            if (equal < 0) {
                Py_DECREF(keys);
                return NULL;
            }
            if (equal) {
                start = i + 1;
                break;
            }
        }
    }
    Py_ssize_t count = PyList_GET_SIZE(keys);
    for (Py_ssize_t i = start; i < count; i++) {
        PyObject *name = PyList_GET_ITEM(keys, i);
        PyObject *descriptor = PyDict_GetItemWithError(dictionary, name);
        if (descriptor == NULL) {
            if (PyErr_Occurred()) {
                Py_DECREF(keys);
                return NULL;
            }
            continue;
        }
        PyObject *set_name = lookup_raw_special(descriptor, "__set_name__");
        if (set_name == NULL) {
            if (PyErr_Occurred()) {
                Py_DECREF(keys);
                return NULL;
            }
            continue;
        }
        Py_DECREF(set_name);
        Py_XSETREF(state->current_name, Py_NewRef(name));
        state->phase = TYPE_CONSTRUCTION_SET_NAME;
        PyObject *called = PyObject_CallMethod(
            descriptor,
            "__set_name__",
            "OO",
            state->owner,
            name
        );
        if (called == NULL) {
            Py_DECREF(keys);
            return NULL;
        }
        Py_DECREF(called);
    }
    Py_DECREF(keys);
    return type_construction_init_subclass(state);
}

static PyObject *
type_construction_build_from_namespace(TypeConstructionState *state)
{
    Py_ssize_t count = PyTuple_GET_SIZE(state->build_args);
    if (count < 2 || state->namespace == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "incomplete __build_class__ adapter state");
        return NULL;
    }
    PyObject *name = PyTuple_GET_ITEM(state->build_args, 1);
    PyObject *bases = PyTuple_GetSlice(state->build_args, 2, count);
    if (bases == NULL) {
        return NULL;
    }
    PyObject *keywords = state->build_kwargs == NULL
        ? PyDict_New()
        : PyDict_Copy(state->build_kwargs);
    if (keywords == NULL) {
        Py_DECREF(bases);
        return NULL;
    }
    PyObject *metaclass = PyDict_GetItemString(keywords, "metaclass");
    Py_XINCREF(metaclass);
    if (PyDict_DelItemString(keywords, "metaclass") < 0) {
        PyErr_Clear();
    }
    if (metaclass == NULL) {
        metaclass = PyTuple_GET_SIZE(bases) == 0
            ? Py_NewRef((PyObject *)&PyType_Type)
            : Py_NewRef((PyObject *)Py_TYPE(PyTuple_GET_ITEM(bases, 0)));
    }
    PyObject *call_args = PyTuple_Pack(3, name, bases, state->namespace);
    Py_DECREF(bases);
    if (call_args == NULL) {
        Py_DECREF(metaclass);
        Py_DECREF(keywords);
        return NULL;
    }
    PyObject *result = PyObject_Call(metaclass, call_args, keywords);
    Py_DECREF(call_args);
    Py_DECREF(metaclass);
    Py_DECREF(keywords);
    return result;
}

static PyObject *
type_construction_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    TypeConstructionState *state = type_construction_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &type_construction_vtable, state);
    PyObject *result;
    if (state->phase == TYPE_CONSTRUCTION_BODY) {
        result = type_construction_build_from_namespace(state);
    }
    else if (state->phase == TYPE_CONSTRUCTION_SET_NAME && state->owner != NULL) {
        result = type_construction_continue_set_names(state);
    }
    else if (
        state->phase == TYPE_CONSTRUCTION_INIT_SUBCLASS &&
        state->owner != NULL
    ) {
        result = Py_NewRef(state->owner);
    }
    else {
        result = Py_NewRef(value);
    }
    adapter_leave(&frame);
    type_construction_free_state(state);
    return result;
}

static PyObject *
adapter_build_class(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    PyObject *body_code = NULL;
    if (PyTuple_GET_SIZE(args) > 0) {
        body_code = PyObject_GetAttrString(PyTuple_GET_ITEM(args, 0), "__code__");
        if (body_code == NULL) {
            PyErr_Clear();
        }
    }
    TypeConstructionState state = {
        .build_args = args,
        .build_kwargs = kwargs,
        .body_code = body_code,
        .namespace = NULL,
        .owner = NULL,
        .current_name = NULL,
        .phase = TYPE_CONSTRUCTION_UNKNOWN,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &type_construction_vtable, &state);
    PyObject *result = PyObject_Call(original_build_class, args, kwargs);
    adapter_leave(&frame);
    Py_XDECREF(body_code);
    return result;
}

static PyObject *
adapter_type_vectorcall(
    PyObject *callable,
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames
)
{
    if (
        callable != (PyObject *)&PyType_Type ||
        PyVectorcall_NARGS(nargsf) != 3
    ) {
        return original_type_vectorcall(callable, args, nargsf, kwnames);
    }
    TypeConstructionState state = {
        .build_args = NULL,
        .build_kwargs = NULL,
        .body_code = NULL,
        .namespace = NULL,
        .owner = NULL,
        .current_name = NULL,
        .phase = TYPE_CONSTRUCTION_UNKNOWN,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &type_construction_vtable, &state);
    PyObject *result = original_type_vectorcall(callable, args, nargsf, kwnames);
    adapter_leave(&frame);
    return result;
}

typedef struct {
    int base;
} NumberBaseState;

static void *
number_base_copy_state(const void *raw_state)
{
    const NumberBaseState *state = raw_state;
    NumberBaseState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->base = state->base;
    return copy;
}

static PyObject *
number_base_resume(const void *raw_state, PyObject *value)
{
    const NumberBaseState *state = raw_state;
    if (value == NULL) {
        return NULL;
    }
    return PyNumber_ToBase(value, state->base);
}

static const AleffAdapterVTable number_base_vtable = {
    .copy_state = number_base_copy_state,
    .free_state = empty_free_state,
    .resume = number_base_resume,
};

static PyObject *
adapter_number_base(PyObject *object, int base)
{
    NumberBaseState state = {.base = base};
    AleffAdapterFrame frame;
    adapter_enter(&frame, &number_base_vtable, &state);
    PyObject *result = PyNumber_ToBase(object, base);
    adapter_leave(&frame);
    return result;
}

static PyObject *
adapter_bin(PyObject *Py_UNUSED(self), PyObject *object)
{
    return adapter_number_base(object, 2);
}

static PyObject *
adapter_oct(PyObject *Py_UNUSED(self), PyObject *object)
{
    return adapter_number_base(object, 8);
}

static PyObject *
adapter_hex(PyObject *Py_UNUSED(self), PyObject *object)
{
    return adapter_number_base(object, 16);
}

typedef enum {
    EXTREME_WAIT_NEXT,
    EXTREME_WAIT_KEY,
    EXTREME_WAIT_COMPARE,
} ExtremePhase;

typedef struct {
    PyObject *iterator;
    PyObject *key_function;
    PyObject *default_value;
    PyObject *best_item;
    PyObject *best_value;
    PyObject *current_item;
    PyObject *current_value;
    int comparison_op;
    const char *name;
    ExtremePhase phase;
} ExtremeState;

static void *
extreme_copy_state(const void *raw_state)
{
    const ExtremeState *state = raw_state;
    ExtremeState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = (ExtremeState){
        .iterator = Py_NewRef(state->iterator),
        .key_function = Py_XNewRef(state->key_function),
        .default_value = Py_XNewRef(state->default_value),
        .best_item = Py_XNewRef(state->best_item),
        .best_value = Py_XNewRef(state->best_value),
        .current_item = Py_XNewRef(state->current_item),
        .current_value = Py_XNewRef(state->current_value),
        .comparison_op = state->comparison_op,
        .name = state->name,
        .phase = state->phase,
    };
    return copy;
}

static void
extreme_free_state(void *raw_state)
{
    ExtremeState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->iterator);
    Py_XDECREF(state->key_function);
    Py_XDECREF(state->default_value);
    Py_XDECREF(state->best_item);
    Py_XDECREF(state->best_value);
    Py_XDECREF(state->current_item);
    Py_XDECREF(state->current_value);
    PyMem_Free(state);
}

static PyObject *
extreme_finish(ExtremeState *state)
{
    if (state->best_item != NULL) {
        return Py_NewRef(state->best_item);
    }
    if (state->default_value != NULL) {
        return Py_NewRef(state->default_value);
    }
    PyErr_Format(PyExc_ValueError, "%s() arg is an empty sequence", state->name);
    return NULL;
}

static PyObject *extreme_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable extreme_vtable = {
    .copy_state = extreme_copy_state,
    .free_state = extreme_free_state,
    .resume = extreme_resume,
};

static PyObject *
extreme_continue(ExtremeState *state, PyObject *resumed_value, int is_resumed)
{
    int comparison = -1;
    if (is_resumed) {
        switch (state->phase) {
            case EXTREME_WAIT_NEXT:
                state->current_item = Py_NewRef(resumed_value);
                break;
            case EXTREME_WAIT_KEY:
                state->current_value = Py_NewRef(resumed_value);
                break;
            case EXTREME_WAIT_COMPARE:
                comparison = PyObject_IsTrue(resumed_value);
                break;
        }
    }

    for (;;) {
        if (state->current_item == NULL) {
            state->phase = EXTREME_WAIT_NEXT;
            state->current_item = Py_TYPE(state->iterator)->tp_iternext(state->iterator);
            if (state->current_item == NULL) {
                if (PyErr_Occurred()) {
                    if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                        return NULL;
                    }
                    PyErr_Clear();
                }
                return extreme_finish(state);
            }
        }

        if (state->current_value == NULL) {
            if (state->key_function == NULL) {
                state->current_value = Py_NewRef(state->current_item);
            }
            else {
                state->phase = EXTREME_WAIT_KEY;
                state->current_value = PyObject_CallOneArg(
                    state->key_function,
                    state->current_item
                );
                if (state->current_value == NULL) {
                    return NULL;
                }
            }
        }

        if (state->best_item == NULL) {
            state->best_item = state->current_item;
            state->current_item = NULL;
            state->best_value = state->current_value;
            state->current_value = NULL;
            continue;
        }

        if (comparison < 0) {
            state->phase = EXTREME_WAIT_COMPARE;
            comparison = PyObject_RichCompareBool(
                state->current_value,
                state->best_value,
                state->comparison_op
            );
        }
        if (comparison < 0) {
            return NULL;
        }
        if (comparison > 0) {
            Py_SETREF(state->best_item, state->current_item);
            state->current_item = NULL;
            Py_SETREF(state->best_value, state->current_value);
            state->current_value = NULL;
        }
        else {
            Py_CLEAR(state->current_item);
            Py_CLEAR(state->current_value);
        }
        comparison = -1;
    }
}

static PyObject *
extreme_resume(const void *raw_state, PyObject *value)
{
    const ExtremeState *source = raw_state;
    if (value == NULL && source->phase != EXTREME_WAIT_NEXT) {
        return NULL;
    }
    if (value == NULL && PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
        return NULL;
    }

    ExtremeState *state = extreme_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &extreme_vtable, state);
    PyObject *result;
    if (value == NULL) {
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        result = extreme_finish(state);
    }
    else {
        result = extreme_continue(state, value, 1);
    }
    adapter_leave(&frame);
    extreme_free_state(state);
    return result;
}

static PyObject *
adapter_extreme(PyObject *args, PyObject *kwargs, int comparison_op, const char *name)
{
    Py_ssize_t positional_count = PyTuple_GET_SIZE(args);
    if (positional_count == 0) {
        PyErr_Format(PyExc_TypeError, "%s expected at least 1 argument, got 0", name);
        return NULL;
    }

    PyObject *key_function = NULL;
    PyObject *default_value = NULL;
    static char *keyword_names[] = {"key", "default", NULL};
    PyObject *empty = PyTuple_New(0);
    if (empty == NULL) {
        return NULL;
    }
    char format[16];
    PyOS_snprintf(format, sizeof(format), "|$OO:%s", name);
    int parsed = PyArg_ParseTupleAndKeywords(
        empty,
        kwargs,
        format,
        keyword_names,
        &key_function,
        &default_value
    );
    Py_DECREF(empty);
    if (!parsed) {
        return NULL;
    }
    if (positional_count > 1 && default_value != NULL) {
        PyErr_Format(
            PyExc_TypeError,
            "Cannot specify a default for %s() with multiple positional arguments",
            name
        );
        return NULL;
    }
    if (key_function == Py_None) {
        key_function = NULL;
    }

    PyObject *iterable = positional_count > 1 ? args : PyTuple_GET_ITEM(args, 0);
    PyObject *iterator = PyObject_GetIter(iterable);
    if (iterator == NULL) {
        return NULL;
    }
    ExtremeState state = {
        .iterator = iterator,
        .key_function = key_function,
        .default_value = default_value,
        .best_item = NULL,
        .best_value = NULL,
        .current_item = NULL,
        .current_value = NULL,
        .comparison_op = comparison_op,
        .name = name,
        .phase = EXTREME_WAIT_NEXT,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &extreme_vtable, &state);
    PyObject *result = extreme_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_DECREF(iterator);
    Py_XDECREF(state.best_item);
    Py_XDECREF(state.best_value);
    Py_XDECREF(state.current_item);
    Py_XDECREF(state.current_value);
    return result;
}

static PyObject *
adapter_min(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    return adapter_extreme(args, kwargs, Py_LT, "min");
}

static PyObject *
adapter_max(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    return adapter_extreme(args, kwargs, Py_GT, "max");
}
