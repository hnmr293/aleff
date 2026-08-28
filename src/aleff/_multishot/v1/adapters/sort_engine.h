#ifndef ALEFF_SORT_ENGINE_H
#define ALEFF_SORT_ENGINE_H

#include <Python.h>

typedef struct AleffSortEngine AleffSortEngine;

typedef enum {
    ALEFF_SORT_REQUEST_NONE,
    ALEFF_SORT_REQUEST_KEY,
    ALEFF_SORT_REQUEST_LT,
} AleffSortRequestKind;

typedef struct {
    AleffSortRequestKind kind;
    PyObject *left;
    PyObject *right;
} AleffSortRequest;

AleffSortEngine *aleff_sort_engine_new(
    PyObject *items,
    int has_key_function,
    int reverse
);
AleffSortEngine *aleff_sort_engine_copy(const AleffSortEngine *source);
void aleff_sort_engine_free(AleffSortEngine *engine);

/* Run until the engine either needs one Python callback or completes.
 * Returns 1 with request populated, 0 on completion, and -1 on error. */
int aleff_sort_engine_advance(
    AleffSortEngine *engine,
    AleffSortRequest *request
);
int aleff_sort_engine_accept_key(AleffSortEngine *engine, PyObject *key);
int aleff_sort_engine_accept_lt(AleffSortEngine *engine, int result);

/* Restore merge invariants after a callback error and apply the same
 * key/reverse cleanup ordering as list.sort.  The active exception is
 * preserved by the caller. */
void aleff_sort_engine_abort(AleffSortEngine *engine);

PyObject *aleff_sort_engine_materialize(const AleffSortEngine *engine);

#endif
