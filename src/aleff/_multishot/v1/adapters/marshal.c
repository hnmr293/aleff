#include "marshal.h"

#include "marshal_stream.h"

typedef enum {
    MARSHAL_DUMP,
    MARSHAL_DUMPS,
    MARSHAL_LOAD,
    MARSHAL_LOADS,
} MarshalOperation;

typedef enum {
    MARSHAL_WAIT_VERSION,
    MARSHAL_WAIT_ALLOW_CODE,
    MARSHAL_WAIT_STREAM,
    MARSHAL_READY,
} MarshalPhase;

typedef struct {
    PyObject *original;
    PyObject *args;
    PyObject *kwargs;
    PyObject *version_object;
    PyObject *allow_code_object;
    PyObject *normalized_version;
    PyObject *normalized_allow_code;
    MarshalOperation operation;
    MarshalPhase phase;
} MarshalState;

static const AleffAdapterVTable marshal_vtable;
static const char *const marshal_names[] = {"dump", "dumps", "load", "loads"};
static PyObject *original_functions[4];
static PyObject *installed_module;
static PyMethodDef replacement_methods[4];
static int marshal_installed;

static void
marshal_clear_state(MarshalState *state)
{
    Py_CLEAR(state->normalized_allow_code);
    Py_CLEAR(state->normalized_version);
    Py_CLEAR(state->allow_code_object);
    Py_CLEAR(state->version_object);
    Py_CLEAR(state->kwargs);
    Py_CLEAR(state->args);
    Py_CLEAR(state->original);
}

static void
marshal_free_state(void *raw_state)
{
    MarshalState *state = raw_state;
    if (state == NULL) {
        return;
    }
    marshal_clear_state(state);
    PyMem_Free(state);
}

static void *
marshal_copy_state(const void *raw_state)
{
    const MarshalState *source = raw_state;
    MarshalState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->original = Py_NewRef(source->original);
    copy->args = Py_NewRef(source->args);
    copy->kwargs = Py_XNewRef(source->kwargs);
    copy->version_object = Py_XNewRef(source->version_object);
    copy->allow_code_object = Py_XNewRef(source->allow_code_object);
    copy->normalized_version = Py_XNewRef(source->normalized_version);
    copy->normalized_allow_code = Py_XNewRef(source->normalized_allow_code);
    return copy;
}

static PyObject *
marshal_call_original(MarshalState *state)
{
    if (state->operation == MARSHAL_LOAD) {
        int allow_code = 1;
#if PY_VERSION_HEX >= 0x030d0000
        PyObject *allow_code_object = state->normalized_allow_code;
        if (allow_code_object == NULL && state->kwargs != NULL) {
            allow_code_object = PyDict_GetItemString(
                state->kwargs,
                "allow_code"
            );
        }
        if (allow_code_object != NULL) {
            allow_code = allow_code_object == Py_True;
        }
#endif
        AleffMarshalStream *stream = aleff_marshal_stream_new(
            state->original,
            original_functions[MARSHAL_LOADS],
            PyTuple_GET_ITEM(state->args, 0),
            allow_code
        );
        if (stream == NULL) {
            return NULL;
        }
        state->phase = MARSHAL_WAIT_STREAM;
        PyObject *result = aleff_marshal_stream_run(stream);
        aleff_marshal_stream_free(stream);
        return result;
    }

    Py_ssize_t size = PyTuple_GET_SIZE(state->args);
    PyObject *args = PyTuple_New(size);
    if (args == NULL) {
        return NULL;
    }
    Py_ssize_t version_index = state->operation == MARSHAL_DUMP ? 2 : 1;
    for (Py_ssize_t index = 0; index < size; index++) {
        PyObject *item = state->normalized_version != NULL &&
            index == version_index
            ? state->normalized_version
            : PyTuple_GET_ITEM(state->args, index);
        PyTuple_SET_ITEM(args, index, Py_NewRef(item));
    }
    PyObject *kwargs = state->kwargs == NULL
        ? NULL
        : PyDict_Copy(state->kwargs);
    if (state->kwargs != NULL && kwargs == NULL) {
        Py_DECREF(args);
        return NULL;
    }
    if (state->normalized_allow_code != NULL &&
        PyDict_SetItemString(
            kwargs,
            "allow_code",
            state->normalized_allow_code
        ) < 0) {
        Py_DECREF(args);
        Py_DECREF(kwargs);
        return NULL;
    }
    PyObject *result = PyObject_Call(state->original, args, kwargs);
    Py_DECREF(args);
    Py_XDECREF(kwargs);
    return result;
}

static int
marshal_normalize_version(MarshalState *state, PyObject *value)
{
    PyObject *index = PyNumber_Index(value);
    if (index == NULL) {
        return -1;
    }
    Py_XSETREF(state->normalized_version, index);
    state->phase = state->allow_code_object == NULL
        ? MARSHAL_READY
        : MARSHAL_WAIT_ALLOW_CODE;
    return 0;
}

static int
marshal_normalize_resumed_version(MarshalState *state, PyObject *value)
{
    if (!PyLong_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "__index__ returned non-int (type %.200s)",
            Py_TYPE(value)->tp_name
        );
        return -1;
    }
    PyObject *index = PyNumber_Index(value);
    if (index == NULL) {
        return -1;
    }
    Py_XSETREF(state->normalized_version, index);
    state->phase = state->allow_code_object == NULL
        ? MARSHAL_READY
        : MARSHAL_WAIT_ALLOW_CODE;
    return 0;
}

static int
marshal_normalize_allow_code(MarshalState *state, PyObject *value)
{
    int truth = PyObject_IsTrue(value);
    if (truth < 0) {
        return -1;
    }
    PyObject *normalized = truth ? Py_True : Py_False;
    Py_INCREF(normalized);
    Py_XSETREF(state->normalized_allow_code, normalized);
    state->phase = MARSHAL_READY;
    return 0;
}

static int
marshal_normalize_resumed_allow_code(MarshalState *state, PyObject *value)
{
    if (!PyBool_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "__bool__ should return bool, returned %.200s",
            Py_TYPE(value)->tp_name
        );
        return -1;
    }
    Py_XSETREF(state->normalized_allow_code, Py_NewRef(value));
    state->phase = MARSHAL_READY;
    return 0;
}

static PyObject *
marshal_continue(MarshalState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed) {
        if (state->phase == MARSHAL_WAIT_STREAM) {
            return Py_NewRef(resumed_value);
        }
        if (state->phase == MARSHAL_WAIT_VERSION) {
            if (marshal_normalize_resumed_version(state, resumed_value) < 0) {
                return NULL;
            }
        }
        else if (state->phase == MARSHAL_WAIT_ALLOW_CODE) {
            if (marshal_normalize_resumed_allow_code(state, resumed_value) < 0) {
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "invalid marshal resume phase");
            return NULL;
        }
    }

    if (state->phase == MARSHAL_WAIT_VERSION) {
        if (marshal_normalize_version(state, state->version_object) < 0) {
            return NULL;
        }
    }
    if (state->phase == MARSHAL_WAIT_ALLOW_CODE) {
        if (marshal_normalize_allow_code(state, state->allow_code_object) < 0) {
            return NULL;
        }
    }
    return marshal_call_original(state);
}

static PyObject *
marshal_resume(const void *raw_state, PyObject *value)
{
    MarshalState *state = marshal_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    if (value == NULL) {
        marshal_free_state(state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &marshal_vtable, state) < 0) {
        marshal_free_state(state);
        return NULL;
    }
    PyObject *result = marshal_continue(state, value, 1);
    adapter_leave(&frame);
    marshal_free_state(state);
    return result;
}

static const AleffAdapterVTable marshal_vtable = {
    .copy_state = marshal_copy_state,
    .free_state = marshal_free_state,
    .resume = marshal_resume,
    .prepare_resume = NULL,
};

#if PY_VERSION_HEX >= 0x030d0000
static int
marshal_keyword_is_allow_code(PyObject *name)
{
    return PyUnicode_Check(name) &&
        PyUnicode_CompareWithASCIIString(name, "allow_code") == 0;
}
#endif

static int
marshal_valid_shape(
    MarshalOperation operation,
    Py_ssize_t positional_count,
    PyObject *keyword_names
)
{
    Py_ssize_t minimum = operation == MARSHAL_DUMP ? 2 : 1;
    Py_ssize_t maximum =
        operation == MARSHAL_DUMP ? 3 :
        operation == MARSHAL_DUMPS ? 2 : 1;
    if (positional_count < minimum || positional_count > maximum) {
        return 0;
    }
#if PY_VERSION_HEX < 0x030d0000
    return keyword_names == NULL || PyTuple_GET_SIZE(keyword_names) == 0;
#else
    if (keyword_names == NULL) {
        return 1;
    }
    if (PyTuple_GET_SIZE(keyword_names) != 1) {
        return 0;
    }
    return marshal_keyword_is_allow_code(PyTuple_GET_ITEM(keyword_names, 0));
#endif
}

static int
marshal_unpack_arguments(
    PyObject *const *values,
    Py_ssize_t positional_count,
    PyObject *keyword_names,
    PyObject **args,
    PyObject **kwargs
)
{
    *args = PyTuple_New(positional_count);
    *kwargs = NULL;
    if (*args == NULL) {
        return -1;
    }
    for (Py_ssize_t index = 0; index < positional_count; index++) {
        PyTuple_SET_ITEM(*args, index, Py_NewRef(values[index]));
    }
    if (keyword_names == NULL || PyTuple_GET_SIZE(keyword_names) == 0) {
        return 0;
    }
    *kwargs = PyDict_New();
    if (*kwargs == NULL) {
        Py_CLEAR(*args);
        return -1;
    }
    for (Py_ssize_t index = 0; index < PyTuple_GET_SIZE(keyword_names); index++) {
        if (PyDict_SetItem(
                *kwargs,
                PyTuple_GET_ITEM(keyword_names, index),
                values[positional_count + index]
            ) < 0) {
            Py_CLEAR(*kwargs);
            Py_CLEAR(*args);
            return -1;
        }
    }
    return 0;
}

static PyObject *
marshal_dispatch(
    MarshalOperation operation,
    PyObject *const *values,
    Py_ssize_t positional_count,
    PyObject *keyword_names
)
{
    PyObject *original = original_functions[operation];
    if (!marshal_valid_shape(operation, positional_count, keyword_names)) {
        return PyObject_Vectorcall(
            original,
            values,
            (size_t)positional_count,
            keyword_names
        );
    }

    PyObject *version_object = NULL;
    if (operation == MARSHAL_DUMP && positional_count == 3) {
        version_object = values[2];
    }
    else if (operation == MARSHAL_DUMPS && positional_count == 2) {
        version_object = values[1];
    }
    PyObject *allow_code_object = keyword_names == NULL
        ? NULL
        : values[positional_count];
    int custom_version = version_object != NULL &&
        !PyLong_CheckExact(version_object);
    int custom_allow_code = allow_code_object != NULL &&
        !PyBool_Check(allow_code_object);
    if (operation != MARSHAL_LOAD &&
        !custom_version && !custom_allow_code) {
        return PyObject_Vectorcall(
            original,
            values,
            (size_t)positional_count,
            keyword_names
        );
    }

    MarshalState state = {
        .original = Py_NewRef(original),
        .operation = operation,
        .phase = custom_version
            ? MARSHAL_WAIT_VERSION
            : custom_allow_code
                ? MARSHAL_WAIT_ALLOW_CODE
                : MARSHAL_READY,
    };
    if (marshal_unpack_arguments(
            values,
            positional_count,
            keyword_names,
            &state.args,
            &state.kwargs
        ) < 0) {
        marshal_clear_state(&state);
        return NULL;
    }
    state.version_object = custom_version
        ? Py_NewRef(version_object) : NULL;
    state.allow_code_object = custom_allow_code
        ? Py_NewRef(allow_code_object) : NULL;

    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &marshal_vtable, &state) < 0) {
        marshal_clear_state(&state);
        return NULL;
    }
    PyObject *result = marshal_continue(&state, NULL, 0);
    adapter_leave(&frame);
    marshal_clear_state(&state);
    return result;
}

#if PY_VERSION_HEX >= 0x030d0000
#define DEFINE_MARSHAL_WRAPPER(name, operation) \
    static PyObject *name( \
        PyObject *self, \
        PyObject *const *args, \
        Py_ssize_t nargs, \
        PyObject *kwnames \
    ) { \
        (void)self; \
        return marshal_dispatch(operation, args, nargs, kwnames); \
    }

DEFINE_MARSHAL_WRAPPER(marshal_dump_wrapper, MARSHAL_DUMP)
DEFINE_MARSHAL_WRAPPER(marshal_dumps_wrapper, MARSHAL_DUMPS)
DEFINE_MARSHAL_WRAPPER(marshal_load_wrapper, MARSHAL_LOAD)
DEFINE_MARSHAL_WRAPPER(marshal_loads_wrapper, MARSHAL_LOADS)
#else
static PyObject *
marshal_dump_wrapper(
    PyObject *self,
    PyObject *const *args,
    Py_ssize_t nargs
)
{
    (void)self;
    return marshal_dispatch(MARSHAL_DUMP, args, nargs, NULL);
}

static PyObject *
marshal_dumps_wrapper(
    PyObject *self,
    PyObject *const *args,
    Py_ssize_t nargs
)
{
    (void)self;
    return marshal_dispatch(MARSHAL_DUMPS, args, nargs, NULL);
}

static PyObject *
marshal_load_wrapper(PyObject *self, PyObject *argument)
{
    (void)self;
    PyObject *args[1] = {argument};
    return marshal_dispatch(MARSHAL_LOAD, args, 1, NULL);
}

static PyObject *
marshal_loads_wrapper(PyObject *self, PyObject *argument)
{
    (void)self;
    return PyObject_CallOneArg(original_functions[MARSHAL_LOADS], argument);
}
#endif

static PyCFunction const marshal_wrappers[] = {
    _PyCFunction_CAST(marshal_dump_wrapper),
    _PyCFunction_CAST(marshal_dumps_wrapper),
    _PyCFunction_CAST(marshal_load_wrapper),
    _PyCFunction_CAST(marshal_loads_wrapper),
};

int
adapter_marshal_install(PyObject *marshal_module)
{
    if (marshal_installed) {
        return 0;
    }
    installed_module = Py_NewRef(marshal_module);
    PyObject *replacements[4] = {NULL, NULL, NULL, NULL};
    for (int index = 0; index < 4; index++) {
        PyObject *original = PyObject_GetAttrString(
            marshal_module,
            marshal_names[index]
        );
        if (original == NULL || !PyCFunction_Check(original)) {
            Py_XDECREF(original);
            PyErr_Format(
                PyExc_RuntimeError,
                "marshal.%s is not a C function",
                marshal_names[index]
            );
            goto error;
        }
        original_functions[index] = original;
        replacement_methods[index] = *((PyCFunctionObject *)original)->m_ml;
        replacement_methods[index].ml_meth = marshal_wrappers[index];
        PyObject *module_name = PyObject_GetAttrString(original, "__module__");
        replacements[index] = module_name == NULL
            ? NULL
            : PyCFunction_NewEx(
                &replacement_methods[index],
                PyCFunction_GET_SELF(original),
                module_name
            );
        Py_XDECREF(module_name);
        if (replacements[index] == NULL) {
            goto error;
        }
    }
    for (int index = 0; index < 4; index++) {
        if (PyObject_SetAttrString(
                marshal_module,
                marshal_names[index],
                replacements[index]
            ) < 0) {
            goto error;
        }
    }
    for (int index = 0; index < 4; index++) {
        Py_DECREF(replacements[index]);
    }
    marshal_installed = 1;
    return 0;

error:
    for (int index = 0; index < 4; index++) {
        Py_XDECREF(replacements[index]);
    }
    adapter_marshal_rollback();
    return -1;
}

void
adapter_marshal_rollback(void)
{
    if (installed_module == NULL) {
        return;
    }
    for (int index = 0; index < 4; index++) {
        if (original_functions[index] != NULL) {
            if (PyObject_SetAttrString(
                    installed_module,
                    marshal_names[index],
                    original_functions[index]
                ) < 0) {
                PyErr_Clear();
            }
            Py_CLEAR(original_functions[index]);
        }
    }
    Py_CLEAR(installed_module);
    marshal_installed = 0;
}
