#ifndef ALEFF_CONTINUATION_ADAPTERS_STRUCT_H
#define ALEFF_CONTINUATION_ADAPTERS_STRUCT_H

#include "internal.h"

int adapter_struct_install(PyObject *struct_module);
void adapter_struct_rollback(void);

#endif
