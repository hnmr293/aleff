#include <stdatomic.h>
#include <string.h>

#include "api.h"
#include "critical_sections.h"
#include "internal.h"

#if defined(_MSC_VER)
#  define ALEFF_THREAD_LOCAL __declspec(thread)
#else
#  define ALEFF_THREAD_LOCAL _Thread_local
#endif

typedef struct AleffAdapterNode AleffAdapterNode;

struct AleffAdapterNode {
    _Atomic unsigned int references;
    _Atomic int owner_alive;
    AleffAdapterNode *previous;
    const AleffAdapterVTable *vtable;
    const void *state;
    PyFrameObject *outer_frame;
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

static ALEFF_THREAD_LOCAL AleffAdapterNode *active_adapter = NULL;

static void
adapter_node_retain(AleffAdapterNode *node)
{
    if (node != NULL) {
        atomic_fetch_add_explicit(&node->references, 1, memory_order_relaxed);
    }
}

static void
adapter_node_release(AleffAdapterNode *node)
{
    while (node != NULL && atomic_fetch_sub_explicit(
        &node->references,
        1,
        memory_order_acq_rel
    ) == 1) {
        AleffAdapterNode *previous = node->previous;
        Py_XDECREF(node->outer_frame);
        PyMem_Free(node);
        node = previous;
    }
}

int
adapter_enter(AleffAdapterFrame *frame, const AleffAdapterVTable *vtable, const void *state)
{
    frame->node = NULL;
    AleffAdapterNode *node = PyMem_Calloc(1, sizeof(*node));
    if (node == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    atomic_init(&node->references, 1);
    atomic_init(&node->owner_alive, 1);
    node->previous = active_adapter;
    node->vtable = vtable;
    node->state = state;
    node->outer_frame = PyEval_GetFrame();
    Py_XINCREF(node->outer_frame);

    adapter_node_retain(node->previous);
    adapter_node_retain(node);
    AleffAdapterNode *previous = active_adapter;
    active_adapter = node;
    adapter_node_release(previous);
    frame->node = node;
    return 0;
}

void
adapter_leave(AleffAdapterFrame *frame)
{
    AleffAdapterNode *node = frame->node;
    frame->node = NULL;
    if (node == NULL) {
        return;
    }
    if (atomic_exchange_explicit(&node->owner_alive, 0, memory_order_acq_rel) == 0) {
        return;
    }
    node->state = NULL;
    if (active_adapter == node) {
        AleffAdapterNode *previous = node->previous;
        adapter_node_retain(previous);
        active_adapter = previous;
        adapter_node_release(node);
    }
    adapter_node_release(node);
}

void *
adapter_find_state(const AleffAdapterVTable *vtable)
{
    for (AleffAdapterNode *node = active_adapter;
         node != NULL;
         node = node->previous) {
        if (node->vtable == vtable && node->state != NULL) {
            return (void *)node->state;
        }
    }
    return NULL;
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
    for (AleffAdapterNode *node = active_adapter; node != NULL; node = node->previous) {
        if (!atomic_load_explicit(&node->owner_alive, memory_order_acquire)) {
            PyErr_SetString(PyExc_RuntimeError, "adapter owner is no longer alive");
            return NULL;
        }
        if (outer_frame_index(start_frame, depth, node->outer_frame) >= 0) {
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
    for (AleffAdapterNode *node = active_adapter; node != NULL; node = node->previous) {
        int index = outer_frame_index(start_frame, depth, node->outer_frame);
        if (index < 0) {
            continue;
        }
        AleffAdapterSnapshotItem *item = &snapshot->items[item_index];
        item->vtable = node->vtable;
        item->outer_frame_index = index;
        item->prepared = 0;
        item->state = node->vtable->copy_state(node->state);
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
    AleffAdapterNode *live_head;
    PyThreadState *owner_thread;
    PyFrameObject *owner_frame;
    AleffCriticalSectionState critical_sections;
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
    adapter_node_release(token->live_head);
    Py_XDECREF(token->owner_frame);
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
    if (snapshot == NULL) {
        Py_DECREF(frame);
        return NULL;
    }
    AleffAdapterToken *token = PyMem_Calloc(1, sizeof(*token));
    if (token == NULL) {
        aleff_adapter_snapshot_free(snapshot);
        Py_DECREF(frame);
        PyErr_NoMemory();
        return NULL;
    }
    token->snapshot = snapshot;
    token->live_head = active_adapter;
    token->owner_thread = PyThreadState_Get();
    token->owner_frame = frame;
    token->restored = 0;
    adapter_node_retain(token->live_head);
    adapter_node_release(active_adapter);
    active_adapter = NULL;

    PyObject *capsule = PyCapsule_New(token, adapter_token_name, adapter_token_destructor);
    if (capsule == NULL) {
        active_adapter = token->live_head;
        adapter_node_retain(active_adapter);
        adapter_node_release(token->live_head);
        token->live_head = NULL;
        aleff_adapter_snapshot_free(snapshot);
        Py_DECREF(token->owner_frame);
        PyMem_Free(token);
        return NULL;
    }
    /* Only adapter callbacks keep their C frame suspended until this token is
     * restored.  A generic caller (notably asyncio) may unwind first. */
    if (token->live_head != NULL) {
        aleff_critical_sections_suspend(&token->critical_sections);
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
    if (token->restored) {
        PyErr_SetString(PyExc_RuntimeError, "adapter token is already restored");
        return NULL;
    }
    if (token->owner_thread != PyThreadState_Get()) {
        PyErr_SetString(PyExc_RuntimeError, "adapter token belongs to another thread");
        return NULL;
    }
    if (PyEval_GetFrame() != token->owner_frame) {
        PyErr_SetString(PyExc_RuntimeError, "adapter token belongs to another owner");
        return NULL;
    }
    aleff_critical_sections_restore(&token->critical_sections);
    if (active_adapter != NULL) {
        PyErr_SetString(PyExc_RuntimeError, "another adapter owner is active");
        return NULL;
    }
    for (AleffAdapterNode *node = token->live_head; node != NULL; node = node->previous) {
        if (!atomic_load_explicit(&node->owner_alive, memory_order_acquire)) {
            PyErr_SetString(PyExc_RuntimeError, "adapter token owner is no longer alive");
            return NULL;
        }
    }

    /* The owner checks above are the last operation that needs the strong
     * frame reference.  Release it before publishing the restored token so
     * the token capsule cannot retain this frame through its own local
     * ``token`` variable.  Failed restores return before this point and can
     * therefore be retried by the original owner. */
    token->restored = 1;
    Py_CLEAR(token->owner_frame);

    AleffAdapterNode *head = token->live_head;
    adapter_node_retain(head);
    active_adapter = head;
    token->live_head = NULL;
    adapter_node_release(head);
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

int
aleff_adapter_snapshot_prepare(AleffAdapterSnapshot *snapshot)
{
    if (snapshot == NULL) {
        return 0;
    }
    for (Py_ssize_t i = 0; i < snapshot->count; i++) {
        snapshot->items[i].prepared = 0;
    }
    for (Py_ssize_t i = 0; i < snapshot->count; i++) {
        AleffAdapterSnapshotItem *item = &snapshot->items[i];
        if (item->vtable->prepare_resume != NULL &&
            item->vtable->prepare_resume(item->state) < 0) {
            return -1;
        }
        item->prepared = 1;
    }
    return 0;
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
