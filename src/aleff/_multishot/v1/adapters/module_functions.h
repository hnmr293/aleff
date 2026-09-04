#ifndef ALEFF_CONTINUATION_ADAPTERS_MODULE_FUNCTIONS_H
#define ALEFF_CONTINUATION_ADAPTERS_MODULE_FUNCTIONS_H

#include "internal.h"

typedef enum {
    ALEFF_MODULE_FUNCTION_ALLOW_CALLABLE,
    ALEFF_MODULE_FUNCTION_REQUIRE_C,
} AleffModuleFunctionKind;

PyObject *adapter_module_function_create(
    PyObject *original,
    const char *module_name,
    const char *function_name,
    PyCFunction wrapper,
    PyMethodDef *method,
    AleffModuleFunctionKind kind
);

int adapter_module_functions_install(
    PyObject *module,
    const char *module_name,
    const char *const *function_names,
    PyObject **originals,
    PyMethodDef *methods,
    PyCFunction *wrappers,
    Py_ssize_t count,
    AleffModuleFunctionKind kind
);
int adapter_module_function_is_registered(PyCFunction wrapper);

#endif
