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
    adapter_enter(&frame, &chain_vtable, state);
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
    adapter_enter(&frame, &chain_vtable, &state);
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
    adapter_enter(&frame, &chain_constructor_vtable, &state);
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
