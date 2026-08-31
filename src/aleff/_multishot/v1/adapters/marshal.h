#ifndef ALEFF_CONTINUATION_ADAPTERS_MARSHAL_H
#define ALEFF_CONTINUATION_ADAPTERS_MARSHAL_H

#include "internal.h"

int adapter_marshal_install(PyObject *marshal_module);
void adapter_marshal_rollback(void);

#endif
