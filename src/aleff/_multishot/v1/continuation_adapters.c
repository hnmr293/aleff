#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "continuation_adapters.h"

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
};

typedef struct {
    const AleffAdapterVTable *vtable;
    void *state;
    int outer_frame_index;
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
    for (Py_ssize_t i = 0; i < snapshot->count; i++) {
        AleffAdapterSnapshotItem *item = &snapshot->items[i];
        if (item->outer_frame_index != outer_frame_index_value) {
            continue;
        }
        PyObject *next = item->vtable->resume(item->state, current);
        Py_XDECREF(current);
        current = next;
    }
    return current;
}

typedef enum {
    SUM_WAIT_NEXT,
    SUM_WAIT_ADD,
} SumPhase;

typedef struct {
    PyObject *iterator;
    PyObject *result;
    SumPhase phase;
} SumState;

static void *
sum_copy_state(const void *raw_state)
{
    const SumState *state = raw_state;
    SumState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->iterator = Py_NewRef(state->iterator);
    copy->result = Py_NewRef(state->result);
    copy->phase = state->phase;
    return copy;
}

static void
sum_free_state(void *raw_state)
{
    SumState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->iterator);
    Py_DECREF(state->result);
    PyMem_Free(state);
}

static PyObject *sum_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable sum_vtable = {
    .copy_state = sum_copy_state,
    .free_state = sum_free_state,
    .resume = sum_resume,
};

static PyObject *
sum_continue(SumState *state, PyObject *resumed_value, int is_resumed)
{
    PyObject *item = NULL;
    if (is_resumed) {
        if (state->phase == SUM_WAIT_NEXT) {
            item = Py_NewRef(resumed_value);
        }
        else {
            Py_SETREF(state->result, Py_NewRef(resumed_value));
        }
    }

    for (;;) {
        if (item == NULL) {
            state->phase = SUM_WAIT_NEXT;
            item = PyIter_Next(state->iterator);
            if (item == NULL) {
                if (PyErr_Occurred()) {
                    return NULL;
                }
                return Py_NewRef(state->result);
            }
        }

        state->phase = SUM_WAIT_ADD;
        PyObject *next_result = PyNumber_Add(state->result, item);
        Py_DECREF(item);
        item = NULL;
        if (next_result == NULL) {
            return NULL;
        }
        Py_SETREF(state->result, next_result);
    }
}

static PyObject *
sum_resume(const void *raw_state, PyObject *value)
{
    const SumState *source = raw_state;
    if (value == NULL) {
        if (source->phase == SUM_WAIT_NEXT && PyErr_ExceptionMatches(PyExc_StopIteration)) {
            PyErr_Clear();
            return Py_NewRef(source->result);
        }
        return NULL;
    }
    SumState *state = sum_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &sum_vtable, state);
    PyObject *result = sum_continue(state, value, 1);
    adapter_leave(&frame);
    sum_free_state(state);
    return result;
}

static PyObject *
adapter_sum(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"iterable", "start", NULL};
    PyObject *iterable;
    PyObject *start = NULL;
    PyObject *owned_start = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O:sum", keywords, &iterable, &start)) {
        return NULL;
    }
    if (start == NULL) {
        owned_start = PyLong_FromLong(0);
        if (owned_start == NULL) {
            return NULL;
        }
        start = owned_start;
    }
    if (PyUnicode_Check(start)) {
        PyErr_SetString(PyExc_TypeError, "sum() can't sum strings [use ''.join(seq) instead]");
        Py_XDECREF(owned_start);
        return NULL;
    }
    if (PyBytes_Check(start)) {
        PyErr_SetString(PyExc_TypeError, "sum() can't sum bytes [use b''.join(seq) instead]");
        Py_XDECREF(owned_start);
        return NULL;
    }
    if (PyByteArray_Check(start)) {
        PyErr_SetString(PyExc_TypeError, "sum() can't sum bytearray [use b''.join(seq) instead]");
        Py_XDECREF(owned_start);
        return NULL;
    }

    SumState state = {
        .iterator = PyObject_GetIter(iterable),
        .result = Py_NewRef(start),
        .phase = SUM_WAIT_NEXT,
    };
    if (state.iterator == NULL) {
        Py_DECREF(state.result);
        Py_XDECREF(owned_start);
        return NULL;
    }
    Py_XDECREF(owned_start);

    AleffAdapterFrame frame;
    adapter_enter(&frame, &sum_vtable, &state);
    PyObject *result = sum_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_DECREF(state.iterator);
    Py_DECREF(state.result);
    return result;
}

typedef enum {
    REDUCE_WAIT_NEXT,
    REDUCE_WAIT_CALL,
} ReducePhase;

typedef struct {
    PyObject *iterator;
    PyObject *function;
    PyObject *accumulator;
    PyObject *item;
    ReducePhase phase;
} ReduceState;

static void *
reduce_copy_state(const void *raw_state)
{
    const ReduceState *state = raw_state;
    ReduceState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = (ReduceState){
        .iterator = Py_NewRef(state->iterator),
        .function = Py_NewRef(state->function),
        .accumulator = Py_XNewRef(state->accumulator),
        .item = Py_XNewRef(state->item),
        .phase = state->phase,
    };
    return copy;
}

static void
reduce_free_state(void *raw_state)
{
    ReduceState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->iterator);
    Py_DECREF(state->function);
    Py_XDECREF(state->accumulator);
    Py_XDECREF(state->item);
    PyMem_Free(state);
}

static PyObject *reduce_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable reduce_vtable = {
    .copy_state = reduce_copy_state,
    .free_state = reduce_free_state,
    .resume = reduce_resume,
};

static PyObject *
reduce_finish(ReduceState *state)
{
    if (state->accumulator != NULL) {
        return Py_NewRef(state->accumulator);
    }
    PyErr_SetString(
        PyExc_TypeError,
        "reduce() of empty iterable with no initial value"
    );
    return NULL;
}

static PyObject *
reduce_continue(ReduceState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed) {
        if (state->phase == REDUCE_WAIT_NEXT) {
            state->item = Py_NewRef(resumed_value);
        }
        else {
            Py_SETREF(state->accumulator, Py_NewRef(resumed_value));
            Py_CLEAR(state->item);
        }
    }

    for (;;) {
        if (state->item == NULL) {
            state->phase = REDUCE_WAIT_NEXT;
            state->item = Py_TYPE(state->iterator)->tp_iternext(state->iterator);
            if (state->item == NULL) {
                if (PyErr_Occurred()) {
                    if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                        return NULL;
                    }
                    PyErr_Clear();
                }
                return reduce_finish(state);
            }
        }
        if (state->accumulator == NULL) {
            state->accumulator = state->item;
            state->item = NULL;
            continue;
        }
        state->phase = REDUCE_WAIT_CALL;
        PyObject *next_accumulator = PyObject_CallFunctionObjArgs(
            state->function,
            state->accumulator,
            state->item,
            NULL
        );
        Py_CLEAR(state->item);
        if (next_accumulator == NULL) {
            return NULL;
        }
        Py_SETREF(state->accumulator, next_accumulator);
    }
}

static PyObject *
reduce_resume(const void *raw_state, PyObject *value)
{
    const ReduceState *source = raw_state;
    if (value == NULL) {
        if (source->phase != REDUCE_WAIT_NEXT) {
            return NULL;
        }
        if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
            return NULL;
        }
    }
    ReduceState *state = reduce_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &reduce_vtable, state);
    PyObject *result;
    if (value == NULL) {
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        result = reduce_finish(state);
    }
    else {
        result = reduce_continue(state, value, 1);
    }
    adapter_leave(&frame);
    reduce_free_state(state);
    return result;
}

static PyObject *
adapter_reduce(PyObject *Py_UNUSED(self), PyObject *args)
{
    PyObject *function;
    PyObject *iterable;
    PyObject *initial = NULL;
    if (!PyArg_ParseTuple(args, "OO|O:reduce", &function, &iterable, &initial)) {
        return NULL;
    }
    PyObject *iterator = PyObject_GetIter(iterable);
    if (iterator == NULL) {
        return NULL;
    }
    ReduceState state = {
        .iterator = iterator,
        .function = function,
        .accumulator = Py_XNewRef(initial),
        .item = NULL,
        .phase = REDUCE_WAIT_NEXT,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &reduce_vtable, &state);
    PyObject *result = reduce_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_DECREF(iterator);
    Py_XDECREF(state.accumulator);
    Py_XDECREF(state.item);
    return result;
}

static void *
empty_copy_state(const void *Py_UNUSED(state))
{
    return PyMem_Malloc(1);
}

static void
empty_free_state(void *state)
{
    PyMem_Free(state);
}

static PyObject *
map_resume(const void *Py_UNUSED(state), PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    return Py_NewRef(value);
}

static const AleffAdapterVTable map_vtable = {
    .copy_state = empty_copy_state,
    .free_state = empty_free_state,
    .resume = map_resume,
};

static iternextfunc original_map_next = NULL;
static newfunc original_map_new = NULL;

static PyObject *
adapter_map_next(PyObject *map)
{
    AleffAdapterFrame frame;
    adapter_enter(&frame, &map_vtable, NULL);
    PyObject *result = original_map_next(map);
    adapter_leave(&frame);
    return result;
}

typedef struct {
    PyTypeObject *type;
    PyObject *args;
    PyObject *kwargs;
    PyObject *converted_args;
    Py_ssize_t index;
} MapConstructorState;

static void *
map_constructor_copy_state(const void *raw_state)
{
    const MapConstructorState *state = raw_state;
    MapConstructorState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->type = (PyTypeObject *)Py_NewRef((PyObject *)state->type);
    copy->args = Py_NewRef(state->args);
    copy->kwargs = Py_XNewRef(state->kwargs);
    copy->converted_args = PyList_GetSlice(
        state->converted_args,
        0,
        PyList_GET_SIZE(state->converted_args)
    );
    if (copy->converted_args == NULL) {
        Py_DECREF(copy->type);
        Py_DECREF(copy->args);
        Py_XDECREF(copy->kwargs);
        PyMem_Free(copy);
        return NULL;
    }
    copy->index = state->index;
    return copy;
}

static void
map_constructor_free_state(void *raw_state)
{
    MapConstructorState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->type);
    Py_DECREF(state->args);
    Py_XDECREF(state->kwargs);
    Py_DECREF(state->converted_args);
    PyMem_Free(state);
}

static PyObject *map_constructor_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable map_constructor_vtable = {
    .copy_state = map_constructor_copy_state,
    .free_state = map_constructor_free_state,
    .resume = map_constructor_resume,
};

static PyObject *
map_constructor_continue(
    MapConstructorState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (!PyIter_Check(resumed_value)) {
            PyErr_Format(
                PyExc_TypeError,
                "iter() returned non-iterator of type '%.200s'",
                Py_TYPE(resumed_value)->tp_name
            );
            return NULL;
        }
        if (PyList_Append(state->converted_args, resumed_value) < 0) {
            return NULL;
        }
        state->index++;
    }
    Py_ssize_t argument_count = PyTuple_GET_SIZE(state->args);
    while (state->index < argument_count) {
        PyObject *iterator = PyObject_GetIter(
            PyTuple_GET_ITEM(state->args, state->index)
        );
        if (iterator == NULL) {
            return NULL;
        }
        int status = PyList_Append(state->converted_args, iterator);
        Py_DECREF(iterator);
        if (status < 0) {
            return NULL;
        }
        state->index++;
    }
    PyObject *converted_tuple = PyList_AsTuple(state->converted_args);
    if (converted_tuple == NULL) {
        return NULL;
    }
    PyObject *result = original_map_new(
        state->type,
        converted_tuple,
        state->kwargs
    );
    Py_DECREF(converted_tuple);
    return result;
}

static PyObject *
map_constructor_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    MapConstructorState *state = map_constructor_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &map_constructor_vtable, state);
    PyObject *result = map_constructor_continue(state, value, 1);
    adapter_leave(&frame);
    map_constructor_free_state(state);
    return result;
}

static PyObject *
adapter_map_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    if (PyTuple_GET_SIZE(args) < 2) {
        return original_map_new(type, args, kwargs);
    }
    PyObject *converted_args = PyList_New(1);
    if (converted_args == NULL) {
        return NULL;
    }
    PyList_SET_ITEM(
        converted_args,
        0,
        Py_NewRef(PyTuple_GET_ITEM(args, 0))
    );
    MapConstructorState state = {
        .type = type,
        .args = args,
        .kwargs = kwargs,
        .converted_args = converted_args,
        .index = 1,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &map_constructor_vtable, &state);
    PyObject *result = map_constructor_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_DECREF(converted_args);
    return result;
}

typedef struct {
    PyObject_HEAD
    Py_ssize_t tuplesize;
    PyObject *ittuple;
    PyObject *result;
    int strict;
} AleffZipObject;

static newfunc original_zip_new = NULL;

static PyObject *zip_constructor_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable zip_constructor_vtable = {
    .copy_state = map_constructor_copy_state,
    .free_state = map_constructor_free_state,
    .resume = zip_constructor_resume,
};

static PyObject *
zip_constructor_continue(
    MapConstructorState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (!PyIter_Check(resumed_value)) {
            PyErr_Format(
                PyExc_TypeError,
                "iter() returned non-iterator of type '%.200s'",
                Py_TYPE(resumed_value)->tp_name
            );
            return NULL;
        }
        if (PyList_Append(state->converted_args, resumed_value) < 0) {
            return NULL;
        }
        state->index++;
    }
    while (state->index < PyTuple_GET_SIZE(state->args)) {
        PyObject *iterator = PyObject_GetIter(PyTuple_GET_ITEM(state->args, state->index));
        if (iterator == NULL) {
            return NULL;
        }
        int status = PyList_Append(state->converted_args, iterator);
        Py_DECREF(iterator);
        if (status < 0) {
            return NULL;
        }
        state->index++;
    }
    PyObject *converted = PyList_AsTuple(state->converted_args);
    if (converted == NULL) {
        return NULL;
    }
    PyObject *result = original_zip_new(state->type, converted, state->kwargs);
    Py_DECREF(converted);
    return result;
}

static PyObject *
zip_constructor_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    MapConstructorState *state = map_constructor_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &zip_constructor_vtable, state);
    PyObject *result = zip_constructor_continue(state, value, 1);
    adapter_leave(&frame);
    map_constructor_free_state(state);
    return result;
}

static PyObject *
adapter_zip_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *converted = PyList_New(0);
    if (converted == NULL) {
        return NULL;
    }
    MapConstructorState state = {
        .type = type,
        .args = args,
        .kwargs = kwargs,
        .converted_args = converted,
        .index = 0,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &zip_constructor_vtable, &state);
    PyObject *result = zip_constructor_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_DECREF(converted);
    return result;
}

typedef enum {
    ZIP_WAIT_NEXT,
    ZIP_WAIT_STRICT_PROBE,
} ZipPhase;

typedef struct {
    AleffZipObject *zip;
    PyObject *iterators;
    PyObject *items;
    Py_ssize_t index;
    ZipPhase phase;
} ZipState;

typedef struct {
    PyObject_HEAD
    Py_ssize_t index;
    PyObject *sequence;
} AleffTupleIterator;

static PyTypeObject *tuple_iterator_type = NULL;

static PyObject *
clone_iterator_for_snapshot(PyObject *iterator)
{
    if (Py_IS_TYPE(iterator, tuple_iterator_type)) {
        AleffTupleIterator *source = (AleffTupleIterator *)iterator;
        if (source->sequence == NULL) {
            return Py_NewRef(iterator);
        }
        PyObject *copy = PyObject_GetIter(source->sequence);
        if (copy == NULL) {
            return NULL;
        }
        ((AleffTupleIterator *)copy)->index = source->index;
        return copy;
    }
    return Py_NewRef(iterator);
}

static PyObject *
zip_snapshot_iterators(const ZipState *state)
{
    Py_ssize_t size = PyTuple_GET_SIZE(state->iterators);
    PyObject *copy = PyTuple_New(size);
    if (copy == NULL) {
        return NULL;
    }
    for (Py_ssize_t index = 0; index < size; index++) {
        PyObject *iterator = PyTuple_GET_ITEM(state->iterators, index);
        PyObject *item = index > state->index
            ? clone_iterator_for_snapshot(iterator)
            : Py_NewRef(iterator);
        if (item == NULL) {
            Py_DECREF(copy);
            return NULL;
        }
        PyTuple_SET_ITEM(copy, index, item);
    }
    return copy;
}

static void *
zip_copy_state(const void *raw_state)
{
    const ZipState *state = raw_state;
    ZipState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->zip = (AleffZipObject *)Py_NewRef((PyObject *)state->zip);
    copy->iterators = zip_snapshot_iterators(state);
    if (copy->iterators == NULL) {
        Py_DECREF(copy->zip);
        PyMem_Free(copy);
        return NULL;
    }
    copy->items = PyList_GetSlice(state->items, 0, PyList_GET_SIZE(state->items));
    if (copy->items == NULL) {
        Py_DECREF(copy->zip);
        Py_DECREF(copy->iterators);
        PyMem_Free(copy);
        return NULL;
    }
    copy->index = state->index;
    copy->phase = state->phase;
    return copy;
}

static void
zip_free_state(void *raw_state)
{
    ZipState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->zip);
    Py_DECREF(state->iterators);
    Py_DECREF(state->items);
    PyMem_Free(state);
}

static PyObject *zip_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable zip_vtable = {
    .copy_state = zip_copy_state,
    .free_state = zip_free_state,
    .resume = zip_resume,
};

static PyObject *
zip_strict_error(Py_ssize_t argument, int longer)
{
    PyErr_Format(
        PyExc_ValueError,
        "zip() argument %zd is %s than argument 1",
        argument + 1,
        longer ? "longer" : "shorter"
    );
    return NULL;
}

static PyObject *
zip_continue(ZipState *state, PyObject *resumed_value, int is_resumed)
{
    Py_ssize_t size = state->zip->tuplesize;
    if (size == 0) {
        return NULL;
    }
    if (is_resumed) {
        if (state->phase == ZIP_WAIT_STRICT_PROBE) {
            if (resumed_value != NULL) {
                return zip_strict_error(state->index, 1);
            }
            if (PyErr_Occurred()) {
                if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                    return NULL;
                }
                PyErr_Clear();
            }
            state->index++;
        }
        else if (resumed_value != NULL) {
            if (PyList_Append(state->items, resumed_value) < 0) {
                return NULL;
            }
            state->index++;
        }
        else {
            if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                return NULL;
            }
            PyErr_Clear();
            if (!state->zip->strict) {
                return NULL;
            }
            if (state->index > 0) {
                return zip_strict_error(state->index, 0);
            }
            state->phase = ZIP_WAIT_STRICT_PROBE;
            state->index = 1;
        }
    }

    if (state->phase == ZIP_WAIT_STRICT_PROBE) {
        while (state->index < size) {
            PyObject *iterator = PyTuple_GET_ITEM(state->iterators, state->index);
            PyObject *extra = Py_TYPE(iterator)->tp_iternext(iterator);
            if (extra != NULL) {
                Py_DECREF(extra);
                return zip_strict_error(state->index, 1);
            }
            if (PyErr_Occurred()) {
                if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                    return NULL;
                }
                PyErr_Clear();
            }
            state->index++;
        }
        return NULL;
    }

    while (state->index < size) {
        PyObject *iterator = PyTuple_GET_ITEM(state->iterators, state->index);
        PyObject *item = Py_TYPE(iterator)->tp_iternext(iterator);
        if (item == NULL) {
            if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                return NULL;
            }
            PyErr_Clear();
            if (!state->zip->strict) {
                return NULL;
            }
            if (state->index > 0) {
                return zip_strict_error(state->index, 0);
            }
            state->phase = ZIP_WAIT_STRICT_PROBE;
            state->index = 1;
            return zip_continue(state, NULL, 0);
        }
        int status = PyList_Append(state->items, item);
        Py_DECREF(item);
        if (status < 0) {
            return NULL;
        }
        state->index++;
    }
    return PyList_AsTuple(state->items);
}

static PyObject *
zip_resume(const void *raw_state, PyObject *value)
{
    ZipState *state = zip_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &zip_vtable, state);
    Py_SETREF(state->zip->ittuple, Py_NewRef(state->iterators));
    PyObject *result = zip_continue(state, value, 1);
    adapter_leave(&frame);
    zip_free_state(state);
    return result;
}

static PyObject *
adapter_zip_next(PyObject *object)
{
    AleffZipObject *zip = (AleffZipObject *)object;
    ZipState state = {
        .zip = zip,
        .iterators = Py_NewRef(zip->ittuple),
        .items = PyList_New(0),
        .index = 0,
        .phase = ZIP_WAIT_NEXT,
    };
    if (state.items == NULL) {
        Py_DECREF(state.iterators);
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &zip_vtable, &state);
    PyObject *result = zip_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_DECREF(state.iterators);
    Py_DECREF(state.items);
    return result;
}

typedef struct {
    PyObject_HEAD
    Py_ssize_t index;
    PyObject *iterator;
    PyObject *result;
    PyObject *long_index;
    PyObject *one;
} AleffEnumerateObject;

typedef enum {
    ENUM_CONSTRUCTOR_WAIT_INDEX,
    ENUM_CONSTRUCTOR_WAIT_ITER,
} EnumerateConstructorPhase;

typedef struct {
    PyTypeObject *type;
    PyObject *iterable;
    PyObject *start;
    PyObject *iterator;
    EnumerateConstructorPhase phase;
} EnumerateConstructorState;

static newfunc original_enumerate_new = NULL;
static vectorcallfunc original_enumerate_vectorcall = NULL;

static void *
enumerate_constructor_copy_state(const void *raw_state)
{
    const EnumerateConstructorState *state = raw_state;
    EnumerateConstructorState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->type = (PyTypeObject *)Py_NewRef((PyObject *)state->type);
    copy->iterable = Py_NewRef(state->iterable);
    copy->start = Py_XNewRef(state->start);
    copy->iterator = Py_XNewRef(state->iterator);
    copy->phase = state->phase;
    return copy;
}

static void
enumerate_constructor_free_state(void *raw_state)
{
    EnumerateConstructorState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->type);
    Py_DECREF(state->iterable);
    Py_XDECREF(state->start);
    Py_XDECREF(state->iterator);
    PyMem_Free(state);
}

static PyObject *enumerate_constructor_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable enumerate_constructor_vtable = {
    .copy_state = enumerate_constructor_copy_state,
    .free_state = enumerate_constructor_free_state,
    .resume = enumerate_constructor_resume,
};

static PyObject *
enumerate_constructor_finish(EnumerateConstructorState *state)
{
    PyObject *args = state->start == NULL
        ? PyTuple_Pack(1, state->iterator)
        : PyTuple_Pack(2, state->iterator, state->start);
    if (args == NULL) {
        return NULL;
    }
    PyObject *result = original_enumerate_new(state->type, args, NULL);
    Py_DECREF(args);
    return result;
}

static PyObject *
enumerate_constructor_continue(
    EnumerateConstructorState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (state->phase == ENUM_CONSTRUCTOR_WAIT_INDEX) {
            PyObject *index = PyNumber_Index(resumed_value);
            if (index == NULL) {
                return NULL;
            }
            Py_XSETREF(state->start, index);
        }
        else {
            if (!PyIter_Check(resumed_value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "iter() returned non-iterator of type '%.200s'",
                    Py_TYPE(resumed_value)->tp_name
                );
                return NULL;
            }
            Py_XSETREF(state->iterator, Py_NewRef(resumed_value));
            return enumerate_constructor_finish(state);
        }
    }
    else if (state->start != NULL) {
        state->phase = ENUM_CONSTRUCTOR_WAIT_INDEX;
        PyObject *index = PyNumber_Index(state->start);
        if (index == NULL) {
            return NULL;
        }
        Py_SETREF(state->start, index);
    }

    state->phase = ENUM_CONSTRUCTOR_WAIT_ITER;
    state->iterator = PyObject_GetIter(state->iterable);
    if (state->iterator == NULL) {
        return NULL;
    }
    return enumerate_constructor_finish(state);
}

static PyObject *
enumerate_constructor_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    EnumerateConstructorState *state = enumerate_constructor_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &enumerate_constructor_vtable, state);
    PyObject *result = enumerate_constructor_continue(state, value, 1);
    adapter_leave(&frame);
    enumerate_constructor_free_state(state);
    return result;
}

static PyObject *
adapter_enumerate_construct(PyTypeObject *type, PyObject *iterable, PyObject *start)
{
    EnumerateConstructorState state = {
        .type = type,
        .iterable = iterable,
        .start = Py_XNewRef(start),
        .iterator = NULL,
        .phase = ENUM_CONSTRUCTOR_WAIT_ITER,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &enumerate_constructor_vtable, &state);
    PyObject *result = enumerate_constructor_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.start);
    Py_XDECREF(state.iterator);
    return result;
}

static PyObject *
adapter_enumerate_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"iterable", "start", NULL};
    PyObject *iterable;
    PyObject *start = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O:enumerate", keywords, &iterable, &start)) {
        return NULL;
    }
    return adapter_enumerate_construct(type, iterable, start);
}

static PyObject *
adapter_enumerate_vectorcall(
    PyObject *callable,
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames
)
{
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    Py_ssize_t nkwargs = kwnames == NULL ? 0 : PyTuple_GET_SIZE(kwnames);
    if (nargs > 2 || nargs + nkwargs < 1 || nargs + nkwargs > 2) {
        return original_enumerate_vectorcall(callable, args, nargsf, kwnames);
    }
    PyObject *iterable = nargs > 0 ? args[0] : NULL;
    PyObject *start = nargs > 1 ? args[1] : NULL;
    for (Py_ssize_t index = 0; index < nkwargs; index++) {
        PyObject *name = PyTuple_GET_ITEM(kwnames, index);
        PyObject *value = args[nargs + index];
        if (PyUnicode_CompareWithASCIIString(name, "iterable") == 0 && iterable == NULL) {
            iterable = value;
        }
        else if (PyUnicode_CompareWithASCIIString(name, "start") == 0 && start == NULL) {
            start = value;
        }
        else {
            return original_enumerate_vectorcall(callable, args, nargsf, kwnames);
        }
    }
    if (iterable == NULL) {
        return original_enumerate_vectorcall(callable, args, nargsf, kwnames);
    }
    return adapter_enumerate_construct((PyTypeObject *)callable, iterable, start);
}

typedef struct {
    AleffEnumerateObject *enumerate;
    PyObject *index;
} EnumerateNextState;

static void *
enumerate_next_copy_state(const void *raw_state)
{
    const EnumerateNextState *state = raw_state;
    EnumerateNextState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->enumerate = (AleffEnumerateObject *)Py_NewRef((PyObject *)state->enumerate);
    copy->index = Py_NewRef(state->index);
    return copy;
}

static void
enumerate_next_free_state(void *raw_state)
{
    EnumerateNextState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->enumerate);
    Py_DECREF(state->index);
    PyMem_Free(state);
}

static PyObject *
enumerate_next_finish(EnumerateNextState *state, PyObject *item)
{
    AleffEnumerateObject *enumerate = state->enumerate;
    PyObject *next_index = PyNumber_Add(state->index, enumerate->one);
    if (next_index == NULL) {
        return NULL;
    }
    if (enumerate->long_index != NULL || enumerate->index == PY_SSIZE_T_MAX) {
        enumerate->index = PY_SSIZE_T_MAX;
        Py_XSETREF(enumerate->long_index, next_index);
    }
    else {
        Py_ssize_t value = PyLong_AsSsize_t(next_index);
        Py_DECREF(next_index);
        if (value == -1 && PyErr_Occurred()) {
            return NULL;
        }
        enumerate->index = value;
    }
    return PyTuple_Pack(2, state->index, item);
}

static PyObject *
enumerate_next_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    EnumerateNextState *state = enumerate_next_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    PyObject *result = enumerate_next_finish(state, value);
    enumerate_next_free_state(state);
    return result;
}

static const AleffAdapterVTable enumerate_next_vtable = {
    .copy_state = enumerate_next_copy_state,
    .free_state = enumerate_next_free_state,
    .resume = enumerate_next_resume,
};

static PyObject *
adapter_enumerate_next(PyObject *object)
{
    AleffEnumerateObject *enumerate = (AleffEnumerateObject *)object;
    PyObject *index = enumerate->long_index != NULL
        ? Py_NewRef(enumerate->long_index)
        : PyLong_FromSsize_t(enumerate->index);
    if (index == NULL) {
        return NULL;
    }
    EnumerateNextState state = {.enumerate = enumerate, .index = index};
    AleffAdapterFrame frame;
    adapter_enter(&frame, &enumerate_next_vtable, &state);
    PyObject *item = Py_TYPE(enumerate->iterator)->tp_iternext(enumerate->iterator);
    PyObject *result = item == NULL ? NULL : enumerate_next_finish(&state, item);
    Py_XDECREF(item);
    adapter_leave(&frame);
    Py_DECREF(index);
    return result;
}

typedef struct {
    PyObject_HEAD
    Py_ssize_t index;
    PyObject *sequence;
} AleffReversedObject;

typedef struct {
    PyTypeObject *type;
    PyObject *sequence;
} ReversedConstructorState;

static newfunc original_reversed_new = NULL;
static vectorcallfunc original_reversed_vectorcall = NULL;

static void *
reversed_constructor_copy_state(const void *raw_state)
{
    const ReversedConstructorState *state = raw_state;
    ReversedConstructorState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->type = (PyTypeObject *)Py_NewRef((PyObject *)state->type);
    copy->sequence = Py_NewRef(state->sequence);
    return copy;
}

static void
reversed_constructor_free_state(void *raw_state)
{
    ReversedConstructorState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->type);
    Py_DECREF(state->sequence);
    PyMem_Free(state);
}

static PyObject *
reversed_constructor_finish(ReversedConstructorState *state, PyObject *value)
{
    Py_ssize_t length = PyNumber_AsSsize_t(value, PyExc_OverflowError);
    if (length < 0) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_ValueError, "__len__() should return >= 0");
        }
        return NULL;
    }
    AleffReversedObject *result = (AleffReversedObject *)state->type->tp_alloc(state->type, 0);
    if (result == NULL) {
        return NULL;
    }
    result->index = length - 1;
    result->sequence = Py_NewRef(state->sequence);
    return (PyObject *)result;
}

static PyObject *
reversed_constructor_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    return reversed_constructor_finish((ReversedConstructorState *)raw_state, value);
}

static const AleffAdapterVTable reversed_constructor_vtable = {
    .copy_state = reversed_constructor_copy_state,
    .free_state = reversed_constructor_free_state,
    .resume = reversed_constructor_resume,
};

static int
reversed_uses_sequence_fallback(PyObject *sequence)
{
    PyObject *descriptor = lookup_raw_special(sequence, "__reversed__");
    if (descriptor != NULL) {
        Py_DECREF(descriptor);
        return 0;
    }
    return PySequence_Check(sequence);
}

static PyObject *
adapter_reversed_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    if (
        PyTuple_GET_SIZE(args) != 1 ||
        (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0) ||
        !reversed_uses_sequence_fallback(PyTuple_GET_ITEM(args, 0))
    ) {
        return original_reversed_new(type, args, kwargs);
    }
    ReversedConstructorState state = {
        .type = type,
        .sequence = PyTuple_GET_ITEM(args, 0),
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &reversed_constructor_vtable, &state);
    PyObject *result = original_reversed_new(type, args, kwargs);
    adapter_leave(&frame);
    return result;
}

static PyObject *
adapter_reversed_vectorcall(
    PyObject *callable,
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames
)
{
    if (
        PyVectorcall_NARGS(nargsf) != 1 ||
        (kwnames != NULL && PyTuple_GET_SIZE(kwnames) != 0) ||
        !reversed_uses_sequence_fallback(args[0])
    ) {
        return original_reversed_vectorcall(callable, args, nargsf, kwnames);
    }
    ReversedConstructorState state = {
        .type = (PyTypeObject *)callable,
        .sequence = args[0],
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &reversed_constructor_vtable, &state);
    PyObject *result = original_reversed_vectorcall(callable, args, nargsf, kwnames);
    adapter_leave(&frame);
    return result;
}

typedef struct {
    AleffReversedObject *reversed;
    Py_ssize_t index;
    PyObject *sequence;
} ReversedNextState;

static void *
reversed_next_copy_state(const void *raw_state)
{
    const ReversedNextState *state = raw_state;
    ReversedNextState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->reversed = (AleffReversedObject *)Py_NewRef((PyObject *)state->reversed);
    copy->index = state->index;
    copy->sequence = Py_XNewRef(state->sequence);
    return copy;
}

static void
reversed_next_free_state(void *raw_state)
{
    ReversedNextState *state = raw_state;
    if (state != NULL) {
        Py_DECREF(state->reversed);
        Py_XDECREF(state->sequence);
        PyMem_Free(state);
    }
}

static PyObject *
reversed_next_resume(const void *raw_state, PyObject *value)
{
    const ReversedNextState *state = raw_state;
    if (value != NULL) {
        Py_XSETREF(state->reversed->sequence, Py_XNewRef(state->sequence));
        state->reversed->index = state->index - 1;
        return Py_NewRef(value);
    }
    if (
        PyErr_Occurred() &&
        !PyErr_ExceptionMatches(PyExc_IndexError) &&
        !PyErr_ExceptionMatches(PyExc_StopIteration)
    ) {
        return NULL;
    }
    PyErr_Clear();
    state->reversed->index = -1;
    Py_CLEAR(state->reversed->sequence);
    return NULL;
}

static const AleffAdapterVTable reversed_next_vtable = {
    .copy_state = reversed_next_copy_state,
    .free_state = reversed_next_free_state,
    .resume = reversed_next_resume,
};

static iternextfunc original_reversed_next = NULL;

static PyObject *
adapter_reversed_next(PyObject *object)
{
    AleffReversedObject *reversed = (AleffReversedObject *)object;
    ReversedNextState state = {
        .reversed = reversed,
        .index = reversed->index,
        .sequence = reversed->sequence,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &reversed_next_vtable, &state);
    PyObject *result = original_reversed_next(object);
    adapter_leave(&frame);
    return result;
}

typedef struct {
    PyObject_HEAD
    PyObject *func;
    PyObject *iterator;
} AleffFilterObject;

typedef enum {
    FILTER_WAIT_NEXT,
    FILTER_WAIT_PREDICATE,
} FilterPhase;

typedef struct {
    PyObject *iterator;
    PyObject *function;
    PyObject *item;
    FilterPhase phase;
} FilterState;

static void *
filter_copy_state(const void *raw_state)
{
    const FilterState *state = raw_state;
    FilterState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = (FilterState){
        .iterator = Py_NewRef(state->iterator),
        .function = Py_NewRef(state->function),
        .item = Py_XNewRef(state->item),
        .phase = state->phase,
    };
    return copy;
}

static void
filter_free_state(void *raw_state)
{
    FilterState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->iterator);
    Py_DECREF(state->function);
    Py_XDECREF(state->item);
    PyMem_Free(state);
}

static PyObject *filter_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable filter_vtable = {
    .copy_state = filter_copy_state,
    .free_state = filter_free_state,
    .resume = filter_resume,
};

static PyObject *
filter_continue(FilterState *state, PyObject *resumed_value, int is_resumed)
{
    int truth = -1;
    if (is_resumed) {
        if (state->phase == FILTER_WAIT_NEXT) {
            state->item = Py_NewRef(resumed_value);
        }
        else {
            truth = PyObject_IsTrue(resumed_value);
        }
    }

    for (;;) {
        if (state->item == NULL) {
            state->phase = FILTER_WAIT_NEXT;
            state->item = Py_TYPE(state->iterator)->tp_iternext(state->iterator);
            if (state->item == NULL) {
                return NULL;
            }
        }
        if (truth < 0) {
            state->phase = FILTER_WAIT_PREDICATE;
            if (state->function == Py_None) {
                truth = PyObject_IsTrue(state->item);
            }
            else {
                PyObject *predicate = PyObject_CallOneArg(
                    state->function,
                    state->item
                );
                if (predicate == NULL) {
                    return NULL;
                }
                truth = PyObject_IsTrue(predicate);
                Py_DECREF(predicate);
            }
        }
        if (truth < 0) {
            return NULL;
        }
        if (truth) {
            PyObject *result = state->item;
            state->item = NULL;
            return result;
        }
        Py_CLEAR(state->item);
        truth = -1;
    }
}

static PyObject *
filter_resume(const void *raw_state, PyObject *value)
{
    const FilterState *source = raw_state;
    if (value == NULL) {
        if (source->phase != FILTER_WAIT_NEXT) {
            return NULL;
        }
        if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
            return NULL;
        }
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        return NULL;
    }
    FilterState *state = filter_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &filter_vtable, state);
    PyObject *result = filter_continue(state, value, 1);
    adapter_leave(&frame);
    filter_free_state(state);
    return result;
}

static PyObject *
adapter_filter_next(PyObject *object)
{
    AleffFilterObject *filter = (AleffFilterObject *)object;
    FilterState state = {
        .iterator = filter->iterator,
        .function = filter->func,
        .item = NULL,
        .phase = FILTER_WAIT_NEXT,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &filter_vtable, &state);
    PyObject *result = filter_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.item);
    return result;
}

typedef struct {
    PyObject_HEAD
    PyObject *total;
    PyObject *iterator;
    PyObject *binop;
    PyObject *initial;
    void *module_state;
} AleffAccumulateObject;

typedef enum {
    ACCUMULATE_WAIT_NEXT,
    ACCUMULATE_WAIT_BINOP,
} AccumulatePhase;

typedef struct {
    PyObject *owner;
    PyObject *iterator;
    PyObject *binop;
    PyObject *total;
    PyObject *item;
    AccumulatePhase phase;
} AccumulateState;

static void *
accumulate_copy_state(const void *raw_state)
{
    const AccumulateState *state = raw_state;
    AccumulateState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = (AccumulateState){
        .owner = Py_NewRef(state->owner),
        .iterator = Py_NewRef(state->iterator),
        .binop = Py_XNewRef(state->binop),
        .total = Py_XNewRef(state->total),
        .item = Py_XNewRef(state->item),
        .phase = state->phase,
    };
    return copy;
}

static void
accumulate_free_state(void *raw_state)
{
    AccumulateState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->owner);
    Py_DECREF(state->iterator);
    Py_XDECREF(state->binop);
    Py_XDECREF(state->total);
    Py_XDECREF(state->item);
    PyMem_Free(state);
}

static int
accumulate_store_total(AccumulateState *state, PyObject *value)
{
    Py_XSETREF(state->total, Py_NewRef(value));
    AleffAccumulateObject *owner = (AleffAccumulateObject *)state->owner;
    Py_XSETREF(owner->total, Py_NewRef(value));
    return 0;
}

static PyObject *accumulate_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable accumulate_vtable = {
    .copy_state = accumulate_copy_state,
    .free_state = accumulate_free_state,
    .resume = accumulate_resume,
};

static PyObject *
accumulate_continue(AccumulateState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed) {
        if (state->phase == ACCUMULATE_WAIT_NEXT) {
            state->item = Py_NewRef(resumed_value);
        }
        else {
            accumulate_store_total(state, resumed_value);
            return Py_NewRef(resumed_value);
        }
    }

    if (state->item == NULL) {
        state->phase = ACCUMULATE_WAIT_NEXT;
        state->item = Py_TYPE(state->iterator)->tp_iternext(state->iterator);
        if (state->item == NULL) {
            return NULL;
        }
    }
    if (state->total == NULL) {
        accumulate_store_total(state, state->item);
        Py_CLEAR(state->item);
        return Py_NewRef(state->total);
    }

    state->phase = ACCUMULATE_WAIT_BINOP;
    PyObject *next_total = state->binop == NULL || state->binop == Py_None
        ? PyNumber_Add(state->total, state->item)
        : PyObject_CallFunctionObjArgs(
            state->binop,
            state->total,
            state->item,
            NULL
        );
    Py_CLEAR(state->item);
    if (next_total == NULL) {
        return NULL;
    }
    accumulate_store_total(state, next_total);
    Py_DECREF(next_total);
    return Py_NewRef(state->total);
}

static PyObject *
accumulate_resume(const void *raw_state, PyObject *value)
{
    const AccumulateState *source = raw_state;
    if (value == NULL) {
        if (source->phase != ACCUMULATE_WAIT_NEXT) {
            return NULL;
        }
        if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
            return NULL;
        }
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        return NULL;
    }
    AccumulateState *state = accumulate_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAccumulateObject *owner = (AleffAccumulateObject *)state->owner;
    Py_XSETREF(owner->total, Py_XNewRef(state->total));
    AleffAdapterFrame frame;
    adapter_enter(&frame, &accumulate_vtable, state);
    PyObject *result = accumulate_continue(state, value, 1);
    adapter_leave(&frame);
    accumulate_free_state(state);
    return result;
}

static PyObject *
adapter_accumulate_next(PyObject *object)
{
    AleffAccumulateObject *accumulate = (AleffAccumulateObject *)object;
    if (accumulate->initial != NULL && accumulate->initial != Py_None) {
        PyObject *result = accumulate->initial;
        accumulate->initial = Py_NewRef(Py_None);
        Py_XSETREF(accumulate->total, Py_NewRef(result));
        return result;
    }
    AccumulateState state = {
        .owner = object,
        .iterator = accumulate->iterator,
        .binop = accumulate->binop,
        .total = Py_XNewRef(accumulate->total),
        .item = NULL,
        .phase = ACCUMULATE_WAIT_NEXT,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &accumulate_vtable, &state);
    PyObject *result = accumulate_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.total);
    Py_XDECREF(state.item);
    return result;
}

typedef struct {
    PyTypeObject *type;
    PyObject *args;
    PyObject *kwargs;
} BatchedConstructorState;

static newfunc original_batched_new;

static void *
batched_constructor_copy_state(const void *raw_state)
{
    const BatchedConstructorState *state = raw_state;
    BatchedConstructorState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->type = (PyTypeObject *)Py_NewRef((PyObject *)state->type);
    copy->args = Py_NewRef(state->args);
    copy->kwargs = Py_XNewRef(state->kwargs);
    return copy;
}

static void
batched_constructor_free_state(void *raw_state)
{
    BatchedConstructorState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->type);
    Py_DECREF(state->args);
    Py_XDECREF(state->kwargs);
    PyMem_Free(state);
}

static PyObject *
batched_constructor_resume(const void *raw_state, PyObject *value)
{
    const BatchedConstructorState *state = raw_state;
    if (value == NULL) {
        return NULL;
    }
    Py_ssize_t argument_count = PyTuple_GET_SIZE(state->args);
    if (argument_count < 1) {
        PyErr_SetString(PyExc_RuntimeError, "batched constructor lost its iterable");
        return NULL;
    }
    PyObject *replacement_args = PyTuple_New(argument_count);
    if (replacement_args == NULL) {
        return NULL;
    }
    PyTuple_SET_ITEM(replacement_args, 0, Py_NewRef(value));
    for (Py_ssize_t i = 1; i < argument_count; i++) {
        PyTuple_SET_ITEM(
            replacement_args,
            i,
            Py_NewRef(PyTuple_GET_ITEM(state->args, i))
        );
    }
    PyObject *result = original_batched_new(
        state->type,
        replacement_args,
        state->kwargs
    );
    Py_DECREF(replacement_args);
    return result;
}

static const AleffAdapterVTable batched_constructor_vtable = {
    .copy_state = batched_constructor_copy_state,
    .free_state = batched_constructor_free_state,
    .resume = batched_constructor_resume,
};

static PyObject *
adapter_batched_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    BatchedConstructorState state = {
        .type = type,
        .args = args,
        .kwargs = kwargs,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &batched_constructor_vtable, &state);
    PyObject *result = original_batched_new(type, args, kwargs);
    adapter_leave(&frame);
    return result;
}

typedef enum {
    TRUTH_WAIT_NEXT,
    TRUTH_WAIT_VALUE,
} TruthPhase;

typedef struct {
    PyObject *iterator;
    PyObject *item;
    int any_mode;
    TruthPhase phase;
} TruthState;

static void *
truth_copy_state(const void *raw_state)
{
    const TruthState *state = raw_state;
    TruthState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->iterator = Py_NewRef(state->iterator);
    copy->item = Py_XNewRef(state->item);
    copy->any_mode = state->any_mode;
    copy->phase = state->phase;
    return copy;
}

static void
truth_free_state(void *raw_state)
{
    TruthState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->iterator);
    Py_XDECREF(state->item);
    PyMem_Free(state);
}

static PyObject *truth_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable truth_vtable = {
    .copy_state = truth_copy_state,
    .free_state = truth_free_state,
    .resume = truth_resume,
};

static PyObject *
truth_continue(TruthState *state, PyObject *resumed_value, int is_resumed)
{
    int truth = -1;
    if (is_resumed) {
        if (state->phase == TRUTH_WAIT_NEXT) {
            state->item = Py_NewRef(resumed_value);
        }
        else {
            truth = PyObject_IsTrue(resumed_value);
        }
    }

    for (;;) {
        if (state->item == NULL) {
            state->phase = TRUTH_WAIT_NEXT;
            state->item = PyIter_Next(state->iterator);
            if (state->item == NULL) {
                if (PyErr_Occurred()) {
                    return NULL;
                }
                return PyBool_FromLong(!state->any_mode);
            }
        }
        if (truth < 0) {
            state->phase = TRUTH_WAIT_VALUE;
            truth = PyObject_IsTrue(state->item);
        }
        Py_CLEAR(state->item);
        if (truth < 0) {
            return NULL;
        }
        if ((state->any_mode && truth) || (!state->any_mode && !truth)) {
            return PyBool_FromLong(state->any_mode);
        }
        truth = -1;
    }
}

static PyObject *
truth_resume(const void *raw_state, PyObject *value)
{
    const TruthState *source = raw_state;
    if (value == NULL) {
        if (source->phase == TRUTH_WAIT_NEXT && PyErr_ExceptionMatches(PyExc_StopIteration)) {
            PyErr_Clear();
            return PyBool_FromLong(!source->any_mode);
        }
        return NULL;
    }
    TruthState *state = truth_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &truth_vtable, state);
    PyObject *result = truth_continue(state, value, 1);
    adapter_leave(&frame);
    truth_free_state(state);
    return result;
}

static PyObject *
adapter_truth(PyObject *iterable, int any_mode)
{
    TruthState state = {
        .iterator = PyObject_GetIter(iterable),
        .item = NULL,
        .any_mode = any_mode,
        .phase = TRUTH_WAIT_NEXT,
    };
    if (state.iterator == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &truth_vtable, &state);
    PyObject *result = truth_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_DECREF(state.iterator);
    Py_XDECREF(state.item);
    return result;
}

static PyObject *
adapter_all(PyObject *Py_UNUSED(self), PyObject *iterable)
{
    return adapter_truth(iterable, 0);
}

static PyObject *
adapter_any(PyObject *Py_UNUSED(self), PyObject *iterable)
{
    return adapter_truth(iterable, 1);
}

typedef struct {
    PyObject *default_value;
} NextState;

static void *
next_copy_state(const void *raw_state)
{
    const NextState *state = raw_state;
    NextState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->default_value = Py_XNewRef(state->default_value);
    return copy;
}

static void
next_free_state(void *raw_state)
{
    NextState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->default_value);
    PyMem_Free(state);
}

static PyObject *
next_resume(const void *raw_state, PyObject *value)
{
    const NextState *state = raw_state;
    if (value != NULL) {
        return Py_NewRef(value);
    }
    if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
        return NULL;
    }
    if (state->default_value == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetNone(PyExc_StopIteration);
        }
        return NULL;
    }
    if (PyErr_Occurred()) {
        PyErr_Clear();
    }
    return Py_NewRef(state->default_value);
}

static const AleffAdapterVTable next_vtable = {
    .copy_state = next_copy_state,
    .free_state = next_free_state,
    .resume = next_resume,
};

static PyObject *
adapter_next(PyObject *Py_UNUSED(self), PyObject *args)
{
    PyObject *iterator;
    PyObject *default_value = NULL;
    if (!PyArg_ParseTuple(args, "O|O:next", &iterator, &default_value)) {
        return NULL;
    }
    if (!PyIter_Check(iterator)) {
        PyErr_Format(
            PyExc_TypeError,
            "'%s' object is not an iterator",
            Py_TYPE(iterator)->tp_name
        );
        return NULL;
    }

    NextState state = {.default_value = default_value};
    AleffAdapterFrame frame;
    adapter_enter(&frame, &next_vtable, &state);
    PyObject *result = Py_TYPE(iterator)->tp_iternext(iterator);
    adapter_leave(&frame);

    if (result == NULL) {
        if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
            return NULL;
        }
        if (default_value != NULL) {
            if (PyErr_Occurred()) {
                PyErr_Clear();
            }
            return Py_NewRef(default_value);
        }
        if (!PyErr_Occurred()) {
            PyErr_SetNone(PyExc_StopIteration);
        }
    }
    return result;
}

static PyObject *
len_resume(const void *Py_UNUSED(state), PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    Py_ssize_t length = PyNumber_AsSsize_t(value, PyExc_OverflowError);
    if (length < 0) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_ValueError, "__len__() should return >= 0");
        }
        return NULL;
    }
    return PyLong_FromSsize_t(length);
}

static const AleffAdapterVTable len_vtable = {
    .copy_state = empty_copy_state,
    .free_state = empty_free_state,
    .resume = len_resume,
};

static PyObject *
adapter_len(PyObject *Py_UNUSED(self), PyObject *object)
{
    AleffAdapterFrame frame;
    adapter_enter(&frame, &len_vtable, NULL);
    Py_ssize_t length = PyObject_Size(object);
    adapter_leave(&frame);
    if (length < 0) {
        return NULL;
    }
    return PyLong_FromSsize_t(length);
}

typedef struct {
    int base;
} NumberBaseState;

static void *
number_base_copy_state(const void *raw_state)
{
    const NumberBaseState *state = raw_state;
    NumberBaseState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->base = state->base;
    return copy;
}

static PyObject *
number_base_resume(const void *raw_state, PyObject *value)
{
    const NumberBaseState *state = raw_state;
    if (value == NULL) {
        return NULL;
    }
    return PyNumber_ToBase(value, state->base);
}

static const AleffAdapterVTable number_base_vtable = {
    .copy_state = number_base_copy_state,
    .free_state = empty_free_state,
    .resume = number_base_resume,
};

static PyObject *
adapter_number_base(PyObject *object, int base)
{
    NumberBaseState state = {.base = base};
    AleffAdapterFrame frame;
    adapter_enter(&frame, &number_base_vtable, &state);
    PyObject *result = PyNumber_ToBase(object, base);
    adapter_leave(&frame);
    return result;
}

static PyObject *
adapter_bin(PyObject *Py_UNUSED(self), PyObject *object)
{
    return adapter_number_base(object, 2);
}

static PyObject *
adapter_oct(PyObject *Py_UNUSED(self), PyObject *object)
{
    return adapter_number_base(object, 8);
}

static PyObject *
adapter_hex(PyObject *Py_UNUSED(self), PyObject *object)
{
    return adapter_number_base(object, 16);
}

typedef enum {
    EXTREME_WAIT_NEXT,
    EXTREME_WAIT_KEY,
    EXTREME_WAIT_COMPARE,
} ExtremePhase;

typedef struct {
    PyObject *iterator;
    PyObject *key_function;
    PyObject *default_value;
    PyObject *best_item;
    PyObject *best_value;
    PyObject *current_item;
    PyObject *current_value;
    int comparison_op;
    const char *name;
    ExtremePhase phase;
} ExtremeState;

static void *
extreme_copy_state(const void *raw_state)
{
    const ExtremeState *state = raw_state;
    ExtremeState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = (ExtremeState){
        .iterator = Py_NewRef(state->iterator),
        .key_function = Py_XNewRef(state->key_function),
        .default_value = Py_XNewRef(state->default_value),
        .best_item = Py_XNewRef(state->best_item),
        .best_value = Py_XNewRef(state->best_value),
        .current_item = Py_XNewRef(state->current_item),
        .current_value = Py_XNewRef(state->current_value),
        .comparison_op = state->comparison_op,
        .name = state->name,
        .phase = state->phase,
    };
    return copy;
}

static void
extreme_free_state(void *raw_state)
{
    ExtremeState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->iterator);
    Py_XDECREF(state->key_function);
    Py_XDECREF(state->default_value);
    Py_XDECREF(state->best_item);
    Py_XDECREF(state->best_value);
    Py_XDECREF(state->current_item);
    Py_XDECREF(state->current_value);
    PyMem_Free(state);
}

static PyObject *
extreme_finish(ExtremeState *state)
{
    if (state->best_item != NULL) {
        return Py_NewRef(state->best_item);
    }
    if (state->default_value != NULL) {
        return Py_NewRef(state->default_value);
    }
    PyErr_Format(PyExc_ValueError, "%s() arg is an empty sequence", state->name);
    return NULL;
}

static PyObject *extreme_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable extreme_vtable = {
    .copy_state = extreme_copy_state,
    .free_state = extreme_free_state,
    .resume = extreme_resume,
};

static PyObject *
extreme_continue(ExtremeState *state, PyObject *resumed_value, int is_resumed)
{
    int comparison = -1;
    if (is_resumed) {
        switch (state->phase) {
            case EXTREME_WAIT_NEXT:
                state->current_item = Py_NewRef(resumed_value);
                break;
            case EXTREME_WAIT_KEY:
                state->current_value = Py_NewRef(resumed_value);
                break;
            case EXTREME_WAIT_COMPARE:
                comparison = PyObject_IsTrue(resumed_value);
                break;
        }
    }

    for (;;) {
        if (state->current_item == NULL) {
            state->phase = EXTREME_WAIT_NEXT;
            state->current_item = Py_TYPE(state->iterator)->tp_iternext(state->iterator);
            if (state->current_item == NULL) {
                if (PyErr_Occurred()) {
                    if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                        return NULL;
                    }
                    PyErr_Clear();
                }
                return extreme_finish(state);
            }
        }

        if (state->current_value == NULL) {
            if (state->key_function == NULL) {
                state->current_value = Py_NewRef(state->current_item);
            }
            else {
                state->phase = EXTREME_WAIT_KEY;
                state->current_value = PyObject_CallOneArg(
                    state->key_function,
                    state->current_item
                );
                if (state->current_value == NULL) {
                    return NULL;
                }
            }
        }

        if (state->best_item == NULL) {
            state->best_item = state->current_item;
            state->current_item = NULL;
            state->best_value = state->current_value;
            state->current_value = NULL;
            continue;
        }

        if (comparison < 0) {
            state->phase = EXTREME_WAIT_COMPARE;
            comparison = PyObject_RichCompareBool(
                state->current_value,
                state->best_value,
                state->comparison_op
            );
        }
        if (comparison < 0) {
            return NULL;
        }
        if (comparison > 0) {
            Py_SETREF(state->best_item, state->current_item);
            state->current_item = NULL;
            Py_SETREF(state->best_value, state->current_value);
            state->current_value = NULL;
        }
        else {
            Py_CLEAR(state->current_item);
            Py_CLEAR(state->current_value);
        }
        comparison = -1;
    }
}

static PyObject *
extreme_resume(const void *raw_state, PyObject *value)
{
    const ExtremeState *source = raw_state;
    if (value == NULL && source->phase != EXTREME_WAIT_NEXT) {
        return NULL;
    }
    if (value == NULL && PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
        return NULL;
    }

    ExtremeState *state = extreme_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &extreme_vtable, state);
    PyObject *result;
    if (value == NULL) {
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        result = extreme_finish(state);
    }
    else {
        result = extreme_continue(state, value, 1);
    }
    adapter_leave(&frame);
    extreme_free_state(state);
    return result;
}

static PyObject *
adapter_extreme(PyObject *args, PyObject *kwargs, int comparison_op, const char *name)
{
    Py_ssize_t positional_count = PyTuple_GET_SIZE(args);
    if (positional_count == 0) {
        PyErr_Format(PyExc_TypeError, "%s expected at least 1 argument, got 0", name);
        return NULL;
    }

    PyObject *key_function = NULL;
    PyObject *default_value = NULL;
    static char *keyword_names[] = {"key", "default", NULL};
    PyObject *empty = PyTuple_New(0);
    if (empty == NULL) {
        return NULL;
    }
    char format[16];
    PyOS_snprintf(format, sizeof(format), "|$OO:%s", name);
    int parsed = PyArg_ParseTupleAndKeywords(
        empty,
        kwargs,
        format,
        keyword_names,
        &key_function,
        &default_value
    );
    Py_DECREF(empty);
    if (!parsed) {
        return NULL;
    }
    if (positional_count > 1 && default_value != NULL) {
        PyErr_Format(
            PyExc_TypeError,
            "Cannot specify a default for %s() with multiple positional arguments",
            name
        );
        return NULL;
    }
    if (key_function == Py_None) {
        key_function = NULL;
    }

    PyObject *iterable = positional_count > 1 ? args : PyTuple_GET_ITEM(args, 0);
    PyObject *iterator = PyObject_GetIter(iterable);
    if (iterator == NULL) {
        return NULL;
    }
    ExtremeState state = {
        .iterator = iterator,
        .key_function = key_function,
        .default_value = default_value,
        .best_item = NULL,
        .best_value = NULL,
        .current_item = NULL,
        .current_value = NULL,
        .comparison_op = comparison_op,
        .name = name,
        .phase = EXTREME_WAIT_NEXT,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &extreme_vtable, &state);
    PyObject *result = extreme_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_DECREF(iterator);
    Py_XDECREF(state.best_item);
    Py_XDECREF(state.best_value);
    Py_XDECREF(state.current_item);
    Py_XDECREF(state.current_value);
    return result;
}

static PyObject *
adapter_min(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    return adapter_extreme(args, kwargs, Py_LT, "min");
}

static PyObject *
adapter_max(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    return adapter_extreme(args, kwargs, Py_GT, "max");
}

typedef enum {
    COLLECT_LIST,
    COLLECT_TUPLE,
    COLLECT_DICT,
    COLLECT_SET,
    COLLECT_FROZENSET,
    COLLECT_BYTES,
    COLLECT_BYTEARRAY,
} CollectKind;

typedef enum {
    COLLECT_WAIT_ITER,
    COLLECT_WAIT_NEXT,
} CollectPhase;

typedef struct {
    PyObject *iterator;
    PyObject *items;
    CollectKind kind;
    CollectPhase phase;
} CollectState;

static initproc original_bytearray_init;

static void *
collect_copy_state(const void *raw_state)
{
    const CollectState *state = raw_state;
    CollectState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    Py_ssize_t item_count = PyList_GET_SIZE(state->items);
    copy->items = PyList_GetSlice(state->items, 0, item_count);
    if (copy->items == NULL) {
        PyMem_Free(copy);
        return NULL;
    }
    copy->iterator = Py_XNewRef(state->iterator);
    copy->kind = state->kind;
    copy->phase = state->phase;
    return copy;
}

static void
collect_free_state(void *raw_state)
{
    CollectState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->iterator);
    Py_DECREF(state->items);
    PyMem_Free(state);
}

static PyObject *
collect_finish(CollectState *state)
{
    switch (state->kind) {
        case COLLECT_LIST:
            return PyList_GetSlice(state->items, 0, PyList_GET_SIZE(state->items));
        case COLLECT_TUPLE:
            return PyList_AsTuple(state->items);
        case COLLECT_DICT: {
            PyObject *result = PyDict_New();
            if (result == NULL) {
                return NULL;
            }
            if (PyDict_MergeFromSeq2(result, state->items, 1) < 0) {
                Py_DECREF(result);
                return NULL;
            }
            return result;
        }
        case COLLECT_SET:
            return PySet_New(state->items);
        case COLLECT_FROZENSET:
            return PyFrozenSet_New(state->items);
        case COLLECT_BYTES:
            return PyBytes_FromObject(state->items);
        case COLLECT_BYTEARRAY: {
            PyObject *result = PyByteArray_FromStringAndSize(NULL, 0);
            if (result == NULL) {
                return NULL;
            }
            PyObject *args = PyTuple_Pack(1, state->items);
            if (args == NULL) {
                Py_DECREF(result);
                return NULL;
            }
            int status = original_bytearray_init(result, args, NULL);
            Py_DECREF(args);
            if (status < 0) {
                Py_DECREF(result);
                return NULL;
            }
            return result;
        }
    }
    PyErr_SetString(PyExc_RuntimeError, "unknown collector kind");
    return NULL;
}

static PyObject *collect_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable collect_vtable = {
    .copy_state = collect_copy_state,
    .free_state = collect_free_state,
    .resume = collect_resume,
};

static PyObject *
collect_continue(CollectState *state, PyObject *resumed_value, int is_resumed)
{
    PyObject *item = NULL;
    if (is_resumed) {
        if (state->phase == COLLECT_WAIT_ITER) {
            if (!PyIter_Check(resumed_value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "iter() returned non-iterator of type '%.200s'",
                    Py_TYPE(resumed_value)->tp_name
                );
                return NULL;
            }
            state->iterator = Py_NewRef(resumed_value);
        }
        else {
            item = Py_NewRef(resumed_value);
        }
    }

    for (;;) {
        if (state->iterator == NULL) {
            PyErr_SetString(PyExc_RuntimeError, "collector resumed without an iterator");
            return NULL;
        }
        if (item == NULL) {
            state->phase = COLLECT_WAIT_NEXT;
            item = Py_TYPE(state->iterator)->tp_iternext(state->iterator);
            if (item == NULL) {
                if (PyErr_Occurred()) {
                    if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                        return NULL;
                    }
                    PyErr_Clear();
                }
                return collect_finish(state);
            }
        }
        if (PyList_Append(state->items, item) < 0) {
            Py_DECREF(item);
            return NULL;
        }
        Py_DECREF(item);
        item = NULL;
    }
}

static PyObject *
collect_resume(const void *raw_state, PyObject *value)
{
    const CollectState *source = raw_state;
    if (value == NULL) {
        if (source->phase != COLLECT_WAIT_NEXT) {
            return NULL;
        }
        if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
            return NULL;
        }
    }

    CollectState *state = collect_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &collect_vtable, state);
    PyObject *result;
    if (value == NULL) {
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        result = collect_finish(state);
    }
    else {
        result = collect_continue(state, value, 1);
    }
    adapter_leave(&frame);
    collect_free_state(state);
    return result;
}

static PyObject *
collect_iterable(PyObject *iterable, CollectKind kind)
{
    CollectState state = {
        .iterator = NULL,
        .items = PyList_New(0),
        .kind = kind,
        .phase = COLLECT_WAIT_ITER,
    };
    if (state.items == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &collect_vtable, &state);
    state.iterator = PyObject_GetIter(iterable);
    PyObject *result = state.iterator == NULL
        ? NULL
        : collect_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.iterator);
    Py_DECREF(state.items);
    return result;
}

typedef struct {
    PyObject *receiver;
} ListExtendState;

static void *
list_extend_copy_state(const void *raw_state)
{
    const ListExtendState *state = raw_state;
    ListExtendState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->receiver = Py_NewRef(state->receiver);
    return copy;
}

static void
list_extend_free_state(void *raw_state)
{
    ListExtendState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->receiver);
    PyMem_Free(state);
}

static PyObject *
list_extend_apply(PyObject *receiver, PyObject *items)
{
    if (!PyList_Check(items)) {
        PyErr_SetString(PyExc_RuntimeError, "list.extend collector returned a non-list");
        return NULL;
    }
    Py_ssize_t size = PyList_GET_SIZE(receiver);
    if (PyList_SetSlice(receiver, size, size, items) < 0) {
        return NULL;
    }
    return Py_NewRef(Py_None);
}

static PyObject *
list_extend_resume(const void *raw_state, PyObject *value)
{
    const ListExtendState *state = raw_state;
    if (value == NULL) {
        return NULL;
    }
    return list_extend_apply(state->receiver, value);
}

static const AleffAdapterVTable list_extend_vtable = {
    .copy_state = list_extend_copy_state,
    .free_state = list_extend_free_state,
    .resume = list_extend_resume,
};

static PyObject *
adapter_list_extend(PyObject *self, PyObject *iterable)
{
    ListExtendState state = {.receiver = self};
    AleffAdapterFrame frame;
    adapter_enter(&frame, &list_extend_vtable, &state);
    PyObject *items = collect_iterable(iterable, COLLECT_LIST);
    PyObject *result = items == NULL ? NULL : list_extend_apply(self, items);
    Py_XDECREF(items);
    adapter_leave(&frame);
    return result;
}

typedef struct {
    PyObject *receiver;
    PyObject *target;
    PyObject *item;
    Py_ssize_t index;
    Py_ssize_t count;
} ListCountState;

static void *
list_count_copy_state(const void *raw_state)
{
    const ListCountState *state = raw_state;
    ListCountState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = (ListCountState){
        .receiver = Py_NewRef(state->receiver),
        .target = Py_NewRef(state->target),
        .item = Py_XNewRef(state->item),
        .index = state->index,
        .count = state->count,
    };
    return copy;
}

static void
list_count_free_state(void *raw_state)
{
    ListCountState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->receiver);
    Py_DECREF(state->target);
    Py_XDECREF(state->item);
    PyMem_Free(state);
}

static PyObject *list_count_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable list_count_vtable = {
    .copy_state = list_count_copy_state,
    .free_state = list_count_free_state,
    .resume = list_count_resume,
};

static PyObject *
list_count_continue(ListCountState *state, PyObject *resumed_value, int is_resumed)
{
    int equal = -1;
    if (is_resumed) {
        equal = PyObject_IsTrue(resumed_value);
    }
    for (;;) {
        if (state->item == NULL) {
            if (state->index >= PyList_GET_SIZE(state->receiver)) {
                return PyLong_FromSsize_t(state->count);
            }
            state->item = Py_NewRef(
                PyList_GET_ITEM(state->receiver, state->index)
            );
        }
        if (equal < 0) {
            equal = PyObject_RichCompareBool(
                state->item,
                state->target,
                Py_EQ
            );
        }
        if (equal < 0) {
            return NULL;
        }
        state->index++;
        Py_CLEAR(state->item);
        if (equal) {
            state->count++;
        }
        equal = -1;
    }
}

static PyObject *
list_count_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    ListCountState *state = list_count_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &list_count_vtable, state);
    PyObject *result = list_count_continue(state, value, 1);
    adapter_leave(&frame);
    list_count_free_state(state);
    return result;
}

static PyObject *
adapter_list_count(PyObject *self, PyObject *target)
{
    ListCountState state = {
        .receiver = self,
        .target = target,
        .item = NULL,
        .index = 0,
        .count = 0,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &list_count_vtable, &state);
    PyObject *result = list_count_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.item);
    return result;
}

typedef enum {
    DICT_GET_WAIT_HASH,
    DICT_GET_WAIT_CANDIDATE_HASH,
    DICT_GET_WAIT_EQUAL,
} DictGetPhase;

typedef struct {
    PyObject *receiver;
    PyObject *key;
    PyObject *default_value;
    PyObject *candidate_key;
    PyObject *candidate_value;
    Py_ssize_t position;
    Py_hash_t hash;
    Py_hash_t candidate_hash;
    DictGetPhase phase;
} DictGetState;

static void *
dict_get_copy_state(const void *raw_state)
{
    const DictGetState *state = raw_state;
    DictGetState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = (DictGetState){
        .receiver = Py_NewRef(state->receiver),
        .key = Py_NewRef(state->key),
        .default_value = Py_NewRef(state->default_value),
        .candidate_key = Py_XNewRef(state->candidate_key),
        .candidate_value = Py_XNewRef(state->candidate_value),
        .position = state->position,
        .hash = state->hash,
        .candidate_hash = state->candidate_hash,
        .phase = state->phase,
    };
    return copy;
}

static void
dict_get_free_state(void *raw_state)
{
    DictGetState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->receiver);
    Py_DECREF(state->key);
    Py_DECREF(state->default_value);
    Py_XDECREF(state->candidate_key);
    Py_XDECREF(state->candidate_value);
    PyMem_Free(state);
}

static PyObject *dict_get_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable dict_get_vtable = {
    .copy_state = dict_get_copy_state,
    .free_state = dict_get_free_state,
    .resume = dict_get_resume,
};

static int
dict_get_normalize_hash(PyObject *value, Py_hash_t *hash)
{
    if (!PyLong_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "__hash__ method should return an integer, not '%.200s'",
            Py_TYPE(value)->tp_name
        );
        return -1;
    }
    *hash = PyObject_Hash(value);
    return *hash == -1 ? -1 : 0;
}

static PyObject *
dict_get_continue(DictGetState *state, PyObject *resumed_value, int is_resumed)
{
    int equal = -1;
    if (is_resumed) {
        if (state->phase == DICT_GET_WAIT_HASH) {
            if (dict_get_normalize_hash(resumed_value, &state->hash) < 0) {
                return NULL;
            }
        }
        else if (state->phase == DICT_GET_WAIT_CANDIDATE_HASH) {
            if (dict_get_normalize_hash(
                resumed_value,
                &state->candidate_hash
            ) < 0) {
                return NULL;
            }
        }
        else {
            equal = PyObject_IsTrue(resumed_value);
        }
    }

    if (!is_resumed || state->phase == DICT_GET_WAIT_HASH) {
        if (!is_resumed) {
            state->phase = DICT_GET_WAIT_HASH;
            state->hash = PyObject_Hash(state->key);
            if (state->hash == -1) {
                return NULL;
            }
        }
    }

    for (;;) {
        if (state->candidate_key == NULL) {
            PyObject *candidate_key;
            PyObject *candidate_value;
#if PY_VERSION_HEX < 0x030d0000
            Py_hash_t candidate_hash;
            while (_PyDict_Next(
                state->receiver,
                &state->position,
                &candidate_key,
                &candidate_value,
                &candidate_hash
            )) {
                if (candidate_hash != state->hash) {
                    continue;
                }
                if (candidate_key == state->key) {
                    return Py_NewRef(candidate_value);
                }
                state->candidate_key = Py_NewRef(candidate_key);
                state->candidate_value = Py_NewRef(candidate_value);
                state->candidate_hash = candidate_hash;
                break;
            }
#else
            if (!PyDict_Next(
                state->receiver,
                &state->position,
                &candidate_key,
                &candidate_value
            )) {
                return Py_NewRef(state->default_value);
            }
            state->candidate_key = Py_NewRef(candidate_key);
            state->candidate_value = Py_NewRef(candidate_value);
            if (candidate_key == state->key) {
                return Py_NewRef(candidate_value);
            }
            state->phase = DICT_GET_WAIT_CANDIDATE_HASH;
            state->candidate_hash = PyObject_Hash(state->candidate_key);
            if (state->candidate_hash == -1) {
                return NULL;
            }
#endif
            if (state->candidate_key == NULL) {
                return Py_NewRef(state->default_value);
            }
        }
        if (state->candidate_hash != state->hash) {
            Py_CLEAR(state->candidate_key);
            Py_CLEAR(state->candidate_value);
            continue;
        }
        if (equal < 0) {
            state->phase = DICT_GET_WAIT_EQUAL;
            equal = PyObject_RichCompareBool(
                state->candidate_key,
                state->key,
                Py_EQ
            );
        }
        if (equal < 0) {
            return NULL;
        }
        if (equal) {
            return Py_NewRef(state->candidate_value);
        }
        Py_CLEAR(state->candidate_key);
        Py_CLEAR(state->candidate_value);
        equal = -1;
    }
}

static PyObject *
dict_get_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    DictGetState *state = dict_get_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &dict_get_vtable, state);
    PyObject *result = dict_get_continue(state, value, 1);
    adapter_leave(&frame);
    dict_get_free_state(state);
    return result;
}

static PyObject *
adapter_dict_get(PyObject *self, PyObject *args)
{
    PyObject *key;
    PyObject *default_value = Py_None;
    if (!PyArg_ParseTuple(args, "O|O:get", &key, &default_value)) {
        return NULL;
    }
    DictGetState state = {
        .receiver = self,
        .key = key,
        .default_value = default_value,
        .candidate_key = NULL,
        .candidate_value = NULL,
        .position = 0,
        .hash = -1,
        .candidate_hash = -1,
        .phase = DICT_GET_WAIT_HASH,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &dict_get_vtable, &state);
    PyObject *result = dict_get_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.candidate_key);
    Py_XDECREF(state.candidate_value);
    return result;
}

typedef struct {
    PyObject *components[3];
    Py_hash_t hashes[3];
    int index;
} SliceHashState;

static hashfunc original_slice_hash;

static void *
slice_hash_copy_state(const void *raw_state)
{
    const SliceHashState *state = raw_state;
    SliceHashState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->index = state->index;
    for (int i = 0; i < 3; i++) {
        copy->components[i] = Py_NewRef(state->components[i]);
        copy->hashes[i] = state->hashes[i];
    }
    return copy;
}

static void
slice_hash_free_state(void *raw_state)
{
    SliceHashState *state = raw_state;
    if (state == NULL) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        Py_DECREF(state->components[i]);
    }
    PyMem_Free(state);
}

static int
slice_hash_normalize(PyObject *value, Py_hash_t *hash)
{
    if (!PyLong_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "__hash__ method should return an integer, not '%.200s'",
            Py_TYPE(value)->tp_name
        );
        return -1;
    }
    *hash = PyObject_Hash(value);
    return *hash == -1 ? -1 : 0;
}

static PyObject *
slice_hash_finish(SliceHashState *state)
{
    PyObject *proxies[3] = {NULL, NULL, NULL};
    for (int i = 0; i < 3; i++) {
        proxies[i] = PyLong_FromSsize_t(state->hashes[i]);
        if (proxies[i] == NULL) {
            for (int j = 0; j < i; j++) {
                Py_DECREF(proxies[j]);
            }
            return NULL;
        }
    }
    PyObject *proxy_slice = PySlice_New(proxies[0], proxies[1], proxies[2]);
    for (int i = 0; i < 3; i++) {
        Py_DECREF(proxies[i]);
    }
    if (proxy_slice == NULL) {
        return NULL;
    }
    Py_hash_t result = original_slice_hash(proxy_slice);
    Py_DECREF(proxy_slice);
    if (result == -1) {
        return NULL;
    }
    return PyLong_FromSsize_t(result);
}

static PyObject *slice_hash_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable slice_hash_vtable = {
    .copy_state = slice_hash_copy_state,
    .free_state = slice_hash_free_state,
    .resume = slice_hash_resume,
};

static PyObject *
slice_hash_continue(SliceHashState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed) {
        if (slice_hash_normalize(resumed_value, &state->hashes[state->index]) < 0) {
            return NULL;
        }
        state->index++;
    }
    while (state->index < 3) {
        Py_hash_t hash = PyObject_Hash(state->components[state->index]);
        if (hash == -1) {
            return NULL;
        }
        state->hashes[state->index] = hash;
        state->index++;
    }
    return slice_hash_finish(state);
}

static PyObject *
slice_hash_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    SliceHashState *state = slice_hash_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &slice_hash_vtable, state);
    PyObject *result = slice_hash_continue(state, value, 1);
    adapter_leave(&frame);
    slice_hash_free_state(state);
    return result;
}

static Py_hash_t
adapter_slice_hash(PyObject *object)
{
    PySliceObject *slice = (PySliceObject *)object;
    SliceHashState state = {
        .components = {slice->start, slice->stop, slice->step},
        .hashes = {0, 0, 0},
        .index = 0,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &slice_hash_vtable, &state);
    PyObject *result = slice_hash_continue(&state, NULL, 0);
    adapter_leave(&frame);
    if (result == NULL) {
        return -1;
    }
    Py_hash_t hash = PyLong_AsSsize_t(result);
    Py_DECREF(result);
    return hash;
}

typedef enum {
    SORT_WAIT_REVERSE,
    SORT_WAIT_COLLECT,
    SORT_WAIT_KEY,
    SORT_WAIT_COMPARE,
} SortPhase;

typedef struct {
    PyObject *iterable;
    PyObject *key_function;
    PyObject *reverse_object;
    PyObject *items;
    PyObject *keys;
    PyObject *destination_items;
    PyObject *destination_keys;
    Py_ssize_t key_index;
    Py_ssize_t width;
    Py_ssize_t left;
    Py_ssize_t middle;
    Py_ssize_t end;
    Py_ssize_t first;
    Py_ssize_t second;
    Py_ssize_t output;
    int reverse;
    int reverse_ready;
    int merge_ready;
    SortPhase phase;
} SortState;

static PyObject *
copy_optional_list(PyObject *list)
{
    return list == NULL
        ? NULL
        : PyList_GetSlice(list, 0, PyList_GET_SIZE(list));
}

static void *
sort_copy_state(const void *raw_state)
{
    const SortState *state = raw_state;
    SortState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->iterable = Py_NewRef(state->iterable);
    copy->key_function = Py_XNewRef(state->key_function);
    copy->reverse_object = Py_NewRef(state->reverse_object);
    copy->items = copy_optional_list(state->items);
    if (state->items != NULL && copy->items == NULL) {
        goto error;
    }
    copy->keys = copy_optional_list(state->keys);
    if (state->keys != NULL && copy->keys == NULL) {
        goto error;
    }
    copy->destination_items = copy_optional_list(state->destination_items);
    if (state->destination_items != NULL && copy->destination_items == NULL) {
        goto error;
    }
    copy->destination_keys = copy_optional_list(state->destination_keys);
    if (state->destination_keys != NULL && copy->destination_keys == NULL) {
        goto error;
    }
    copy->key_index = state->key_index;
    copy->width = state->width;
    copy->left = state->left;
    copy->middle = state->middle;
    copy->end = state->end;
    copy->first = state->first;
    copy->second = state->second;
    copy->output = state->output;
    copy->reverse = state->reverse;
    copy->reverse_ready = state->reverse_ready;
    copy->merge_ready = state->merge_ready;
    copy->phase = state->phase;
    return copy;

error:
    Py_DECREF(copy->iterable);
    Py_XDECREF(copy->key_function);
    Py_DECREF(copy->reverse_object);
    Py_XDECREF(copy->items);
    Py_XDECREF(copy->keys);
    Py_XDECREF(copy->destination_items);
    Py_XDECREF(copy->destination_keys);
    PyMem_Free(copy);
    return NULL;
}

static void
sort_free_state(void *raw_state)
{
    SortState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->iterable);
    Py_XDECREF(state->key_function);
    Py_DECREF(state->reverse_object);
    Py_XDECREF(state->items);
    Py_XDECREF(state->keys);
    Py_XDECREF(state->destination_items);
    Py_XDECREF(state->destination_keys);
    PyMem_Free(state);
}

static PyObject *
new_none_list(Py_ssize_t size)
{
    PyObject *result = PyList_New(size);
    if (result == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < size; i++) {
        PyList_SET_ITEM(result, i, Py_NewRef(Py_None));
    }
    return result;
}

static int
sort_store_entry(SortState *state, int choose_second)
{
    Py_ssize_t source = choose_second ? state->second++ : state->first++;
    if (
        PyList_SetItem(
            state->destination_items,
            state->output,
            Py_NewRef(PyList_GET_ITEM(state->items, source))
        ) < 0 ||
        PyList_SetItem(
            state->destination_keys,
            state->output,
            Py_NewRef(PyList_GET_ITEM(state->keys, source))
        ) < 0
    ) {
        return -1;
    }
    state->output++;
    return 0;
}

static PyObject *sort_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable sort_vtable = {
    .copy_state = sort_copy_state,
    .free_state = sort_free_state,
    .resume = sort_resume,
};

static PyObject *
sort_continue(SortState *state, PyObject *resumed_value, int is_resumed)
{
    int comparison = -1;
    if (is_resumed) {
        switch (state->phase) {
            case SORT_WAIT_REVERSE:
                state->reverse = PyObject_IsTrue(resumed_value);
                if (state->reverse < 0) {
                    return NULL;
                }
                state->reverse_ready = 1;
                break;
            case SORT_WAIT_COLLECT:
                if (!PyList_Check(resumed_value)) {
                    PyErr_SetString(PyExc_RuntimeError, "sorted collector returned a non-list");
                    return NULL;
                }
                state->items = PyList_GetSlice(
                    resumed_value,
                    0,
                    PyList_GET_SIZE(resumed_value)
                );
                if (state->items == NULL) {
                    return NULL;
                }
                break;
            case SORT_WAIT_KEY:
                if (PyList_Append(state->keys, resumed_value) < 0) {
                    return NULL;
                }
                state->key_index++;
                break;
            case SORT_WAIT_COMPARE:
                comparison = PyObject_IsTrue(resumed_value);
                if (comparison < 0) {
                    return NULL;
                }
                break;
        }
    }

    if (!state->reverse_ready) {
        state->phase = SORT_WAIT_REVERSE;
        state->reverse = PyObject_IsTrue(state->reverse_object);
        if (state->reverse < 0) {
            return NULL;
        }
        state->reverse_ready = 1;
    }
    if (state->items == NULL) {
        state->phase = SORT_WAIT_COLLECT;
        state->items = collect_iterable(state->iterable, COLLECT_LIST);
        if (state->items == NULL) {
            return NULL;
        }
    }
    if (state->keys == NULL) {
        state->keys = PyList_New(0);
        if (state->keys == NULL) {
            return NULL;
        }
    }
    while (state->key_index < PyList_GET_SIZE(state->items)) {
        PyObject *item = PyList_GET_ITEM(state->items, state->key_index);
        if (state->key_function == NULL) {
            if (PyList_Append(state->keys, item) < 0) {
                return NULL;
            }
        }
        else {
            state->phase = SORT_WAIT_KEY;
            PyObject *key = PyObject_CallOneArg(state->key_function, item);
            if (key == NULL) {
                return NULL;
            }
            int status = PyList_Append(state->keys, key);
            Py_DECREF(key);
            if (status < 0) {
                return NULL;
            }
        }
        state->key_index++;
    }

    Py_ssize_t size = PyList_GET_SIZE(state->items);
    if (state->destination_items == NULL) {
        state->destination_items = new_none_list(size);
        state->destination_keys = new_none_list(size);
        if (state->destination_items == NULL || state->destination_keys == NULL) {
            return NULL;
        }
        state->width = 1;
        state->left = 0;
    }

    for (;;) {
        if (state->width >= size) {
            return Py_NewRef(state->items);
        }
        if (state->left >= size) {
            PyObject *temporary = state->items;
            state->items = state->destination_items;
            state->destination_items = temporary;
            temporary = state->keys;
            state->keys = state->destination_keys;
            state->destination_keys = temporary;
            state->width = state->width > size / 2
                ? size
                : state->width * 2;
            state->left = 0;
            state->merge_ready = 0;
            continue;
        }
        if (!state->merge_ready) {
            state->middle = state->left + state->width;
            if (state->middle > size) {
                state->middle = size;
            }
            state->end = state->middle + state->width;
            if (state->end > size) {
                state->end = size;
            }
            state->first = state->left;
            state->second = state->middle;
            state->output = state->left;
            state->merge_ready = 1;
        }

        while (state->first < state->middle && state->second < state->end) {
            if (comparison < 0) {
                PyObject *left_key = PyList_GET_ITEM(state->keys, state->first);
                PyObject *right_key = PyList_GET_ITEM(state->keys, state->second);
                state->phase = SORT_WAIT_COMPARE;
                comparison = state->reverse
                    ? PyObject_RichCompareBool(left_key, right_key, Py_LT)
                    : PyObject_RichCompareBool(right_key, left_key, Py_LT);
            }
            if (comparison < 0) {
                return NULL;
            }
            if (sort_store_entry(state, comparison) < 0) {
                return NULL;
            }
            comparison = -1;
        }
        while (state->first < state->middle) {
            if (sort_store_entry(state, 0) < 0) {
                return NULL;
            }
        }
        while (state->second < state->end) {
            if (sort_store_entry(state, 1) < 0) {
                return NULL;
            }
        }
        state->left = state->end;
        state->merge_ready = 0;
    }
}

static PyObject *
sort_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    SortState *state = sort_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &sort_vtable, state);
    PyObject *result = sort_continue(state, value, 1);
    adapter_leave(&frame);
    sort_free_state(state);
    return result;
}

static PyObject *
adapter_sorted(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"iterable", "key", "reverse", NULL};
    PyObject *iterable;
    PyObject *key_function = Py_None;
    PyObject *reverse_object = Py_False;
    if (!PyArg_ParseTupleAndKeywords(
        args,
        kwargs,
        "O|$OO:sorted",
        keywords,
        &iterable,
        &key_function,
        &reverse_object
    )) {
        return NULL;
    }
    SortState state = {
        .iterable = iterable,
        .key_function = key_function == Py_None ? NULL : key_function,
        .reverse_object = reverse_object,
        .items = NULL,
        .keys = NULL,
        .destination_items = NULL,
        .destination_keys = NULL,
        .key_index = 0,
        .width = 0,
        .left = 0,
        .middle = 0,
        .end = 0,
        .first = 0,
        .second = 0,
        .output = 0,
        .reverse = 0,
        .reverse_ready = 0,
        .merge_ready = 0,
        .phase = SORT_WAIT_REVERSE,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &sort_vtable, &state);
    PyObject *result = sort_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.items);
    Py_XDECREF(state.keys);
    Py_XDECREF(state.destination_items);
    Py_XDECREF(state.destination_keys);
    return result;
}

static initproc original_list_init;
static vectorcallfunc original_list_vectorcall;
static newfunc original_tuple_new;
static vectorcallfunc original_tuple_vectorcall;
static initproc original_dict_init;
static vectorcallfunc original_dict_vectorcall;
static initproc original_set_init;
static vectorcallfunc original_set_vectorcall;
static newfunc original_frozenset_new;
static vectorcallfunc original_frozenset_vectorcall;
static newfunc original_bytes_new;

static PyObject *
adapter_collect_vectorcall(
    PyObject *callable,
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames,
    PyTypeObject *expected_type,
    CollectKind kind,
    vectorcallfunc original
)
{
    if (
        callable != (PyObject *)expected_type ||
        PyVectorcall_NARGS(nargsf) != 1 ||
        (kwnames != NULL && PyTuple_GET_SIZE(kwnames) != 0) ||
        (kind == COLLECT_TUPLE && PyTuple_CheckExact(args[0])) ||
        (kind == COLLECT_FROZENSET && PyFrozenSet_CheckExact(args[0])) ||
        (kind == COLLECT_DICT && PyMapping_Check(args[0]))
    ) {
        return original(callable, args, nargsf, kwnames);
    }
    return collect_iterable(args[0], kind);
}

static PyObject *
adapter_list_vectorcall(
    PyObject *callable,
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames
)
{
    return adapter_collect_vectorcall(
        callable, args, nargsf, kwnames,
        &PyList_Type, COLLECT_LIST, original_list_vectorcall
    );
}

static PyObject *
adapter_tuple_vectorcall(
    PyObject *callable,
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames
)
{
    return adapter_collect_vectorcall(
        callable, args, nargsf, kwnames,
        &PyTuple_Type, COLLECT_TUPLE, original_tuple_vectorcall
    );
}

static PyObject *
adapter_dict_vectorcall(
    PyObject *callable,
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames
)
{
    return adapter_collect_vectorcall(
        callable, args, nargsf, kwnames,
        &PyDict_Type, COLLECT_DICT, original_dict_vectorcall
    );
}

static PyObject *
adapter_set_vectorcall(
    PyObject *callable,
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames
)
{
    return adapter_collect_vectorcall(
        callable, args, nargsf, kwnames,
        &PySet_Type, COLLECT_SET, original_set_vectorcall
    );
}

static PyObject *
adapter_frozenset_vectorcall(
    PyObject *callable,
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames
)
{
    return adapter_collect_vectorcall(
        callable, args, nargsf, kwnames,
        &PyFrozenSet_Type, COLLECT_FROZENSET,
        original_frozenset_vectorcall
    );
}

static int
adapter_list_init(PyObject *self, PyObject *args, PyObject *kwargs)
{
    if (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0) {
        return original_list_init(self, args, kwargs);
    }
    if (PyTuple_GET_SIZE(args) != 1) {
        return original_list_init(self, args, kwargs);
    }
    PyObject *result = collect_iterable(PyTuple_GET_ITEM(args, 0), COLLECT_LIST);
    if (result == NULL) {
        return -1;
    }
    int status = PyList_SetSlice(self, 0, PyList_GET_SIZE(self), result);
    Py_DECREF(result);
    return status;
}

static PyObject *
adapter_tuple_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    if (
        type != &PyTuple_Type || PyTuple_GET_SIZE(args) != 1 ||
        (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0) ||
        PyTuple_CheckExact(PyTuple_GET_ITEM(args, 0))
    ) {
        return original_tuple_new(type, args, kwargs);
    }
    return collect_iterable(PyTuple_GET_ITEM(args, 0), COLLECT_TUPLE);
}

static int
adapter_dict_init(PyObject *self, PyObject *args, PyObject *kwargs)
{
    if (
        PyTuple_GET_SIZE(args) != 1 ||
        (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0) ||
        PyMapping_Check(PyTuple_GET_ITEM(args, 0))
    ) {
        return original_dict_init(self, args, kwargs);
    }
    PyObject *result = collect_iterable(PyTuple_GET_ITEM(args, 0), COLLECT_DICT);
    if (result == NULL) {
        return -1;
    }
    PyDict_Clear(self);
    int status = PyDict_Update(self, result);
    Py_DECREF(result);
    return status;
}

static int
adapter_set_init(PyObject *self, PyObject *args, PyObject *kwargs)
{
    if (
        PyTuple_GET_SIZE(args) != 1 ||
        (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0)
    ) {
        return original_set_init(self, args, kwargs);
    }
    PyObject *items = collect_iterable(PyTuple_GET_ITEM(args, 0), COLLECT_LIST);
    if (items == NULL) {
        return -1;
    }
    PyObject *replacement_args = PyTuple_Pack(1, items);
    Py_DECREF(items);
    if (replacement_args == NULL) {
        return -1;
    }
    int status = original_set_init(self, replacement_args, NULL);
    Py_DECREF(replacement_args);
    return status;
}

static PyObject *
adapter_frozenset_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    if (
        type != &PyFrozenSet_Type || PyTuple_GET_SIZE(args) != 1 ||
        (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0) ||
        PyFrozenSet_CheckExact(PyTuple_GET_ITEM(args, 0))
    ) {
        return original_frozenset_new(type, args, kwargs);
    }
    return collect_iterable(PyTuple_GET_ITEM(args, 0), COLLECT_FROZENSET);
}

typedef enum {
    BYTES_WAIT_BYTES,
    BYTES_WAIT_INDEX,
    BYTES_WAIT_BUFFER_ACQUIRE,
    BYTES_WAIT_BUFFER_RELEASE,
} BytesPhase;

typedef struct {
    BytesPhase phase;
    int make_bytearray;
    PyObject *input;
    PyObject *view;
    PyObject *result;
} BytesState;

static void *
bytes_copy_state(const void *raw_state)
{
    const BytesState *state = raw_state;
    BytesState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->phase = state->phase;
    copy->make_bytearray = state->make_bytearray;
    copy->input = Py_XNewRef(state->input);
    copy->view = Py_XNewRef(state->view);
    copy->result = Py_XNewRef(state->result);
    return copy;
}

static void
bytes_free_state(void *raw_state)
{
    BytesState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->input);
    Py_XDECREF(state->view);
    Py_XDECREF(state->result);
    PyMem_Free(state);
}

static PyObject *
lookup_raw_special(PyObject *object, const char *name)
{
    PyObject *mro = Py_TYPE(object)->tp_mro;
    if (mro == NULL || !PyTuple_Check(mro)) {
        return NULL;
    }
    Py_ssize_t size = PyTuple_GET_SIZE(mro);
    for (Py_ssize_t index = 0; index < size; index++) {
        PyObject *base = PyTuple_GET_ITEM(mro, index);
        if (!PyType_Check(base)) {
            continue;
        }
        PyObject *dictionary = PyType_GetDict((PyTypeObject *)base);
        PyObject *descriptor = PyDict_GetItemString(dictionary, name);
        if (descriptor != NULL) {
            return Py_NewRef(descriptor);
        }
    }
    return NULL;
}

static PyObject *
bytes_from_index_result(PyObject *value, int make_bytearray)
{
    PyObject *index = PyNumber_Index(value);
    if (index == NULL) {
        return NULL;
    }
    PyObject *args = PyTuple_Pack(1, index);
    Py_DECREF(index);
    if (args == NULL) {
        return NULL;
    }
    PyObject *result;
    if (make_bytearray) {
        result = PyByteArray_FromStringAndSize(NULL, 0);
        if (result != NULL && original_bytearray_init(result, args, NULL) < 0) {
            Py_CLEAR(result);
        }
    }
    else {
        result = original_bytes_new(&PyBytes_Type, args, NULL);
    }
    Py_DECREF(args);
    return result;
}

static PyObject *
bytearray_copy_buffer(PyObject *source)
{
    Py_buffer view;
    if (PyObject_GetBuffer(source, &view, PyBUF_FULL_RO) < 0) {
        return NULL;
    }
    PyObject *result = PyByteArray_FromStringAndSize(NULL, view.len);
    if (
        result != NULL &&
        PyBuffer_ToContiguous(PyByteArray_AS_STRING(result), &view, view.len, 'C') < 0
    ) {
        Py_CLEAR(result);
    }
    PyBuffer_Release(&view);
    return result;
}

static int
bytearray_replace_buffer(PyObject *target, PyObject *source)
{
    Py_buffer view;
    if (PyObject_GetBuffer(source, &view, PyBUF_FULL_RO) < 0) {
        return -1;
    }
    int status = PyByteArray_Resize(target, view.len);
    if (
        status == 0 &&
        PyBuffer_ToContiguous(PyByteArray_AS_STRING(target), &view, view.len, 'C') < 0
    ) {
        status = -1;
    }
    PyBuffer_Release(&view);
    return status;
}

static PyObject *
bytes_buffer_continue(BytesState *state, PyObject *value)
{
    if (!PyMemoryView_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "__buffer__ returned non-memoryview object");
        return NULL;
    }
    Py_XSETREF(state->view, Py_NewRef(value));
    PyObject *converted = state->make_bytearray
        ? bytearray_copy_buffer(state->view)
        : PyBytes_FromObject(state->view);
    if (converted == NULL) {
        return NULL;
    }
    Py_XSETREF(state->result, converted);

    PyObject *release_descriptor = lookup_raw_special(state->input, "__release_buffer__");
    if (release_descriptor == NULL) {
        return Py_NewRef(state->result);
    }
    Py_DECREF(release_descriptor);

    state->phase = BYTES_WAIT_BUFFER_RELEASE;
    PyObject *released = PyObject_CallMethod(
        state->input,
        "__release_buffer__",
        "O",
        state->view
    );
    if (released == NULL) {
        PyErr_WriteUnraisable(state->input);
    }
    else {
        Py_DECREF(released);
    }
    return Py_NewRef(state->result);
}

static const AleffAdapterVTable bytes_vtable;

static PyObject *
bytes_resume(const void *raw_state, PyObject *value)
{
    const BytesState *state = raw_state;
    if (value == NULL && state->phase != BYTES_WAIT_BUFFER_RELEASE) {
        return NULL;
    }
    switch (state->phase) {
        case BYTES_WAIT_BYTES:
            if (!PyBytes_Check(value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "__bytes__ returned non-bytes (type %.200s)",
                    Py_TYPE(value)->tp_name
                );
                return NULL;
            }
            return Py_NewRef(value);
        case BYTES_WAIT_INDEX:
            return bytes_from_index_result(value, state->make_bytearray);
        case BYTES_WAIT_BUFFER_ACQUIRE: {
            BytesState *copy = bytes_copy_state(state);
            if (copy == NULL) {
                return NULL;
            }
            AleffAdapterFrame frame;
            adapter_enter(&frame, &bytes_vtable, copy);
            PyObject *result = bytes_buffer_continue(copy, value);
            adapter_leave(&frame);
            bytes_free_state(copy);
            return result;
        }
        case BYTES_WAIT_BUFFER_RELEASE:
            if (value == NULL && PyErr_Occurred()) {
                PyErr_WriteUnraisable(state->input);
            }
            return Py_NewRef(state->result);
    }
    PyErr_SetString(PyExc_RuntimeError, "unknown bytes conversion phase");
    return NULL;
}

static const AleffAdapterVTable bytes_vtable = {
    .copy_state = bytes_copy_state,
    .free_state = bytes_free_state,
    .resume = bytes_resume,
};

static PyObject *
convert_python_buffer(PyObject *input, int make_bytearray)
{
    BytesState state = {
        .phase = BYTES_WAIT_BUFFER_ACQUIRE,
        .make_bytearray = make_bytearray,
        .input = Py_NewRef(input),
        .view = NULL,
        .result = NULL,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &bytes_vtable, &state);
    PyObject *view = PyObject_CallMethod(input, "__buffer__", "i", PyBUF_FULL_RO);
    PyObject *result = view == NULL ? NULL : bytes_buffer_continue(&state, view);
    Py_XDECREF(view);
    adapter_leave(&frame);
    Py_DECREF(state.input);
    Py_XDECREF(state.view);
    Py_XDECREF(state.result);
    return result;
}

static PyObject *
adapter_bytes_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    if (
        type != &PyBytes_Type || PyTuple_GET_SIZE(args) != 1 ||
        (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0)
    ) {
        return original_bytes_new(type, args, kwargs);
    }
    PyObject *input = PyTuple_GET_ITEM(args, 0);
    if (PyBytes_CheckExact(input)) {
        return original_bytes_new(type, args, kwargs);
    }

    PyObject *bytes_descriptor = lookup_raw_special(input, "__bytes__");
    if (bytes_descriptor != NULL) {
        Py_DECREF(bytes_descriptor);
        BytesState state = {
            .phase = BYTES_WAIT_BYTES,
            .make_bytearray = 0,
            .input = NULL,
            .view = NULL,
            .result = NULL,
        };
        AleffAdapterFrame frame;
        adapter_enter(&frame, &bytes_vtable, &state);
        PyObject *result = original_bytes_new(type, args, kwargs);
        adapter_leave(&frame);
        return result;
    }
    if (PyUnicode_Check(input)) {
        return original_bytes_new(type, args, kwargs);
    }
    if (PyIndex_Check(input)) {
        BytesState state = {
            .phase = BYTES_WAIT_INDEX,
            .make_bytearray = 0,
            .input = NULL,
            .view = NULL,
            .result = NULL,
        };
        AleffAdapterFrame frame;
        adapter_enter(&frame, &bytes_vtable, &state);
        PyObject *result = original_bytes_new(type, args, kwargs);
        adapter_leave(&frame);
        return result;
    }
    if (PyObject_CheckBuffer(input)) {
        PyObject *buffer_descriptor = lookup_raw_special(input, "__buffer__");
        if (
            buffer_descriptor == NULL ||
            Py_IS_TYPE(buffer_descriptor, &PyWrapperDescr_Type)
        ) {
            Py_XDECREF(buffer_descriptor);
            return original_bytes_new(type, args, kwargs);
        }
        Py_DECREF(buffer_descriptor);
        return convert_python_buffer(input, 0);
    }
    return collect_iterable(input, COLLECT_BYTES);
}

static int
adapter_bytearray_init(PyObject *self, PyObject *args, PyObject *kwargs)
{
    if (
        PyTuple_GET_SIZE(args) != 1 ||
        (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0) ||
        PyUnicode_Check(PyTuple_GET_ITEM(args, 0))
    ) {
        return original_bytearray_init(self, args, kwargs);
    }
    PyObject *input = PyTuple_GET_ITEM(args, 0);
    if (PyIndex_Check(input)) {
        BytesState state = {
            .phase = BYTES_WAIT_INDEX,
            .make_bytearray = 1,
            .input = NULL,
            .view = NULL,
            .result = NULL,
        };
        AleffAdapterFrame frame;
        adapter_enter(&frame, &bytes_vtable, &state);
        int status = original_bytearray_init(self, args, kwargs);
        adapter_leave(&frame);
        return status;
    }

    PyObject *result;
    if (PyObject_CheckBuffer(input)) {
        PyObject *buffer_descriptor = lookup_raw_special(input, "__buffer__");
        if (
            buffer_descriptor == NULL ||
            Py_IS_TYPE(buffer_descriptor, &PyWrapperDescr_Type)
        ) {
            Py_XDECREF(buffer_descriptor);
            return original_bytearray_init(self, args, kwargs);
        }
        Py_DECREF(buffer_descriptor);
        result = convert_python_buffer(input, 1);
    }
    else {
        result = collect_iterable(input, COLLECT_BYTEARRAY);
    }
    if (result == NULL) {
        return -1;
    }
    int status = bytearray_replace_buffer(self, result);
    Py_DECREF(result);
    return status;
}

static PyMethodDef sum_method = {
    .ml_name = "sum",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_sum,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Return the sum of a 'start' value plus an iterable of numbers.",
};

static PyMethodDef reduce_method = {
    .ml_name = "reduce",
    .ml_meth = adapter_reduce,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Apply a function of two arguments cumulatively to an iterable.",
};

static PyMethodDef bin_method = {
    .ml_name = "bin",
    .ml_meth = adapter_bin,
    .ml_flags = METH_O,
    .ml_doc = "Return the binary representation of an integer.",
};

static PyMethodDef oct_method = {
    .ml_name = "oct",
    .ml_meth = adapter_oct,
    .ml_flags = METH_O,
    .ml_doc = "Return the octal representation of an integer.",
};

static PyMethodDef hex_method = {
    .ml_name = "hex",
    .ml_meth = adapter_hex,
    .ml_flags = METH_O,
    .ml_doc = "Return the hexadecimal representation of an integer.",
};

static PyMethodDef sorted_method = {
    .ml_name = "sorted",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_sorted,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Return a new list containing all items from the iterable in ascending order.",
};

static PyMethodDef list_extend_method = {
    .ml_name = "extend",
    .ml_meth = adapter_list_extend,
    .ml_flags = METH_O,
    .ml_doc = "Extend list by appending elements from the iterable.",
};

static PyMethodDef list_count_method = {
    .ml_name = "count",
    .ml_meth = adapter_list_count,
    .ml_flags = METH_O,
    .ml_doc = "Return number of occurrences of value.",
};

static PyMethodDef dict_get_method = {
    .ml_name = "get",
    .ml_meth = adapter_dict_get,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the value for key if key is in the dictionary.",
};

static PyMethodDef all_method = {
    .ml_name = "all",
    .ml_meth = adapter_all,
    .ml_flags = METH_O,
    .ml_doc = "Return True if bool(x) is True for all values x in the iterable.",
};

static PyMethodDef any_method = {
    .ml_name = "any",
    .ml_meth = adapter_any,
    .ml_flags = METH_O,
    .ml_doc = "Return True if bool(x) is True for any value x in the iterable.",
};

static PyMethodDef next_method = {
    .ml_name = "next",
    .ml_meth = adapter_next,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the next item from the iterator.",
};

static PyMethodDef len_method = {
    .ml_name = "len",
    .ml_meth = adapter_len,
    .ml_flags = METH_O,
    .ml_doc = "Return the number of items in a container.",
};

static PyMethodDef min_method = {
    .ml_name = "min",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_min,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Return the smallest item in an iterable or of two or more arguments.",
};

static PyMethodDef max_method = {
    .ml_name = "max",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_max,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Return the largest item in an iterable or of two or more arguments.",
};

static int adapters_installed = 0;

static int
replace_builtin(PyObject *builtins, const char *name, PyMethodDef *method)
{
    PyObject *function = PyCFunction_NewEx(method, NULL, NULL);
    if (function == NULL) {
        return -1;
    }
    int result = PyDict_SetItemString(builtins, name, function);
    Py_DECREF(function);
    return result;
}

static int
replace_type_method(PyTypeObject *type, const char *name, PyMethodDef *method)
{
    PyObject *descriptor = PyDescr_NewMethod(type, method);
    if (descriptor == NULL) {
        return -1;
    }
    PyObject *type_dict = PyType_GetDict(type);
    if (type_dict == NULL) {
        Py_DECREF(descriptor);
        return -1;
    }
    int result = PyDict_SetItemString(type_dict, name, descriptor);
    Py_DECREF(descriptor);
    if (result == 0) {
        PyType_Modified(type);
    }
    return result;
}

int
aleff_adapter_install(void)
{
    if (adapters_installed) {
        return 0;
    }
    PyObject *builtins = PyEval_GetBuiltins();
    if (!PyDict_Check(builtins)) {
        PyErr_SetString(PyExc_RuntimeError, "cannot access the builtins dictionary");
        return -1;
    }

    PyObject *map_type_object = PyDict_GetItemString(builtins, "map");
    if (map_type_object == NULL || !PyType_Check(map_type_object)) {
        PyErr_SetString(PyExc_RuntimeError, "cannot access the built-in map type");
        return -1;
    }
    PyTypeObject *map_type = (PyTypeObject *)map_type_object;
    original_map_new = map_type->tp_new;
    map_type->tp_new = adapter_map_new;
    original_map_next = map_type->tp_iternext;
    map_type->tp_iternext = adapter_map_next;
    PyType_Modified(map_type);

    PyObject *empty_tuple = PyTuple_New(0);
    PyObject *tuple_iterator = empty_tuple == NULL ? NULL : PyObject_GetIter(empty_tuple);
    Py_XDECREF(empty_tuple);
    if (tuple_iterator == NULL) {
        return -1;
    }
    tuple_iterator_type = Py_TYPE(tuple_iterator);
    Py_DECREF(tuple_iterator);

    PyObject *zip_type_object = PyDict_GetItemString(builtins, "zip");
    if (
        zip_type_object == NULL || !PyType_Check(zip_type_object) ||
        ((PyTypeObject *)zip_type_object)->tp_basicsize <
            (Py_ssize_t)sizeof(AleffZipObject)
    ) {
        PyErr_SetString(PyExc_RuntimeError, "unsupported built-in zip layout");
        return -1;
    }
    PyTypeObject *zip_type = (PyTypeObject *)zip_type_object;
    original_zip_new = zip_type->tp_new;
    zip_type->tp_new = adapter_zip_new;
    zip_type->tp_iternext = adapter_zip_next;
    PyType_Modified(zip_type);

    PyObject *enumerate_type_object = PyDict_GetItemString(builtins, "enumerate");
    if (
        enumerate_type_object == NULL || !PyType_Check(enumerate_type_object) ||
        ((PyTypeObject *)enumerate_type_object)->tp_basicsize <
            (Py_ssize_t)sizeof(AleffEnumerateObject) ||
        ((PyTypeObject *)enumerate_type_object)->tp_vectorcall == NULL
    ) {
        PyErr_SetString(PyExc_RuntimeError, "unsupported built-in enumerate layout");
        return -1;
    }
    PyTypeObject *enumerate_type = (PyTypeObject *)enumerate_type_object;
    original_enumerate_new = enumerate_type->tp_new;
    enumerate_type->tp_new = adapter_enumerate_new;
    original_enumerate_vectorcall = enumerate_type->tp_vectorcall;
    enumerate_type->tp_vectorcall = adapter_enumerate_vectorcall;
    enumerate_type->tp_iternext = adapter_enumerate_next;
    PyType_Modified(enumerate_type);

    PyObject *reversed_type_object = PyDict_GetItemString(builtins, "reversed");
    if (
        reversed_type_object == NULL || !PyType_Check(reversed_type_object) ||
        ((PyTypeObject *)reversed_type_object)->tp_basicsize <
            (Py_ssize_t)sizeof(AleffReversedObject) ||
        ((PyTypeObject *)reversed_type_object)->tp_vectorcall == NULL
    ) {
        PyErr_SetString(PyExc_RuntimeError, "unsupported built-in reversed layout");
        return -1;
    }
    PyTypeObject *reversed_type = (PyTypeObject *)reversed_type_object;
    original_reversed_new = reversed_type->tp_new;
    reversed_type->tp_new = adapter_reversed_new;
    original_reversed_vectorcall = reversed_type->tp_vectorcall;
    reversed_type->tp_vectorcall = adapter_reversed_vectorcall;
    original_reversed_next = reversed_type->tp_iternext;
    reversed_type->tp_iternext = adapter_reversed_next;
    PyType_Modified(reversed_type);

    PyObject *filter_type_object = PyDict_GetItemString(builtins, "filter");
    if (
        filter_type_object == NULL || !PyType_Check(filter_type_object) ||
        ((PyTypeObject *)filter_type_object)->tp_basicsize <
            (Py_ssize_t)sizeof(AleffFilterObject)
    ) {
        PyErr_SetString(PyExc_RuntimeError, "unsupported built-in filter layout");
        return -1;
    }
    PyTypeObject *filter_type = (PyTypeObject *)filter_type_object;
    filter_type->tp_iternext = adapter_filter_next;
    PyType_Modified(filter_type);

    PyObject *itertools = PyImport_ImportModule("itertools");
    if (itertools == NULL) {
        return -1;
    }
    PyObject *accumulate_type_object = PyObject_GetAttrString(itertools, "accumulate");
    PyObject *batched_type_object = PyObject_GetAttrString(itertools, "batched");
    Py_DECREF(itertools);
    if (
        accumulate_type_object == NULL || !PyType_Check(accumulate_type_object) ||
        ((PyTypeObject *)accumulate_type_object)->tp_basicsize <
            (Py_ssize_t)sizeof(AleffAccumulateObject)
    ) {
        Py_XDECREF(accumulate_type_object);
        PyErr_SetString(PyExc_RuntimeError, "unsupported itertools.accumulate layout");
        return -1;
    }
    PyTypeObject *accumulate_type = (PyTypeObject *)accumulate_type_object;
    accumulate_type->tp_iternext = adapter_accumulate_next;
    PyType_Modified(accumulate_type);
    Py_DECREF(accumulate_type_object);
    if (batched_type_object == NULL || !PyType_Check(batched_type_object)) {
        Py_XDECREF(batched_type_object);
        PyErr_SetString(PyExc_RuntimeError, "cannot access itertools.batched type");
        return -1;
    }
    PyTypeObject *batched_type = (PyTypeObject *)batched_type_object;
    original_batched_new = batched_type->tp_new;
    batched_type->tp_new = adapter_batched_new;
    PyType_Modified(batched_type);
    Py_DECREF(batched_type_object);

    original_list_init = PyList_Type.tp_init;
    PyList_Type.tp_init = adapter_list_init;
    original_list_vectorcall = PyList_Type.tp_vectorcall;
    PyList_Type.tp_vectorcall = adapter_list_vectorcall;
    original_tuple_new = PyTuple_Type.tp_new;
    PyTuple_Type.tp_new = adapter_tuple_new;
    original_tuple_vectorcall = PyTuple_Type.tp_vectorcall;
    PyTuple_Type.tp_vectorcall = adapter_tuple_vectorcall;
    original_dict_init = PyDict_Type.tp_init;
    PyDict_Type.tp_init = adapter_dict_init;
    original_dict_vectorcall = PyDict_Type.tp_vectorcall;
    PyDict_Type.tp_vectorcall = adapter_dict_vectorcall;
    original_set_init = PySet_Type.tp_init;
    PySet_Type.tp_init = adapter_set_init;
    original_set_vectorcall = PySet_Type.tp_vectorcall;
    PySet_Type.tp_vectorcall = adapter_set_vectorcall;
    original_frozenset_new = PyFrozenSet_Type.tp_new;
    PyFrozenSet_Type.tp_new = adapter_frozenset_new;
    original_frozenset_vectorcall = PyFrozenSet_Type.tp_vectorcall;
    PyFrozenSet_Type.tp_vectorcall = adapter_frozenset_vectorcall;
    original_bytes_new = PyBytes_Type.tp_new;
    PyBytes_Type.tp_new = adapter_bytes_new;
    original_bytearray_init = PyByteArray_Type.tp_init;
    PyByteArray_Type.tp_init = adapter_bytearray_init;
    original_slice_hash = PySlice_Type.tp_hash;
    if (original_slice_hash == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "slice is not hashable on this CPython");
        return -1;
    }
    PySlice_Type.tp_hash = adapter_slice_hash;
    PyType_Modified(&PyList_Type);
    PyType_Modified(&PyTuple_Type);
    PyType_Modified(&PyDict_Type);
    PyType_Modified(&PySet_Type);
    PyType_Modified(&PyFrozenSet_Type);
    PyType_Modified(&PyBytes_Type);
    PyType_Modified(&PyByteArray_Type);
    PyType_Modified(&PySlice_Type);

    if (replace_builtin(builtins, "sum", &sum_method) < 0 ||
        replace_builtin(builtins, "all", &all_method) < 0 ||
        replace_builtin(builtins, "any", &any_method) < 0 ||
        replace_builtin(builtins, "next", &next_method) < 0 ||
        replace_builtin(builtins, "len", &len_method) < 0 ||
        replace_builtin(builtins, "min", &min_method) < 0 ||
        replace_builtin(builtins, "max", &max_method) < 0 ||
        replace_builtin(builtins, "bin", &bin_method) < 0 ||
        replace_builtin(builtins, "oct", &oct_method) < 0 ||
        replace_builtin(builtins, "hex", &hex_method) < 0 ||
        replace_builtin(builtins, "sorted", &sorted_method) < 0 ||
        replace_type_method(&PyList_Type, "extend", &list_extend_method) < 0 ||
        replace_type_method(&PyList_Type, "count", &list_count_method) < 0 ||
        replace_type_method(&PyDict_Type, "get", &dict_get_method) < 0) {
        return -1;
    }
    PyObject *functools = PyImport_ImportModule("functools");
    if (functools == NULL) {
        return -1;
    }
    PyObject *functools_name = PyUnicode_FromString("functools");
    if (functools_name == NULL) {
        Py_DECREF(functools);
        return -1;
    }
    PyObject *reduce_function = PyCFunction_NewEx(
        &reduce_method,
        NULL,
        functools_name
    );
    Py_DECREF(functools_name);
    if (reduce_function == NULL) {
        Py_DECREF(functools);
        return -1;
    }
    int reduce_status = PyObject_SetAttrString(
        functools,
        "reduce",
        reduce_function
    );
    Py_DECREF(reduce_function);
    Py_DECREF(functools);
    if (reduce_status < 0) {
        return -1;
    }
    adapters_installed = 1;
    return 0;
}
