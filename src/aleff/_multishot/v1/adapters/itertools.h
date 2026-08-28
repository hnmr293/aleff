#ifndef ALEFF_CONTINUATION_ADAPTERS_ITERTOOLS_H
#define ALEFF_CONTINUATION_ADAPTERS_ITERTOOLS_H

#include "internal.h"

PyObject *adapter_chain_next(PyObject *);
PyObject *adapter_chain_from_iterable(PyObject *, PyObject *);
PyObject *adapter_accumulate_next(PyObject *);
PyObject *adapter_batched_new(PyTypeObject *, PyObject *, PyObject *);
Py_ssize_t adapter_chain_basicsize(void);
Py_ssize_t adapter_accumulate_basicsize(void);
int adapter_itertools_install(PyObject *);
void adapter_itertools_rollback(void);

extern newfunc original_batched_new;
extern PyTypeObject *original_batched_type;
extern iternextfunc original_accumulate_next;
extern PyTypeObject *original_accumulate_type;

#endif
