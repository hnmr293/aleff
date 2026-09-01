#ifndef ALEFF_CONTINUATION_ADAPTERS_BISECT_H
#define ALEFF_CONTINUATION_ADAPTERS_BISECT_H

#include "internal.h"

int adapter_bisect_install(PyObject *bisect_module);
void adapter_bisect_rollback(void);

#endif
