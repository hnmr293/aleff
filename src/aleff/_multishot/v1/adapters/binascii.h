#ifndef ALEFF_CONTINUATION_ADAPTERS_BINASCII_H
#define ALEFF_CONTINUATION_ADAPTERS_BINASCII_H

#include "internal.h"

int adapter_binascii_install(PyObject *module);
void adapter_binascii_rollback(void);

#endif
