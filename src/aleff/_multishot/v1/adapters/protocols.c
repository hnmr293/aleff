#include "internal.h"
#include "protocols.h"

PyObject *original_repr = NULL;
PyObject *original_format = NULL;
PyObject *original_hash = NULL;
vectorcallfunc original_type_vectorcall = NULL;
vectorcallfunc original_bool_vectorcall = NULL;
vectorcallfunc original_int_vectorcall = NULL;
vectorcallfunc original_float_vectorcall = NULL;
vectorcallfunc original_complex_vectorcall = NULL;
vectorcallfunc original_str_vectorcall = NULL;

typedef struct {
    ProtocolResumeKind kind;
} ProtocolState;

static int
protocol_type_has_method(PyObject *object, const char *name)
{
    PyObject *name_object = PyUnicode_FromString(name);
    if (name_object == NULL) {
        return -1;
    }
    PyObject *mro = Py_TYPE(object)->tp_mro;
    if (mro == NULL) {
        Py_DECREF(name_object);
        return 0;
    }
    Py_ssize_t size = PyTuple_GET_SIZE(mro);
    for (Py_ssize_t index = 0; index < size; index++) {
        PyTypeObject *type = (PyTypeObject *)PyTuple_GET_ITEM(mro, index);
        PyObject *dict = PyType_GetDict(type);
        if (dict == NULL) {
            Py_DECREF(name_object);
            return -1;
        }
        int contains = PyDict_Contains(dict, name_object);
        if (contains < 0) {
            Py_DECREF(name_object);
            return -1;
        }
        if (contains > 0) {
            Py_DECREF(name_object);
            return 1;
        }
    }
    Py_DECREF(name_object);
    return 0;
}

static int
protocol_type_has_python_method(PyObject *object, const char *name)
{
    PyObject *mro = Py_TYPE(object)->tp_mro;
    if (mro == NULL) {
        return 0;
    }
    for (Py_ssize_t index = 0; index < PyTuple_GET_SIZE(mro); index++) {
        PyTypeObject *type = (PyTypeObject *)PyTuple_GET_ITEM(mro, index);
        PyObject *dict = PyType_GetDict(type);
        if (dict == NULL) {
            return -1;
        }
        PyObject *method = PyDict_GetItemString(dict, name);
        if (method != NULL) {
            return PyFunction_Check(method);
        }
    }
    return 0;
}

static int
protocol_kind_for_object(
    PyObject *object,
    const char *first,
    const char *second,
    const char *third,
    ProtocolResumeKind first_kind,
    ProtocolResumeKind second_kind,
    ProtocolResumeKind third_kind,
    ProtocolResumeKind *kind
)
{
    const char *names[3] = {first, second, third};
    ProtocolResumeKind kinds[3] = {first_kind, second_kind, third_kind};
    for (int index = 0; index < 3; index++) {
        if (names[index] == NULL) {
            continue;
        }
        int found = protocol_type_has_method(object, names[index]);
        if (found < 0) {
            return -1;
        }
        if (found) {
            *kind = kinds[index];
            return 0;
        }
    }
    *kind = first_kind;
    return 0;
}

static void *
protocol_copy_state(const void *raw_state)
{
    const ProtocolState *state = raw_state;
    ProtocolState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    return copy;
}

static void
protocol_free_state(void *raw_state)
{
    PyMem_Free(raw_state);
}

static PyObject *
protocol_len_result(PyObject *value)
{
    Py_ssize_t length = PyNumber_AsSsize_t(value, PyExc_OverflowError);
    if (length < 0) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_ValueError, "__len__() should return >= 0");
        }
        return NULL;
    }
    return PyLong_FromSsize_t(length);
}

static PyObject *
protocol_int_result(PyObject *value, const char *method)
{
    if (!PyLong_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "%s returned non-int (type %.200s)",
            method,
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
            "%s returned non-int (type %.200s).  The ability to return an "
            "instance of a strict subclass of int is deprecated, and may "
            "be removed in a future version of Python.",
            method,
            Py_TYPE(value)->tp_name
        )) {
        return NULL;
    }
    return PyObject_CallOneArg((PyObject *)&PyLong_Type, value);
}

static PyObject *
protocol_float_result(PyObject *value)
{
    if (!PyFloat_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "__float__ returned non-float (type %.200s)",
            Py_TYPE(value)->tp_name
        );
        return NULL;
    }
    if (PyFloat_CheckExact(value)) {
        return Py_NewRef(value);
    }
    if (PyErr_WarnFormat(
            PyExc_DeprecationWarning,
            1,
            "__float__ returned non-float (type %.200s).  The ability to "
            "return an instance of a strict subclass of float is deprecated, "
            "and may be removed in a future version of Python.",
            Py_TYPE(value)->tp_name
        )) {
        return NULL;
    }
    return PyFloat_FromDouble(PyFloat_AS_DOUBLE(value));
}

static PyObject *
protocol_index_result(PyObject *value)
{
    return PyNumber_Index(value);
}

static PyObject *
protocol_float_index_result(PyObject *value)
{
    PyObject *index = PyNumber_Index(value);
    if (index == NULL) {
        return NULL;
    }
    double number = PyLong_AsDouble(index);
    Py_DECREF(index);
    if (number == -1.0 && PyErr_Occurred()) {
        return NULL;
    }
    return PyFloat_FromDouble(number);
}

static PyObject *
protocol_complex_result(PyObject *value)
{
    if (!PyComplex_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "__complex__ returned non-complex (type %.200s)",
            Py_TYPE(value)->tp_name
        );
        return NULL;
    }
    Py_complex number = PyComplex_AsCComplex(value);
    return PyComplex_FromCComplex(number);
}

static PyObject *
protocol_complex_real_result(PyObject *value, int is_index)
{
    PyObject *number;
    if (is_index) {
        number = protocol_float_index_result(value);
    }
    else {
        number = protocol_float_result(value);
    }
    if (number == NULL) {
        return NULL;
    }
    double real = PyFloat_AS_DOUBLE(number);
    Py_DECREF(number);
    return PyComplex_FromDoubles(real, 0.0);
}

static PyObject *
protocol_hash_result(PyObject *value)
{
    if (!PyLong_Check(value)) {
        PyErr_SetString(
            PyExc_TypeError,
            "__hash__ method should return an integer"
        );
        return NULL;
    }
    Py_ssize_t hash = PyLong_AsSsize_t(value);
    if (hash == -1 && PyErr_Occurred()) {
        PyErr_Clear();
        hash = PyLong_Type.tp_hash(value);
    }
    if (hash == -1) {
        hash = -2;
    }
    return PyLong_FromSsize_t(hash);
}

static PyObject *
protocol_resume(const void *raw_state, PyObject *value)
{
    const ProtocolState *state = raw_state;
    if (value == NULL) {
        return NULL;
    }
    switch (state->kind) {
        case PROTOCOL_BOOL:
            if (!PyBool_Check(value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "__bool__ should return bool, returned %s",
                    Py_TYPE(value)->tp_name
                );
                return NULL;
            }
            return Py_NewRef(value);
        case PROTOCOL_LEN: {
            PyObject *length = protocol_len_result(value);
            if (length == NULL) {
                return NULL;
            }
            int truth = PyObject_IsTrue(length);
            Py_DECREF(length);
            if (truth < 0) {
                return NULL;
            }
            return PyBool_FromLong(truth);
        }
        case PROTOCOL_INT:
            return protocol_int_result(value, "__int__");
        case PROTOCOL_INDEX:
            return protocol_index_result(value);
        case PROTOCOL_TRUNC:
            return protocol_int_result(value, "__trunc__");
        case PROTOCOL_FLOAT:
            return protocol_float_result(value);
        case PROTOCOL_FLOAT_INDEX:
            return protocol_float_index_result(value);
        case PROTOCOL_COMPLEX:
            return protocol_complex_result(value);
        case PROTOCOL_COMPLEX_FLOAT:
            return protocol_complex_real_result(value, 0);
        case PROTOCOL_COMPLEX_INDEX:
            return protocol_complex_real_result(value, 1);
        case PROTOCOL_STR:
            if (!PyUnicode_Check(value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "__str__ returned non-string (type %.200s)",
                    Py_TYPE(value)->tp_name
                );
                return NULL;
            }
            return Py_NewRef(value);
        case PROTOCOL_REPR:
            if (!PyUnicode_Check(value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "__repr__ returned non-string (type %.200s)",
                    Py_TYPE(value)->tp_name
                );
                return NULL;
            }
            return Py_NewRef(value);
        case PROTOCOL_FORMAT:
            if (!PyUnicode_Check(value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "__format__ must return a str, not %.200s",
                    Py_TYPE(value)->tp_name
                );
                return NULL;
            }
            return Py_NewRef(value);
        case PROTOCOL_HASH:
            return protocol_hash_result(value);
    }
    PyErr_SetString(PyExc_RuntimeError, "unknown protocol resume kind");
    return NULL;
}

static const AleffAdapterVTable protocol_vtable = {
    .copy_state = protocol_copy_state,
    .free_state = protocol_free_state,
    .resume = protocol_resume,
};

static PyObject *
protocol_call_args(
    PyObject *original,
    PyObject *args,
    ProtocolResumeKind kind
)
{
    ProtocolState state = {.kind = kind};
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &protocol_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = PyObject_Call(original, args, NULL);
    adapter_leave(&frame);
    return result;
}

ProtocolResumeKind
protocol_vectorcall_kind(
    PyObject *const *args,
    size_t nargsf,
    const char *first,
    const char *second,
    const char *third,
    ProtocolResumeKind first_kind,
    ProtocolResumeKind second_kind,
    ProtocolResumeKind third_kind
)
{
    ProtocolResumeKind kind = first_kind;
    if (PyVectorcall_NARGS(nargsf) > 0) {
        if (protocol_kind_for_object(
                args[0], first, second, third,
                first_kind, second_kind, third_kind, &kind
            ) < 0) {
            return -1;
        }
    }
    return kind;
}

static PyObject *
protocol_type_vectorcall_with(
    vectorcallfunc original,
    PyObject *callable,
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames,
    ProtocolResumeKind kind
)
{
    ProtocolState state = {.kind = kind};
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &protocol_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = original(callable, args, nargsf, kwnames);
    adapter_leave(&frame);
    return result;
}

PyObject *
protocol_type_vectorcall(
    PyObject *callable,
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames,
    ProtocolResumeKind kind
)
{
    return protocol_type_vectorcall_with(
        original_type_vectorcall, callable, args, nargsf, kwnames, kind
    );
}

typedef struct {
    PyObject *real_argument;
    PyObject *imag_argument;
    PyObject *real_value;
    ProtocolResumeKind real_kind;
    ProtocolResumeKind imag_kind;
    int phase;
} ComplexTwoState;

enum {
    COMPLEX_TWO_WAIT_REAL,
    COMPLEX_TWO_WAIT_IMAG,
};

static void *
complex_two_copy_state(const void *raw_state)
{
    const ComplexTwoState *state = raw_state;
    ComplexTwoState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->real_argument = Py_NewRef(state->real_argument);
    copy->imag_argument = Py_NewRef(state->imag_argument);
    copy->real_value = Py_XNewRef(state->real_value);
    return copy;
}

static void
complex_two_free_state(void *raw_state)
{
    ComplexTwoState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->real_argument);
    Py_DECREF(state->imag_argument);
    Py_XDECREF(state->real_value);
    PyMem_Free(state);
}

static PyObject *
complex_two_real_result(ComplexTwoState *state, PyObject *value)
{
    switch (state->real_kind) {
        case PROTOCOL_COMPLEX:
            return protocol_complex_result(value);
        case PROTOCOL_COMPLEX_INDEX:
            return protocol_complex_real_result(value, 1);
        default:
            return protocol_complex_real_result(value, 0);
    }
}

static PyObject *
complex_two_imag_result(ComplexTwoState *state, PyObject *value)
{
    PyObject *number = state->imag_kind == PROTOCOL_COMPLEX_INDEX
        ? protocol_float_index_result(value)
        : protocol_float_result(value);
    if (number == NULL) {
        return NULL;
    }
    double imaginary = PyFloat_AS_DOUBLE(number);
    Py_DECREF(number);
    return PyComplex_FromDoubles(0.0, imaginary);
}

static PyObject *
complex_two_finish(ComplexTwoState *state, PyObject *imaginary)
{
    return PyNumber_Add(state->real_value, imaginary);
}

static PyObject *
complex_two_continue(
    ComplexTwoState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    vectorcallfunc complex_vectorcall = original_complex_vectorcall == NULL
        ? original_type_vectorcall
        : original_complex_vectorcall;
    if (is_resumed) {
        if (state->phase == COMPLEX_TWO_WAIT_REAL) {
            state->real_value = complex_two_real_result(state, resumed_value);
            if (state->real_value == NULL) {
                return NULL;
            }
        }
        else if (state->phase == COMPLEX_TWO_WAIT_IMAG) {
            PyObject *imaginary = complex_two_imag_result(state, resumed_value);
            if (imaginary == NULL) {
                return NULL;
            }
            PyObject *result = complex_two_finish(state, imaginary);
            Py_DECREF(imaginary);
            return result;
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "invalid complex conversion phase");
            return NULL;
        }
    }

    if (state->real_value == NULL) {
        state->phase = COMPLEX_TWO_WAIT_REAL;
        PyObject *arguments[1] = {state->real_argument};
        state->real_value = complex_vectorcall(
            (PyObject *)&PyComplex_Type,
            arguments,
            1,
            NULL
        );
        if (state->real_value == NULL) {
            return NULL;
        }
    }

    state->phase = COMPLEX_TWO_WAIT_IMAG;
    PyObject *zero = PyLong_FromLong(0);
    if (zero == NULL) {
        return NULL;
    }
    PyObject *arguments[2] = {zero, state->imag_argument};
    PyObject *imaginary = complex_vectorcall(
        (PyObject *)&PyComplex_Type,
        arguments,
        2,
        NULL
    );
    Py_DECREF(zero);
    if (imaginary == NULL) {
        return NULL;
    }
    PyObject *result = complex_two_finish(state, imaginary);
    Py_DECREF(imaginary);
    return result;
}

static PyObject *complex_two_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable complex_two_vtable = {
    .copy_state = complex_two_copy_state,
    .free_state = complex_two_free_state,
    .resume = complex_two_resume,
};

static PyObject *
complex_two_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    ComplexTwoState *state = complex_two_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &complex_two_vtable, state) < 0) {
        complex_two_free_state(state);
        return NULL;
    }
    PyObject *result = complex_two_continue(state, value, 1);
    adapter_leave(&frame);
    complex_two_free_state(state);
    return result;
}

static int
complex_two_arguments(
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames,
    PyObject **real,
    PyObject **imag
)
{
    Py_ssize_t positional = PyVectorcall_NARGS(nargsf);
    if (positional > 2) {
        return 0;
    }
    *real = positional >= 1 ? args[0] : NULL;
    *imag = positional >= 2 ? args[1] : NULL;
    Py_ssize_t keyword_count = kwnames == NULL ? 0 : PyTuple_GET_SIZE(kwnames);
    for (Py_ssize_t index = 0; index < keyword_count; index++) {
        PyObject *name = PyTuple_GET_ITEM(kwnames, index);
        PyObject *argument = args[positional + index];
        if (PyUnicode_CompareWithASCIIString(name, "real") == 0 && *real == NULL) {
            *real = argument;
        }
        else if (PyUnicode_CompareWithASCIIString(name, "imag") == 0 && *imag == NULL) {
            *imag = argument;
        }
        else {
            return 0;
        }
    }
    return *real != NULL && *imag != NULL;
}

static PyObject *
adapter_complex_two(
    PyObject *real,
    PyObject *imag
)
{
    ComplexTwoState state = {
        .real_argument = real,
        .imag_argument = imag,
        .real_value = NULL,
        .real_kind = PROTOCOL_COMPLEX,
        .imag_kind = PROTOCOL_COMPLEX_FLOAT,
        .phase = COMPLEX_TWO_WAIT_REAL,
    };
    if (protocol_kind_for_object(
            real,
            "__complex__",
            "__float__",
            "__index__",
            PROTOCOL_COMPLEX,
            PROTOCOL_COMPLEX_FLOAT,
            PROTOCOL_COMPLEX_INDEX,
            &state.real_kind
        ) < 0 || protocol_kind_for_object(
            imag,
            "__float__",
            "__index__",
            NULL,
            PROTOCOL_COMPLEX_FLOAT,
            PROTOCOL_COMPLEX_INDEX,
            PROTOCOL_COMPLEX_FLOAT,
            &state.imag_kind
        ) < 0) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &complex_two_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = complex_two_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.real_value);
    return result;
}

PyObject *
adapter_core_type_vectorcall(
    PyObject *callable,
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames
)
{
    if (callable == (PyObject *)&PyComplex_Type) {
        PyObject *real;
        PyObject *imaginary;
        if (complex_two_arguments(
                args, nargsf, kwnames, &real, &imaginary
            )) {
            int real_python = protocol_type_has_python_method(
                real, "__complex__"
            );
            if (real_python == 0) {
                real_python = protocol_type_has_python_method(real, "__float__");
            }
            int imaginary_python = protocol_type_has_python_method(
                imaginary, "__float__"
            );
            if (imaginary_python == 0) {
                imaginary_python = protocol_type_has_python_method(
                    imaginary, "__index__"
                );
            }
            if (real_python < 0 || imaginary_python < 0) {
                return NULL;
            }
            if ((real_python || imaginary_python) &&
                !PyUnicode_Check(real) &&
                !PyBytes_Check(real) &&
                !PyByteArray_Check(real)) {
                return adapter_complex_two(real, imaginary);
            }
        }
    }
    vectorcallfunc original = NULL;
    ProtocolResumeKind kind = PROTOCOL_INT;
    if (callable == (PyObject *)&PyBool_Type) {
        original = original_bool_vectorcall;
        kind = protocol_vectorcall_kind(
            args, nargsf, "__bool__", "__len__", NULL,
            PROTOCOL_BOOL, PROTOCOL_LEN, PROTOCOL_BOOL
        );
    }
    else if (callable == (PyObject *)&PyLong_Type) {
        original = original_int_vectorcall;
        kind = protocol_vectorcall_kind(
            args, nargsf, "__int__", "__index__",
#if PY_VERSION_HEX < 0x030e0000
            "__trunc__",
#else
            NULL,
#endif
            PROTOCOL_INT, PROTOCOL_INDEX, PROTOCOL_TRUNC
        );
    }
    else if (callable == (PyObject *)&PyFloat_Type) {
        original = original_float_vectorcall;
        kind = protocol_vectorcall_kind(
            args, nargsf, "__float__", "__index__", NULL,
            PROTOCOL_FLOAT, PROTOCOL_FLOAT_INDEX, PROTOCOL_FLOAT
        );
    }
    else if (callable == (PyObject *)&PyComplex_Type) {
        original = original_complex_vectorcall;
        kind = protocol_vectorcall_kind(
            args, nargsf, "__complex__", "__float__", "__index__",
            PROTOCOL_COMPLEX, PROTOCOL_COMPLEX_FLOAT, PROTOCOL_COMPLEX_INDEX
        );
    }
    else if (callable == (PyObject *)&PyUnicode_Type) {
        original = original_str_vectorcall;
        kind = PROTOCOL_STR;
    }
    else {
        return original_type_vectorcall(callable, args, nargsf, kwnames);
    }
    if (kind < 0) {
        return NULL;
    }
    if (original == NULL) {
        original = original_type_vectorcall;
    }
    return protocol_type_vectorcall_with(
        original, callable, args, nargsf, kwnames, kind
    );
}

PyObject *
adapter_repr(PyObject *Py_UNUSED(self), PyObject *args)
{
    return protocol_call_args(original_repr, args, PROTOCOL_REPR);
}

PyObject *
adapter_format(PyObject *Py_UNUSED(self), PyObject *args)
{
    ProtocolState state = {.kind = PROTOCOL_FORMAT};
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &protocol_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = PyObject_Call(original_format, args, NULL);
    adapter_leave(&frame);
    return result;
}

PyObject *
adapter_hash(PyObject *Py_UNUSED(self), PyObject *args)
{
    return protocol_call_args(original_hash, args, PROTOCOL_HASH);
}
