#ifndef ALEFF_CONTINUATION_ADAPTERS_BUILTINS_H
#define ALEFF_CONTINUATION_ADAPTERS_BUILTINS_H

#include "internal.h"

PyObject *adapter_next(PyObject *, PyObject *);
PyObject *adapter_len(PyObject *, PyObject *);
PyObject *adapter_ascii(PyObject *, PyObject *);
PyObject *adapter_hasattr(PyObject *, PyObject *);
PyObject *adapter_getattr(PyObject *, PyObject *);
PyObject *adapter_dir(PyObject *, PyObject *);
PyObject *adapter_isinstance(PyObject *, PyObject *);
PyObject *adapter_issubclass(PyObject *, PyObject *);
PyObject *adapter_setattr(PyObject *, PyObject *);
PyObject *adapter_delattr(PyObject *, PyObject *);
PyObject *adapter_anext(PyObject *, PyObject *);
PyObject *adapter_input(PyObject *, PyObject *);
PyObject *adapter_open(PyObject *, PyObject *, PyObject *);
PyObject *adapter_import(PyObject *, PyObject *, PyObject *);
PyObject *adapter_build_class(PyObject *, PyObject *, PyObject *);
PyObject *adapter_print(PyObject *, PyObject *, PyObject *);
PyObject *adapter_type_vectorcall(
    PyObject *, PyObject *const *, size_t, PyObject *
);
PyObject *adapter_bin(PyObject *, PyObject *);
PyObject *adapter_oct(PyObject *, PyObject *);
PyObject *adapter_hex(PyObject *, PyObject *);
PyObject *adapter_min(PyObject *, PyObject *, PyObject *);
PyObject *adapter_max(PyObject *, PyObject *, PyObject *);

extern PyObject *original_dir;
extern PyObject *original_input;
extern PyObject *original_anext;
extern PyObject *original_open;
extern PyObject *original_import;
extern PyObject *original_build_class;
extern PyObject *import_get_module_lock;
extern PyObject *import_global_lock_held;
extern PyObject *import_global_lock_acquire;
extern PyTypeObject AleffAnextAwaitable_Type;

#endif
