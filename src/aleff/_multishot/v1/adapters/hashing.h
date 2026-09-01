#ifndef ALEFF_CONTINUATION_ADAPTERS_HASHING_H
#define ALEFF_CONTINUATION_ADAPTERS_HASHING_H

#include "internal.h"

int adapter_hashing_install(PyObject *hashlib_module, PyObject *hmac_module);
void adapter_hashing_rollback(void);

#endif
