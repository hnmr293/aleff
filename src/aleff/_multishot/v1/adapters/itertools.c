typedef struct {
    PyObject_HEAD
    PyObject *source;
    PyObject *active;
} AleffChainObject;

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

static PyObject *
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

static PyObject *
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
} BatchedConstructorState;

static newfunc original_batched_new;
static PyTypeObject *original_batched_type = NULL;
static iternextfunc original_accumulate_next = NULL;
static PyTypeObject *original_accumulate_type = NULL;

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
    if (adapter_enter(&frame, &batched_constructor_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = original_batched_new(type, args, kwargs);
    adapter_leave(&frame);
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
static destructor original_itertools_dealloc[17] = {NULL};

typedef struct ItRuntimeState ItRuntimeState;

static int adapter_itertools_runtime_next(PyObject *object, PyObject **result);

static PyObject *
adapter_itertools_next(PyObject *object)
{
    PyObject *runtime_result = NULL;
    int runtime_handled = adapter_itertools_runtime_next(object, &runtime_result);
    if (runtime_handled) {
        return runtime_result;
    }
    PyTypeObject *type = Py_TYPE(object);
    for (int index = 0; index < 17; index++) {
        if (itertools_next_types[index] == type && original_itertools_next[index] != NULL) {
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

static int it_runtime_register_from_constructor(
    PyObject *object,
    ItIteratorKind kind,
    PyObject *converted,
    PyObject *kwargs,
    PyObject *args
);

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

static PyObject *
it_constructor_continue(ItConstructorState *state, PyObject *resumed_value, int is_resumed)
{
    Py_ssize_t count = PyTuple_GET_SIZE(state->args);
    if (is_resumed) {
        if (!PyIter_Check(resumed_value)) {
            PyErr_Format(
                PyExc_TypeError,
                "iter() returned non-iterator of type '%.200s'",
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
        PyObject *value = it_constructor_is_iterable_arg(
            state->kind,
            state->index,
            count
        ) ? PyObject_GetIter(argument) : Py_NewRef(argument);
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
    PyObject *result = original_itertools_new[state->kind](state->type, converted, state->kwargs);
    if (result != NULL && it_runtime_register_from_constructor(
            result,
            state->kind,
            converted,
            state->kwargs,
            state->args
        ) < 0) {
        Py_DECREF(result);
        result = NULL;
    }
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

static PyObject *
adapter_itertools_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    ItIteratorKind kind = -1;
    for (int index = 0; index < 20; index++) {
        if (itertools_new_types[index] == type) {
            kind = (ItIteratorKind)index;
            break;
        }
    }
    if (kind < 0 || kind >= 20 || original_itertools_new[kind] == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "unknown itertools constructor type");
        return NULL;
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
    IT_RUNTIME_ZIP_ITEM,
    IT_RUNTIME_COUNT_ADD,
} ItRuntimePhase;

struct ItRuntimeState {
    PyObject *owner;
    ItIteratorKind kind;
    PyObject *source;
    PyObject *function;
    PyObject *sources;
    PyObject *source_templates;
    PyObject *source_positions;
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
    Py_ssize_t index;
    Py_ssize_t source_count;
    Py_ssize_t limit;
    Py_ssize_t position;
    Py_ssize_t source_position;
    int started;
    int exhausted;
    int group_active;
    int count_fast;
    Py_ssize_t count_value;
    ItRuntimePhase phase;
};

static PyObject *itertools_runtime_registry = NULL;

static int it_runtime_is_predicate(ItIteratorKind kind);

static void
it_runtime_free(ItRuntimeState *state)
{
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->source);
    Py_XDECREF(state->function);
    Py_XDECREF(state->sources);
    Py_XDECREF(state->source_templates);
    Py_XDECREF(state->source_positions);
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
    PyMem_Free(state);
}

static void
it_runtime_capsule_destructor(PyObject *capsule)
{
    it_runtime_free(PyCapsule_GetPointer(capsule, "aleff.itertools.state"));
}

static int
it_runtime_replayable_template(PyObject *object)
{
    return PyTuple_CheckExact(object) ||
        PyList_CheckExact(object) ||
        PyRange_Check(object);
}

static PyObject *
it_runtime_clone_sources(const ItRuntimeState *state)
{
    if (state->sources == NULL || state->source_templates == NULL ||
        state->source_positions == NULL) {
        return Py_XNewRef(state->sources);
    }
    Py_ssize_t count = PyTuple_GET_SIZE(state->sources);
    PyObject *sources = PyTuple_New(count);
    if (sources == NULL) {
        return NULL;
    }
    for (Py_ssize_t index = 0; index < count; index++) {
        PyObject *template = PyTuple_GET_ITEM(state->source_templates, index);
        PyObject *iterator = NULL;
        if (template != Py_None) {
            iterator = PyObject_GetIter(template);
            if (iterator == NULL) {
                Py_DECREF(sources);
                return NULL;
            }
            Py_ssize_t position = PyLong_AsSsize_t(
                PyList_GET_ITEM(state->source_positions, index)
            );
            if (position == -1 && PyErr_Occurred()) {
                Py_DECREF(iterator);
                Py_DECREF(sources);
                return NULL;
            }
            for (Py_ssize_t skipped = 0; skipped < position; skipped++) {
                PyObject *item = PyIter_Next(iterator);
                if (item == NULL) {
                    if (PyErr_Occurred()) {
                        Py_DECREF(iterator);
                        Py_DECREF(sources);
                        return NULL;
                    }
                    break;
                }
                Py_DECREF(item);
            }
        }
        else {
            iterator = Py_NewRef(PyTuple_GET_ITEM(state->sources, index));
        }
        PyTuple_SET_ITEM(sources, index, iterator);
    }
    return sources;
}

static PyObject *
it_runtime_clone_source(const ItRuntimeState *state)
{
    if (state->source == NULL || state->source_templates == NULL ||
        state->source_positions == NULL ||
        PyTuple_GET_ITEM(state->source_templates, 0) == Py_None) {
        return Py_XNewRef(state->source);
    }
    PyObject *iterator = PyObject_GetIter(PyTuple_GET_ITEM(state->source_templates, 0));
    if (iterator == NULL) {
        return NULL;
    }
    Py_ssize_t position = PyLong_AsSsize_t(
        PyList_GET_ITEM(state->source_positions, 0)
    );
    if (position == -1 && PyErr_Occurred()) {
        Py_DECREF(iterator);
        return NULL;
    }
    for (Py_ssize_t skipped = 0; skipped < position; skipped++) {
        PyObject *item = PyIter_Next(iterator);
        if (item == NULL) {
            if (PyErr_Occurred()) {
                Py_DECREF(iterator);
                return NULL;
            }
            break;
        }
        Py_DECREF(item);
    }
    return iterator;
}

static ItRuntimeState *
it_runtime_copy(const ItRuntimeState *source)
{
    ItRuntimeState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->owner = source->owner;
    copy->kind = source->kind;
    copy->source = it_runtime_clone_source(source);
    copy->function = Py_XNewRef(source->function);
    copy->sources = it_runtime_clone_sources(source);
    copy->source_templates = Py_XNewRef(source->source_templates);
    copy->source_positions = source->source_positions == NULL
        ? NULL
        : PyList_GetSlice(source->source_positions, 0, PyList_GET_SIZE(source->source_positions));
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
        : PyList_GetSlice(source->done_sources, 0, PyList_GET_SIZE(source->done_sources));
    copy->count_current = Py_XNewRef(source->count_current);
    copy->count_step = Py_XNewRef(source->count_step);
    copy->count_pending = Py_XNewRef(source->count_pending);
    copy->index = source->index;
    copy->source_count = source->source_count;
    copy->started = source->started;
    copy->exhausted = source->exhausted;
    copy->group_active = source->group_active;
    copy->count_fast = source->count_fast;
    copy->count_value = source->count_value;
    copy->phase = source->phase;
    copy->limit = source->limit;
    copy->position = source->position;
    copy->source_position = source->source_position;
    if ((source->source != NULL && copy->source == NULL) ||
        (source->items != NULL && copy->items == NULL) ||
        (source->cache != NULL && copy->cache == NULL) ||
        (source->sources != NULL && copy->sources == NULL) ||
        (source->done_sources != NULL && copy->done_sources == NULL) ||
        (source->count_current != NULL && copy->count_current == NULL) ||
        (source->count_step != NULL && copy->count_step == NULL) ||
        (source->count_pending != NULL && copy->count_pending == NULL)) {
        it_runtime_free(copy);
        return NULL;
    }
    if (source->source_positions != NULL && copy->source_positions == NULL) {
        it_runtime_free(copy);
        return NULL;
    }
    return copy;
}

static int
it_runtime_assign(ItRuntimeState *target, const ItRuntimeState *source)
{
    PyObject *items = source->items == NULL
        ? NULL
        : PyList_GetSlice(source->items, 0, PyList_GET_SIZE(source->items));
    PyObject *cache = source->cache == NULL
        ? NULL
        : PyList_GetSlice(source->cache, 0, PyList_GET_SIZE(source->cache));
    PyObject *done_sources = source->done_sources == NULL
        ? NULL
        : PyList_GetSlice(source->done_sources, 0, PyList_GET_SIZE(source->done_sources));
    PyObject *source_positions = source->source_positions == NULL
        ? NULL
        : PyList_GetSlice(source->source_positions, 0, PyList_GET_SIZE(source->source_positions));
    if ((source->items != NULL && items == NULL) ||
        (source->cache != NULL && cache == NULL) ||
        (source->done_sources != NULL && done_sources == NULL) ||
        (source->source_positions != NULL && source_positions == NULL)) {
        Py_XDECREF(items);
        Py_XDECREF(cache);
        Py_XDECREF(done_sources);
        Py_XDECREF(source_positions);
        return -1;
    }
    target->owner = source->owner;
    target->kind = source->kind;
    Py_XSETREF(target->source, Py_XNewRef(source->source));
    Py_XSETREF(target->function, Py_XNewRef(source->function));
    Py_XSETREF(target->sources, Py_XNewRef(source->sources));
    Py_XSETREF(target->source_templates, Py_XNewRef(source->source_templates));
    Py_XSETREF(target->source_positions, source_positions);
    Py_XSETREF(target->fillvalue, Py_XNewRef(source->fillvalue));
    Py_XSETREF(target->item, Py_XNewRef(source->item));
    Py_XSETREF(target->items, items);
    Py_XSETREF(target->cache, cache);
    Py_XSETREF(target->key, Py_XNewRef(source->key));
    Py_XSETREF(target->pending_key, Py_XNewRef(source->pending_key));
    Py_XSETREF(target->pending_item, Py_XNewRef(source->pending_item));
    Py_XSETREF(target->done_sources, done_sources);
    Py_XSETREF(target->count_current, Py_XNewRef(source->count_current));
    Py_XSETREF(target->count_step, Py_XNewRef(source->count_step));
    Py_XSETREF(target->count_pending, Py_XNewRef(source->count_pending));
    target->index = source->index;
    target->source_count = source->source_count;
    target->started = source->started;
    target->exhausted = source->exhausted;
    target->group_active = source->group_active;
    target->count_fast = source->count_fast;
    target->count_value = source->count_value;
    target->phase = source->phase;
    target->limit = source->limit;
    target->position = source->position;
    target->source_position = source->source_position;
    return 0;
}

static int
it_runtime_register(PyObject *object, ItRuntimeState *state)
{
    if (itertools_runtime_registry == NULL) {
        itertools_runtime_registry = PyDict_New();
        if (itertools_runtime_registry == NULL) {
            return -1;
        }
    }
    PyObject *key = PyLong_FromVoidPtr(object);
    if (key == NULL) {
        return -1;
    }
    PyObject *capsule = PyCapsule_New(state, "aleff.itertools.state", it_runtime_capsule_destructor);
    if (capsule == NULL) {
        Py_DECREF(key);
        return -1;
    }
    if (PyDict_SetItem(itertools_runtime_registry, key, capsule) < 0) {
        Py_DECREF(key);
        Py_DECREF(capsule);
        return -1;
    }
    Py_DECREF(key);
    Py_DECREF(capsule);
    return 0;
}

static int
it_runtime_register_from_constructor(
    PyObject *object,
    ItIteratorKind kind,
    PyObject *converted,
    PyObject *kwargs,
    PyObject *args
)
{
    if (kind != ITERTOOLS_CYCLE &&
        kind != ITERTOOLS_ISLICE &&
        !it_runtime_is_predicate(kind) &&
        kind != ITERTOOLS_GROUPBY &&
        kind != ITERTOOLS_ZIP_LONGEST &&
        kind != ITERTOOLS_COUNT) {
        return 0;
    }
    ItRuntimeState *state = PyMem_Calloc(1, sizeof(*state));
    if (state == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    state->owner = object;
    state->kind = kind;
    state->phase = IT_RUNTIME_SOURCE;
    state->started = kind == ITERTOOLS_FILTERFALSE;
    if (kind == ITERTOOLS_COUNT) {
        PyObject *start = PyTuple_GET_SIZE(args) > 0
            ? PyTuple_GET_ITEM(args, 0)
            : (kwargs == NULL ? NULL : PyDict_GetItemString(kwargs, "start"));
        PyObject *step = PyTuple_GET_SIZE(args) > 1
            ? PyTuple_GET_ITEM(args, 1)
            : (kwargs == NULL ? NULL : PyDict_GetItemString(kwargs, "step"));
        if (start == NULL) {
            start = PyLong_FromLong(0);
        }
        else {
            Py_INCREF(start);
        }
        if (step == NULL) {
            step = PyLong_FromLong(1);
        }
        else {
            Py_INCREF(step);
        }
        if (start == NULL || step == NULL) {
            Py_XDECREF(start);
            Py_XDECREF(step);
            it_runtime_free(state);
            return -1;
        }
        state->count_step = step;
        state->count_fast = PyLong_Check(start) && PyLong_Check(step);
        if (state->count_fast) {
            state->count_value = PyLong_AsSsize_t(start);
            if (state->count_value == -1 && PyErr_Occurred()) {
                if (!PyErr_ExceptionMatches(PyExc_OverflowError)) {
                    Py_DECREF(start);
                    it_runtime_free(state);
                    return -1;
                }
                PyErr_Clear();
                state->count_fast = 0;
            }
        }
        if (state->count_fast) {
            long step_value = PyLong_AsLong(step);
            if (step_value == -1 && PyErr_Occurred()) {
                PyErr_Clear();
                state->count_fast = 0;
            }
            else if (step_value != 1) {
                state->count_fast = 0;
            }
        }
        if (state->count_fast) {
            Py_DECREF(start);
        }
        else {
            state->count_current = start;
        }
    }
    else if (kind == ITERTOOLS_ISLICE) {
        state->source = Py_NewRef(PyTuple_GET_ITEM(converted, 0));
        state->limit = PyLong_AsSsize_t(PyTuple_GET_ITEM(converted, 1));
        if (state->limit == -1 && PyErr_Occurred()) {
            it_runtime_free(state);
            return -1;
        }
    }
    else if (kind == ITERTOOLS_ZIP_LONGEST) {
        state->sources = Py_NewRef(converted);
        state->source_count = PyTuple_GET_SIZE(converted);
        state->source_templates = PyTuple_New(state->source_count);
        state->source_positions = PyList_New(state->source_count);
        state->items = PyList_New(0);
        state->done_sources = PyList_New(state->source_count);
        if (state->source_templates != NULL && state->source_positions != NULL) {
            for (Py_ssize_t index = 0; index < state->source_count; index++) {
                PyObject *argument = PyTuple_GET_ITEM(args, index);
                PyObject *template = it_runtime_replayable_template(argument)
                    ? Py_NewRef(argument)
                    : Py_NewRef(Py_None);
                if (template == NULL) {
                    it_runtime_free(state);
                    return -1;
                }
                PyTuple_SET_ITEM(state->source_templates, index, template);
                PyObject *zero = PyLong_FromLong(0);
                if (zero == NULL) {
                    it_runtime_free(state);
                    return -1;
                }
                PyList_SET_ITEM(state->source_positions, index, zero);
            }
        }
        if (state->done_sources != NULL) {
            for (Py_ssize_t index = 0; index < state->source_count; index++) {
                Py_INCREF(Py_False);
                PyList_SET_ITEM(state->done_sources, index, Py_False);
            }
        }
        PyObject *fillvalue = kwargs == NULL
            ? NULL
            : PyDict_GetItemString(kwargs, "fillvalue");
        state->fillvalue = Py_XNewRef(fillvalue == NULL ? Py_None : fillvalue);
    }
    else {
        if (kind == ITERTOOLS_CYCLE) {
            state->source = Py_NewRef(PyTuple_GET_ITEM(converted, 0));
            state->cache = PyList_New(0);
        }
        else if (kind == ITERTOOLS_GROUPBY) {
            state->source = Py_NewRef(PyTuple_GET_ITEM(converted, 0));
            state->source_templates = PyTuple_New(1);
            state->source_positions = PyList_New(1);
            if (state->source_templates == NULL || state->source_positions == NULL) {
                it_runtime_free(state);
                return -1;
            }
            PyObject *argument = PyTuple_GET_ITEM(args, 0);
            PyTuple_SET_ITEM(
                state->source_templates,
                0,
                it_runtime_replayable_template(argument)
                    ? Py_NewRef(argument)
                    : Py_NewRef(Py_None)
            );
            PyObject *zero = PyLong_FromLong(0);
            if (zero == NULL) {
                it_runtime_free(state);
                return -1;
            }
            PyList_SET_ITEM(state->source_positions, 0, zero);
            PyObject *key = PyTuple_GET_SIZE(converted) > 1
                ? PyTuple_GET_ITEM(converted, 1)
                : (kwargs == NULL ? NULL : PyDict_GetItemString(kwargs, "key"));
            state->function = Py_NewRef(key == NULL ? Py_None : key);
        }
        else {
            state->function = Py_NewRef(PyTuple_GET_ITEM(converted, 0));
            state->source = Py_NewRef(PyTuple_GET_ITEM(converted, 1));
        }
    }
    if ((kind == ITERTOOLS_ZIP_LONGEST &&
            (state->items == NULL || state->done_sources == NULL ||
             state->source_templates == NULL || state->source_positions == NULL)) ||
        (kind == ITERTOOLS_CYCLE && state->cache == NULL)) {
        it_runtime_free(state);
        return -1;
    }
    if (it_runtime_register(object, state) < 0) {
        it_runtime_free(state);
        return -1;
    }
    return 0;
}

static ItRuntimeState *
it_runtime_lookup(PyObject *object)
{
    if (itertools_runtime_registry == NULL) {
        return NULL;
    }
    PyObject *key = PyLong_FromVoidPtr(object);
    if (key == NULL) {
        PyErr_Clear();
        return NULL;
    }
    PyObject *capsule = PyDict_GetItemWithError(itertools_runtime_registry, key);
    Py_DECREF(key);
    if (capsule == NULL) {
        return NULL;
    }
    return PyCapsule_GetPointer(capsule, "aleff.itertools.state");
}

static void
it_runtime_unregister(PyObject *object)
{
    if (itertools_runtime_registry != NULL) {
        PyObject *key = PyLong_FromVoidPtr(object);
        if (key == NULL) {
            PyErr_Clear();
            return;
        }
        if (PyDict_DelItem(itertools_runtime_registry, key) < 0) {
            PyErr_Clear();
        }
        Py_DECREF(key);
    }
}

static void
adapter_itertools_dealloc(PyObject *object)
{
    PyTypeObject *type = Py_TYPE(object);
    it_runtime_unregister(object);
    for (int index = 0; index < 17; index++) {
        if (itertools_next_types[index] == type && original_itertools_dealloc[index] != NULL) {
            original_itertools_dealloc[index](object);
            return;
        }
    }
    type->tp_free(object);
}

static int
it_runtime_is_predicate(ItIteratorKind kind)
{
    return kind == ITERTOOLS_DROPWHILE ||
        kind == ITERTOOLS_FILTERFALSE ||
        kind == ITERTOOLS_TAKEWHILE;
}

typedef struct {
    PyObject_HEAD
    PyObject *owner;
    PyObject *key;
    int done;
} AleffGroupbyGrouper;

static PyTypeObject AleffGroupbyGrouper_Type;

static PyObject *
it_runtime_make_grouper(ItRuntimeState *state, PyObject *key)
{
    AleffGroupbyGrouper *grouper = PyObject_New(
        AleffGroupbyGrouper,
        &AleffGroupbyGrouper_Type
    );
    if (grouper == NULL) {
        return NULL;
    }
    grouper->owner = Py_NewRef(state->owner);
    grouper->key = Py_NewRef(key);
    grouper->done = 0;
    Py_XSETREF(state->key, Py_NewRef(key));
    state->started = 1;
    state->group_active = 0;
    return (PyObject *)grouper;
}

static PyObject *
it_runtime_groupby_next(ItRuntimeState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed && state->phase == IT_RUNTIME_CALLBACK) {
        if (state->group_active) {
            int same = PyObject_RichCompareBool(resumed_value, state->key, Py_EQ);
            if (same < 0) {
                return NULL;
            }
            if (same) {
                PyObject *item = state->item;
                state->item = NULL;
                return item;
            }
            state->pending_item = state->item;
            state->pending_key = Py_NewRef(resumed_value);
            state->item = NULL;
            return NULL;
        }
        PyObject *key = Py_NewRef(resumed_value);
        PyObject *grouper = it_runtime_make_grouper(state, key);
        Py_DECREF(key);
        return grouper == NULL ? NULL : PyTuple_Pack(2, state->key, grouper);
    }
    if (state->pending_item != NULL) {
        PyObject *item = state->pending_item;
        PyObject *key = state->pending_key;
        state->pending_item = NULL;
        state->pending_key = NULL;
        PyObject *grouper = it_runtime_make_grouper(state, key);
        if (grouper == NULL) {
            Py_DECREF(item);
            Py_XDECREF(key);
            return NULL;
        }
        state->item = item;
        Py_DECREF(key);
        return PyTuple_Pack(2, state->key, grouper);
    }
    state->phase = IT_RUNTIME_SOURCE;
    state->item = Py_TYPE(state->source)->tp_iternext(state->source);
    state->source_position++;
    if (state->item == NULL) {
        state->exhausted = 1;
        return NULL;
    }
    state->phase = IT_RUNTIME_CALLBACK;
    PyObject *key = state->function == Py_None
        ? Py_NewRef(state->item)
        : PyObject_CallOneArg(state->function, state->item);
    if (key == NULL) {
        return NULL;
    }
    PyObject *grouper = it_runtime_make_grouper(state, key);
    Py_DECREF(key);
    if (grouper == NULL) {
        return NULL;
    }
    PyObject *result = PyTuple_Pack(2, state->key, grouper);
    Py_DECREF(grouper);
    return result;
}

static PyObject *
adapter_groupby_grouper_next(PyObject *object)
{
    AleffGroupbyGrouper *grouper = (AleffGroupbyGrouper *)object;
    if (grouper->done) {
        return NULL;
    }
    ItRuntimeState *state = it_runtime_lookup(grouper->owner);
    if (state == NULL) {
        grouper->done = 1;
        return NULL;
    }
    if (state->item != NULL) {
        PyObject *item = state->item;
        state->item = NULL;
        return item;
    }
    for (;;) {
        state->phase = IT_RUNTIME_SOURCE;
        PyObject *item = Py_TYPE(state->source)->tp_iternext(state->source);
        state->source_position++;
        if (item == NULL) {
            grouper->done = 1;
            return NULL;
        }
        state->item = item;
        state->group_active = 1;
        state->phase = IT_RUNTIME_CALLBACK;
        PyObject *key = state->function == Py_None
            ? Py_NewRef(item)
            : PyObject_CallOneArg(state->function, item);
        if (key == NULL) {
            return NULL;
        }
        int same = PyObject_RichCompareBool(key, grouper->key, Py_EQ);
        if (same < 0) {
            Py_DECREF(key);
            return NULL;
        }
        if (same) {
            Py_DECREF(key);
            state->item = NULL;
            return item;
        }
        state->pending_item = item;
        state->pending_key = key;
        state->item = NULL;
        grouper->done = 1;
        return NULL;
    }
}

static void
adapter_groupby_grouper_dealloc(PyObject *object)
{
    AleffGroupbyGrouper *grouper = (AleffGroupbyGrouper *)object;
    Py_XDECREF(grouper->owner);
    Py_XDECREF(grouper->key);
    Py_TYPE(object)->tp_free(object);
}

static PyTypeObject AleffGroupbyGrouper_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "aleff._itertools_grouper",
    .tp_basicsize = sizeof(AleffGroupbyGrouper),
    .tp_dealloc = adapter_groupby_grouper_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = adapter_groupby_grouper_next,
    .tp_free = PyObject_Del,
};

typedef struct {
    PyObject_HEAD
    PyObject *source;
    PyObject *values;
    PyObject *indexes;
    int done;
} AleffTeeData;

typedef struct {
    PyObject_HEAD
    AleffTeeData *data;
    Py_ssize_t index;
    Py_ssize_t slot;
} AleffTeeIterator;

static PyTypeObject AleffTeeData_Type;
static PyTypeObject AleffTeeIterator_Type;

static void
adapter_tee_data_dealloc(PyObject *object)
{
    AleffTeeData *data = (AleffTeeData *)object;
    Py_XDECREF(data->source);
    Py_XDECREF(data->values);
    Py_XDECREF(data->indexes);
    Py_TYPE(object)->tp_free(object);
}

static void
adapter_tee_iterator_dealloc(PyObject *object)
{
    AleffTeeIterator *iterator = (AleffTeeIterator *)object;
    Py_XDECREF(iterator->data);
    Py_TYPE(object)->tp_free(object);
}

static PyObject *tee_next_resume(const void *raw_state, PyObject *value);

typedef struct {
    AleffTeeIterator *owner;
    AleffTeeData *data;
    PyObject *values;
    PyObject *source;
    PyObject *indexes;
    Py_ssize_t index;
    Py_ssize_t slot;
    int done;
    int phase;
} TeeNextState;

static void *
tee_next_copy(const void *raw_state)
{
    const TeeNextState *source = raw_state;
    TeeNextState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->owner = (AleffTeeIterator *)Py_NewRef((PyObject *)source->owner);
    copy->data = (AleffTeeData *)Py_NewRef((PyObject *)source->data);
    copy->values = PyList_GetSlice(source->values, 0, PyList_GET_SIZE(source->values));
    copy->source = Py_NewRef(source->source);
    copy->indexes = PyList_GetSlice(source->indexes, 0, PyList_GET_SIZE(source->indexes));
    copy->index = source->index;
    copy->slot = source->slot;
    copy->done = source->done;
    copy->phase = source->phase;
    if (copy->values == NULL || copy->indexes == NULL) {
        Py_DECREF(copy->owner);
        Py_DECREF(copy->data);
        Py_DECREF(copy->source);
        Py_XDECREF(copy->values);
        Py_XDECREF(copy->indexes);
        PyMem_Free(copy);
        return NULL;
    }
    return copy;
}

static void
tee_next_free(void *raw_state)
{
    TeeNextState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->owner);
    Py_DECREF(state->data);
    Py_DECREF(state->values);
    Py_DECREF(state->source);
    Py_DECREF(state->indexes);
    PyMem_Free(state);
}

static int
tee_set_index(TeeNextState *state)
{
    PyObject *value = PyLong_FromSsize_t(state->index);
    if (value == NULL) {
        return -1;
    }
    if (PyList_SetItem(state->indexes, state->slot, value) < 0) {
        Py_DECREF(value);
        return -1;
    }
    return 0;
}

static PyObject *
tee_next_continue(TeeNextState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed && state->phase == IT_RUNTIME_SOURCE) {
        if (resumed_value == NULL) {
            if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                return NULL;
            }
            PyErr_Clear();
            state->done = 1;
            return NULL;
        }
        if (PyList_Append(state->values, resumed_value) < 0) {
            return NULL;
        }
        state->index++;
        if (tee_set_index(state) < 0) {
            return NULL;
        }
        return Py_NewRef(resumed_value);
    }
    if (state->index < PyList_GET_SIZE(state->values)) {
        PyObject *item = Py_NewRef(PyList_GET_ITEM(state->values, state->index++));
        if (tee_set_index(state) < 0) {
            Py_DECREF(item);
            return NULL;
        }
        return item;
    }
    if (state->done) {
        return NULL;
    }
    state->phase = IT_RUNTIME_SOURCE;
    PyObject *item = Py_TYPE(state->source)->tp_iternext(state->source);
    if (item == NULL) {
        if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
            return NULL;
        }
        PyErr_Clear();
        state->done = 1;
        return NULL;
    }
    if (PyList_Append(state->values, item) < 0) {
        Py_DECREF(item);
        return NULL;
    }
    state->index++;
    if (tee_set_index(state) < 0) {
        Py_DECREF(item);
        return NULL;
    }
    return item;
}

static const AleffAdapterVTable tee_next_vtable = {
    .copy_state = tee_next_copy,
    .free_state = tee_next_free,
    .resume = tee_next_resume,
};

static PyObject *
tee_next_resume(const void *raw_state, PyObject *value)
{
    TeeNextState *state = tee_next_copy(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &tee_next_vtable, state) < 0) {
        return NULL;
    }
    PyObject *result = tee_next_continue(state, value, 1);
    Py_XSETREF(state->data->values, Py_NewRef(state->values));
    Py_XSETREF(state->data->indexes, Py_NewRef(state->indexes));
    state->data->done = state->done;
    state->owner->index = state->index;
    adapter_leave(&frame);
    tee_next_free(state);
    return result;
}

static PyObject *
adapter_tee_iterator_next(PyObject *object)
{
    AleffTeeIterator *owner = (AleffTeeIterator *)object;
    TeeNextState state = {
        .owner = owner,
        .data = owner->data,
        .values = Py_NewRef(owner->data->values),
        .source = Py_NewRef(owner->data->source),
        .indexes = Py_NewRef(owner->data->indexes),
        .index = owner->index,
        .slot = owner->slot,
        .done = owner->data->done,
        .phase = IT_RUNTIME_SOURCE,
    };
    PyObject *index = PyList_GET_ITEM(state.indexes, state.slot);
    state.index = PyLong_AsSsize_t(index);
    if (state.index == -1 && PyErr_Occurred()) {
        Py_DECREF(state.values);
        Py_DECREF(state.source);
        Py_DECREF(state.indexes);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &tee_next_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = tee_next_continue(&state, NULL, 0);
    Py_XSETREF(owner->data->values, Py_NewRef(state.values));
    Py_XSETREF(owner->data->indexes, Py_NewRef(state.indexes));
    owner->data->done = state.done;
    owner->index = state.index;
    adapter_leave(&frame);
    Py_DECREF(state.values);
    Py_DECREF(state.source);
    Py_DECREF(state.indexes);
    return result;
}

static PyTypeObject AleffTeeData_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "aleff._tee_data",
    .tp_basicsize = sizeof(AleffTeeData),
    .tp_dealloc = adapter_tee_data_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_free = PyObject_Del,
};

static PyTypeObject AleffTeeIterator_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "aleff._tee",
    .tp_basicsize = sizeof(AleffTeeIterator),
    .tp_dealloc = adapter_tee_iterator_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = adapter_tee_iterator_next,
    .tp_free = PyObject_Del,
};

typedef struct {
    PyObject *iterable;
    Py_ssize_t n;
} TeeConstructorState;

static void *
tee_constructor_copy(const void *raw_state)
{
    const TeeConstructorState *source = raw_state;
    TeeConstructorState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->iterable = Py_NewRef(source->iterable);
    copy->n = source->n;
    return copy;
}

static void
tee_constructor_free(void *raw_state)
{
    TeeConstructorState *state = raw_state;
    if (state != NULL) {
        Py_DECREF(state->iterable);
        PyMem_Free(state);
    }
}

static PyObject *tee_constructor_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable tee_constructor_vtable = {
    .copy_state = tee_constructor_copy,
    .free_state = tee_constructor_free,
    .resume = tee_constructor_resume,
};

static PyObject *
it_runtime_make_tee_pair(PyObject *iterator, Py_ssize_t n)
{
    AleffTeeData *data = PyObject_New(AleffTeeData, &AleffTeeData_Type);
    if (data == NULL) {
        return NULL;
    }
    data->source = Py_NewRef(iterator);
    data->values = PyList_New(0);
    data->indexes = PyList_New(n);
    data->done = 0;
    if (data->values == NULL || data->indexes == NULL) {
        Py_DECREF(data);
        return NULL;
    }
    for (Py_ssize_t index = 0; index < n; index++) {
        PyObject *zero = PyLong_FromLong(0);
        if (zero == NULL) {
            Py_DECREF(data);
            return NULL;
        }
        PyList_SET_ITEM(data->indexes, index, zero);
    }
    PyObject *result = PyTuple_New(n);
    if (result == NULL) {
        Py_DECREF(data);
        return NULL;
    }
    for (Py_ssize_t index = 0; index < n; index++) {
        AleffTeeIterator *tee = PyObject_New(
            AleffTeeIterator,
            &AleffTeeIterator_Type
        );
        if (tee == NULL) {
            Py_DECREF(data);
            Py_DECREF(result);
            return NULL;
        }
        tee->data = (AleffTeeData *)Py_NewRef((PyObject *)data);
        tee->index = 0;
        tee->slot = index;
        PyTuple_SET_ITEM(result, index, (PyObject *)tee);
    }
    Py_DECREF(data);
    return result;
}

static PyObject *
tee_constructor_resume(const void *raw_state, PyObject *value)
{
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
    return it_runtime_make_tee_pair(value, ((const TeeConstructorState *)raw_state)->n);
}

static PyObject *
adapter_itertools_tee(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"iterable", "n", NULL};
    PyObject *iterable;
    PyObject *n_object = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O:tee", keywords, &iterable, &n_object)) {
        return NULL;
    }
    Py_ssize_t n = 2;
    if (n_object != NULL) {
        n = PyNumber_AsSsize_t(n_object, PyExc_OverflowError);
        if (n == -1 && PyErr_Occurred()) {
            return NULL;
        }
        if (n < 0) {
            PyErr_SetString(PyExc_ValueError, "n must be >= 0");
            return NULL;
        }
    }
    TeeConstructorState state = {.iterable = iterable, .n = n};
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &tee_constructor_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *iterator = PyObject_GetIter(iterable);
    PyObject *result = iterator == NULL ? NULL : it_runtime_make_tee_pair(iterator, n);
    Py_XDECREF(iterator);
    adapter_leave(&frame);
    return result;
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
                state->exhausted = 1;
                return NULL;
            }
            state->position++;
            return Py_NewRef(resumed_value);
        }
        if (state->position >= state->limit) {
            state->exhausted = 1;
            return NULL;
        }
        state->phase = IT_RUNTIME_SOURCE;
        PyObject *item = Py_TYPE(state->source)->tp_iternext(state->source);
        if (item == NULL) {
            if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                return NULL;
            }
            PyErr_Clear();
            state->exhausted = 1;
            return NULL;
        }
        state->position++;
        return item;
    }
    if (state->kind == ITERTOOLS_GROUPBY) {
        return it_runtime_groupby_next(state, resumed_value, is_resumed);
    }
    if (it_runtime_is_predicate(state->kind)) {
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
                int truth = PyObject_IsTrue(resumed_value);
                if (truth < 0) {
                    return NULL;
                }
                if (state->kind == ITERTOOLS_TAKEWHILE && !truth) {
                    return NULL;
                }
                if (state->kind == ITERTOOLS_TAKEWHILE && truth) {
                    PyObject *item = state->item;
                    state->item = NULL;
                    return item;
                }
                if (state->kind == ITERTOOLS_DROPWHILE && truth && !state->started) {
                    Py_CLEAR(state->item);
                    state->phase = IT_RUNTIME_SOURCE;
                }
                else if (state->kind == ITERTOOLS_FILTERFALSE ? !truth : !state->started || !truth) {
                    PyObject *item = state->item;
                    state->item = NULL;
                    state->started = 1;
                    return item;
                }
                else {
                    Py_CLEAR(state->item);
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
            PyObject *predicate = PyObject_CallOneArg(state->function, state->item);
            if (predicate == NULL) {
                return NULL;
            }
            int truth = PyObject_IsTrue(predicate);
            Py_DECREF(predicate);
            if (truth < 0) {
                return NULL;
            }
            if (state->kind == ITERTOOLS_TAKEWHILE && !truth) {
                return NULL;
            }
            if (state->kind == ITERTOOLS_TAKEWHILE && truth) {
                PyObject *item = state->item;
                state->item = NULL;
                return item;
            }
            if (state->kind == ITERTOOLS_DROPWHILE && truth && !state->started) {
                Py_CLEAR(state->item);
                continue;
            }
            if (state->kind == ITERTOOLS_FILTERFALSE ? !truth : !state->started || !truth) {
                PyObject *item = state->item;
                state->item = NULL;
                state->started = 1;
                return item;
            }
            Py_CLEAR(state->item);
        }
    }

    if (state->kind == ITERTOOLS_CYCLE) {
        if (is_resumed) {
            if (state->phase == IT_RUNTIME_SOURCE) {
                if (resumed_value == NULL) {
                    state->exhausted = 1;
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
                PyObject *position = PyList_GET_ITEM(state->source_positions, state->index);
                Py_ssize_t current = PyLong_AsSsize_t(position);
                if (current == -1 && PyErr_Occurred()) {
                    return NULL;
                }
                PyObject *next_position = PyLong_FromSsize_t(current + 1);
                if (next_position == NULL) {
                    Py_XDECREF(item);
                    return NULL;
                }
                if (PyList_SetItem(state->source_positions, state->index, next_position) < 0) {
                    Py_DECREF(next_position);
                    Py_XDECREF(item);
                    return NULL;
                }
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

static const AleffAdapterVTable it_runtime_vtable = {
    .copy_state = (void *(*)(const void *))it_runtime_copy,
    .free_state = (void (*)(void *))it_runtime_free,
    .resume = it_runtime_resume,
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
        return NULL;
    }
    PyObject *result = it_runtime_continue(state, value, 1);
    ItRuntimeState *target = it_runtime_lookup(state->owner);
    if (target != NULL && it_runtime_assign(target, state) < 0) {
        Py_XDECREF(result);
        result = NULL;
    }
    adapter_leave(&frame);
    it_runtime_free(state);
    return result;
}

static int
adapter_itertools_runtime_next(PyObject *object, PyObject **result)
{
    ItRuntimeState *state = it_runtime_lookup(object);
    if (state == NULL) {
        return 0;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &it_runtime_vtable, state) < 0) {
        return -1;
    }
    *result = it_runtime_continue(state, NULL, 0);
    adapter_leave(&frame);
    return 1;
}

/* Called by adapters_bootstrap.c after the module and the existing special
 * adapters have been initialized.  Keeping registration here avoids exposing
 * CPython layout details to the bootstrap translation unit. */
static int
adapter_itertools_install(PyObject *itertools)
{
    if (PyType_Ready(&AleffGroupbyGrouper_Type) < 0) {
        return -1;
    }
    if (PyType_Ready(&AleffTeeData_Type) < 0 ||
        PyType_Ready(&AleffTeeIterator_Type) < 0) {
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
        original_itertools_dealloc[index] = type->tp_dealloc;
        type->tp_iternext = adapter_itertools_next;
        type->tp_dealloc = adapter_itertools_dealloc;
        PyType_Modified(type);
        Py_DECREF(object);
    }

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
        itertools_new_types[index] = type;
        /* accumulate and batched already have their specialized constructor
         * adapters installed by the bootstrap. */
        if (index >= ITERTOOLS_COMBINATIONS && index != ITERTOOLS_TEE) {
            original_itertools_new[index] = type->tp_new;
            type->tp_new = index == ITERTOOLS_REPEAT
                ? adapter_repeat_new
                : adapter_itertools_new;
            PyType_Modified(type);
        }
        Py_DECREF(object);
    }
    static PyMethodDef tee_method = {
        .ml_name = "tee",
        .ml_meth = (PyCFunction)(void(*)(void))adapter_itertools_tee,
        .ml_flags = METH_VARARGS | METH_KEYWORDS,
        .ml_doc = "Return n independent iterators from an iterable.",
    };
    PyObject *tee_function = PyCFunction_NewEx(&tee_method, NULL, NULL);
    if (tee_function == NULL) {
        return -1;
    }
    int tee_status = PyObject_SetAttrString(itertools, "tee", tee_function);
    Py_DECREF(tee_function);
    if (tee_status < 0) {
        return -1;
    }
    return 0;
}

static void
adapter_itertools_rollback(void)
{
    if (original_batched_type != NULL && original_batched_new != NULL) {
        original_batched_type->tp_new = original_batched_new;
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
            type->tp_dealloc = original_itertools_dealloc[index];
            PyType_Modified(type);
        }
        itertools_next_types[index] = NULL;
        original_itertools_next[index] = NULL;
        original_itertools_dealloc[index] = NULL;
    }
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
    original_batched_type = NULL;
}
