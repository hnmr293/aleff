#ifndef ALEFF_CONTINUATION_ADAPTERS_ITERATORS_H
#define ALEFF_CONTINUATION_ADAPTERS_ITERATORS_H

#include "internal.h"

PyObject *adapter_reversed_next(PyObject *object);
extern const AleffAdapterVTable map_vtable;

PyObject *adapter_sum(PyObject *, PyObject *, PyObject *);
#if PY_VERSION_HEX >= 0x030e0000
PyObject *adapter_reduce(PyObject *, PyObject *, PyObject *);
#else
PyObject *adapter_reduce(PyObject *, PyObject *);
#endif
PyObject *adapter_all(PyObject *, PyObject *);
PyObject *adapter_any(PyObject *, PyObject *);
PyObject *adapter_map_next(PyObject *);
PyObject *adapter_map_new(PyTypeObject *, PyObject *, PyObject *);
PyObject *adapter_map_vectorcall(
    PyObject *, PyObject *const *, size_t, PyObject *
);
PyObject *adapter_filter_next(PyObject *);
PyObject *adapter_filter_new(PyTypeObject *, PyObject *, PyObject *);
PyObject *adapter_filter_vectorcall(
    PyObject *, PyObject *const *, size_t, PyObject *
);
PyObject *adapter_zip_new(PyTypeObject *, PyObject *, PyObject *);
PyObject *adapter_zip_next(PyObject *);
PyObject *adapter_enumerate_new(PyTypeObject *, PyObject *, PyObject *);
PyObject *adapter_enumerate_vectorcall(
    PyObject *, PyObject *const *, size_t, PyObject *
);
PyObject *adapter_enumerate_next(PyObject *);
PyObject *adapter_reversed_new(PyTypeObject *, PyObject *, PyObject *);
PyObject *adapter_reversed_vectorcall(
    PyObject *, PyObject *const *, size_t, PyObject *
);
Py_ssize_t adapter_zip_basicsize(void);
Py_ssize_t adapter_enumerate_basicsize(void);
Py_ssize_t adapter_reversed_basicsize(void);
Py_ssize_t adapter_filter_basicsize(void);

extern newfunc original_map_new;
extern vectorcallfunc original_map_vectorcall;
extern iternextfunc original_map_next;
extern newfunc original_filter_new;
extern vectorcallfunc original_filter_vectorcall;
extern newfunc original_zip_new;
extern PyTypeObject *tuple_iterator_type;
extern PyTypeObject *list_iterator_type;
extern newfunc original_enumerate_new;
extern vectorcallfunc original_enumerate_vectorcall;
extern newfunc original_reversed_new;
extern vectorcallfunc original_reversed_vectorcall;
extern iternextfunc original_reversed_next;

#endif
