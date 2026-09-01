#ifndef ALEFF_CONTINUATION_ADAPTERS_CODECS_H
#define ALEFF_CONTINUATION_ADAPTERS_CODECS_H

#include "internal.h"

int adapter_codecs_install(PyObject *codecs_module);
void adapter_codecs_rollback(void);

#endif
