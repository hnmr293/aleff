#include "numeric.h"

typedef enum {
    NUMERIC_FLOAT,
    NUMERIC_INDEX,
    NUMERIC_COMPLEX,
    NUMERIC_CEIL,
    NUMERIC_FLOOR,
    NUMERIC_TRUNC,
    NUMERIC_PASSTHROUGH,
} NumericConversion;

typedef enum {
    NUMERIC_WAIT_CONVERSION,
    NUMERIC_WAIT_COMPLEX_FLOAT,
    NUMERIC_WAIT_COMPLEX_INDEX,
    NUMERIC_WAIT_FLOAT_INDEX,
    NUMERIC_WAIT_TERMINAL_RESULT,
} NumericConversionPhase;

typedef enum {
    NUMERIC_POSITIONAL_ARGUMENTS,
    NUMERIC_KEYWORD_ARGUMENTS,
} NumericArgumentPhase;

typedef struct {
    Py_ssize_t position;
    NumericConversion conversion;
} NumericPositionalArgument;

typedef struct {
    const char *name;
    NumericConversion conversion;
} NumericKeywordArgument;

typedef struct {
    const char *name;
    const NumericPositionalArgument *positional;
    Py_ssize_t positional_count;
    const NumericKeywordArgument *keywords;
    Py_ssize_t keyword_count;
} NumericArgumentSchema;

typedef struct {
    PyObject *function;
    PyObject *args;
    PyObject *converted;
    PyObject *converted_kwargs;
    PyObject *pending_object;
    const NumericArgumentSchema *schema;
    NumericConversion conversion;
    NumericConversion pending_conversion;
    NumericConversionPhase conversion_phase;
    NumericArgumentPhase argument_phase;
    Py_ssize_t positional_index;
    Py_ssize_t keyword_index;
} NumericState;

typedef struct {
    PyObject *module;
    PyObject *original;
    const char *name;
} NumericInstallation;

#define NUMERIC_INSTALLATION_MAX 96
static NumericInstallation numeric_installations[NUMERIC_INSTALLATION_MAX];
static Py_ssize_t numeric_installation_count;
static PyMethodDef numeric_methods[NUMERIC_INSTALLATION_MAX];

static const NumericPositionalArgument numeric_ldexp_positional[] = {
    {1, NUMERIC_PASSTHROUGH},
};

static const NumericKeywordArgument numeric_isclose_keywords[] = {
    {"rel_tol", NUMERIC_FLOAT},
    {"abs_tol", NUMERIC_FLOAT},
};

static const NumericKeywordArgument numeric_nextafter_keywords[] = {
    {"steps", NUMERIC_INDEX},
};

static const NumericArgumentSchema numeric_argument_schemas[] = {
    {
        "isclose",
        NULL,
        0,
        numeric_isclose_keywords,
        (Py_ssize_t)(
            sizeof(numeric_isclose_keywords) /
            sizeof(*numeric_isclose_keywords)
        ),
    },
    {
        "nextafter",
        NULL,
        0,
        numeric_nextafter_keywords,
        (Py_ssize_t)(
            sizeof(numeric_nextafter_keywords) /
            sizeof(*numeric_nextafter_keywords)
        ),
    },
    {
        "ldexp",
        numeric_ldexp_positional,
        (Py_ssize_t)(
            sizeof(numeric_ldexp_positional) /
            sizeof(*numeric_ldexp_positional)
        ),
        NULL,
        0,
    },
};

static PyObject *numeric_resume(const void *, PyObject *);

PyObject *
adapter_numeric_validate_index_result(PyObject *value)
{
    if (!PyLong_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "__index__ returned non-int (type %.200s)",
            Py_TYPE(value)->tp_name
        );
        return NULL;
    }
    if (PyLong_CheckExact(value)) {
        return Py_NewRef(value);
    }
    if (PyErr_WarnFormat(
            PyExc_DeprecationWarning,
            1,
            "__index__ returned non-int (type %.200s).  The ability to return "
            "an instance of a strict subclass of int is deprecated, and may "
            "be removed in a future version of Python.",
            Py_TYPE(value)->tp_name
        ) < 0) {
        return NULL;
    }
    return _PyLong_Copy((PyLongObject *)value);
}

static const NumericArgumentSchema *
numeric_schema_for_name(PyObject *name)
{
    Py_ssize_t count = (Py_ssize_t)(
        sizeof(numeric_argument_schemas) / sizeof(*numeric_argument_schemas)
    );
    for (Py_ssize_t index = 0; index < count; index++) {
        const NumericArgumentSchema *schema = &numeric_argument_schemas[index];
        if (PyUnicode_CompareWithASCIIString(name, schema->name) == 0) {
            return schema;
        }
    }
    return NULL;
}

static void *
numeric_copy_state(const void *raw_state)
{
    const NumericState *source = raw_state;
    NumericState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->function = Py_NewRef(source->function);
    copy->args = Py_NewRef(source->args);
    copy->converted = PyList_GetSlice(
        source->converted,
        0,
        PyList_GET_SIZE(source->converted)
    );
    if (copy->converted == NULL) {
        Py_DECREF(copy->function);
        Py_DECREF(copy->args);
        PyMem_Free(copy);
        return NULL;
    }
    copy->converted_kwargs = source->converted_kwargs == NULL
        ? NULL
        : PyDict_Copy(source->converted_kwargs);
    if (source->converted_kwargs != NULL && copy->converted_kwargs == NULL) {
        Py_DECREF(copy->function);
        Py_DECREF(copy->args);
        Py_DECREF(copy->converted);
        PyMem_Free(copy);
        return NULL;
    }
    copy->pending_object = Py_XNewRef(source->pending_object);
    return copy;
}

static void
numeric_free_state(void *raw_state)
{
    NumericState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->function);
    Py_XDECREF(state->args);
    Py_XDECREF(state->converted);
    Py_XDECREF(state->converted_kwargs);
    Py_XDECREF(state->pending_object);
    PyMem_Free(state);
}

static const AleffAdapterVTable numeric_vtable = {
    .copy_state = numeric_copy_state,
    .free_state = numeric_free_state,
    .resume = numeric_resume,
    .prepare_resume = NULL,
};

static int
numeric_has_protocol(PyObject *object, const char *name)
{
    PyObject *attribute = lookup_raw_special(object, name);
    if (attribute == NULL) {
        return 0;
    }
    Py_DECREF(attribute);
    return 1;
}

static PyObject *
numeric_call_special_noargs(PyObject *object, const char *name)
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
    PyObject *result = PyObject_CallNoArgs(callable);
    Py_DECREF(callable);
    return result;
}

static PyObject *
numeric_convert(
    NumericState *state,
    PyObject *object,
    NumericConversion conversion
)
{
    switch (conversion) {
        case NUMERIC_FLOAT:
            if (PyFloat_Check(object) || PyLong_Check(object)) {
                return Py_NewRef(object);
            }
            if (numeric_has_protocol(object, "__float__")) {
                state->pending_conversion = NUMERIC_FLOAT;
                return PyNumber_Float(object);
            }
            if (numeric_has_protocol(object, "__index__")) {
                state->pending_conversion = NUMERIC_INDEX;
                state->conversion_phase = NUMERIC_WAIT_FLOAT_INDEX;
                return PyNumber_Index(object);
            }
            return Py_NewRef(object);
        case NUMERIC_INDEX:
            if (PyLong_Check(object) ||
                !numeric_has_protocol(object, "__index__")) {
                return Py_NewRef(object);
            }
            return PyNumber_Index(object);
        case NUMERIC_COMPLEX:
            if (PyComplex_Check(object)) {
                return Py_NewRef(object);
            }
            if (numeric_has_protocol(object, "__complex__")) {
                return PyObject_CallOneArg((PyObject *)&PyComplex_Type, object);
            }
            if (PyFloat_Check(object) || PyLong_Check(object)) {
                return Py_NewRef(object);
            }
            if (numeric_has_protocol(object, "__float__")) {
                state->conversion_phase = NUMERIC_WAIT_COMPLEX_FLOAT;
                state->pending_conversion = NUMERIC_FLOAT;
                return PyNumber_Float(object);
            }
            if (numeric_has_protocol(object, "__index__")) {
                state->conversion_phase = NUMERIC_WAIT_COMPLEX_INDEX;
                state->pending_conversion = NUMERIC_INDEX;
                return PyNumber_Index(object);
            }
            return Py_NewRef(object);
        case NUMERIC_CEIL:
            if (PyFloat_CheckExact(object) || PyLong_CheckExact(object)) {
                return Py_NewRef(object);
            }
            if (numeric_has_protocol(object, "__ceil__")) {
                state->conversion_phase = NUMERIC_WAIT_TERMINAL_RESULT;
                return numeric_call_special_noargs(object, "__ceil__");
            }
            return numeric_convert(state, object, NUMERIC_FLOAT);
        case NUMERIC_FLOOR:
            if (PyFloat_CheckExact(object) || PyLong_CheckExact(object)) {
                return Py_NewRef(object);
            }
            if (numeric_has_protocol(object, "__floor__")) {
                state->conversion_phase = NUMERIC_WAIT_TERMINAL_RESULT;
                return numeric_call_special_noargs(object, "__floor__");
            }
            return numeric_convert(state, object, NUMERIC_FLOAT);
        case NUMERIC_TRUNC:
            if (PyFloat_CheckExact(object) || PyLong_CheckExact(object) ||
                !numeric_has_protocol(object, "__trunc__")) {
                return Py_NewRef(object);
            }
            state->conversion_phase = NUMERIC_WAIT_TERMINAL_RESULT;
            return numeric_call_special_noargs(object, "__trunc__");
        case NUMERIC_PASSTHROUGH:
            return Py_NewRef(object);
    }
    PyErr_SetString(PyExc_RuntimeError, "invalid numeric conversion");
    return NULL;
}

static PyObject *
numeric_finish_complex(PyObject *value, NumericConversionPhase phase)
{
    double real;
    switch (phase) {
        case NUMERIC_WAIT_COMPLEX_FLOAT:
            real = PyFloat_AS_DOUBLE(value);
            return PyComplex_FromDoubles(real, 0.0);
        case NUMERIC_WAIT_COMPLEX_INDEX:
            real = PyLong_AsDouble(value);
            if (real == -1.0 && PyErr_Occurred()) {
                return NULL;
            }
            return PyComplex_FromDoubles(real, 0.0);
        case NUMERIC_WAIT_FLOAT_INDEX:
            return PyNumber_Float(value);
        case NUMERIC_WAIT_CONVERSION:
            return Py_NewRef(value);
        case NUMERIC_WAIT_TERMINAL_RESULT:
            break;
    }
    PyErr_SetString(PyExc_RuntimeError, "invalid numeric conversion phase");
    return NULL;
}

static PyObject *
numeric_validate_resumed(
    PyObject *value,
    PyObject *original,
    NumericConversion conversion
)
{
    if (value == NULL) {
        return NULL;
    }
    switch (conversion) {
        case NUMERIC_FLOAT:
            if (!PyFloat_Check(value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "%.50s.__float__ returned non-float (type %.50s)",
                    original == NULL ? "object" : Py_TYPE(original)->tp_name,
                    Py_TYPE(value)->tp_name
                );
                return NULL;
            }
            if (PyFloat_CheckExact(value)) return Py_NewRef(value);
            if (PyErr_WarnFormat(
                    PyExc_DeprecationWarning,
                    1,
                    "%.200s.__float__ returned non-float (type %.200s).  "
                    "The ability to return an instance of a strict subclass "
                    "of float is deprecated, and may be removed in a future "
                    "version of Python.",
                    original == NULL ? "object" : Py_TYPE(original)->tp_name,
                    Py_TYPE(value)->tp_name
                ) < 0) return NULL;
            return PyFloat_FromDouble(PyFloat_AS_DOUBLE(value));
        case NUMERIC_INDEX:
            return adapter_numeric_validate_index_result(value);
        case NUMERIC_COMPLEX:
            if (!PyComplex_Check(value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "__complex__ returned non-complex (type %.200s)",
                    Py_TYPE(value)->tp_name
                );
                return NULL;
            }
            return Py_NewRef(value);
        case NUMERIC_CEIL:
        case NUMERIC_FLOOR:
        case NUMERIC_TRUNC:
            return Py_NewRef(value);
        case NUMERIC_PASSTHROUGH:
            return Py_NewRef(value);
    }
    PyErr_SetString(PyExc_RuntimeError, "invalid numeric conversion");
    return NULL;
}

static NumericConversion
numeric_positional_conversion_at(
    const NumericState *state,
    Py_ssize_t position
)
{
    if (state->schema != NULL) {
        for (Py_ssize_t index = 0;
             index < state->schema->positional_count;
             index++) {
            const NumericPositionalArgument *argument =
                &state->schema->positional[index];
            if (argument->position == position) {
                return argument->conversion;
            }
        }
    }
    return state->conversion;
}

static PyObject *
numeric_call_original(NumericState *state)
{
    PyObject *args = PyList_AsTuple(state->converted);
    if (args == NULL) {
        return NULL;
    }
    PyObject *result = PyObject_Call(
        state->function,
        args,
        state->converted_kwargs
    );
    Py_DECREF(args);
    return result;
}

static int
numeric_store_converted(NumericState *state, PyObject *converted)
{
    if (state->argument_phase == NUMERIC_POSITIONAL_ARGUMENTS) {
        if (PyList_SetItem(
                state->converted,
                state->positional_index,
                converted
            ) < 0) {
            return -1;
        }
        state->positional_index++;
        return 0;
    }

    if (state->schema == NULL ||
        state->schema->keywords == NULL ||
        state->converted_kwargs == NULL ||
        state->keyword_index >= state->schema->keyword_count) {
        Py_DECREF(converted);
        PyErr_SetString(PyExc_RuntimeError, "invalid numeric keyword state");
        return -1;
    }
    const NumericKeywordArgument *argument =
        &state->schema->keywords[state->keyword_index];
    if (PyDict_SetItemString(
            state->converted_kwargs,
            argument->name,
            converted
        ) < 0) {
        Py_DECREF(converted);
        return -1;
    }
    Py_DECREF(converted);
    state->keyword_index++;
    return 0;
}

static PyObject *
numeric_continue(
    NumericState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (state->conversion_phase == NUMERIC_WAIT_TERMINAL_RESULT) {
            return Py_XNewRef(resumed_value);
        }
        if (state->conversion_phase != NUMERIC_WAIT_CONVERSION &&
            state->conversion_phase != NUMERIC_WAIT_COMPLEX_FLOAT &&
            state->conversion_phase != NUMERIC_WAIT_COMPLEX_INDEX &&
            state->conversion_phase != NUMERIC_WAIT_FLOAT_INDEX) {
            PyErr_SetString(PyExc_RuntimeError, "numeric adapter is not awaiting a conversion");
            return NULL;
        }
        PyObject *converted = numeric_validate_resumed(
            resumed_value,
            state->pending_object,
            state->pending_conversion
        );
        if (converted == NULL) {
            return NULL;
        }
        if (state->conversion_phase == NUMERIC_WAIT_COMPLEX_FLOAT ||
            state->conversion_phase == NUMERIC_WAIT_COMPLEX_INDEX ||
            state->conversion_phase == NUMERIC_WAIT_FLOAT_INDEX) {
            PyObject *complex_value = numeric_finish_complex(
                converted,
                state->conversion_phase
            );
            Py_DECREF(converted);
            converted = complex_value;
            if (converted == NULL) {
                return NULL;
            }
        }
        state->conversion_phase = NUMERIC_WAIT_CONVERSION;
        if (numeric_store_converted(state, converted) < 0) {
            return NULL;
        }
        Py_CLEAR(state->pending_object);
    }

    Py_ssize_t count = PyTuple_GET_SIZE(state->args);
    while (state->argument_phase == NUMERIC_POSITIONAL_ARGUMENTS &&
           state->positional_index < count) {
        PyObject *object = PyTuple_GET_ITEM(
            state->args,
            state->positional_index
        );
        state->pending_conversion = numeric_positional_conversion_at(
            state,
            state->positional_index
        );
        state->conversion_phase = NUMERIC_WAIT_CONVERSION;
        Py_XSETREF(state->pending_object, Py_NewRef(object));
        PyObject *converted = numeric_convert(
            state,
            object,
            state->pending_conversion
        );
        if (converted == NULL) {
            return NULL;
        }
        if (state->conversion_phase == NUMERIC_WAIT_TERMINAL_RESULT) {
            return converted;
        }
        if (state->conversion_phase == NUMERIC_WAIT_COMPLEX_FLOAT ||
            state->conversion_phase == NUMERIC_WAIT_COMPLEX_INDEX ||
            state->conversion_phase == NUMERIC_WAIT_FLOAT_INDEX) {
            PyObject *complex_value = numeric_finish_complex(
                converted,
                state->conversion_phase
            );
            Py_DECREF(converted);
            converted = complex_value;
            if (converted == NULL) {
                return NULL;
            }
            state->conversion_phase = NUMERIC_WAIT_CONVERSION;
        }
        if (numeric_store_converted(state, converted) < 0) {
            return NULL;
        }
        Py_CLEAR(state->pending_object);
    }

    state->argument_phase = NUMERIC_KEYWORD_ARGUMENTS;
    while (state->schema != NULL &&
           state->keyword_index < state->schema->keyword_count) {
        const NumericKeywordArgument *argument =
            &state->schema->keywords[state->keyword_index];
        PyObject *object = state->converted_kwargs == NULL
            ? NULL
            : PyDict_GetItemString(state->converted_kwargs, argument->name);
        if (object == NULL) {
            state->keyword_index++;
            continue;
        }
        state->pending_conversion = argument->conversion;
        state->conversion_phase = NUMERIC_WAIT_CONVERSION;
        Py_XSETREF(state->pending_object, Py_NewRef(object));
        PyObject *converted = numeric_convert(
            state,
            object,
            state->pending_conversion
        );
        if (converted == NULL) {
            return NULL;
        }
        if (state->conversion_phase == NUMERIC_WAIT_TERMINAL_RESULT) {
            return converted;
        }
        if (state->conversion_phase == NUMERIC_WAIT_COMPLEX_FLOAT ||
            state->conversion_phase == NUMERIC_WAIT_COMPLEX_INDEX ||
            state->conversion_phase == NUMERIC_WAIT_FLOAT_INDEX) {
            PyObject *complex_value = numeric_finish_complex(
                converted,
                state->conversion_phase
            );
            Py_DECREF(converted);
            converted = complex_value;
            if (converted == NULL) {
                return NULL;
            }
            state->conversion_phase = NUMERIC_WAIT_CONVERSION;
        }
        if (numeric_store_converted(state, converted) < 0) {
            return NULL;
        }
        Py_CLEAR(state->pending_object);
    }
    return numeric_call_original(state);
}

static PyObject *
numeric_resume(const void *raw_state, PyObject *value)
{
    NumericState *state = numeric_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &numeric_vtable, state) < 0) {
        numeric_free_state(state);
        return NULL;
    }
    PyObject *result = numeric_continue(state, value, 1);
    adapter_leave(&frame);
    numeric_free_state(state);
    return result;
}

static int
numeric_name_is(PyObject *name, const char *expected)
{
    return PyUnicode_CompareWithASCIIString(name, expected) == 0;
}

static int
numeric_keyword_allowed(PyObject *function_name, PyObject *keyword)
{
    if (!PyUnicode_Check(keyword)) return 0;
    if (numeric_name_is(function_name, "isclose")) {
        return PyUnicode_CompareWithASCIIString(keyword, "rel_tol") == 0 ||
            PyUnicode_CompareWithASCIIString(keyword, "abs_tol") == 0;
    }
    if (numeric_name_is(function_name, "nextafter")) {
        return PyUnicode_CompareWithASCIIString(keyword, "steps") == 0;
    }
    return 0;
}

static int
numeric_call_shape_is_invalid(
    PyObject *function,
    PyObject *function_name,
    PyObject *args,
    PyObject *kwargs
)
{
    if (kwargs != NULL) {
        Py_ssize_t position = 0;
        PyObject *key;
        PyObject *value;
        while (PyDict_Next(kwargs, &position, &key, &value)) {
            if (!numeric_keyword_allowed(function_name, key)) return 1;
        }
    }
    Py_ssize_t count = PyTuple_GET_SIZE(args);
    if (PyCFunction_Check(function) &&
        (PyCFunction_GET_FLAGS(function) & METH_O) != 0) {
        return count != 1 || (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0);
    }
    Py_ssize_t minimum = -1;
    Py_ssize_t maximum = -1;
    static const char *const exact_two[] = {
        "atan2", "comb", "copysign", "dist", "fmod", "isclose",
        "ldexp", "nextafter", "pow", "rect", "remainder",
    };
    for (Py_ssize_t index = 0;
         index < (Py_ssize_t)(sizeof(exact_two) / sizeof(*exact_two));
         index++) {
        if (numeric_name_is(function_name, exact_two[index])) {
            minimum = maximum = 2;
            break;
        }
    }
    if (numeric_name_is(function_name, "fma")) minimum = maximum = 3;
    else if (numeric_name_is(function_name, "log") ||
             numeric_name_is(function_name, "perm")) {
        minimum = 1;
        maximum = 2;
    }
    else if (numeric_name_is(function_name, "gcd") ||
             numeric_name_is(function_name, "lcm") ||
             numeric_name_is(function_name, "hypot")) {
        minimum = 0;
        maximum = PY_SSIZE_T_MAX;
    }
    return minimum >= 0 && (count < minimum || count > maximum);
}

static PyObject *
numeric_function(PyObject *tag, PyObject *args, PyObject *kwargs)
{
    if (!PyTuple_Check(tag) || PyTuple_GET_SIZE(tag) != 2) {
        PyErr_SetString(PyExc_RuntimeError, "invalid numeric adapter state");
        return NULL;
    }
    PyObject *function = PyTuple_GET_ITEM(tag, 0);
    long conversion = PyLong_AsLong(PyTuple_GET_ITEM(tag, 1));
    if (conversion < 0 && PyErr_Occurred()) {
        return NULL;
    }
    PyObject *converted = PySequence_List(args);
    if (converted == NULL) {
        return NULL;
    }
    PyObject *converted_kwargs = kwargs == NULL ? NULL : PyDict_Copy(kwargs);
    if (kwargs != NULL && converted_kwargs == NULL) {
        Py_DECREF(converted);
        return NULL;
    }
    PyObject *name = PyObject_GetAttrString(function, "__name__");
    if (name == NULL) {
        Py_DECREF(converted);
        Py_XDECREF(converted_kwargs);
        return NULL;
    }
    if (!PyUnicode_Check(name)) {
        Py_DECREF(name);
        Py_DECREF(converted);
        Py_XDECREF(converted_kwargs);
        PyErr_SetString(PyExc_RuntimeError, "invalid numeric adapter name");
        return NULL;
    }
    if (numeric_call_shape_is_invalid(function, name, args, kwargs)) {
        Py_DECREF(name);
        Py_DECREF(converted);
        Py_XDECREF(converted_kwargs);
        return PyObject_Call(function, args, kwargs);
    }
    const NumericArgumentSchema *schema = numeric_schema_for_name(name);
    Py_DECREF(name);
    NumericState state = {
        .function = function,
        .args = args,
        .converted = converted,
        .converted_kwargs = converted_kwargs,
        .schema = schema,
        .conversion = (NumericConversion)conversion,
        .pending_conversion = (NumericConversion)conversion,
        .conversion_phase = NUMERIC_WAIT_CONVERSION,
        .argument_phase = NUMERIC_POSITIONAL_ARGUMENTS,
        .positional_index = 0,
        .keyword_index = 0,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &numeric_vtable, &state) < 0) {
        Py_DECREF(converted);
        Py_XDECREF(converted_kwargs);
        return NULL;
    }
    PyObject *result = numeric_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.pending_object);
    Py_DECREF(converted);
    Py_XDECREF(converted_kwargs);
    return result;
}

static int
numeric_replace(
    PyObject *module,
    const char *name,
    NumericConversion conversion
)
{
    if (numeric_installation_count >= NUMERIC_INSTALLATION_MAX) {
        PyErr_SetString(PyExc_RuntimeError, "too many numeric adapters");
        return -1;
    }
    PyObject *original = PyObject_GetAttrString(module, name);
    if (original == NULL) {
        PyErr_Clear();
        return 0;
    }
    if (!PyCallable_Check(original)) {
        Py_DECREF(original);
        return 0;
    }
    PyObject *kind = PyLong_FromLong((long)conversion);
    PyObject *tag = kind == NULL ? NULL : PyTuple_Pack(2, original, kind);
    Py_XDECREF(kind);
    if (tag == NULL) {
        Py_DECREF(original);
        return -1;
    }
    PyObject *module_name = PyObject_GetAttrString(original, "__module__");
    if (module_name == NULL) {
        Py_DECREF(tag);
        Py_DECREF(original);
        return -1;
    }
    PyMethodDef *method = &numeric_methods[numeric_installation_count];
    *method = (PyMethodDef){
        .ml_name = name,
        .ml_meth = (PyCFunction)(void(*)(void))numeric_function,
        .ml_flags = METH_VARARGS | METH_KEYWORDS,
        .ml_doc = PyCFunction_Check(original)
            ? ((PyCFunctionObject *)original)->m_ml->ml_doc
            : NULL,
    };
    PyObject *replacement = PyCFunction_NewEx(method, tag, module_name);
    Py_DECREF(module_name);
    Py_DECREF(tag);
    if (replacement == NULL) {
        Py_DECREF(original);
        return -1;
    }
    if (PyObject_SetAttrString(module, name, replacement) < 0) {
        Py_DECREF(replacement);
        Py_DECREF(original);
        return -1;
    }
    NumericInstallation *installation =
        &numeric_installations[numeric_installation_count++];
    installation->module = Py_NewRef(module);
    installation->original = original;
    installation->name = name;
    Py_DECREF(replacement);
    return 0;
}

static int
numeric_replace_many(
    PyObject *module,
    const char *const *names,
    Py_ssize_t count,
    NumericConversion conversion
)
{
    for (Py_ssize_t index = 0; index < count; index++) {
        if (numeric_replace(module, names[index], conversion) < 0) {
            return -1;
        }
    }
    return 0;
}

int
adapter_numeric_install(PyObject *math_module, PyObject *cmath_module)
{
    if (numeric_installation_count != 0) {
        return 0;
    }
    static const char *float_names[] = {
        "acos", "acosh", "asin", "asinh", "atan", "atan2", "atanh",
        "cbrt", "copysign", "cos", "cosh", "degrees", "erf", "erfc",
        "exp", "exp2", "expm1", "fabs", "fma", "fmod", "frexp",
        "gamma", "isclose", "isfinite", "isinf", "isnan", "ldexp",
        "lgamma", "log", "log10", "log1p", "log2", "modf", "nextafter",
        "pow", "radians", "remainder", "sin", "sinh", "sqrt", "tan",
        "tanh", "ulp",
        "hypot",
    };
    static const char *index_names[] = {
        "comb", "factorial", "gcd", "isqrt", "lcm", "perm",
    };
    static const char *special_names[] = {"ceil", "floor", "trunc"};
    static const char *complex_names[] = {
        "acos", "acosh", "asin", "asinh", "atan", "atanh", "cos",
        "cosh", "exp", "isclose", "isfinite", "isinf", "isnan", "log",
        "log10", "phase", "polar", "sin", "sinh", "sqrt", "tan", "tanh",
    };
    Py_ssize_t float_count = (Py_ssize_t)(sizeof(float_names) / sizeof(*float_names));
    Py_ssize_t index_count = (Py_ssize_t)(sizeof(index_names) / sizeof(*index_names));
    Py_ssize_t complex_count = (Py_ssize_t)(sizeof(complex_names) / sizeof(*complex_names));
    if (numeric_replace_many(math_module, float_names, float_count, NUMERIC_FLOAT) < 0 ||
        numeric_replace_many(math_module, index_names, index_count, NUMERIC_INDEX) < 0 ||
        numeric_replace(math_module, special_names[0], NUMERIC_CEIL) < 0 ||
        numeric_replace(math_module, special_names[1], NUMERIC_FLOOR) < 0 ||
        numeric_replace(math_module, special_names[2], NUMERIC_TRUNC) < 0 ||
        numeric_replace_many(cmath_module, complex_names, complex_count, NUMERIC_COMPLEX) < 0) {
        adapter_numeric_rollback();
        return -1;
    }
    /* cmath.rect is the one cmath entry point whose arguments are real.  It
     * is replaced separately so the same state machine can convert both. */
    if (numeric_replace(cmath_module, "rect", NUMERIC_FLOAT) < 0) {
        adapter_numeric_rollback();
        return -1;
    }
    return 0;
}

void
adapter_numeric_rollback(void)
{
    while (numeric_installation_count > 0) {
        NumericInstallation *installation =
            &numeric_installations[--numeric_installation_count];
        if (PyObject_SetAttrString(
                installation->module,
                installation->name,
                installation->original
            ) < 0) {
            PyErr_Clear();
        }
        Py_DECREF(installation->module);
        Py_DECREF(installation->original);
        installation->module = NULL;
        installation->original = NULL;
    }
}
