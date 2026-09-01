#ifndef ALEFF_CONTINUATION_ADAPTERS_COMPRESSION_H
#define ALEFF_CONTINUATION_ADAPTERS_COMPRESSION_H

#include "internal.h"

int adapter_compression_install(
    PyObject *zlib_module,
    PyObject *bz2_module,
    PyObject *lzma_module,
    PyObject *zstd_module
);
void adapter_compression_rollback(void);

#endif
