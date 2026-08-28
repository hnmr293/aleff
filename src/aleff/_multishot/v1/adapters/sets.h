#ifndef ALEFF_CONTINUATION_ADAPTERS_SETS_H
#define ALEFF_CONTINUATION_ADAPTERS_SETS_H

#include "internal.h"

PyObject *adapter_set_vectorcall(
    PyObject *, PyObject *const *, size_t, PyObject *
);
PyObject *adapter_frozenset_vectorcall(
    PyObject *, PyObject *const *, size_t, PyObject *
);
int adapter_set_init(PyObject *, PyObject *, PyObject *);
PyObject *adapter_frozenset_new(PyTypeObject *, PyObject *, PyObject *);

extern PyMethodDef containers_set_update_method;
extern PyMethodDef containers_set_intersection_update_method;
extern PyMethodDef containers_set_difference_update_method;
extern PyMethodDef containers_set_symmetric_difference_update_method;
extern PyMethodDef containers_set_union_method;
extern PyMethodDef containers_set_intersection_method;
extern PyMethodDef containers_set_difference_method;
extern PyMethodDef containers_set_symmetric_difference_method;
extern PyMethodDef containers_set_isdisjoint_method;
extern PyMethodDef containers_set_issubset_method;
extern PyMethodDef containers_set_issuperset_method;
extern PyMethodDef containers_frozenset_union_method;
extern PyMethodDef containers_frozenset_intersection_method;
extern PyMethodDef containers_frozenset_difference_method;
extern PyMethodDef containers_frozenset_symmetric_difference_method;
extern PyMethodDef containers_frozenset_isdisjoint_method;
extern PyMethodDef containers_frozenset_issubset_method;
extern PyMethodDef containers_frozenset_issuperset_method;

#endif
