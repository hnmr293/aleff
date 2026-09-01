#ifndef ALEFF_CONTINUATION_ADAPTERS_PICKLE_H
#define ALEFF_CONTINUATION_ADAPTERS_PICKLE_H

#include "internal.h"

int adapter_pickle_install(PyObject *pickle_module);
void adapter_pickle_rollback(void);

/* Complete object.__reduce_ex__ after its Python __getstate__ callback has
 * escaped into a continuation.  The C implementation's call boundary is not
 * represented in a Python frame snapshot. */
PyObject *adapter_pickle_complete_default_reduce(PyObject *worker, PyObject *object, PyObject *state);
PyObject *adapter_pickle_normalize_result(void *snapshot, PyObject *value);

#endif
