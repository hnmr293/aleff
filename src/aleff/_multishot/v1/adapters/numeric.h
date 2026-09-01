#ifndef ALEFF_CONTINUATION_ADAPTERS_NUMERIC_H
#define ALEFF_CONTINUATION_ADAPTERS_NUMERIC_H

#include "internal.h"

int adapter_numeric_install(PyObject *math_module, PyObject *cmath_module);
void adapter_numeric_rollback(void);
PyObject *adapter_numeric_validate_index_result(PyObject *value);

#endif
