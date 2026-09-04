#ifndef ALEFF_CONTINUATION_ADAPTERS_FUNCTOOLS_H
#define ALEFF_CONTINUATION_ADAPTERS_FUNCTOOLS_H

#include "internal.h"

int adapter_functools_install(PyObject *functools);
int adapter_functools_has_callable(PyObject *callable);
void adapter_functools_rollback(void);

#endif
