#ifndef ALEFF_CONTINUATION_ADAPTERS_HEAPQ_H
#define ALEFF_CONTINUATION_ADAPTERS_HEAPQ_H

#include "internal.h"

int adapter_heapq_install(PyObject *heapq);
void adapter_heapq_rollback(void);

#endif
