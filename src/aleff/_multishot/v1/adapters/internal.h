#ifndef ALEFF_CONTINUATION_ADAPTERS_INTERNAL_H
#define ALEFF_CONTINUATION_ADAPTERS_INTERNAL_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

typedef struct AleffAdapterVTable AleffAdapterVTable;
typedef struct AleffAdapterNode AleffAdapterNode;

typedef struct AleffAdapterFrame {
    AleffAdapterNode *node;
} AleffAdapterFrame;

struct AleffAdapterVTable {
    void *(*copy_state)(const void *state);
    void (*free_state)(void *state);
    PyObject *(*resume)(const void *state, PyObject *value);
    int (*prepare_resume)(void *state);
};

int adapter_enter(
    AleffAdapterFrame *frame,
    const AleffAdapterVTable *vtable,
    const void *state
);
void adapter_leave(AleffAdapterFrame *frame);
void *adapter_find_state(const AleffAdapterVTable *vtable);

PyObject *lookup_raw_special(PyObject *object, const char *name);
PyObject *clone_iterator_for_snapshot(PyObject *iterator);

#endif
