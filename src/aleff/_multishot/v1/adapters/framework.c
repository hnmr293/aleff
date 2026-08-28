#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>

#include "api.h"

#if defined(_MSC_VER)
#  define ALEFF_THREAD_LOCAL __declspec(thread)
#else
#  define ALEFF_THREAD_LOCAL _Thread_local
#endif

typedef struct AleffAdapterVTable AleffAdapterVTable;

typedef struct AleffAdapterFrame {
    struct AleffAdapterFrame *previous;
    const AleffAdapterVTable *vtable;
    const void *state;
    PyFrameObject *outer_frame;
} AleffAdapterFrame;

struct AleffAdapterVTable {
    void *(*copy_state)(const void *state);
    void (*free_state)(void *state);
    PyObject *(*resume)(const void *state, PyObject *value);
    int (*prepare_resume)(void *state);
};

typedef struct {
    const AleffAdapterVTable *vtable;
    void *state;
    int outer_frame_index;
    int prepared;
} AleffAdapterSnapshotItem;

struct AleffAdapterSnapshot {
    AleffAdapterSnapshotItem *items;
    Py_ssize_t count;
};

static ALEFF_THREAD_LOCAL AleffAdapterFrame *active_adapter = NULL;

static PyObject *lookup_raw_special(PyObject *object, const char *name);

static void
adapter_enter(AleffAdapterFrame *frame, const AleffAdapterVTable *vtable, const void *state)
{
    frame->previous = active_adapter;
    frame->vtable = vtable;
    frame->state = state;
    frame->outer_frame = PyEval_GetFrame();
    Py_XINCREF(frame->outer_frame);
    active_adapter = frame;
}

static void
adapter_leave(AleffAdapterFrame *frame)
{
    if (active_adapter == frame) {
        active_adapter = frame->previous;
    }
    Py_CLEAR(frame->outer_frame);
}

static int
outer_frame_index(PyFrameObject *start_frame, int depth, PyFrameObject *target)
{
    PyFrameObject *frame = start_frame;
    Py_INCREF(frame);
    int index = 0;
    while (frame != NULL && (depth < 0 || index < depth)) {
        if (frame == target) {
            Py_DECREF(frame);
            return index;
        }
        PyFrameObject *back = PyFrame_GetBack(frame);
        Py_DECREF(frame);
        frame = back;
        index++;
    }
    Py_XDECREF(frame);
    return -1;
}

AleffAdapterSnapshot *
aleff_adapter_snapshot_capture(PyFrameObject *start_frame, int depth)
{
    Py_ssize_t count = 0;
    for (AleffAdapterFrame *frame = active_adapter; frame != NULL; frame = frame->previous) {
        if (outer_frame_index(start_frame, depth, frame->outer_frame) >= 0) {
            count++;
        }
    }

    AleffAdapterSnapshot *snapshot = PyMem_Calloc(1, sizeof(*snapshot));
    if (snapshot == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    if (count == 0) {
        return snapshot;
    }

    snapshot->items = PyMem_Calloc((size_t)count, sizeof(*snapshot->items));
    if (snapshot->items == NULL) {
        PyMem_Free(snapshot);
        PyErr_NoMemory();
        return NULL;
    }

    Py_ssize_t item_index = 0;
    for (AleffAdapterFrame *frame = active_adapter; frame != NULL; frame = frame->previous) {
        int index = outer_frame_index(start_frame, depth, frame->outer_frame);
        if (index < 0) {
            continue;
        }
        AleffAdapterSnapshotItem *item = &snapshot->items[item_index];
        item->vtable = frame->vtable;
        item->outer_frame_index = index;
        item->prepared = 0;
        item->state = frame->vtable->copy_state(frame->state);
        if (item->state == NULL && PyErr_Occurred()) {
            snapshot->count = item_index;
            aleff_adapter_snapshot_free(snapshot);
            return NULL;
        }
        item_index++;
    }
    snapshot->count = item_index;
    return snapshot;
}

void
aleff_adapter_snapshot_free(AleffAdapterSnapshot *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    for (Py_ssize_t i = 0; i < snapshot->count; i++) {
        AleffAdapterSnapshotItem *item = &snapshot->items[i];
        item->vtable->free_state(item->state);
    }
    PyMem_Free(snapshot->items);
    PyMem_Free(snapshot);
}

static AleffAdapterSnapshot *
adapter_snapshot_clone(const AleffAdapterSnapshot *source)
{
    AleffAdapterSnapshot *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    if (source == NULL || source->count == 0) {
        return copy;
    }
    copy->items = PyMem_Calloc((size_t)source->count, sizeof(*copy->items));
    if (copy->items == NULL) {
        PyMem_Free(copy);
        PyErr_NoMemory();
        return NULL;
    }
    for (Py_ssize_t i = 0; i < source->count; i++) {
        const AleffAdapterSnapshotItem *source_item = &source->items[i];
        AleffAdapterSnapshotItem *copy_item = &copy->items[i];
        copy_item->vtable = source_item->vtable;
        copy_item->outer_frame_index = source_item->outer_frame_index;
        copy_item->prepared = 0;
        copy_item->state = source_item->vtable->copy_state(source_item->state);
        if (copy_item->state == NULL && PyErr_Occurred()) {
            copy->count = i;
            aleff_adapter_snapshot_free(copy);
            return NULL;
        }
        copy->count++;
    }
    return copy;
}

typedef struct {
    AleffAdapterSnapshot *snapshot;
    AleffAdapterFrame *live_head;
    int restored;
} AleffAdapterToken;

static const char adapter_token_name[] = "aleff.continuation_adapter_token";

static void
adapter_token_destructor(PyObject *capsule)
{
    AleffAdapterToken *token = PyCapsule_GetPointer(capsule, adapter_token_name);
    if (token == NULL) {
        PyErr_Clear();
        return;
    }
    aleff_adapter_snapshot_free(token->snapshot);
    PyMem_Free(token);
}

PyObject *
aleff_adapter_suspend(void)
{
    PyFrameObject *frame = PyThreadState_GetFrame(PyThreadState_Get());
    if (frame == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "cannot suspend adapters without a Python frame");
        return NULL;
    }
    AleffAdapterSnapshot *snapshot = aleff_adapter_snapshot_capture(frame, -1);
    Py_DECREF(frame);
    if (snapshot == NULL) {
        return NULL;
    }
    AleffAdapterToken *token = PyMem_Malloc(sizeof(*token));
    if (token == NULL) {
        aleff_adapter_snapshot_free(snapshot);
        PyErr_NoMemory();
        return NULL;
    }
    token->snapshot = snapshot;
    token->live_head = active_adapter;
    token->restored = 0;
    active_adapter = NULL;

    PyObject *capsule = PyCapsule_New(token, adapter_token_name, adapter_token_destructor);
    if (capsule == NULL) {
        active_adapter = token->live_head;
        aleff_adapter_snapshot_free(snapshot);
        PyMem_Free(token);
        return NULL;
    }
    return capsule;
}

PyObject *
aleff_adapter_restore(PyObject *capsule)
{
    AleffAdapterToken *token = PyCapsule_GetPointer(capsule, adapter_token_name);
    if (token == NULL) {
        return NULL;
    }
    if (!token->restored) {
        active_adapter = token->live_head;
        token->restored = 1;
    }
    Py_RETURN_NONE;
}

AleffAdapterSnapshot *
aleff_adapter_snapshot_from_token(PyObject *capsule)
{
    if (capsule == NULL || capsule == Py_None) {
        return adapter_snapshot_clone(NULL);
    }
    AleffAdapterToken *token = PyCapsule_GetPointer(capsule, adapter_token_name);
    if (token == NULL) {
        return NULL;
    }
    return adapter_snapshot_clone(token->snapshot);
}

PyObject *
aleff_adapter_resume_before_frame(
    AleffAdapterSnapshot *snapshot,
    int outer_frame_index_value,
    PyObject *value
)
{
    if (snapshot == NULL) {
        return value;
    }
    PyObject *current = value;
    PyObject *pending_exception = NULL;
    if (current == NULL && PyErr_Occurred()) {
        pending_exception = PyErr_GetRaisedException();
    }
    for (Py_ssize_t i = 0; i < snapshot->count; i++) {
        AleffAdapterSnapshotItem *item = &snapshot->items[i];
        if (item->prepared || item->vtable->prepare_resume == NULL) {
            continue;
        }
        if (item->vtable->prepare_resume(item->state) < 0) {
            Py_XDECREF(current);
            Py_XDECREF(pending_exception);
            return NULL;
        }
        item->prepared = 1;
    }
    if (pending_exception != NULL) {
        PyErr_SetRaisedException(pending_exception);
    }
    for (Py_ssize_t i = 0; i < snapshot->count; i++) {
        AleffAdapterSnapshotItem *item = &snapshot->items[i];
        if (item->outer_frame_index != outer_frame_index_value) {
            continue;
        }
        PyObject *next = item->vtable->resume(item->state, current);
        Py_XDECREF(current);
        current = next;
        item->prepared = 0;
    }
    return current;
}


#include "iterators.c"
#include "builtins.c"
#include "containers.c"
#include "adapters_bootstrap.c"
