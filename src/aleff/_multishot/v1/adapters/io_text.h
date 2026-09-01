#ifndef ALEFF_CONTINUATION_ADAPTERS_IO_TEXT_H
#define ALEFF_CONTINUATION_ADAPTERS_IO_TEXT_H

#include "internal.h"

int adapter_io_text_install(PyObject *io_module);
void adapter_io_text_rollback(void);

#endif
