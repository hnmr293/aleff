#ifndef ALEFF_CONTINUATION_ADAPTERS_MAPPINGS_H
#define ALEFF_CONTINUATION_ADAPTERS_MAPPINGS_H

#include "internal.h"

PyObject *adapter_dict_richcompare(PyObject *, PyObject *, int);
PyObject *adapter_dict_get(PyObject *, PyObject *);
PyObject *adapter_dict_pop(PyObject *, PyObject *);
PyObject *adapter_dict_vectorcall(
    PyObject *, PyObject *const *, size_t, PyObject *
);
int adapter_dict_init(PyObject *, PyObject *, PyObject *);

extern PyMethodDef containers_dict_fromkeys_method;
extern PyMethodDef containers_dict_update_method;
extern PyMethodDef containers_dict_getitem_method;
extern PyMethodDef containers_dict_setitem_method;
extern PyMethodDef containers_dict_delitem_method;
extern PyMethodDef containers_dict_contains_method;
extern PyMethodDef containers_dict_eq_method;
extern PyMethodDef containers_dict_ne_method;

#endif
