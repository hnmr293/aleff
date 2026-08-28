#include "internal.h"
#include "iterators.h"
#include "itertools.h"
#include <stdbool.h>

typedef struct {
    PyObject_HEAD
    PyObject *source;
    PyObject *active;
} AleffChainObject;

Py_ssize_t
adapter_chain_basicsize(void)
{
    return (Py_ssize_t)sizeof(AleffChainObject);
}

typedef enum {
    CHAIN_WAIT_SOURCE_NEXT,
    CHAIN_WAIT_ACTIVE_ITER,
    CHAIN_WAIT_ACTIVE_NEXT,
} ChainPhase;

typedef struct {
    AleffChainObject *chain;
    PyObject *source;
    PyObject *active;
    ChainPhase phase;
} ChainState;

static void *
chain_copy_state(const void *raw_state)
{
    const ChainState *state = raw_state;
    ChainState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->chain = (AleffChainObject *)Py_NewRef((PyObject *)state->chain);
    copy->source = state->source == NULL
        ? NULL
        : clone_iterator_for_snapshot(state->source);
    if (state->source != NULL && copy->source == NULL) {
        Py_DECREF(copy->chain);
        PyMem_Free(copy);
        return NULL;
    }
    copy->active = Py_XNewRef(state->active);
    copy->phase = state->phase;
    return copy;
}

static void
chain_free_state(void *raw_state)
{
    ChainState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->chain);
    Py_XDECREF(state->source);
    Py_XDECREF(state->active);
    PyMem_Free(state);
}

static void
chain_restore(ChainState *state)
{
    Py_XSETREF(state->chain->source, Py_XNewRef(state->source));
    Py_XSETREF(state->chain->active, Py_XNewRef(state->active));
}

static PyObject *chain_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable chain_vtable = {
    .copy_state = chain_copy_state,
    .free_state = chain_free_state,
    .resume = chain_resume,
};

static PyObject *
chain_continue(ChainState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed) {
        chain_restore(state);
        if (state->phase == CHAIN_WAIT_SOURCE_NEXT) {
            if (resumed_value == NULL) {
                if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                    return NULL;
                }
                PyErr_Clear();
                Py_CLEAR(state->chain->source);
                return NULL;
            }
            state->phase = CHAIN_WAIT_ACTIVE_ITER;
            PyObject *iterator = PyObject_GetIter(resumed_value);
            if (iterator == NULL) {
                Py_CLEAR(state->chain->source);
                return NULL;
            }
            Py_XSETREF(state->chain->active, iterator);
            Py_XSETREF(state->active, Py_NewRef(iterator));
        }
        else if (state->phase == CHAIN_WAIT_ACTIVE_ITER) {
            if (!PyIter_Check(resumed_value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "iter() returned non-iterator of type '%.200s'",
                    Py_TYPE(resumed_value)->tp_name
                );
                Py_CLEAR(state->chain->source);
                return NULL;
            }
            Py_XSETREF(state->chain->active, Py_NewRef(resumed_value));
            Py_XSETREF(state->active, Py_NewRef(resumed_value));
        }
        else if (resumed_value != NULL) {
            return Py_NewRef(resumed_value);
        }
        else {
            if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                return NULL;
            }
            PyErr_Clear();
            Py_CLEAR(state->chain->active);
            Py_CLEAR(state->active);
        }
    }

    while (state->chain->source != NULL) {
        if (state->chain->active == NULL) {
            state->phase = CHAIN_WAIT_SOURCE_NEXT;
            PyObject *iterable = Py_TYPE(state->chain->source)->tp_iternext(
                state->chain->source
            );
            if (iterable == NULL) {
                if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                    return NULL;
                }
                PyErr_Clear();
                Py_CLEAR(state->chain->source);
                return NULL;
            }
            state->phase = CHAIN_WAIT_ACTIVE_ITER;
            PyObject *iterator = PyObject_GetIter(iterable);
            Py_DECREF(iterable);
            if (iterator == NULL) {
                Py_CLEAR(state->chain->source);
                return NULL;
            }
            Py_XSETREF(state->chain->active, iterator);
            Py_XSETREF(state->active, Py_NewRef(iterator));
        }

        state->phase = CHAIN_WAIT_ACTIVE_NEXT;
        PyObject *item = Py_TYPE(state->chain->active)->tp_iternext(state->chain->active);
        if (item != NULL) {
            return item;
        }
        if (PyErr_Occurred()) {
            if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                return NULL;
            }
            PyErr_Clear();
        }
        Py_CLEAR(state->chain->active);
        Py_CLEAR(state->active);
    }
    return NULL;
}

static PyObject *
chain_resume(const void *raw_state, PyObject *value)
{
    ChainState *state = chain_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &chain_vtable, state) < 0) {
        return NULL;
    }
    PyObject *result = chain_continue(state, value, 1);
    adapter_leave(&frame);
    chain_free_state(state);
    return result;
}

PyObject *
adapter_chain_next(PyObject *object)
{
    AleffChainObject *chain = (AleffChainObject *)object;
    ChainState state = {
        .chain = chain,
        .source = Py_XNewRef(chain->source),
        .active = Py_XNewRef(chain->active),
        .phase = CHAIN_WAIT_SOURCE_NEXT,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &chain_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = chain_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.source);
    Py_XDECREF(state.active);
    return result;
}

typedef struct {
    PyTypeObject *type;
} ChainConstructorState;

static void *
chain_constructor_copy_state(const void *raw_state)
{
    const ChainConstructorState *state = raw_state;
    ChainConstructorState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->type = (PyTypeObject *)Py_NewRef((PyObject *)state->type);
    return copy;
}

static void
chain_constructor_free_state(void *raw_state)
{
    ChainConstructorState *state = raw_state;
    if (state != NULL) {
        Py_DECREF(state->type);
        PyMem_Free(state);
    }
}

static PyObject *
chain_from_iterator(PyTypeObject *type, PyObject *iterator)
{
    if (!PyIter_Check(iterator)) {
        PyErr_Format(
            PyExc_TypeError,
            "iter() returned non-iterator of type '%.200s'",
            Py_TYPE(iterator)->tp_name
        );
        return NULL;
    }
    AleffChainObject *chain = (AleffChainObject *)type->tp_alloc(type, 0);
    if (chain == NULL) {
        return NULL;
    }
    chain->source = Py_NewRef(iterator);
    chain->active = NULL;
    return (PyObject *)chain;
}

static PyObject *
chain_constructor_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    const ChainConstructorState *state = raw_state;
    return chain_from_iterator(state->type, value);
}

static const AleffAdapterVTable chain_constructor_vtable = {
    .copy_state = chain_constructor_copy_state,
    .free_state = chain_constructor_free_state,
    .resume = chain_constructor_resume,
};

PyObject *
adapter_chain_from_iterable(PyObject *type_object, PyObject *iterable)
{
    ChainConstructorState state = {.type = (PyTypeObject *)type_object};
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &chain_constructor_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *iterator = PyObject_GetIter(iterable);
    PyObject *result = iterator == NULL
        ? NULL
        : chain_from_iterator(state.type, iterator);
    Py_XDECREF(iterator);
    adapter_leave(&frame);
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

Py_ssize_t
adapter_accumulate_basicsize(void)
{
    return (Py_ssize_t)sizeof(AleffAccumulateObject);
}

static newfunc original_accumulate_new;

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
    if (adapter_enter(&frame, &accumulate_vtable, state) < 0) {
        return NULL;
    }
    PyObject *result = accumulate_continue(state, value, 1);
    adapter_leave(&frame);
    accumulate_free_state(state);
    return result;
}

PyObject *
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
    if (adapter_enter(&frame, &accumulate_vtable, &state) < 0) {
        return NULL;
    }
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
} AccumulateConstructorState;

static void *
accumulate_constructor_copy_state(const void *raw_state)
{
    const AccumulateConstructorState *state = raw_state;
    AccumulateConstructorState *copy = PyMem_Malloc(sizeof(*copy));
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
accumulate_constructor_free_state(void *raw_state)
{
    AccumulateConstructorState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->type);
    Py_DECREF(state->args);
    Py_XDECREF(state->kwargs);
    PyMem_Free(state);
}

static PyObject *accumulate_constructor_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable accumulate_constructor_vtable = {
    .copy_state = accumulate_constructor_copy_state,
    .free_state = accumulate_constructor_free_state,
    .resume = accumulate_constructor_resume,
};

static PyObject *
accumulate_constructor_resume(const void *raw_state, PyObject *value)
{
    const AccumulateConstructorState *state = raw_state;
    if (value == NULL) {
        return NULL;
    }
    Py_ssize_t argument_count = PyTuple_GET_SIZE(state->args);
    if (argument_count < 1) {
        PyErr_SetString(PyExc_RuntimeError, "accumulate constructor lost its iterable");
        return NULL;
    }
    PyObject *replacement_args = PyTuple_New(argument_count);
    if (replacement_args == NULL) {
        return NULL;
    }
    PyTuple_SET_ITEM(replacement_args, 0, Py_NewRef(value));
    for (Py_ssize_t index = 1; index < argument_count; index++) {
        PyTuple_SET_ITEM(
            replacement_args,
            index,
            Py_NewRef(PyTuple_GET_ITEM(state->args, index))
        );
    }
    PyObject *result = original_accumulate_new(
        state->type,
        replacement_args,
        state->kwargs
    );
    Py_DECREF(replacement_args);
    return result;
}

static PyObject *
adapter_accumulate_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    AccumulateConstructorState state = {
        .type = type,
        .args = args,
        .kwargs = kwargs,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &accumulate_constructor_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = original_accumulate_new(type, args, kwargs);
    adapter_leave(&frame);
    return result;
}

typedef struct {
    PyTypeObject *type;
    PyObject *args;
    PyObject *kwargs;
} BatchedConstructorState;

newfunc original_batched_new;
static iternextfunc original_batched_next;
PyTypeObject *original_batched_type = NULL;
iternextfunc original_accumulate_next = NULL;
PyTypeObject *original_accumulate_type = NULL;

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

PyObject *
adapter_batched_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    BatchedConstructorState state = {
        .type = type,
        .args = args,
        .kwargs = kwargs,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &batched_constructor_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = original_batched_new(type, args, kwargs);
    adapter_leave(&frame);
    return result;
}

typedef struct {
    PyObject_HEAD
    PyObject *iterator;
    Py_ssize_t batch_size;
#if PY_VERSION_HEX >= 0x030d0000
    bool strict;
#endif
} AleffBatchedObject;

typedef enum {
    BATCHED_WAIT_INPUT,
} BatchedPhase;

typedef struct {
    PyObject *owner;
    PyObject *iterator;
    PyObject *items;
    Py_ssize_t batch_size;
    int strict;
    BatchedPhase phase;
} BatchedState;

static void *
batched_copy_state(const void *raw_state)
{
    const BatchedState *state = raw_state;
    BatchedState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->owner = Py_NewRef(state->owner);
    copy->iterator = Py_NewRef(state->iterator);
    copy->items = PyList_GetSlice(
        state->items,
        0,
        PyList_GET_SIZE(state->items)
    );
    if (copy->items == NULL) {
        Py_DECREF(copy->owner);
        Py_DECREF(copy->iterator);
        PyMem_Free(copy);
        return NULL;
    }
    copy->batch_size = state->batch_size;
    copy->strict = state->strict;
    copy->phase = state->phase;
    return copy;
}

static void
batched_free_state(void *raw_state)
{
    BatchedState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->owner);
    Py_DECREF(state->iterator);
    Py_DECREF(state->items);
    PyMem_Free(state);
}

static PyObject *batched_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable batched_vtable = {
    .copy_state = batched_copy_state,
    .free_state = batched_free_state,
    .resume = batched_resume,
};

static PyObject *
batched_continue(BatchedState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed) {
        if (state->phase != BATCHED_WAIT_INPUT || resumed_value == NULL) {
            return NULL;
        }
        if (PyList_Append(state->items, resumed_value) < 0) {
            return NULL;
        }
    }
    while (PyList_GET_SIZE(state->items) < state->batch_size) {
        state->phase = BATCHED_WAIT_INPUT;
        PyObject *item = Py_TYPE(state->iterator)->tp_iternext(state->iterator);
        if (item == NULL) {
            if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                return NULL;
            }
            PyErr_Clear();
            Py_ssize_t item_count = PyList_GET_SIZE(state->items);
            if (item_count == 0) {
                return NULL;
            }
            if (state->strict && item_count != state->batch_size) {
                PyErr_SetString(PyExc_ValueError, "batched(): incomplete batch");
                return NULL;
            }
            return PyList_AsTuple(state->items);
        }
        if (PyList_Append(state->items, item) < 0) {
            Py_DECREF(item);
            return NULL;
        }
        Py_DECREF(item);
    }
    return PyList_AsTuple(state->items);
}

static PyObject *
batched_resume(const void *raw_state, PyObject *value)
{
    BatchedState *state = batched_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &batched_vtable, state) < 0) {
        batched_free_state(state);
        return NULL;
    }
    PyObject *result = batched_continue(state, value, 1);
    adapter_leave(&frame);
    batched_free_state(state);
    return result;
}

static PyObject *
adapter_batched_next(PyObject *object)
{
    AleffBatchedObject *batched = (AleffBatchedObject *)object;
    BatchedState state = {
        .owner = object,
        .iterator = batched->iterator,
        .items = PyList_New(0),
        .batch_size = batched->batch_size,
#if PY_VERSION_HEX >= 0x030d0000
        .strict = batched->strict,
#else
        .strict = 0,
#endif
        .phase = BATCHED_WAIT_INPUT,
    };
    if (state.items == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &batched_vtable, &state) < 0) {
        Py_DECREF(state.items);
        return NULL;
    }
    PyObject *result = batched_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_DECREF(state.items);
    return result;
}

/*
 * The remaining itertools implementations are stateful C iterators.  Their
 * callbacks are nevertheless ordinary Python calls, so the C boundary only
 * needs to preserve the callback's result while the Python continuation is
 * being restored.  The empty adapter is deliberately shared with map: it
 * does not invent iterator state, and lets the owning itertools object keep
 * its normal state transitions when the call completes.
 */
static iternextfunc original_itertools_next[17] = {NULL};
static PyTypeObject *itertools_next_types[17] = {NULL};

static int adapter_native_itertools_next(PyObject *object, PyObject **result);

static PyObject *
adapter_itertools_next(PyObject *object)
{
    PyObject *native_result = NULL;
    int native_handled = adapter_native_itertools_next(object, &native_result);
    if (native_handled != 0) {
        return native_handled < 0 ? NULL : native_result;
    }
    PyTypeObject *type = Py_TYPE(object);
    for (int index = 0; index < 17; index++) {
        if (itertools_next_types[index] != NULL &&
            PyType_IsSubtype(type, itertools_next_types[index]) &&
            original_itertools_next[index] != NULL) {
            AleffAdapterFrame frame;
            if (adapter_enter(&frame, &map_vtable, NULL) < 0) {
                return NULL;
            }
            PyObject *result = original_itertools_next[index](object);
            adapter_leave(&frame);
            return result;
        }
    }
    PyErr_SetString(PyExc_RuntimeError, "unknown itertools iterator type");
    return NULL;
}

typedef enum {
    ITERTOOLS_ACCUMULATE,
    ITERTOOLS_BATCHED,
    ITERTOOLS_CHAIN,
    ITERTOOLS_COMBINATIONS,
    ITERTOOLS_COMBINATIONS_REPLACEMENT,
    ITERTOOLS_COMPRESS,
    ITERTOOLS_CYCLE,
    ITERTOOLS_DROPWHILE,
    ITERTOOLS_FILTERFALSE,
    ITERTOOLS_GROUPBY,
    ITERTOOLS_ISLICE,
    ITERTOOLS_PAIRWISE,
    ITERTOOLS_PERMUTATIONS,
    ITERTOOLS_PRODUCT,
    ITERTOOLS_STARMAP,
    ITERTOOLS_TAKEWHILE,
    ITERTOOLS_TEE,
    ITERTOOLS_ZIP_LONGEST,
    ITERTOOLS_COUNT,
    ITERTOOLS_REPEAT,
} ItIteratorKind;

typedef struct {
    PyTypeObject *type;
    PyObject *args;
    PyObject *kwargs;
    PyObject *converted;
    Py_ssize_t index;
    ItIteratorKind kind;
} ItConstructorState;

static newfunc original_itertools_new[20] = {NULL};
static PyTypeObject *itertools_new_types[20] = {NULL};

typedef struct {
    PyObject_HEAD
    PyObject *iterator;
} ItIteratorHolder;

static int it_runtime_is_builtin_position_iterator(PyObject *iterator);
static PyObject *it_runtime_clone_position_iterator(PyObject *iterator);

typedef enum {
    IT_POOL_WAIT_ITERATOR,
    IT_POOL_WAIT_ITEM,
} ItPoolPhase;

typedef struct {
    PyObject *iterable;
    PyObject *iterator;
    PyObject *items;
    ItPoolPhase phase;
} ItPoolState;

static void *
it_pool_copy_state(const void *raw_state)
{
    const ItPoolState *state = raw_state;
    ItPoolState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->iterable = Py_NewRef(state->iterable);
    copy->iterator = state->iterator == NULL
        ? NULL
        : it_runtime_is_builtin_position_iterator(state->iterator)
            ? it_runtime_clone_position_iterator(state->iterator)
            : Py_NewRef(state->iterator);
    copy->items = PyList_GetSlice(
        state->items, 0, PyList_GET_SIZE(state->items)
    );
    copy->phase = state->phase;
    if ((state->iterator != NULL && copy->iterator == NULL) ||
        copy->items == NULL) {
        Py_DECREF(copy->iterable);
        Py_XDECREF(copy->iterator);
        Py_XDECREF(copy->items);
        PyMem_Free(copy);
        return NULL;
    }
    return copy;
}

static void
it_pool_free_state(void *raw_state)
{
    ItPoolState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->iterable);
    Py_XDECREF(state->iterator);
    Py_DECREF(state->items);
    PyMem_Free(state);
}

static PyObject *it_pool_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable it_pool_vtable = {
    .copy_state = it_pool_copy_state,
    .free_state = it_pool_free_state,
    .resume = it_pool_resume,
};

static PyObject *
it_pool_continue(ItPoolState *state, PyObject *value, int is_resumed)
{
    PyObject *item = NULL;
    if (is_resumed && state->phase == IT_POOL_WAIT_ITERATOR) {
        if (value == NULL || !PyIter_Check(value)) {
            if (value != NULL) {
                PyErr_Format(
                    PyExc_TypeError,
                    "iter() returned non-iterator of type '%.200s'",
                    Py_TYPE(value)->tp_name
                );
            }
            return NULL;
        }
        Py_XSETREF(state->iterator, Py_NewRef(value));
    }
    else if (is_resumed && state->phase == IT_POOL_WAIT_ITEM) {
        if (value == NULL) {
            if (PyErr_Occurred() &&
                !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                return NULL;
            }
            PyErr_Clear();
            return PyList_AsTuple(state->items);
        }
        item = Py_NewRef(value);
    }
    if (state->iterator == NULL) {
        state->phase = IT_POOL_WAIT_ITERATOR;
        state->iterator = PyObject_GetIter(state->iterable);
        if (state->iterator == NULL) {
            return NULL;
        }
    }
    for (;;) {
        if (item == NULL) {
            state->phase = IT_POOL_WAIT_ITEM;
            item = Py_TYPE(state->iterator)->tp_iternext(state->iterator);
            if (item == NULL) {
                if (PyErr_Occurred() &&
                    !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                    return NULL;
                }
                PyErr_Clear();
                return PyList_AsTuple(state->items);
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
it_pool_resume(const void *raw_state, PyObject *value)
{
    ItPoolState *state = it_pool_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &it_pool_vtable, state) < 0) {
        it_pool_free_state(state);
        return NULL;
    }
    PyObject *result = it_pool_continue(state, value, 1);
    adapter_leave(&frame);
    it_pool_free_state(state);
    return result;
}

static PyObject *
it_pool_collect(PyObject *iterable)
{
    ItPoolState state = {
        .iterable = iterable,
        .iterator = NULL,
        .items = PyList_New(0),
        .phase = IT_POOL_WAIT_ITERATOR,
    };
    if (state.items == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &it_pool_vtable, &state) < 0) {
        Py_DECREF(state.items);
        return NULL;
    }
    PyObject *result = it_pool_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.iterator);
    Py_DECREF(state.items);
    return result;
}

static PyObject *
it_iterator_holder_iter(PyObject *object)
{
    return Py_NewRef(((ItIteratorHolder *)object)->iterator);
}

static void
it_iterator_holder_dealloc(PyObject *object)
{
    Py_DECREF(((ItIteratorHolder *)object)->iterator);
    Py_TYPE(object)->tp_free(object);
}

static PyTypeObject ItIteratorHolderType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "aleff._itertools_iterator_holder",
    .tp_basicsize = sizeof(ItIteratorHolder),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_iter = it_iterator_holder_iter,
    .tp_dealloc = it_iterator_holder_dealloc,
};

static PyObject *
it_iterator_holder_new(PyObject *iterator)
{
    ItIteratorHolder *holder = PyObject_New(
        ItIteratorHolder, &ItIteratorHolderType
    );
    if (holder == NULL) {
        return NULL;
    }
    holder->iterator = Py_NewRef(iterator);
    return (PyObject *)holder;
}

static void *
it_constructor_copy_state(const void *raw_state)
{
    const ItConstructorState *state = raw_state;
    ItConstructorState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->type = (PyTypeObject *)Py_NewRef((PyObject *)state->type);
    copy->args = Py_NewRef(state->args);
    copy->kwargs = Py_XNewRef(state->kwargs);
    copy->converted = PyList_GetSlice(state->converted, 0, PyList_GET_SIZE(state->converted));
    copy->index = state->index;
    copy->kind = state->kind;
    if (copy->converted == NULL) {
        Py_DECREF(copy->type);
        Py_DECREF(copy->args);
        Py_XDECREF(copy->kwargs);
        PyMem_Free(copy);
        return NULL;
    }
    return copy;
}

static void
it_constructor_free_state(void *raw_state)
{
    ItConstructorState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->type);
    Py_DECREF(state->args);
    Py_XDECREF(state->kwargs);
    Py_DECREF(state->converted);
    PyMem_Free(state);
}

static int
it_constructor_is_iterable_arg(ItIteratorKind kind, Py_ssize_t index, Py_ssize_t count)
{
    switch (kind) {
        case ITERTOOLS_ACCUMULATE:
        case ITERTOOLS_BATCHED:
        case ITERTOOLS_COMBINATIONS:
        case ITERTOOLS_COMBINATIONS_REPLACEMENT:
        case ITERTOOLS_CYCLE:
        case ITERTOOLS_GROUPBY:
        case ITERTOOLS_ISLICE:
        case ITERTOOLS_PAIRWISE:
        case ITERTOOLS_PERMUTATIONS:
        case ITERTOOLS_TEE:
            return index == 0;
        case ITERTOOLS_COMPRESS:
            return index < 2;
        case ITERTOOLS_DROPWHILE:
        case ITERTOOLS_FILTERFALSE:
        case ITERTOOLS_STARMAP:
        case ITERTOOLS_TAKEWHILE:
            return index == 1;
        case ITERTOOLS_PRODUCT:
        case ITERTOOLS_CHAIN:
        case ITERTOOLS_ZIP_LONGEST:
            return index < count;
        default:
            return 0;
    }
}

static int
it_constructor_uses_eager_pool(ItIteratorKind kind)
{
    return kind == ITERTOOLS_COMBINATIONS ||
        kind == ITERTOOLS_COMBINATIONS_REPLACEMENT ||
        kind == ITERTOOLS_PERMUTATIONS ||
        kind == ITERTOOLS_PRODUCT;
}

static PyObject *
it_constructor_continue(ItConstructorState *state, PyObject *resumed_value, int is_resumed)
{
    Py_ssize_t count = PyTuple_GET_SIZE(state->args);
    if (is_resumed) {
        int eager_pool = it_constructor_uses_eager_pool(state->kind);
        if ((!eager_pool && !PyIter_Check(resumed_value)) ||
            (eager_pool && !PyTuple_CheckExact(resumed_value))) {
            PyErr_Format(
                PyExc_TypeError,
                eager_pool
                    ? "iterator pool conversion returned %.200s instead of tuple"
                    : "iter() returned non-iterator of type '%.200s'",
                Py_TYPE(resumed_value)->tp_name
            );
            return NULL;
        }
        if (PyList_Append(state->converted, resumed_value) < 0) {
            return NULL;
        }
        state->index++;
    }
    while (state->index < count) {
        PyObject *argument = PyTuple_GET_ITEM(state->args, state->index);
        int iterable = it_constructor_is_iterable_arg(
            state->kind, state->index, count
        );
        PyObject *value;
        if (!iterable) {
            value = Py_NewRef(argument);
        }
        else if (!it_constructor_uses_eager_pool(state->kind)) {
            value = PyObject_GetIter(argument);
        }
        else if (PyTuple_CheckExact(argument)) {
            value = Py_NewRef(argument);
        }
        else {
            value = it_pool_collect(argument);
        }
        if (value == NULL) {
            return NULL;
        }
        if (PyList_Append(state->converted, value) < 0) {
            Py_DECREF(value);
            return NULL;
        }
        Py_DECREF(value);
        state->index++;
    }
    PyObject *converted = PyList_AsTuple(state->converted);
    if (converted == NULL) {
        return NULL;
    }
    PyObject *native_args = Py_NewRef(converted);
    if (state->kind == ITERTOOLS_GROUPBY) {
        PyObject *holder = it_iterator_holder_new(
            PyTuple_GET_ITEM(converted, 0)
        );
        if (holder == NULL) {
            Py_DECREF(native_args);
            Py_DECREF(converted);
            return NULL;
        }
        Py_DECREF(native_args);
        Py_ssize_t count = PyTuple_GET_SIZE(converted);
        native_args = PyTuple_New(count);
        if (native_args == NULL) {
            Py_DECREF(holder);
            Py_DECREF(converted);
            return NULL;
        }
        PyTuple_SET_ITEM(native_args, 0, holder);
        for (Py_ssize_t index = 1; index < count; index++) {
            PyTuple_SET_ITEM(
                native_args,
                index,
                Py_NewRef(PyTuple_GET_ITEM(converted, index))
            );
        }
    }
    PyObject *result = original_itertools_new[state->kind](
        state->type, native_args, state->kwargs
    );
    Py_DECREF(native_args);
    Py_DECREF(converted);
    return result;
}

static const AleffAdapterVTable it_constructor_vtable;

static PyObject *
it_constructor_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    ItConstructorState *state = it_constructor_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &it_constructor_vtable, state) < 0) {
        return NULL;
    }
    PyObject *result = it_constructor_continue(state, value, 1);
    adapter_leave(&frame);
    it_constructor_free_state(state);
    return result;
}

static const AleffAdapterVTable it_constructor_vtable = {
    .copy_state = it_constructor_copy_state,
    .free_state = it_constructor_free_state,
    .resume = it_constructor_resume,
};

typedef struct {
    PyTypeObject *type;
    PyObject *args;
    PyObject *kwargs;
    PyObject *times;
    PyObject *indexed;
} RepeatConstructorState;

static void *
repeat_constructor_copy(const void *raw_state)
{
    const RepeatConstructorState *state = raw_state;
    RepeatConstructorState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->type = (PyTypeObject *)Py_NewRef((PyObject *)state->type);
    copy->args = Py_NewRef(state->args);
    copy->kwargs = Py_XNewRef(state->kwargs);
    copy->times = Py_NewRef(state->times);
    copy->indexed = Py_XNewRef(state->indexed);
    return copy;
}

static void
repeat_constructor_free(void *raw_state)
{
    RepeatConstructorState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->type);
    Py_DECREF(state->args);
    Py_XDECREF(state->kwargs);
    Py_DECREF(state->times);
    Py_XDECREF(state->indexed);
    PyMem_Free(state);
}

static PyObject *repeat_constructor_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable repeat_constructor_vtable = {
    .copy_state = repeat_constructor_copy,
    .free_state = repeat_constructor_free,
    .resume = repeat_constructor_resume,
};

static PyObject *
repeat_constructor_continue(
    RepeatConstructorState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (resumed_value == NULL) {
            PyErr_SetString(PyExc_TypeError, "__index__ returned non-int (type NoneType)");
            return NULL;
        }
        if (!PyLong_Check(resumed_value)) {
            PyErr_Format(
                PyExc_TypeError,
                "__index__ returned non-int (type %.200s)",
                Py_TYPE(resumed_value)->tp_name
            );
            return NULL;
        }
        Py_ssize_t resumed_index = PyLong_AsSsize_t(resumed_value);
        if (resumed_index == -1 && PyErr_Occurred()) {
            return NULL;
        }
        PyObject *normalized = PyLong_FromSsize_t(resumed_index);
        if (normalized == NULL) {
            return NULL;
        }
        Py_XSETREF(state->indexed, normalized);
    }
    if (state->indexed == NULL) {
        state->indexed = PyNumber_Index(state->times);
        if (state->indexed == NULL) {
            return NULL;
        }
    }
    Py_ssize_t ignored = PyNumber_AsSsize_t(state->indexed, PyExc_OverflowError);
    if (ignored == -1 && PyErr_Occurred()) {
        return NULL;
    }

    Py_ssize_t nargs = PyTuple_GET_SIZE(state->args);
    PyObject *args = NULL;
    PyObject *kwargs = state->kwargs == NULL ? NULL : PyDict_Copy(state->kwargs);
    if (state->kwargs != NULL && kwargs == NULL) {
        return NULL;
    }
    if (nargs >= 2) {
        args = PyTuple_GetSlice(state->args, 0, nargs);
        if (args != NULL) {
            Py_DECREF(PyTuple_GET_ITEM(args, 1));
            Py_INCREF(state->indexed);
            PyTuple_SET_ITEM(args, 1, state->indexed);
        }
    }
    else if (nargs == 1 &&
             (state->kwargs == NULL ||
              PyDict_GetItemString(state->kwargs, "times") == NULL)) {
        args = PyTuple_New(2);
        if (args != NULL) {
            PyTuple_SET_ITEM(args, 0, Py_NewRef(PyTuple_GET_ITEM(state->args, 0)));
            PyTuple_SET_ITEM(args, 1, Py_NewRef(state->indexed));
        }
    }
    else {
        args = Py_NewRef(state->args);
        if (kwargs != NULL && PyDict_SetItemString(kwargs, "times", state->indexed) < 0) {
            Py_DECREF(args);
            Py_DECREF(kwargs);
            return NULL;
        }
    }
    if (args == NULL) {
        Py_XDECREF(kwargs);
        return NULL;
    }
    PyObject *result = original_itertools_new[ITERTOOLS_REPEAT](state->type, args, kwargs);
    Py_DECREF(args);
    Py_XDECREF(kwargs);
    return result;
}

static PyObject *
repeat_constructor_resume(const void *raw_state, PyObject *value)
{
    RepeatConstructorState *state = repeat_constructor_copy(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &repeat_constructor_vtable, state) < 0) {
        return NULL;
    }
    PyObject *result = repeat_constructor_continue(state, value, 1);
    adapter_leave(&frame);
    repeat_constructor_free(state);
    return result;
}

static PyObject *
adapter_repeat_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"object", "times", NULL};
    PyObject *element;
    PyObject *times = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O:repeat", keywords, &element, &times)) {
        return NULL;
    }
    if (times == NULL) {
        return original_itertools_new[ITERTOOLS_REPEAT](type, args, kwargs);
    }
    RepeatConstructorState state = {
        .type = type,
        .args = args,
        .kwargs = kwargs,
        .times = times,
        .indexed = NULL,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &repeat_constructor_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = repeat_constructor_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.indexed);
    return result;
}

typedef struct {
    PyTypeObject *type;
} PairwiseConstructorState;

static void *
pairwise_constructor_copy(const void *raw_state)
{
    const PairwiseConstructorState *state = raw_state;
    PairwiseConstructorState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->type = (PyTypeObject *)Py_NewRef((PyObject *)state->type);
    return copy;
}

static void
pairwise_constructor_free(void *raw_state)
{
    PairwiseConstructorState *state = raw_state;
    if (state != NULL) {
        Py_DECREF(state->type);
        PyMem_Free(state);
    }
}

static PyObject *
pairwise_constructor_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    const PairwiseConstructorState *state = raw_state;
    PyObject *args = PyTuple_Pack(1, value);
    if (args == NULL) {
        return NULL;
    }
    PyObject *result = original_itertools_new[ITERTOOLS_PAIRWISE](
        state->type, args, NULL
    );
    Py_DECREF(args);
    return result;
}

static const AleffAdapterVTable pairwise_constructor_vtable = {
    .copy_state = pairwise_constructor_copy,
    .free_state = pairwise_constructor_free,
    .resume = pairwise_constructor_resume,
};

static PyObject *
adapter_groupby_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *iterable;
    PyObject *key = NULL;
    static char *keywords[] = {"iterable", "key", NULL};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "O|O:groupby", keywords, &iterable, &key)) {
        return NULL;
    }
    PyObject *normalized = key == NULL
        ? PyTuple_Pack(1, iterable)
        : PyTuple_Pack(2, iterable, key);
    if (normalized == NULL) {
        return NULL;
    }
    PyObject *converted = PyList_New(0);
    if (converted == NULL) {
        Py_DECREF(normalized);
        return NULL;
    }
    ItConstructorState state = {
        .type = type,
        .args = normalized,
        .kwargs = NULL,
        .converted = converted,
        .index = 0,
        .kind = ITERTOOLS_GROUPBY,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &it_constructor_vtable, &state) < 0) {
        Py_DECREF(converted);
        Py_DECREF(normalized);
        return NULL;
    }
    PyObject *result = it_constructor_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_DECREF(converted);
    Py_DECREF(normalized);
    return result;
}

static int
it_constructor_has_unknown_product_keyword(PyObject *kwargs)
{
    if (kwargs == NULL) {
        return 0;
    }
    PyObject *key;
    PyObject *value;
    Py_ssize_t position = 0;
    while (PyDict_Next(kwargs, &position, &key, &value)) {
        int equal = PyUnicode_Check(key) &&
            PyUnicode_CompareWithASCIIString(key, "repeat") == 0;
        if (!equal) {
            return 1;
        }
    }
    return 0;
}

static int
it_constructor_shape_is_invalid(
    ItIteratorKind kind,
    PyObject *args,
    PyObject *kwargs
)
{
    Py_ssize_t count = PyTuple_GET_SIZE(args);
    if (kind == ITERTOOLS_PRODUCT) {
        return it_constructor_has_unknown_product_keyword(kwargs);
    }
    PyObject *iterable = kwargs == NULL
        ? NULL
        : PyDict_GetItemString(kwargs, "iterable");
    PyObject *r = kwargs == NULL ? NULL : PyDict_GetItemString(kwargs, "r");
    if (kind == ITERTOOLS_PERMUTATIONS) {
        return count > 2 || (count == 0 && iterable == NULL) ||
            (count >= 1 && iterable != NULL) || (count >= 2 && r != NULL);
    }
    return count > 2 || (count == 0 && iterable == NULL) ||
        (count < 2 && r == NULL) || (count >= 1 && iterable != NULL) ||
        (count >= 2 && r != NULL);
}

static int
it_constructor_validate_shape(
    PyTypeObject *type,
    ItIteratorKind kind,
    PyObject *args,
    PyObject *kwargs
)
{
    if (!it_constructor_shape_is_invalid(kind, args, kwargs)) {
        return 0;
    }
    Py_ssize_t count = PyTuple_GET_SIZE(args);
    PyObject *empty = PyTuple_New(0);
    PyObject *safe_args = PyTuple_New(count);
    PyObject *safe_kwargs = kwargs == NULL ? NULL : PyDict_Copy(kwargs);
    if (empty == NULL || safe_args == NULL ||
        (kwargs != NULL && safe_kwargs == NULL)) {
        Py_XDECREF(empty);
        Py_XDECREF(safe_args);
        Py_XDECREF(safe_kwargs);
        return -1;
    }
    for (Py_ssize_t index = 0; index < count; index++) {
        PyObject *argument = it_constructor_is_iterable_arg(kind, index, count)
            ? empty
            : PyTuple_GET_ITEM(args, index);
        PyTuple_SET_ITEM(safe_args, index, Py_NewRef(argument));
    }
    if (safe_kwargs != NULL &&
        PyDict_GetItemString(safe_kwargs, "iterable") != NULL &&
        PyDict_SetItemString(safe_kwargs, "iterable", empty) < 0) {
        Py_DECREF(empty);
        Py_DECREF(safe_args);
        Py_DECREF(safe_kwargs);
        return -1;
    }
    PyObject *result = original_itertools_new[kind](
        type, safe_args, safe_kwargs
    );
    Py_DECREF(empty);
    Py_DECREF(safe_args);
    Py_XDECREF(safe_kwargs);
    if (result == NULL) {
        return -1;
    }
    Py_DECREF(result);
    PyErr_SetString(PyExc_RuntimeError, "itertools argument validation mismatch");
    return -1;
}

static PyObject *
adapter_itertools_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    int kind = -1;
    int is_exact_type = 0;
    for (int index = 0; index < 20; index++) {
        if (itertools_new_types[index] == type) {
            kind = index;
            is_exact_type = 1;
            break;
        }
    }
    if (kind < 0) {
        for (int index = 0; index < 20; index++) {
            if (itertools_new_types[index] != NULL &&
                (type->tp_base == itertools_new_types[index] ||
                 PyType_IsSubtype(type, itertools_new_types[index]))) {
                kind = index;
                break;
            }
        }
    }
    if (kind < 0 || kind >= 20 || original_itertools_new[kind] == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "unknown itertools constructor type");
        return NULL;
    }
    if (!is_exact_type) {
        return original_itertools_new[kind](type, args, kwargs);
    }
    if (kind == ITERTOOLS_GROUPBY) {
        return adapter_groupby_new(type, args, kwargs);
    }
    if (kind == ITERTOOLS_ISLICE) {
        return original_itertools_new[kind](type, args, kwargs);
    }
    if (kind == ITERTOOLS_STARMAP) {
        return original_itertools_new[kind](type, args, kwargs);
    }
    if (kind == ITERTOOLS_PAIRWISE) {
        PairwiseConstructorState state = {.type = type};
        AleffAdapterFrame frame;
        if (adapter_enter(
                &frame, &pairwise_constructor_vtable, &state) < 0) {
            return NULL;
        }
        PyObject *result = original_itertools_new[kind](type, args, kwargs);
        adapter_leave(&frame);
        return result;
    }
    if (it_constructor_uses_eager_pool((ItIteratorKind)kind) &&
        it_constructor_validate_shape(
            type, (ItIteratorKind)kind, args, kwargs
        ) < 0) {
        return NULL;
    }
    if (kind == ITERTOOLS_PRODUCT && kwargs != NULL) {
        PyObject *repeat = PyDict_GetItemString(kwargs, "repeat");
        if (
            repeat != NULL &&
            (PyLong_CheckExact(repeat) || PyBool_Check(repeat))
        ) {
            int is_zero = PyObject_RichCompareBool(repeat, Py_False, Py_EQ);
            if (is_zero < 0) {
                return NULL;
            }
            if (is_zero) {
                return original_itertools_new[kind](type, args, kwargs);
            }
        }
    }
    PyObject *converted = PyList_New(0);
    if (converted == NULL) {
        return NULL;
    }
    ItConstructorState state = {
        .type = type,
        .args = args,
        .kwargs = kwargs,
        .converted = converted,
        .index = 0,
        .kind = kind,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &it_constructor_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = it_constructor_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_DECREF(converted);
    return result;
}

/*
 * A number of itertools next methods do more work after calling Python.  In
 * particular, a predicate result is not the item yielded by dropwhile or
 * takewhile, and zip_longest still has to assemble the result tuple.  The
 * small runtime state below keeps those operations out of the generic map
 * adapter.  The original itertools object remains the public object; its
 * state is shadowed only while the adapter is installed.
 */
typedef enum {
    IT_RUNTIME_SOURCE,
    IT_RUNTIME_CALLBACK,
    IT_RUNTIME_COMPARE,
    IT_RUNTIME_TRUTH,
    IT_RUNTIME_ZIP_ITEM,
    IT_RUNTIME_COUNT_ADD,
} ItRuntimePhase;

typedef struct ItRuntimeState ItRuntimeState;

struct ItRuntimeState {
    PyObject *owner;
    ItIteratorKind kind;
    PyObject *source;
    PyObject *function;
    PyObject *sources;
    PyObject *fillvalue;
    PyObject *item;
    PyObject *items;
    PyObject *cache;
    PyObject *key;
    PyObject *pending_key;
    PyObject *pending_item;
    PyObject *done_sources;
    PyObject *count_current;
    PyObject *count_step;
    PyObject *count_pending;
    PyObject *grouper;
    Py_ssize_t index;
    Py_ssize_t source_count;
    Py_ssize_t limit;
    Py_ssize_t step;
    Py_ssize_t position;
    Py_ssize_t source_position;
    int started;
    int exhausted;
    int group_active;
    int count_fast;
    int unbounded;
    Py_ssize_t count_value;
    ItRuntimePhase phase;
};

static int it_runtime_is_predicate(ItIteratorKind kind);
static const AleffAdapterVTable it_runtime_vtable;
static PyObject *it_runtime_clone_sources(PyObject *sources);

static void
it_runtime_free(ItRuntimeState *state)
{
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->owner);
    Py_XDECREF(state->source);
    Py_XDECREF(state->function);
    Py_XDECREF(state->sources);
    Py_XDECREF(state->fillvalue);
    Py_XDECREF(state->item);
    Py_XDECREF(state->items);
    Py_XDECREF(state->cache);
    Py_XDECREF(state->key);
    Py_XDECREF(state->pending_key);
    Py_XDECREF(state->pending_item);
    Py_XDECREF(state->done_sources);
    Py_XDECREF(state->count_current);
    Py_XDECREF(state->count_step);
    Py_XDECREF(state->count_pending);
    Py_XDECREF(state->grouper);
    PyMem_Free(state);
}

static ItRuntimeState *
it_runtime_copy(const ItRuntimeState *source)
{
    ItRuntimeState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->owner = Py_XNewRef(source->owner);
    copy->kind = source->kind;
    copy->source = Py_XNewRef(source->source);
    copy->function = Py_XNewRef(source->function);
    copy->sources = source->sources == NULL
        ? NULL
        : it_runtime_clone_sources(source->sources);
    copy->fillvalue = Py_XNewRef(source->fillvalue);
    copy->item = Py_XNewRef(source->item);
    copy->items = source->items == NULL
        ? NULL
        : PyList_GetSlice(source->items, 0, PyList_GET_SIZE(source->items));
    copy->cache = source->cache == NULL
        ? NULL
        : PyList_GetSlice(source->cache, 0, PyList_GET_SIZE(source->cache));
    copy->key = Py_XNewRef(source->key);
    copy->pending_key = Py_XNewRef(source->pending_key);
    copy->pending_item = Py_XNewRef(source->pending_item);
    copy->done_sources = source->done_sources == NULL
        ? NULL
        : PyList_GetSlice(
            source->done_sources,
            0,
            PyList_GET_SIZE(source->done_sources)
        );
    copy->count_current = Py_XNewRef(source->count_current);
    copy->count_step = Py_XNewRef(source->count_step);
    copy->count_pending = Py_XNewRef(source->count_pending);
    copy->grouper = Py_XNewRef(source->grouper);
    copy->index = source->index;
    copy->source_count = source->source_count;
    copy->limit = source->limit;
    copy->step = source->step;
    copy->position = source->position;
    copy->source_position = source->source_position;
    copy->started = source->started;
    copy->exhausted = source->exhausted;
    copy->group_active = source->group_active;
    copy->count_fast = source->count_fast;
    copy->unbounded = source->unbounded;
    copy->count_value = source->count_value;
    copy->phase = source->phase;
    if ((source->owner != NULL && copy->owner == NULL) ||
        (source->source != NULL && copy->source == NULL) ||
        (source->sources != NULL && copy->sources == NULL) ||
        (source->items != NULL && copy->items == NULL) ||
        (source->cache != NULL && copy->cache == NULL) ||
        (source->done_sources != NULL && copy->done_sources == NULL)) {
        it_runtime_free(copy);
        return NULL;
    }
    return copy;
}

typedef struct {
    PyObject_HEAD
    PyObject *iterator;
    Py_ssize_t next;
    Py_ssize_t stop;
    Py_ssize_t step;
    Py_ssize_t count;
} AleffNativeIsliceObject;

typedef struct {
    PyObject_HEAD
    PyObject *function;
    PyObject *iterator;
} AleffNativeStarmapObject;

typedef struct {
    PyObject_HEAD
    PyObject *iterator;
    PyObject *old;
#if PY_VERSION_HEX >= 0x030d0000
    PyObject *result;
#endif
} AleffNativePairwiseObject;

typedef struct {
    PyObject_HEAD
    PyObject *iterator;
    PyObject *saved;
    Py_ssize_t index;
    int firstpass;
} AleffNativeCycleObject;

typedef struct {
    PyObject_HEAD
    PyObject *function;
    PyObject *iterator;
    long flag;
} AleffNativePredicateObject;

typedef struct {
    PyObject_HEAD
    PyObject *iterator;
    PyObject *keyfunc;
    PyObject *target_key;
    PyObject *current_key;
    PyObject *current_value;
    const void *current_grouper;
    void *module_state;
} AleffNativeGroupbyObject;

typedef struct {
    PyObject_HEAD
    PyObject *parent;
    PyObject *target_key;
} AleffNativeGrouperObject;

typedef struct {
    PyObject_HEAD
    Py_ssize_t count;
    PyObject *long_count;
    PyObject *long_step;
} AleffNativeCountObject;

typedef struct {
    PyObject_HEAD
    Py_ssize_t tuple_size;
    Py_ssize_t active_count;
    PyObject *iterators;
    PyObject *result;
    PyObject *fillvalue;
} AleffNativeZipLongestObject;

static PyObject *
it_runtime_clone_nullable_tuple(PyObject *tuple)
{
    Py_ssize_t size = PyTuple_GET_SIZE(tuple);
    PyObject *copy = PyTuple_New(size);
    if (copy == NULL) {
        return NULL;
    }
    for (Py_ssize_t index = 0; index < size; index++) {
        PyObject *item = PyTuple_GET_ITEM(tuple, index);
        if (item != NULL) {
            PyTuple_SET_ITEM(copy, index, Py_NewRef(item));
        }
    }
    return copy;
}

static int
it_runtime_is_builtin_position_iterator(PyObject *iterator)
{
    const char *name = Py_TYPE(iterator)->tp_name;
    return strcmp(name, "tuple_iterator") == 0 ||
        strcmp(name, "list_iterator") == 0 ||
        strcmp(name, "range_iterator") == 0 ||
        strcmp(name, "longrange_iterator") == 0;
}

static PyObject *
it_runtime_clone_position_iterator(PyObject *iterator)
{
    PyObject *reduced = PyObject_CallMethod(iterator, "__reduce__", NULL);
    if (reduced == NULL) {
        return NULL;
    }
    if (!PyTuple_Check(reduced) || PyTuple_GET_SIZE(reduced) < 2) {
        Py_DECREF(reduced);
        return Py_NewRef(iterator);
    }
    PyObject *constructor = PyTuple_GET_ITEM(reduced, 0);
    PyObject *args = PyTuple_GET_ITEM(reduced, 1);
    PyObject *copy = PyObject_Call(constructor, args, NULL);
    if (copy != NULL && PyTuple_GET_SIZE(reduced) >= 3) {
        PyObject *result = PyObject_CallMethod(
            copy, "__setstate__", "O", PyTuple_GET_ITEM(reduced, 2)
        );
        if (result == NULL) {
            Py_CLEAR(copy);
        }
        else {
            Py_DECREF(result);
        }
    }
    Py_DECREF(reduced);
    return copy;
}

static PyObject *
it_runtime_clone_sources(PyObject *sources)
{
    Py_ssize_t count = PyTuple_GET_SIZE(sources);
    PyObject *copy = PyTuple_New(count);
    if (copy == NULL) {
        return NULL;
    }
    for (Py_ssize_t index = 0; index < count; index++) {
        PyObject *iterator = PyTuple_GET_ITEM(sources, index);
        if (iterator == NULL) {
            continue;
        }
        PyObject *item = it_runtime_is_builtin_position_iterator(iterator)
            ? it_runtime_clone_position_iterator(iterator)
            : Py_NewRef(iterator);
        if (item == NULL) {
            Py_DECREF(copy);
            return NULL;
        }
        PyTuple_SET_ITEM(copy, index, item);
    }
    return copy;
}

static ItRuntimeState *
it_runtime_from_native(PyObject *object, ItIteratorKind kind)
{
    ItRuntimeState *state = PyMem_Calloc(1, sizeof(*state));
    if (state == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    state->owner = Py_NewRef(object);
    state->kind = kind;
    state->phase = IT_RUNTIME_SOURCE;
    switch (kind) {
        case ITERTOOLS_COUNT: {
            AleffNativeCountObject *native = (AleffNativeCountObject *)object;
#ifdef Py_GIL_DISABLED
            state->count_value = _Py_atomic_load_ssize_relaxed(&native->count);
#else
            state->count_value = native->count;
#endif
            state->count_fast = state->count_value != PY_SSIZE_T_MAX;
            state->count_current = Py_XNewRef(native->long_count);
            state->count_step = Py_NewRef(native->long_step);
            break;
        }
        case ITERTOOLS_CYCLE: {
            AleffNativeCycleObject *native = (AleffNativeCycleObject *)object;
            state->source = Py_XNewRef(native->iterator);
            state->cache = PyList_GetSlice(
                native->saved, 0, PyList_GET_SIZE(native->saved)
            );
            state->index = native->index;
            state->started = native->firstpass;
            state->exhausted = native->iterator == NULL;
            break;
        }
        case ITERTOOLS_DROPWHILE:
        case ITERTOOLS_FILTERFALSE:
        case ITERTOOLS_TAKEWHILE: {
            AleffNativePredicateObject *native =
                (AleffNativePredicateObject *)object;
            state->function = Py_NewRef(native->function);
            state->source = Py_NewRef(native->iterator);
            state->started = kind == ITERTOOLS_FILTERFALSE
                ? 1
                : native->flag != 0;
            state->exhausted =
                kind == ITERTOOLS_TAKEWHILE && native->flag != 0;
            state->items = PyList_New(0);
            break;
        }
        case ITERTOOLS_GROUPBY: {
            AleffNativeGroupbyObject *native =
                (AleffNativeGroupbyObject *)object;
            state->source = Py_NewRef(native->iterator);
            state->function = Py_NewRef(native->keyfunc);
            state->key = Py_XNewRef(native->target_key);
            state->pending_key = Py_XNewRef(native->current_key);
            state->item = Py_XNewRef(native->current_value);
            state->started = native->current_key != NULL;
            break;
        }
        case ITERTOOLS_ISLICE: {
            AleffNativeIsliceObject *native =
                (AleffNativeIsliceObject *)object;
            state->source = Py_XNewRef(native->iterator);
            state->index = native->next;
            state->limit = native->stop;
            state->unbounded = native->stop < 0;
            state->step = native->step;
            state->position = native->count;
            state->exhausted = native->iterator == NULL;
            break;
        }
        case ITERTOOLS_PAIRWISE: {
            AleffNativePairwiseObject *native =
                (AleffNativePairwiseObject *)object;
            state->source = Py_XNewRef(native->iterator);
            state->item = Py_XNewRef(native->old);
            state->exhausted = native->iterator == NULL;
            break;
        }
        case ITERTOOLS_STARMAP: {
            AleffNativeStarmapObject *native =
                (AleffNativeStarmapObject *)object;
            state->function = Py_NewRef(native->function);
            state->source = Py_NewRef(native->iterator);
            break;
        }
        case ITERTOOLS_ZIP_LONGEST: {
            AleffNativeZipLongestObject *native =
                (AleffNativeZipLongestObject *)object;
            state->sources = it_runtime_clone_nullable_tuple(native->iterators);
            state->source_count = native->tuple_size;
            state->fillvalue = Py_NewRef(native->fillvalue);
            state->items = PyList_New(0);
            state->done_sources = PyList_New(native->tuple_size);
            state->exhausted = native->active_count == 0;
            if (state->sources != NULL && state->done_sources != NULL) {
                for (Py_ssize_t index = 0; index < native->tuple_size; index++) {
                    PyObject *iterator = PyTuple_GET_ITEM(
                        native->iterators, index
                    );
                    PyObject *done = iterator == NULL ? Py_True : Py_False;
                    PyList_SET_ITEM(
                        state->done_sources, index, Py_NewRef(done)
                    );
                }
            }
            break;
        }
        default:
            PyErr_SetString(
                PyExc_RuntimeError,
                "unsupported native itertools state"
            );
            it_runtime_free(state);
            return NULL;
    }
    if ((kind == ITERTOOLS_CYCLE && state->cache == NULL) ||
        (it_runtime_is_predicate(kind) && state->items == NULL) ||
        (kind == ITERTOOLS_ZIP_LONGEST &&
            (state->sources == NULL || state->items == NULL ||
             state->done_sources == NULL))) {
        it_runtime_free(state);
        return NULL;
    }
    return state;
}

static int
it_runtime_commit_native(const ItRuntimeState *state)
{
    switch (state->kind) {
        case ITERTOOLS_COUNT: {
            AleffNativeCountObject *native =
                (AleffNativeCountObject *)state->owner;
#ifdef Py_GIL_DISABLED
            Py_BEGIN_CRITICAL_SECTION(native);
#endif
            Py_ssize_t count = state->count_fast
                ? state->count_value
                : PY_SSIZE_T_MAX;
#ifdef Py_GIL_DISABLED
            _Py_atomic_store_ssize_relaxed(&native->count, count);
#else
            native->count = count;
#endif
            Py_XSETREF(native->long_count, Py_XNewRef(state->count_current));
#ifdef Py_GIL_DISABLED
            Py_END_CRITICAL_SECTION();
#endif
            return 0;
        }
        case ITERTOOLS_CYCLE: {
            AleffNativeCycleObject *native =
                (AleffNativeCycleObject *)state->owner;
            Py_XSETREF(native->iterator, Py_XNewRef(state->source));
            Py_SETREF(native->saved, Py_NewRef(state->cache));
            native->index = state->index;
            native->firstpass = state->started;
            return 0;
        }
        case ITERTOOLS_DROPWHILE:
        case ITERTOOLS_FILTERFALSE:
        case ITERTOOLS_TAKEWHILE: {
            AleffNativePredicateObject *native =
                (AleffNativePredicateObject *)state->owner;
            Py_SETREF(native->iterator, Py_NewRef(state->source));
            if (state->kind != ITERTOOLS_FILTERFALSE) {
                native->flag = state->kind == ITERTOOLS_TAKEWHILE
                    ? state->exhausted
                    : state->started;
            }
            return 0;
        }
        case ITERTOOLS_GROUPBY: {
            AleffNativeGroupbyObject *native =
                (AleffNativeGroupbyObject *)state->owner;
            Py_XSETREF(native->target_key, Py_XNewRef(state->key));
            Py_XSETREF(native->current_key, Py_XNewRef(state->pending_key));
            Py_XSETREF(native->current_value, Py_XNewRef(state->item));
            return 0;
        }
        case ITERTOOLS_ISLICE: {
            AleffNativeIsliceObject *native =
                (AleffNativeIsliceObject *)state->owner;
            Py_XSETREF(native->iterator, Py_XNewRef(state->source));
            native->next = state->index;
            native->stop = state->limit;
            native->step = state->step;
            native->count = state->position;
            return 0;
        }
        case ITERTOOLS_PAIRWISE: {
            AleffNativePairwiseObject *native =
                (AleffNativePairwiseObject *)state->owner;
            Py_XSETREF(native->iterator, Py_XNewRef(state->source));
            Py_XSETREF(native->old, Py_XNewRef(state->item));
            return 0;
        }
        case ITERTOOLS_STARMAP:
            return 0;
        case ITERTOOLS_ZIP_LONGEST: {
            AleffNativeZipLongestObject *native =
                (AleffNativeZipLongestObject *)state->owner;
            PyObject *iterators = PyTuple_New(state->source_count);
            if (iterators == NULL) {
                return -1;
            }
            Py_ssize_t active = 0;
            for (Py_ssize_t index = 0; index < state->source_count; index++) {
                int done = PyObject_IsTrue(
                    PyList_GET_ITEM(state->done_sources, index)
                );
                if (done < 0) {
                    Py_DECREF(iterators);
                    return -1;
                }
                if (!done) {
                    PyObject *iterator = PyTuple_GET_ITEM(
                        state->sources, index
                    );
                    PyTuple_SET_ITEM(iterators, index, Py_NewRef(iterator));
                    active++;
                }
            }
            Py_SETREF(native->iterators, iterators);
            native->active_count = state->exhausted ? 0 : active;
            return 0;
        }
        default:
            return 0;
    }
}

static int
it_runtime_is_predicate(ItIteratorKind kind)
{
    return kind == ITERTOOLS_DROPWHILE ||
        kind == ITERTOOLS_FILTERFALSE ||
        kind == ITERTOOLS_TAKEWHILE;
}

static PyTypeObject *native_grouper_type = NULL;
static iternextfunc original_grouper_next = NULL;

static int
it_runtime_groupby_store_step(ItRuntimeState *state, PyObject *new_key)
{
    Py_XSETREF(state->item, state->pending_item);
    state->pending_item = NULL;
    Py_XSETREF(state->pending_key, Py_NewRef(new_key));
    state->started = 1;
    return 0;
}

static int
it_runtime_groupby_step(
    ItRuntimeState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    PyObject *item = NULL;
    if (is_resumed && state->phase == IT_RUNTIME_SOURCE) {
        if (resumed_value == NULL) {
            return -1;
        }
        item = Py_NewRef(resumed_value);
    }
    else {
        state->phase = IT_RUNTIME_SOURCE;
        item = Py_TYPE(state->source)->tp_iternext(state->source);
        if (item == NULL) {
            return -1;
        }
    }
    Py_XSETREF(state->pending_item, item);
    if (state->function == Py_None) {
        return it_runtime_groupby_store_step(state, item);
    }
    state->phase = IT_RUNTIME_CALLBACK;
    PyObject *key = PyObject_CallOneArg(state->function, item);
    if (key == NULL) {
        return -1;
    }
    int result = it_runtime_groupby_store_step(state, key);
    Py_DECREF(key);
    return result;
}

static PyObject *
it_runtime_make_native_grouper(ItRuntimeState *state)
{
    if (native_grouper_type == NULL || state->pending_key == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "native itertools._grouper unavailable");
        return NULL;
    }
    Py_XSETREF(state->key, Py_NewRef(state->pending_key));
    PyObject *grouper = PyObject_CallFunctionObjArgs(
        (PyObject *)native_grouper_type,
        state->owner,
        state->key,
        NULL
    );
    if (grouper == NULL) {
        return NULL;
    }
    PyObject *result = PyTuple_Pack(2, state->pending_key, grouper);
    Py_DECREF(grouper);
    return result;
}

static PyObject *
it_runtime_groupby_next(
    ItRuntimeState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    int resumed = is_resumed;
    PyObject *value = resumed_value;
    for (;;) {
        if (resumed && state->phase == IT_RUNTIME_CALLBACK) {
            if (value == NULL || it_runtime_groupby_store_step(state, value) < 0) {
                return NULL;
            }
            resumed = 0;
        }
        else if (resumed && state->phase == IT_RUNTIME_SOURCE) {
            if (it_runtime_groupby_step(state, value, 1) < 0) {
                return NULL;
            }
            resumed = 0;
        }
        else if (resumed && state->phase == IT_RUNTIME_COMPARE) {
            int same = PyObject_IsTrue(value);
            if (same < 0) {
                return NULL;
            }
            resumed = 0;
            if (!same) {
                return it_runtime_make_native_grouper(state);
            }
            if (it_runtime_groupby_step(state, NULL, 0) < 0) {
                return NULL;
            }
        }

        if (state->pending_key == NULL) {
            if (it_runtime_groupby_step(state, NULL, 0) < 0) {
                return NULL;
            }
            continue;
        }
        if (state->key == NULL) {
            return it_runtime_make_native_grouper(state);
        }
        state->phase = IT_RUNTIME_COMPARE;
        int same = PyObject_RichCompareBool(
            state->key, state->pending_key, Py_EQ
        );
        if (same < 0) {
            return NULL;
        }
        if (!same) {
            return it_runtime_make_native_grouper(state);
        }
        if (it_runtime_groupby_step(state, NULL, 0) < 0) {
            return NULL;
        }
    }
}

static PyObject *
it_runtime_grouper_next(
    ItRuntimeState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    AleffNativeGroupbyObject *parent =
        (AleffNativeGroupbyObject *)state->owner;
    if (parent->current_grouper != state->grouper) {
        return NULL;
    }
    int resumed = is_resumed;
    if (resumed && state->phase == IT_RUNTIME_CALLBACK) {
        if (resumed_value == NULL ||
            it_runtime_groupby_store_step(state, resumed_value) < 0) {
            return NULL;
        }
        resumed = 0;
    }
    else if (resumed && state->phase == IT_RUNTIME_SOURCE) {
        if (it_runtime_groupby_step(state, resumed_value, 1) < 0) {
            return NULL;
        }
        resumed = 0;
    }
    if (state->item == NULL) {
        if (it_runtime_groupby_step(state, NULL, 0) < 0) {
            return NULL;
        }
    }
    int same;
    if (resumed && state->phase == IT_RUNTIME_COMPARE) {
        same = PyObject_IsTrue(resumed_value);
    }
    else {
        state->phase = IT_RUNTIME_COMPARE;
        same = PyObject_RichCompareBool(
            state->key, state->pending_key, Py_EQ
        );
    }
    if (same <= 0) {
        return NULL;
    }
    PyObject *item = state->item;
    state->item = NULL;
    Py_CLEAR(state->pending_key);
    return item;
}

static PyObject *
adapter_groupby_grouper_next(PyObject *object)
{
    AleffNativeGrouperObject *grouper =
        (AleffNativeGrouperObject *)object;
    ItRuntimeState *state = it_runtime_from_native(
        grouper->parent, ITERTOOLS_GROUPBY
    );
    if (state == NULL) {
        return NULL;
    }
    state->group_active = 1;
    state->grouper = Py_NewRef(object);
    Py_XSETREF(state->key, Py_NewRef(grouper->target_key));
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &it_runtime_vtable, state) < 0) {
        it_runtime_free(state);
        return NULL;
    }
    PyObject *result = it_runtime_grouper_next(state, NULL, 0);
    if (it_runtime_commit_native(state) < 0) {
        Py_XDECREF(result);
        result = NULL;
    }
    adapter_leave(&frame);
    it_runtime_free(state);
    return result;
}

static PyObject *
it_runtime_pop_callback_item(ItRuntimeState *state)
{
    Py_ssize_t count = PyList_GET_SIZE(state->items);
    if (count == 0) {
        PyErr_SetString(PyExc_RuntimeError, "missing itertools callback item");
        return NULL;
    }
    PyObject *item = Py_NewRef(PyList_GET_ITEM(state->items, count - 1));
    if (PySequence_DelItem(state->items, count - 1) < 0) {
        Py_DECREF(item);
        return NULL;
    }
    return item;
}

static PyObject *
it_runtime_continue(ItRuntimeState *state, PyObject *resumed_value, int is_resumed)
{
    if (state->kind == ITERTOOLS_COUNT) {
        if (is_resumed && state->phase == IT_RUNTIME_COUNT_ADD) {
            Py_SETREF(state->count_current, Py_NewRef(resumed_value));
            PyObject *result = state->count_pending;
            state->count_pending = NULL;
            return result;
        }
        if (state->count_fast) {
            if (state->count_value < PY_SSIZE_T_MAX) {
                return PyLong_FromSsize_t(state->count_value++);
            }
            state->count_fast = 0;
            state->count_current = PyLong_FromSsize_t(PY_SSIZE_T_MAX);
            if (state->count_current == NULL) {
                return NULL;
            }
        }
        if (state->count_current == NULL) {
            state->count_current = PyLong_FromSsize_t(PY_SSIZE_T_MAX);
            if (state->count_current == NULL) {
                return NULL;
            }
        }
        PyObject *result = Py_NewRef(state->count_current);
        state->count_pending = result;
        state->phase = IT_RUNTIME_COUNT_ADD;
        PyObject *next = PyNumber_Add(state->count_current, state->count_step);
        if (next == NULL) {
            Py_CLEAR(state->count_pending);
            return NULL;
        }
        Py_SETREF(state->count_current, next);
        result = state->count_pending;
        state->count_pending = NULL;
        return result;
    }
    if (state->kind == ITERTOOLS_ISLICE) {
        if (state->exhausted) {
            return NULL;
        }
        if (is_resumed && state->phase == IT_RUNTIME_SOURCE) {
            if (resumed_value == NULL) {
                if (PyErr_Occurred() &&
                    !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                    return NULL;
                }
                PyErr_Clear();
                state->exhausted = 1;
                return NULL;
            }
            state->position++;
            if (state->position > state->index) {
                if (state->index > PY_SSIZE_T_MAX - state->step) {
                    state->index = state->unbounded
                        ? PY_SSIZE_T_MAX
                        : state->limit;
                }
                else {
                    state->index += state->step;
                    if (!state->unbounded && state->index > state->limit) {
                        state->index = state->limit;
                    }
                }
                return Py_NewRef(resumed_value);
            }
        }
        for (;;) {
            if (!state->unbounded && state->position >= state->limit) {
                state->exhausted = 1;
                return NULL;
            }
            int yielding = state->position >= state->index;
            state->phase = IT_RUNTIME_SOURCE;
            PyObject *item = Py_TYPE(state->source)->tp_iternext(state->source);
            if (item == NULL) {
                if (PyErr_Occurred() &&
                    !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                    return NULL;
                }
                PyErr_Clear();
                state->exhausted = 1;
                return NULL;
            }
            state->position++;
            if (!yielding) {
                Py_DECREF(item);
                continue;
            }
            if (state->index > PY_SSIZE_T_MAX - state->step) {
                state->index = state->unbounded
                    ? PY_SSIZE_T_MAX
                    : state->limit;
            }
            else {
                state->index += state->step;
                if (!state->unbounded && state->index > state->limit) {
                    state->index = state->limit;
                }
            }
            return item;
        }
    }
    if (state->kind == ITERTOOLS_GROUPBY) {
        return it_runtime_groupby_next(state, resumed_value, is_resumed);
    }
    if (state->kind == ITERTOOLS_PAIRWISE) {
        PyObject *next_item = NULL;
        if (is_resumed) {
            if (resumed_value == NULL) {
                if (PyErr_Occurred() &&
                    !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                    return NULL;
                }
                PyErr_Clear();
                return NULL;
            }
            next_item = Py_NewRef(resumed_value);
        }
        for (;;) {
            if (next_item == NULL) {
                state->phase = IT_RUNTIME_SOURCE;
                next_item = Py_TYPE(state->source)->tp_iternext(state->source);
                if (next_item == NULL) {
                    return NULL;
                }
            }
            if (state->item == NULL) {
                state->item = next_item;
                next_item = NULL;
                continue;
            }
            PyObject *result = PyTuple_Pack(2, state->item, next_item);
            if (result == NULL) {
                Py_DECREF(next_item);
                return NULL;
            }
            Py_SETREF(state->item, next_item);
            return result;
        }
    }
    if (state->kind == ITERTOOLS_STARMAP) {
        if (is_resumed && state->phase == IT_RUNTIME_CALLBACK) {
            return resumed_value == NULL ? NULL : Py_NewRef(resumed_value);
        }
        PyObject *item;
        if (is_resumed) {
            if (resumed_value == NULL) {
                if (PyErr_Occurred() &&
                    !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                    return NULL;
                }
                PyErr_Clear();
                return NULL;
            }
            item = Py_NewRef(resumed_value);
        }
        else {
            state->phase = IT_RUNTIME_SOURCE;
            item = Py_TYPE(state->source)->tp_iternext(state->source);
            if (item == NULL) {
                return NULL;
            }
        }
        PyObject *arguments = PySequence_Tuple(item);
        Py_DECREF(item);
        if (arguments == NULL) {
            return NULL;
        }
        state->phase = IT_RUNTIME_CALLBACK;
        PyObject *result = PyObject_Call(state->function, arguments, NULL);
        Py_DECREF(arguments);
        return result;
    }
    if (it_runtime_is_predicate(state->kind)) {
        if (state->exhausted) {
            return NULL;
        }
        if (is_resumed) {
            if (state->phase == IT_RUNTIME_SOURCE) {
                if (resumed_value == NULL) {
                    if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                        return NULL;
                    }
                    PyErr_Clear();
                    return NULL;
                }
                Py_XSETREF(state->item, Py_NewRef(resumed_value));
                state->phase = IT_RUNTIME_CALLBACK;
            }
            else {
                PyObject *item = it_runtime_pop_callback_item(state);
                if (item == NULL) {
                    return NULL;
                }
                int truth = PyObject_IsTrue(resumed_value);
                if (truth < 0) {
                    Py_DECREF(item);
                    return NULL;
                }
                if (state->kind == ITERTOOLS_TAKEWHILE && !truth) {
                    state->exhausted = 1;
                    Py_DECREF(item);
                    return NULL;
                }
                if (state->kind == ITERTOOLS_TAKEWHILE && truth) {
                    return item;
                }
                if (state->kind == ITERTOOLS_DROPWHILE && truth && !state->started) {
                    Py_DECREF(item);
                    state->phase = IT_RUNTIME_SOURCE;
                }
                else if (state->kind == ITERTOOLS_FILTERFALSE ? !truth : !state->started || !truth) {
                    state->started = 1;
                    return item;
                }
                else {
                    Py_DECREF(item);
                    state->phase = IT_RUNTIME_SOURCE;
                }
            }
        }
        for (;;) {
            if (state->item == NULL) {
                state->phase = IT_RUNTIME_SOURCE;
                state->item = Py_TYPE(state->source)->tp_iternext(state->source);
        if (state->item == NULL) {
                    state->exhausted = 1;
                    return NULL;
                }
                state->phase = IT_RUNTIME_CALLBACK;
            }
            if (state->kind == ITERTOOLS_DROPWHILE && state->started) {
                PyObject *item = state->item;
                state->item = NULL;
                return item;
            }
            PyObject *item = state->item;
            state->item = NULL;
            if (PyList_Append(state->items, item) < 0) {
                Py_DECREF(item);
                return NULL;
            }
            PyObject *predicate =
                state->kind == ITERTOOLS_FILTERFALSE && state->function == Py_None
                ? Py_NewRef(item)
                : PyObject_CallOneArg(state->function, item);
            Py_DECREF(item);
            if (predicate == NULL) {
                PyObject *discarded = it_runtime_pop_callback_item(state);
                Py_XDECREF(discarded);
                return NULL;
            }
            int truth = PyObject_IsTrue(predicate);
            Py_DECREF(predicate);
            item = it_runtime_pop_callback_item(state);
            if (item == NULL) {
                return NULL;
            }
            if (truth < 0) {
                Py_DECREF(item);
                return NULL;
            }
            if (state->kind == ITERTOOLS_TAKEWHILE && !truth) {
                state->exhausted = 1;
                Py_DECREF(item);
                return NULL;
            }
            if (state->kind == ITERTOOLS_TAKEWHILE && truth) {
                return item;
            }
            if (state->kind == ITERTOOLS_DROPWHILE && truth && !state->started) {
                Py_DECREF(item);
                continue;
            }
            if (state->kind == ITERTOOLS_FILTERFALSE ? !truth : !state->started || !truth) {
                state->started = 1;
                return item;
            }
            Py_DECREF(item);
        }
    }

    if (state->kind == ITERTOOLS_CYCLE) {
        if (is_resumed) {
            if (state->phase == IT_RUNTIME_SOURCE) {
                if (resumed_value == NULL) {
                    state->exhausted = 1;
                }
                else if (state->started) {
                    return Py_NewRef(resumed_value);
                }
                else if (PyList_Append(state->cache, resumed_value) < 0) {
                    return NULL;
                }
                else {
                    state->index = PyList_GET_SIZE(state->cache);
                    return Py_NewRef(resumed_value);
                }
            }
        }
        for (;;) {
            if (!state->exhausted) {
                state->phase = IT_RUNTIME_SOURCE;
                PyObject *item = Py_TYPE(state->source)->tp_iternext(state->source);
                if (item != NULL) {
                    if (state->started) {
                        return item;
                    }
                    if (PyList_Append(state->cache, item) < 0) {
                        Py_DECREF(item);
                        return NULL;
                    }
                    Py_DECREF(item);
                    state->index = PyList_GET_SIZE(state->cache);
                    return Py_NewRef(PyList_GET_ITEM(state->cache, state->index - 1));
                }
                if (PyErr_Occurred()) {
                    if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                        return NULL;
                    }
                    PyErr_Clear();
                }
                state->exhausted = 1;
            }
            if (PyList_GET_SIZE(state->cache) == 0) {
                return NULL;
            }
            if (state->index >= PyList_GET_SIZE(state->cache)) {
                state->index = 0;
            }
            return Py_NewRef(PyList_GET_ITEM(state->cache, state->index++));
        }
    }

    if (state->kind == ITERTOOLS_ZIP_LONGEST) {
        if (state->source_count == 0) {
            return NULL;
        }
        if (state->exhausted) {
            return NULL;
        }
        int all_done = 1;
        for (Py_ssize_t index = 0; index < state->source_count; index++) {
            if (!PyObject_IsTrue(PyList_GET_ITEM(state->done_sources, index))) {
                all_done = 0;
                break;
            }
        }
        if (all_done) {
            state->exhausted = 1;
            return NULL;
        }
        int got_item = 0;
        if (is_resumed && state->phase == IT_RUNTIME_ZIP_ITEM) {
            if (resumed_value == NULL) {
                PyErr_Clear();
                PyList_Append(state->items, state->fillvalue);
            }
            else if (PyList_Append(state->items, resumed_value) < 0) {
                return NULL;
            }
            else {
                got_item = 1;
            }
            state->index++;
        }
        while (state->index < state->source_count) {
            PyObject *item;
            state->phase = IT_RUNTIME_ZIP_ITEM;
            if (PyObject_IsTrue(PyList_GET_ITEM(state->done_sources, state->index))) {
                item = NULL;
            }
            else {
                item = Py_TYPE(PyTuple_GET_ITEM(state->sources, state->index))->tp_iternext(
                    PyTuple_GET_ITEM(state->sources, state->index)
                );
            }
            if (item == NULL) {
                if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                    return NULL;
                }
                PyErr_Clear();
                if (!PyObject_IsTrue(PyList_GET_ITEM(state->done_sources, state->index))) {
                    Py_INCREF(Py_True);
                    PyList_SET_ITEM(state->done_sources, state->index, Py_True);
                }
                if (PyList_Append(state->items, state->fillvalue) < 0) {
                    return NULL;
                }
            }
            else {
                got_item = 1;
                if (PyList_Append(state->items, item) < 0) {
                    Py_DECREF(item);
                    return NULL;
                }
                Py_DECREF(item);
            }
            state->index++;
        }
        all_done = 1;
        for (Py_ssize_t index = 0; index < state->source_count; index++) {
            if (!PyObject_IsTrue(PyList_GET_ITEM(state->done_sources, index))) {
                all_done = 0;
                break;
            }
        }
        if (all_done && !got_item) {
            Py_CLEAR(state->items);
            state->items = PyList_New(0);
            state->index = 0;
            state->exhausted = 1;
            if (state->items == NULL) {
                return NULL;
            }
            return NULL;
        }
        if (PyList_GET_SIZE(state->items) == state->source_count) {
            PyObject *result = PyList_AsTuple(state->items);
            Py_CLEAR(state->items);
            state->items = PyList_New(0);
            state->index = 0;
            state->exhausted = 1;
            for (Py_ssize_t index = 0; index < state->source_count; index++) {
                if (!PyObject_IsTrue(PyList_GET_ITEM(state->done_sources, index))) {
                    state->exhausted = 0;
                    break;
                }
            }
            return result;
        }
    }
    return NULL;
}

static PyObject *it_runtime_resume(const void *raw_state, PyObject *value);

static int
it_runtime_prepare_resume(void *raw_state)
{
    ItRuntimeState *state = raw_state;
    if (state->kind != ITERTOOLS_ZIP_LONGEST) {
        return it_runtime_commit_native(state);
    }
    PyObject *sources = it_runtime_clone_sources(state->sources);
    if (sources == NULL) {
        return -1;
    }
    PyObject *saved_sources = state->sources;
    state->sources = sources;
    int result = it_runtime_commit_native(state);
    state->sources = saved_sources;
    Py_DECREF(sources);
    return result;
}

static const AleffAdapterVTable it_runtime_vtable = {
    .copy_state = (void *(*)(const void *))it_runtime_copy,
    .free_state = (void (*)(void *))it_runtime_free,
    .resume = it_runtime_resume,
    .prepare_resume = it_runtime_prepare_resume,
};

static PyObject *
it_runtime_resume(const void *raw_state, PyObject *value)
{
    ItRuntimeState *state = it_runtime_copy(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &it_runtime_vtable, state) < 0) {
        it_runtime_free(state);
        return NULL;
    }
    PyObject *result = state->group_active
        ? it_runtime_grouper_next(state, value, 1)
        : it_runtime_continue(state, value, 1);
    if (it_runtime_commit_native(state) < 0) {
        Py_XDECREF(result);
        result = NULL;
    }
    adapter_leave(&frame);
    it_runtime_free(state);
    return result;
}

static int
adapter_native_itertools_next(PyObject *object, PyObject **result)
{
    static const int kinds[] = {
        -1,
        -1,
        -1,
        ITERTOOLS_COUNT,
        ITERTOOLS_CYCLE,
        ITERTOOLS_DROPWHILE,
        ITERTOOLS_FILTERFALSE,
        ITERTOOLS_GROUPBY,
        ITERTOOLS_ISLICE,
        ITERTOOLS_PAIRWISE,
        -1,
        -1,
        -1,
        ITERTOOLS_STARMAP,
        ITERTOOLS_TAKEWHILE,
        -1,
        ITERTOOLS_ZIP_LONGEST,
    };
    int kind = -1;
    for (int index = 0; index < 17; index++) {
        if (itertools_next_types[index] != NULL &&
            Py_TYPE(object) == itertools_next_types[index]) {
            kind = kinds[index];
            break;
        }
    }
    if (kind < 0) {
        return 0;
    }
    if (kind == ITERTOOLS_GROUPBY) {
        ((AleffNativeGroupbyObject *)object)->current_grouper = NULL;
    }
    ItRuntimeState *state = it_runtime_from_native(
        object, (ItIteratorKind)kind
    );
    if (state == NULL) {
        return -1;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &it_runtime_vtable, state) < 0) {
        it_runtime_free(state);
        return -1;
    }
    *result = it_runtime_continue(state, NULL, 0);
    if (it_runtime_commit_native(state) < 0) {
        Py_XDECREF(*result);
        *result = NULL;
    }
    adapter_leave(&frame);
    it_runtime_free(state);
    return 1;
}

/* Called by adapters_bootstrap.c after the module and the existing special
 * adapters have been initialized.  Keeping registration here avoids exposing
 * CPython layout details to the bootstrap translation unit. */
int
adapter_itertools_install(PyObject *itertools)
{
    if (PyType_Ready(&ItIteratorHolderType) < 0) {
        return -1;
    }
    static const char *next_names[] = {
        "compress", "combinations", "combinations_with_replacement", "count",
        "cycle", "dropwhile", "filterfalse", "groupby", "islice", "pairwise",
        "permutations", "product", "repeat", "starmap", "takewhile", "tee",
        "zip_longest",
    };
    for (Py_ssize_t index = 0; index < (Py_ssize_t)(sizeof(next_names) / sizeof(next_names[0])); index++) {
        PyObject *object = PyObject_GetAttrString(itertools, next_names[index]);
        if (object == NULL) {
            Py_XDECREF(object);
            PyErr_Format(PyExc_RuntimeError, "cannot access itertools.%s type", next_names[index]);
            return -1;
        }
        PyTypeObject *type;
        if (PyType_Check(object)) {
            type = (PyTypeObject *)object;
        }
        else if (strcmp(next_names[index], "tee") == 0) {
            PyObject *empty = PyTuple_New(0);
            PyObject *sample_args = empty == NULL ? NULL : PyTuple_Pack(1, empty);
            Py_XDECREF(empty);
            PyObject *sample = sample_args == NULL
                ? NULL
                : PyObject_CallObject(object, sample_args);
            Py_XDECREF(sample_args);
            if (sample == NULL) {
                Py_DECREF(object);
                return -1;
            }
            if (!PyTuple_Check(sample) || PyTuple_GET_SIZE(sample) == 0) {
                Py_DECREF(sample);
                Py_DECREF(object);
                PyErr_SetString(PyExc_RuntimeError, "itertools.tee did not return iterators");
                return -1;
            }
            type = Py_TYPE(PyTuple_GET_ITEM(sample, 0));
            Py_DECREF(sample);
        }
        else {
            Py_DECREF(object);
            PyErr_Format(PyExc_RuntimeError, "unsupported itertools.%s type", next_names[index]);
            return -1;
        }
        itertools_next_types[index] = type;
        original_itertools_next[index] = type->tp_iternext;
        type->tp_iternext = adapter_itertools_next;
        PyType_Modified(type);
        Py_DECREF(object);
    }
    PyObject *grouper = PyObject_GetAttrString(itertools, "_grouper");
    if (grouper == NULL || !PyType_Check(grouper)) {
        Py_XDECREF(grouper);
        PyErr_SetString(
            PyExc_RuntimeError,
            "cannot access native itertools._grouper type"
        );
        return -1;
    }
    native_grouper_type = (PyTypeObject *)grouper;
    original_grouper_next = native_grouper_type->tp_iternext;
    native_grouper_type->tp_iternext = adapter_groupby_grouper_next;
    PyType_Modified(native_grouper_type);
    Py_DECREF(grouper);

    static const char *new_names[] = {
        "accumulate", "batched", "chain", "combinations",
        "combinations_with_replacement", "compress", "cycle", "dropwhile",
        "filterfalse", "groupby", "islice", "pairwise", "permutations", "product",
        "starmap", "takewhile", "tee", "zip_longest", "count", "repeat",
    };
    for (Py_ssize_t index = 0; index < (Py_ssize_t)(sizeof(new_names) / sizeof(new_names[0])); index++) {
        PyObject *object = PyObject_GetAttrString(itertools, new_names[index]);
        if (object == NULL || (!PyType_Check(object) && index != ITERTOOLS_TEE)) {
            Py_XDECREF(object);
            PyErr_Format(PyExc_RuntimeError, "cannot access itertools.%s type", new_names[index]);
            return -1;
        }
        if (index == ITERTOOLS_TEE) {
            Py_DECREF(object);
            continue;
        }
        PyTypeObject *type = (PyTypeObject *)object;
        if (
            index == ITERTOOLS_BATCHED &&
            type->tp_basicsize < (Py_ssize_t)sizeof(AleffBatchedObject)
        ) {
            Py_DECREF(object);
            PyErr_SetString(
                PyExc_RuntimeError,
                "unsupported itertools.batched layout"
            );
            return -1;
        }
        itertools_new_types[index] = type;
        if (index == ITERTOOLS_ACCUMULATE) {
            original_accumulate_new = type->tp_new;
            type->tp_new = adapter_accumulate_new;
            PyType_Modified(type);
        }
        else if (index == ITERTOOLS_BATCHED) {
            original_batched_next = type->tp_iternext;
            type->tp_iternext = adapter_batched_next;
            PyType_Modified(type);
        }
        else if (index >= ITERTOOLS_COMBINATIONS && index != ITERTOOLS_TEE) {
            original_itertools_new[index] = type->tp_new;
            type->tp_new = index == ITERTOOLS_REPEAT
                ? adapter_repeat_new
                : adapter_itertools_new;
            PyType_Modified(type);
        }
        Py_DECREF(object);
    }
    return 0;
}

void
adapter_itertools_rollback(void)
{
    if (original_accumulate_type != NULL && original_accumulate_new != NULL) {
        original_accumulate_type->tp_new = original_accumulate_new;
        PyType_Modified(original_accumulate_type);
    }
    if (original_batched_type != NULL && original_batched_new != NULL) {
        original_batched_type->tp_new = original_batched_new;
        PyType_Modified(original_batched_type);
    }
    if (original_batched_type != NULL && original_batched_next != NULL) {
        original_batched_type->tp_iternext = original_batched_next;
        PyType_Modified(original_batched_type);
    }
    if (original_accumulate_next != NULL) {
        if (original_accumulate_type != NULL) {
            original_accumulate_type->tp_iternext = original_accumulate_next;
            PyType_Modified(original_accumulate_type);
        }
    }
    original_accumulate_next = NULL;
    original_accumulate_type = NULL;
    for (Py_ssize_t index = 0; index < 17; index++) {
        PyTypeObject *type = itertools_next_types[index];
        if (type != NULL) {
            type->tp_iternext = original_itertools_next[index];
            PyType_Modified(type);
        }
        itertools_next_types[index] = NULL;
        original_itertools_next[index] = NULL;
    }
    if (native_grouper_type != NULL && original_grouper_next != NULL) {
        native_grouper_type->tp_iternext = original_grouper_next;
        PyType_Modified(native_grouper_type);
    }
    native_grouper_type = NULL;
    original_grouper_next = NULL;
    for (Py_ssize_t index = 0; index < 20; index++) {
        PyTypeObject *type = itertools_new_types[index];
        if (type != NULL) {
            if (original_itertools_new[index] != NULL) {
                type->tp_new = original_itertools_new[index];
                PyType_Modified(type);
            }
        }
        itertools_new_types[index] = NULL;
        original_itertools_new[index] = NULL;
    }
    original_batched_new = NULL;
    original_batched_next = NULL;
    original_batched_type = NULL;
    original_accumulate_new = NULL;
}
