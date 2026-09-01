#ifndef ALEFF_CONTINUATION_ADAPTERS_IO_H
#define ALEFF_CONTINUATION_ADAPTERS_IO_H

#include "internal.h"

int adapter_io_install(PyObject *io_module);
void adapter_io_rollback(void);

#endif
