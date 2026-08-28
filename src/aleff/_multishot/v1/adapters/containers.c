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
    SEQUENCE_SEARCH_COUNT,
    SEQUENCE_SEARCH_INDEX,
    SEQUENCE_SEARCH_REMOVE,
    SEQUENCE_SEARCH_CONTAINS,
} SequenceSearchKind;

typedef struct {
    PyObject *receiver;
    PyObject *target;
    PyObject *item;
    Py_ssize_t index;
    Py_ssize_t stop;
    Py_ssize_t count;
    SequenceSearchKind kind;
} SequenceSearchState;

static Py_ssize_t
sequence_search_size(SequenceSearchState *state)
{
    return PyList_Check(state->receiver)
        ? PyList_GET_SIZE(state->receiver)
        : PyTuple_GET_SIZE(state->receiver);
}

static PyObject *
sequence_search_item(SequenceSearchState *state, Py_ssize_t index)
{
    return PyList_Check(state->receiver)
        ? PyList_GET_ITEM(state->receiver, index)
        : PyTuple_GET_ITEM(state->receiver, index);
}

static void *
sequence_search_copy_state(const void *raw_state)
{
    const SequenceSearchState *state = raw_state;
    SequenceSearchState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->receiver = Py_NewRef(state->receiver);
    copy->target = Py_NewRef(state->target);
    copy->item = Py_XNewRef(state->item);
    return copy;
}

static void
sequence_search_free_state(void *raw_state)
{
    SequenceSearchState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->receiver);
    Py_DECREF(state->target);
    Py_XDECREF(state->item);
    PyMem_Free(state);
}

static PyObject *sequence_search_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable sequence_search_vtable = {
    .copy_state = sequence_search_copy_state,
    .free_state = sequence_search_free_state,
    .resume = sequence_search_resume,
};

static PyObject *
sequence_search_not_found(SequenceSearchState *state)
{
    if (state->kind == SEQUENCE_SEARCH_COUNT) {
        return PyLong_FromSsize_t(state->count);
    }
    if (state->kind == SEQUENCE_SEARCH_CONTAINS) {
        return Py_NewRef(Py_False);
    }
    PyErr_SetString(PyExc_ValueError, "sequence.index(x): x not in sequence");
    return NULL;
}

static PyObject *
sequence_search_continue(
    SequenceSearchState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    int equal = is_resumed ? PyObject_IsTrue(resumed_value) : -1;
    for (;;) {
        Py_ssize_t size = sequence_search_size(state);
        Py_ssize_t limit = state->stop < size ? state->stop : size;
        if (state->item == NULL) {
            if (state->index >= limit) {
                return sequence_search_not_found(state);
            }
            state->item = Py_NewRef(sequence_search_item(state, state->index));
        }
        if (equal < 0) {
            equal = PyObject_RichCompareBool(state->item, state->target, Py_EQ);
        }
        if (equal < 0) {
            return NULL;
        }
        if (equal) {
            if (state->kind == SEQUENCE_SEARCH_CONTAINS) {
                return Py_NewRef(Py_True);
            }
            if (state->kind == SEQUENCE_SEARCH_INDEX) {
                return PyLong_FromSsize_t(state->index);
            }
            if (state->kind == SEQUENCE_SEARCH_REMOVE) {
                if (PySequence_DelItem(state->receiver, state->index) < 0) {
                    return NULL;
                }
                return Py_NewRef(Py_None);
            }
            state->count++;
        }
        state->index++;
        Py_CLEAR(state->item);
        equal = -1;
    }
}

static PyObject *
sequence_search_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    SequenceSearchState *state = sequence_search_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &sequence_search_vtable, state);
    PyObject *result = sequence_search_continue(state, value, 1);
    adapter_leave(&frame);
    sequence_search_free_state(state);
    return result;
}

static PyObject *
sequence_search(
    PyObject *receiver,
    PyObject *target,
    Py_ssize_t start,
    Py_ssize_t stop,
    SequenceSearchKind kind
)
{
    Py_ssize_t size = PyList_Check(receiver)
        ? PyList_GET_SIZE(receiver)
        : PyTuple_GET_SIZE(receiver);
    if (start < 0) {
        start += size;
        if (start < 0) {
            start = 0;
        }
    }
    if (stop < 0) {
        stop += size;
        if (stop < 0) {
            stop = 0;
        }
    }
    SequenceSearchState state = {
        .receiver = receiver,
        .target = target,
        .item = NULL,
        .index = start,
        .stop = stop,
        .count = 0,
        .kind = kind,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &sequence_search_vtable, &state);
    PyObject *result = sequence_search_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.item);
    return result;
}

static PyObject *
adapter_sequence_count(PyObject *self, PyObject *target)
{
    return sequence_search(
        self,
        target,
        0,
        PY_SSIZE_T_MAX,
        SEQUENCE_SEARCH_COUNT
    );
}

static PyObject *
adapter_sequence_index(PyObject *self, PyObject *args)
{
    PyObject *target;
    Py_ssize_t start = 0;
    Py_ssize_t stop = PY_SSIZE_T_MAX;
    if (!PyArg_ParseTuple(args, "O|nn:index", &target, &start, &stop)) {
        return NULL;
    }
    return sequence_search(self, target, start, stop, SEQUENCE_SEARCH_INDEX);
}

static PyObject *
adapter_list_remove(PyObject *self, PyObject *target)
{
    return sequence_search(
        self,
        target,
        0,
        PY_SSIZE_T_MAX,
        SEQUENCE_SEARCH_REMOVE
    );
}

static int
adapter_sequence_contains(PyObject *self, PyObject *target)
{
    PyObject *result = sequence_search(
        self,
        target,
        0,
        PY_SSIZE_T_MAX,
        SEQUENCE_SEARCH_CONTAINS
    );
    if (result == NULL) {
        return -1;
    }
    int truth = PyObject_IsTrue(result);
    Py_DECREF(result);
    return truth;
}

typedef enum {
    SEQUENCE_COMPARE_EQUAL,
    SEQUENCE_COMPARE_FINAL,
} SequenceComparePhase;

typedef struct {
    PyObject *left;
    PyObject *right;
    Py_ssize_t index;
    int operation;
    SequenceComparePhase phase;
} SequenceCompareState;

static Py_ssize_t
sequence_compare_size(PyObject *sequence)
{
    return PyList_Check(sequence)
        ? PyList_GET_SIZE(sequence)
        : PyTuple_GET_SIZE(sequence);
}

static PyObject *
sequence_compare_item(PyObject *sequence, Py_ssize_t index)
{
    return PyList_Check(sequence)
        ? PyList_GET_ITEM(sequence, index)
        : PyTuple_GET_ITEM(sequence, index);
}

static void *
sequence_compare_copy_state(const void *raw_state)
{
    const SequenceCompareState *state = raw_state;
    SequenceCompareState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->left = Py_NewRef(state->left);
    copy->right = Py_NewRef(state->right);
    return copy;
}

static void
sequence_compare_free_state(void *raw_state)
{
    SequenceCompareState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->left);
    Py_DECREF(state->right);
    PyMem_Free(state);
}

static PyObject *sequence_compare_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable sequence_compare_vtable = {
    .copy_state = sequence_compare_copy_state,
    .free_state = sequence_compare_free_state,
    .resume = sequence_compare_resume,
};

static PyObject *
sequence_compare_lengths(SequenceCompareState *state)
{
    Py_ssize_t left_size = sequence_compare_size(state->left);
    Py_ssize_t right_size = sequence_compare_size(state->right);
    int result;
    switch (state->operation) {
        case Py_EQ: result = left_size == right_size; break;
        case Py_NE: result = left_size != right_size; break;
        case Py_LT: result = left_size < right_size; break;
        case Py_LE: result = left_size <= right_size; break;
        case Py_GT: result = left_size > right_size; break;
        default: result = left_size >= right_size; break;
    }
    return PyBool_FromLong(result);
}

static PyObject *
sequence_compare_continue(
    SequenceCompareState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed && state->phase == SEQUENCE_COMPARE_FINAL) {
        return Py_NewRef(resumed_value);
    }
    int equal = is_resumed ? PyObject_IsTrue(resumed_value) : -1;
    for (;;) {
        Py_ssize_t left_size = sequence_compare_size(state->left);
        Py_ssize_t right_size = sequence_compare_size(state->right);
        Py_ssize_t limit = left_size < right_size ? left_size : right_size;
        if (state->index >= limit) {
            return sequence_compare_lengths(state);
        }
        PyObject *left_item = sequence_compare_item(state->left, state->index);
        PyObject *right_item = sequence_compare_item(state->right, state->index);
        if (equal < 0) {
            equal = left_item == right_item
                ? 1
                : PyObject_RichCompareBool(left_item, right_item, Py_EQ);
        }
        if (equal < 0) {
            return NULL;
        }
        if (!equal) {
            if (state->operation == Py_EQ) {
                return Py_NewRef(Py_False);
            }
            if (state->operation == Py_NE) {
                return Py_NewRef(Py_True);
            }
            state->phase = SEQUENCE_COMPARE_FINAL;
            return PyObject_RichCompare(left_item, right_item, state->operation);
        }
        state->index++;
        equal = -1;
    }
}

static PyObject *
sequence_compare_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    SequenceCompareState *state = sequence_compare_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &sequence_compare_vtable, state);
    PyObject *result = sequence_compare_continue(state, value, 1);
    adapter_leave(&frame);
    sequence_compare_free_state(state);
    return result;
}

static richcmpfunc original_list_richcompare = NULL;
static richcmpfunc original_tuple_richcompare = NULL;

static PyObject *
adapter_sequence_richcompare(PyObject *left, PyObject *right, int operation)
{
    int list_comparison = PyList_Check(left) && PyList_Check(right);
    int tuple_comparison = PyTuple_Check(left) && PyTuple_Check(right);
    if (!list_comparison && !tuple_comparison) {
        richcmpfunc original = PyList_Check(left)
            ? original_list_richcompare
            : original_tuple_richcompare;
        return original(left, right, operation);
    }
    SequenceCompareState state = {
        .left = left,
        .right = right,
        .index = 0,
        .operation = operation,
        .phase = SEQUENCE_COMPARE_EQUAL,
    };
    AleffAdapterFrame frame;
    adapter_enter(&frame, &sequence_compare_vtable, &state);
    PyObject *result = sequence_compare_continue(&state, NULL, 0);
    adapter_leave(&frame);
    return result;
}

typedef enum {
    LIST_SORT_WAIT_REVERSE,
    LIST_SORT_WAIT_KEY,
    LIST_SORT_WAIT_COMPARE,
} ListSortPhase;

typedef struct {
    PyObject *receiver;
    PyObject *items;
    PyObject *keys;
    PyObject *key_function;
    Py_ssize_t key_index;
    Py_ssize_t sort_index;
    Py_ssize_t insertion_index;
    int reverse;
    ListSortPhase phase;
} ListSortState;

static void *
list_sort_copy_state(const void *raw_state)
{
    const ListSortState *state = raw_state;
    ListSortState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->receiver = Py_NewRef(state->receiver);
    copy->items = PyList_GetSlice(
        state->items,
        0,
        PyList_GET_SIZE(state->items)
    );
    copy->keys = PyList_GetSlice(
        state->keys,
        0,
        PyList_GET_SIZE(state->keys)
    );
    copy->key_function = Py_XNewRef(state->key_function);
    if (copy->items == NULL || copy->keys == NULL) {
        Py_DECREF(copy->receiver);
        Py_XDECREF(copy->items);
        Py_XDECREF(copy->keys);
        Py_XDECREF(copy->key_function);
        PyMem_Free(copy);
        return NULL;
    }
    return copy;
}

static void
list_sort_free_state(void *raw_state)
{
    ListSortState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->receiver);
    Py_DECREF(state->items);
    Py_DECREF(state->keys);
    Py_XDECREF(state->key_function);
    PyMem_Free(state);
}

static PyObject *list_sort_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable list_sort_vtable = {
    .copy_state = list_sort_copy_state,
    .free_state = list_sort_free_state,
    .resume = list_sort_resume,
};

static int
list_sort_swap(PyObject *items, Py_ssize_t left, Py_ssize_t right)
{
    PyObject *left_item = Py_NewRef(PyList_GET_ITEM(items, left));
    PyObject *right_item = Py_NewRef(PyList_GET_ITEM(items, right));
    if (PyList_SetItem(items, left, right_item) < 0) {
        Py_DECREF(left_item);
        Py_DECREF(right_item);
        return -1;
    }
    if (PyList_SetItem(items, right, left_item) < 0) {
        Py_DECREF(left_item);
        return -1;
    }
    return 0;
}

static PyObject *
list_sort_continue(ListSortState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed) {
        if (state->phase == LIST_SORT_WAIT_REVERSE) {
            int truth = PyObject_IsTrue(resumed_value);
            if (truth < 0) {
                return NULL;
            }
            state->reverse = truth;
        }
        else if (state->phase == LIST_SORT_WAIT_KEY) {
            if (PyList_Append(state->keys, resumed_value) < 0) {
                return NULL;
            }
            state->key_index++;
        }
        else {
            int move = PyObject_IsTrue(resumed_value);
            if (move < 0) {
                return NULL;
            }
            if (move) {
                if (
                    list_sort_swap(
                        state->items,
                        state->insertion_index,
                        state->insertion_index - 1
                    ) < 0 ||
                    list_sort_swap(
                        state->keys,
                        state->insertion_index,
                        state->insertion_index - 1
                    ) < 0
                ) {
                    return NULL;
                }
                state->insertion_index--;
            }
            else {
                state->sort_index++;
                state->insertion_index = state->sort_index;
            }
        }
    }

    Py_ssize_t size = PyList_GET_SIZE(state->items);
    while (state->key_index < size) {
        PyObject *item = PyList_GET_ITEM(state->items, state->key_index);
        if (state->key_function == NULL) {
            if (PyList_Append(state->keys, item) < 0) {
                return NULL;
            }
            state->key_index++;
            continue;
        }
        state->phase = LIST_SORT_WAIT_KEY;
        PyObject *key = PyObject_CallOneArg(state->key_function, item);
        if (key == NULL) {
            return NULL;
        }
        if (PyList_Append(state->keys, key) < 0) {
            Py_DECREF(key);
            return NULL;
        }
        Py_DECREF(key);
        state->key_index++;
    }

    if (state->sort_index < 1) {
        state->sort_index = 1;
        state->insertion_index = 1;
    }
    while (state->sort_index < size) {
        if (state->insertion_index == 0) {
            state->sort_index++;
            state->insertion_index = state->sort_index;
            continue;
        }
        PyObject *current = PyList_GET_ITEM(state->keys, state->insertion_index);
        PyObject *previous = PyList_GET_ITEM(
            state->keys,
            state->insertion_index - 1
        );
        state->phase = LIST_SORT_WAIT_COMPARE;
        int move = PyObject_RichCompareBool(
            current,
            previous,
            state->reverse ? Py_GT : Py_LT
        );
        if (move < 0) {
            return NULL;
        }
        if (move) {
            if (
                list_sort_swap(
                    state->items,
                    state->insertion_index,
                    state->insertion_index - 1
                ) < 0 ||
                list_sort_swap(
                    state->keys,
                    state->insertion_index,
                    state->insertion_index - 1
                ) < 0
            ) {
                return NULL;
            }
            state->insertion_index--;
        }
        else {
            state->sort_index++;
            state->insertion_index = state->sort_index;
        }
    }
    if (
        PyList_SetSlice(
            state->receiver,
            0,
            PyList_GET_SIZE(state->receiver),
            state->items
        ) < 0
    ) {
        return NULL;
    }
    return Py_NewRef(Py_None);
}

static PyObject *
list_sort_resume(const void *raw_state, PyObject *value)
{
    const ListSortState *source = raw_state;
    ListSortState *state = list_sort_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    if (PyList_SetSlice(state->receiver, 0, PyList_GET_SIZE(state->receiver), NULL) < 0) {
        list_sort_free_state(state);
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &list_sort_vtable, state);
    PyObject *result;
    if (value == NULL) {
        result = NULL;
    }
    else {
        result = list_sort_continue(state, value, 1);
    }
    adapter_leave(&frame);
    if (result == NULL) {
        PyList_SetSlice(
            state->receiver,
            0,
            PyList_GET_SIZE(state->receiver),
            state->items
        );
    }
    list_sort_free_state(state);
    (void)source;
    return result;
}

static PyObject *
adapter_list_sort(PyObject *self, PyObject *args, PyObject *kwargs)
{
    if (PyTuple_GET_SIZE(args) != 0) {
        PyErr_SetString(PyExc_TypeError, "sort() takes no positional arguments");
        return NULL;
    }
    PyObject *key_function = NULL;
    PyObject *reverse_object = Py_False;
    static char *names[] = {"key", "reverse", NULL};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "|$OO:sort",
            names,
            &key_function,
            &reverse_object
        )) {
        return NULL;
    }
    if (key_function == Py_None) {
        key_function = NULL;
    }
    Py_ssize_t size = PyList_GET_SIZE(self);
    ListSortState state = {
        .receiver = self,
        .items = PyList_GetSlice(self, 0, size),
        .keys = PyList_New(0),
        .key_function = key_function,
        .key_index = 0,
        .sort_index = 1,
        .insertion_index = 1,
        .reverse = 0,
        .phase = LIST_SORT_WAIT_REVERSE,
    };
    if (state.items == NULL || state.keys == NULL) {
        Py_XDECREF(state.items);
        Py_XDECREF(state.keys);
        return NULL;
    }
    if (PyList_SetSlice(self, 0, size, NULL) < 0) {
        Py_DECREF(state.items);
        Py_DECREF(state.keys);
        return NULL;
    }
    AleffAdapterFrame frame;
    adapter_enter(&frame, &list_sort_vtable, &state);
    int reverse = PyObject_IsTrue(reverse_object);
    PyObject *result = NULL;
    if (reverse >= 0) {
        state.reverse = reverse;
        result = list_sort_continue(&state, NULL, 0);
    }
    adapter_leave(&frame);
    if (result == NULL) {
        PyList_SetSlice(self, 0, PyList_GET_SIZE(self), state.items);
    }
    Py_DECREF(state.items);
    Py_DECREF(state.keys);
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
