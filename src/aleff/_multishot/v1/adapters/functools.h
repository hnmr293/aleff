#ifndef ALEFF_CONTINUATION_ADAPTERS_FUNCTOOLS_H
#define ALEFF_CONTINUATION_ADAPTERS_FUNCTOOLS_H

#include "internal.h"

int adapter_functools_install(PyObject *functools);
void adapter_functools_rollback(void);

#endif
