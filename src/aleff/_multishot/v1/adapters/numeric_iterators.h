#ifndef ALEFF_CONTINUATION_ADAPTERS_NUMERIC_ITERATORS_H
#define ALEFF_CONTINUATION_ADAPTERS_NUMERIC_ITERATORS_H

#include "internal.h"

int adapter_numeric_iterators_install(PyObject *math_module);
void adapter_numeric_iterators_rollback(void);

#endif
