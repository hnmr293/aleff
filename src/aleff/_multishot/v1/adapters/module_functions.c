#include "module_functions.h"

static PyCFunction *registered_wrappers = NULL;
static Py_ssize_t registered_wrapper_count = 0;
static Py_ssize_t registered_wrapper_capacity = 0;

static int
adapter_module_function_register(PyCFunction wrapper)
{
    for (Py_ssize_t index = 0; index < registered_wrapper_count; index++) {
        if (registered_wrappers[index] == wrapper) {
            return 0;
        }
    }
    if (registered_wrapper_count == registered_wrapper_capacity) {
        Py_ssize_t capacity = registered_wrapper_capacity == 0
            ? 16
            : registered_wrapper_capacity * 2;
        if (capacity < registered_wrapper_capacity) {
            PyErr_NoMemory();
            return -1;
        }
        PyCFunction *wrappers = PyMem_Realloc(
            registered_wrappers,
            (size_t)capacity * sizeof(*wrappers)
        );
        if (wrappers == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        registered_wrappers = wrappers;
        registered_wrapper_capacity = capacity;
    }
    registered_wrappers[registered_wrapper_count] = wrapper;
    registered_wrapper_count++;
    return 0;
}

int
adapter_module_function_is_registered(PyCFunction wrapper)
{
    for (Py_ssize_t index = 0; index < registered_wrapper_count; index++) {
        if (registered_wrappers[index] == wrapper) {
            return 1;
        }
    }
    return 0;
}

static PyObject *
wrap_python_callable(PyObject *original, PyObject *bridge)
{
    PyObject *globals = PyDict_New();
    if (globals == NULL) {
        return NULL;
    }
    PyObject *factory = PyRun_String(
        "lambda bridge: (lambda *args, **kwargs: bridge(*args, **kwargs))",
        Py_eval_input,
        globals,
        globals
    );
    Py_DECREF(globals);
    if (factory == NULL) {
        return NULL;
    }
    PyObject *wrapper = PyObject_CallOneArg(factory, bridge);
    Py_DECREF(factory);
    if (wrapper == NULL) {
        return NULL;
    }
    PyObject *functools = PyImport_ImportModule("functools");
    PyObject *update_wrapper = functools == NULL
        ? NULL : PyObject_GetAttrString(functools, "update_wrapper");
    Py_XDECREF(functools);
    if (update_wrapper == NULL) {
        Py_DECREF(wrapper);
        return NULL;
    }
    PyObject *updated = PyObject_CallFunctionObjArgs(
        update_wrapper, wrapper, original, NULL
    );
    Py_DECREF(update_wrapper);
    Py_DECREF(wrapper);
    return updated;
}

PyObject *
adapter_module_function_create(
    PyObject *original,
    const char *module_name,
    const char *function_name,
    PyCFunction wrapper,
    PyMethodDef *method,
    AleffModuleFunctionKind kind
)
{
    int is_c_function = PyCFunction_Check(original);
    if (kind == ALEFF_MODULE_FUNCTION_REQUIRE_C && !is_c_function) {
        PyErr_Format(
            PyExc_RuntimeError,
            "%s.%s is not a C function",
            module_name,
            function_name
        );
        return NULL;
    }
    if (is_c_function) {
        *method = *((PyCFunctionObject *)original)->m_ml;
    }
    else {
        *method = (PyMethodDef){
            .ml_name = function_name,
            .ml_meth = NULL,
            .ml_flags = 0,
            .ml_doc = NULL,
        };
    }
    method->ml_meth = wrapper;
    method->ml_flags = METH_VARARGS | METH_KEYWORDS;
    PyObject *original_module_name = PyObject_GetAttrString(
        original, "__module__"
    );
    if (original_module_name == NULL) {
        return NULL;
    }
    PyObject *self = is_c_function ? PyCFunction_GET_SELF(original) : NULL;
    PyObject *bridge = PyCFunction_NewEx(method, self, original_module_name);
    Py_DECREF(original_module_name);
    if (bridge == NULL || !PyFunction_Check(original)) {
        return bridge;
    }
    PyObject *replacement = wrap_python_callable(original, bridge);
    Py_DECREF(bridge);
    return replacement;
}

int
adapter_module_functions_install(
    PyObject *module,
    const char *module_name,
    const char *const *function_names,
    PyObject **originals,
    PyMethodDef *methods,
    PyCFunction *wrappers,
    Py_ssize_t count,
    AleffModuleFunctionKind kind
)
{
    for (Py_ssize_t index = 0; index < count; index++) {
        originals[index] = PyObject_GetAttrString(
            module, function_names[index]
        );
        if (originals[index] == NULL) {
            return -1;
        }
        PyObject *replacement = adapter_module_function_create(
            originals[index],
            module_name,
            function_names[index],
            wrappers[index],
            &methods[index],
            kind
        );
        if (replacement == NULL) {
            return -1;
        }
        if (adapter_module_function_register(wrappers[index]) < 0) {
            Py_DECREF(replacement);
            return -1;
        }
        int status = PyObject_SetAttrString(
            module, function_names[index], replacement
        );
        Py_DECREF(replacement);
        if (status < 0) {
            return -1;
        }
    }
    return 0;
}
