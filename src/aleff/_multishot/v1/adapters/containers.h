#ifndef ALEFF_CONTINUATION_ADAPTERS_CONTAINERS_H
#define ALEFF_CONTINUATION_ADAPTERS_CONTAINERS_H

#include "internal.h"

typedef enum {
    COLLECT_LIST,
    COLLECT_TUPLE,
    COLLECT_DICT,
    COLLECT_SET,
    COLLECT_FROZENSET,
    COLLECT_BYTES,
    COLLECT_BYTEARRAY,
} CollectKind;

int dict_item_has_python_hash(PyObject *key);
PyObject *collect_set_iterable(PyObject *iterable, CollectKind kind);
PyObject *collect_iterable(PyObject *iterable, CollectKind kind);
PyObject *adapter_collect_vectorcall(
    PyObject *callable,
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames,
    PyTypeObject *expected_type,
    CollectKind kind,
    vectorcallfunc original
);

extern binaryfunc original_dict_subscript;
extern initproc original_dict_init;
extern vectorcallfunc original_dict_vectorcall;
extern initproc original_set_init;
extern vectorcallfunc original_set_vectorcall;
extern newfunc original_frozenset_new;
extern vectorcallfunc original_frozenset_vectorcall;
extern newfunc original_bytes_new;
extern initproc original_bytearray_init;

PyObject *adapter_list_extend(PyObject *, PyObject *);
PyObject *adapter_list_count(PyObject *, PyObject *);
PyObject *adapter_sequence_count(PyObject *, PyObject *);
PyObject *adapter_sequence_index(PyObject *, PyObject *);
PyObject *adapter_list_remove(PyObject *, PyObject *);
int adapter_sequence_contains(PyObject *, PyObject *);
PyObject *adapter_list_repr(PyObject *);
Py_hash_t adapter_tuple_hash(PyObject *);
PyObject *adapter_sequence_richcompare(PyObject *, PyObject *, int);
PyObject *adapter_list_sort(PyObject *, PyObject *, PyObject *);
Py_hash_t adapter_slice_hash(PyObject *);
PyObject *adapter_sorted(PyObject *, PyObject *, PyObject *);
PyObject *adapter_list_vectorcall(
    PyObject *, PyObject *const *, size_t, PyObject *
);
PyObject *adapter_tuple_vectorcall(
    PyObject *, PyObject *const *, size_t, PyObject *
);
int adapter_list_init(PyObject *, PyObject *, PyObject *);
PyObject *adapter_tuple_new(PyTypeObject *, PyObject *, PyObject *);
int adapter_containers_install(void);

extern vectorcallfunc original_list_vectorcall;
extern initproc original_list_init;
extern newfunc original_tuple_new;
extern vectorcallfunc original_tuple_vectorcall;
extern hashfunc original_slice_hash;
extern PyObject *original_slice_indices;
extern hashfunc original_tuple_hash;
extern richcmpfunc original_list_richcompare;
extern richcmpfunc original_tuple_richcompare;

#endif
