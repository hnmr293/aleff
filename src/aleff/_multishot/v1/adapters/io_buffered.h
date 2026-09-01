#ifndef ALEFF_CONTINUATION_ADAPTERS_IO_BUFFERED_H
#define ALEFF_CONTINUATION_ADAPTERS_IO_BUFFERED_H

#include "internal.h"

int adapter_io_buffered_install(PyObject *io_module);
void adapter_io_buffered_rollback(void);

#endif
