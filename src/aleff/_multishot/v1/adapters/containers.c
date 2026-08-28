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
static binaryfunc original_dict_subscript;

static int
dict_item_has_python_hash(PyObject *key)
{
    PyObject *type_dict = PyType_GetDict(Py_TYPE(key));
    if (type_dict == NULL) {
        return 0;
    }
    PyObject *descriptor = PyDict_GetItemString(type_dict, "__hash__");
    return descriptor != NULL && PyFunction_Check(descriptor);
}

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
    if (adapter_enter(&frame, &collect_vtable, state) < 0) {
        return NULL;
    }
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
    if (adapter_enter(&frame, &collect_vtable, &state) < 0) {
        return NULL;
    }
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
    if (adapter_enter(&frame, &list_extend_vtable, &state) < 0) {
        return NULL;
    }
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
    if (adapter_enter(&frame, &list_count_vtable, state) < 0) {
        return NULL;
    }
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
    if (adapter_enter(&frame, &list_count_vtable, &state) < 0) {
        return NULL;
    }
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
    if (adapter_enter(&frame, &sequence_search_vtable, state) < 0) {
        return NULL;
    }
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
    if (adapter_enter(&frame, &sequence_search_vtable, &state) < 0) {
        return NULL;
    }
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
    if (adapter_enter(&frame, &sequence_compare_vtable, state) < 0) {
        return NULL;
    }
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
    if (adapter_enter(&frame, &sequence_compare_vtable, &state) < 0) {
        return NULL;
    }
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
    if (adapter_enter(&frame, &list_sort_vtable, state) < 0) {
        return NULL;
    }
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
    if (adapter_enter(&frame, &list_sort_vtable, &state) < 0) {
        return NULL;
    }
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
    if (adapter_enter(&frame, &slice_hash_vtable, state) < 0) {
        return NULL;
    }
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
    if (adapter_enter(&frame, &slice_hash_vtable, &state) < 0) {
        return -1;
    }
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
    if (adapter_enter(&frame, &sort_vtable, state) < 0) {
        return NULL;
    }
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
    if (adapter_enter(&frame, &sort_vtable, &state) < 0) {
        return NULL;
    }
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

typedef enum {
    RANGE_SEARCH_WAIT_INDEX,
    RANGE_SEARCH_READY,
} RangeSearchPhase;

typedef struct {
    PyObject *receiver;
    PyObject *target;
    PyObject *indexed;
    PyObject *iterator;
    Py_ssize_t index;
    Py_ssize_t count;
    int find_index;
    RangeSearchPhase phase;
} RangeSearchState;

static void *
range_search_copy_state(const void *raw_state)
{
    const RangeSearchState *state = raw_state;
    RangeSearchState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->receiver = Py_NewRef(state->receiver);
    copy->target = Py_NewRef(state->target);
    copy->indexed = Py_XNewRef(state->indexed);
    copy->iterator = Py_XNewRef(state->iterator);
    return copy;
}

static void
range_search_free_state(void *raw_state)
{
    RangeSearchState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->receiver);
    Py_DECREF(state->target);
    Py_XDECREF(state->indexed);
    Py_XDECREF(state->iterator);
    PyMem_Free(state);
}

static PyObject *range_search_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable range_search_vtable = {
    .copy_state = range_search_copy_state,
    .free_state = range_search_free_state,
    .resume = range_search_resume,
};

static PyObject *
range_search_continue(
    RangeSearchState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        PyObject *indexed = PyNumber_Index(resumed_value);
        if (indexed == NULL) {
            return NULL;
        }
        Py_XSETREF(state->indexed, indexed);
        state->phase = RANGE_SEARCH_READY;
    }
    if (state->indexed == NULL) {
        state->phase = RANGE_SEARCH_WAIT_INDEX;
        state->indexed = PyNumber_Index(state->target);
        if (state->indexed == NULL) {
            return NULL;
        }
        state->phase = RANGE_SEARCH_READY;
    }
    if (state->iterator == NULL) {
        state->iterator = PyObject_GetIter(state->receiver);
        if (state->iterator == NULL) {
            return NULL;
        }
    }
    for (;;) {
        PyObject *item = Py_TYPE(state->iterator)->tp_iternext(state->iterator);
        if (item == NULL) {
            if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
                return NULL;
            }
            PyErr_Clear();
            if (state->find_index) {
                PyErr_SetString(PyExc_ValueError, "range.index(x): x not in range");
                return NULL;
            }
            return PyLong_FromSsize_t(state->count);
        }
        int equal = PyObject_RichCompareBool(item, state->indexed, Py_EQ);
        Py_DECREF(item);
        if (equal < 0) {
            return NULL;
        }
        state->index++;
        if (equal) {
            if (state->find_index) {
                return PyLong_FromSsize_t(state->index - 1);
            }
            state->count++;
        }
    }
}

static PyObject *
range_search_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    RangeSearchState *state = range_search_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &range_search_vtable, state) < 0) {
        return NULL;
    }
    PyObject *result = range_search_continue(state, value, 1);
    adapter_leave(&frame);
    range_search_free_state(state);
    return result;
}

static PyObject *
adapter_range_count(PyObject *self, PyObject *target)
{
    RangeSearchState state = {
        .receiver = self,
        .target = target,
        .indexed = NULL,
        .iterator = NULL,
        .index = 0,
        .count = 0,
        .find_index = 0,
        .phase = RANGE_SEARCH_WAIT_INDEX,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &range_search_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = range_search_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.indexed);
    Py_XDECREF(state.iterator);
    return result;
}

static PyObject *
adapter_range_index(PyObject *self, PyObject *target)
{
    RangeSearchState state = {
        .receiver = self,
        .target = target,
        .indexed = NULL,
        .iterator = NULL,
        .index = 0,
        .count = 0,
        .find_index = 1,
        .phase = RANGE_SEARCH_WAIT_INDEX,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &range_search_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = range_search_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.indexed);
    Py_XDECREF(state.iterator);
    return result;
}

typedef enum {
    SLICE_INDICES_WAIT_LENGTH,
    SLICE_INDICES_WAIT_COMPONENT,
    SLICE_INDICES_READY,
} SliceIndicesPhase;

typedef struct {
    PyObject *slice;
    PyObject *length_object;
    PyObject *components[3];
    Py_ssize_t length;
    int component;
    SliceIndicesPhase phase;
} SliceIndicesState;

static void *
slice_indices_copy_state(const void *raw_state)
{
    const SliceIndicesState *state = raw_state;
    SliceIndicesState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->slice = Py_NewRef(state->slice);
    copy->length_object = Py_NewRef(state->length_object);
    for (int i = 0; i < 3; i++) {
        copy->components[i] = Py_XNewRef(state->components[i]);
    }
    return copy;
}

static void
slice_indices_free_state(void *raw_state)
{
    SliceIndicesState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->slice);
    Py_DECREF(state->length_object);
    for (int i = 0; i < 3; i++) {
        Py_XDECREF(state->components[i]);
    }
    PyMem_Free(state);
}

static PyObject *slice_indices_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable slice_indices_vtable = {
    .copy_state = slice_indices_copy_state,
    .free_state = slice_indices_free_state,
    .resume = slice_indices_resume,
};

static PyObject *
slice_indices_finish(SliceIndicesState *state)
{
    PyObject *start = state->components[0] == NULL
        ? Py_NewRef(Py_None) : Py_NewRef(state->components[0]);
    PyObject *stop = state->components[1] == NULL
        ? Py_NewRef(Py_None) : Py_NewRef(state->components[1]);
    PyObject *step = state->components[2] == NULL
        ? Py_NewRef(Py_None) : Py_NewRef(state->components[2]);
    if (start == NULL || stop == NULL || step == NULL) {
        Py_XDECREF(start); Py_XDECREF(stop); Py_XDECREF(step);
        return NULL;
    }
    PyObject *normalized = PySlice_New(start, stop, step);
    Py_DECREF(start); Py_DECREF(stop); Py_DECREF(step);
    if (normalized == NULL) {
        return NULL;
    }
    Py_ssize_t result_start, result_stop, result_step, result_length;
    int status = PySlice_GetIndicesEx(
        normalized,
        state->length,
        &result_start,
        &result_stop,
        &result_step,
        &result_length
    );
    Py_DECREF(normalized);
    if (status < 0) {
        return NULL;
    }
    return Py_BuildValue("nnn", result_start, result_stop, result_step);
}

static PyObject *
slice_indices_continue(
    SliceIndicesState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (state->phase == SLICE_INDICES_WAIT_LENGTH) {
            PyObject *length = PyNumber_Index(resumed_value);
            if (length == NULL) {
                return NULL;
            }
            state->length = PyLong_AsSsize_t(length);
            Py_DECREF(length);
            if (state->length == -1 && PyErr_Occurred()) {
                return NULL;
            }
        }
        else {
            PyObject *component = PyNumber_Index(resumed_value);
            if (component == NULL) {
                return NULL;
            }
            Py_XSETREF(state->components[state->component], component);
            state->component++;
        }
    }
    if (!is_resumed) {
        state->phase = SLICE_INDICES_WAIT_LENGTH;
        PyObject *length = PyNumber_Index(state->length_object);
        if (length == NULL) {
            return NULL;
        }
        state->length = PyLong_AsSsize_t(length);
        Py_DECREF(length);
        if (state->length == -1 && PyErr_Occurred()) {
            return NULL;
        }
    }
    while (state->component < 3) {
        PyObject *value = ((PySliceObject *)state->slice)->start;
        if (state->component == 1) value = ((PySliceObject *)state->slice)->stop;
        if (state->component == 2) value = ((PySliceObject *)state->slice)->step;
        if (value == Py_None) {
            state->component++;
            continue;
        }
        state->phase = SLICE_INDICES_WAIT_COMPONENT;
        PyObject *component = PyNumber_Index(value);
        if (component == NULL) {
            return NULL;
        }
        state->components[state->component++] = component;
    }
    state->phase = SLICE_INDICES_READY;
    return slice_indices_finish(state);
}

static PyObject *
slice_indices_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    SliceIndicesState *state = slice_indices_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &slice_indices_vtable, state) < 0) {
        return NULL;
    }
    PyObject *result = slice_indices_continue(state, value, 1);
    adapter_leave(&frame);
    slice_indices_free_state(state);
    return result;
}

static PyObject *
adapter_slice_indices(PyObject *self, PyObject *length_object)
{
    PySliceObject *slice = (PySliceObject *)self;
    SliceIndicesState state = {
        .slice = self,
        .length_object = length_object,
        .components = {NULL, NULL, NULL},
        .length = 0,
        .component = 0,
        .phase = SLICE_INDICES_WAIT_LENGTH,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &slice_indices_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = slice_indices_continue(&state, NULL, 0);
    adapter_leave(&frame);
    for (int i = 0; i < 3; i++) Py_XDECREF(state.components[i]);
    (void)slice;
    return result;
}

#include "mappings.c"
#include "sets.c"
#include "text.c"


static PyMethodDef containers_range_count_method = {
    .ml_name = "count",
    .ml_meth = adapter_range_count,
    .ml_flags = METH_O,
    .ml_doc = "Return number of occurrences of value.",
};

static PyMethodDef containers_range_index_method = {
    .ml_name = "index",
    .ml_meth = adapter_range_index,
    .ml_flags = METH_O,
    .ml_doc = "Return the index of value.",
};

static PyMethodDef containers_slice_indices_method = {
    .ml_name = "indices",
    .ml_meth = adapter_slice_indices,
    .ml_flags = METH_O,
    .ml_doc = "Return the indices of the slice for a sequence of length length.",
};

static int
containers_replace_type_method(
    PyTypeObject *type,
    const char *name,
    PyMethodDef *method
)
{
    PyObject *descriptor = (method->ml_flags & METH_CLASS) != 0
        ? PyDescr_NewClassMethod(type, method)
        : PyDescr_NewMethod(type, method);
    if (descriptor == NULL) {
        return -1;
    }
    PyObject *type_dict = PyType_GetDict(type);
    if (type_dict == NULL) {
        Py_DECREF(descriptor);
        return -1;
    }
    int status = PyDict_SetItemString(type_dict, name, descriptor);
    Py_DECREF(descriptor);
    if (status == 0) {
        PyType_Modified(type);
    }
    return status;
}

/* Called once by adapters_bootstrap.c after the container type slots are ready. */
int
adapter_containers_install(void)
{
    if (original_dict_subscript == NULL && PyDict_Type.tp_as_mapping != NULL) {
        original_dict_subscript = PyDict_Type.tp_as_mapping->mp_subscript;
    }
    PyDict_Type.tp_richcompare = adapter_dict_richcompare;
    if (
        containers_replace_type_method(&PyDict_Type, "fromkeys", &containers_dict_fromkeys_method) < 0 ||
        containers_replace_type_method(&PyDict_Type, "update", &containers_dict_update_method) < 0 ||
        containers_replace_type_method(&PyDict_Type, "__getitem__", &containers_dict_getitem_method) < 0 ||
        containers_replace_type_method(&PyDict_Type, "__setitem__", &containers_dict_setitem_method) < 0 ||
        containers_replace_type_method(&PyDict_Type, "__delitem__", &containers_dict_delitem_method) < 0 ||
        containers_replace_type_method(&PyDict_Type, "__contains__", &containers_dict_contains_method) < 0 ||
        containers_replace_type_method(&PyDict_Type, "__eq__", &containers_dict_eq_method) < 0 ||
        containers_replace_type_method(&PyDict_Type, "__ne__", &containers_dict_ne_method) < 0 ||
        containers_replace_type_method(&PyUnicode_Type, "join", &containers_str_join_method) < 0 ||
        containers_replace_type_method(&PyUnicode_Type, "encode", &containers_str_encode_method) < 0 ||
        containers_replace_type_method(&PyBytes_Type, "join", &containers_bytes_join_method) < 0 ||
        containers_replace_type_method(&PyBytes_Type, "decode", &containers_bytes_decode_method) < 0 ||
        containers_replace_type_method(&PyByteArray_Type, "join", &containers_bytearray_join_method) < 0 ||
        containers_replace_type_method(&PyByteArray_Type, "decode", &containers_bytearray_decode_method) < 0 ||
        containers_replace_type_method(&PySet_Type, "update", &containers_set_update_method) < 0 ||
        containers_replace_type_method(&PySet_Type, "intersection_update", &containers_set_intersection_update_method) < 0 ||
        containers_replace_type_method(&PySet_Type, "difference_update", &containers_set_difference_update_method) < 0 ||
        containers_replace_type_method(&PySet_Type, "symmetric_difference_update", &containers_set_symmetric_difference_update_method) < 0 ||
        containers_replace_type_method(&PySet_Type, "union", &containers_set_union_method) < 0 ||
        containers_replace_type_method(&PySet_Type, "intersection", &containers_set_intersection_method) < 0 ||
        containers_replace_type_method(&PySet_Type, "difference", &containers_set_difference_method) < 0 ||
        containers_replace_type_method(&PySet_Type, "symmetric_difference", &containers_set_symmetric_difference_method) < 0 ||
        containers_replace_type_method(&PySet_Type, "isdisjoint", &containers_set_isdisjoint_method) < 0 ||
        containers_replace_type_method(&PySet_Type, "issubset", &containers_set_issubset_method) < 0 ||
        containers_replace_type_method(&PySet_Type, "issuperset", &containers_set_issuperset_method) < 0 ||
        containers_replace_type_method(&PyFrozenSet_Type, "union", &containers_frozenset_union_method) < 0 ||
        containers_replace_type_method(&PyFrozenSet_Type, "intersection", &containers_frozenset_intersection_method) < 0 ||
        containers_replace_type_method(&PyFrozenSet_Type, "difference", &containers_frozenset_difference_method) < 0 ||
        containers_replace_type_method(&PyFrozenSet_Type, "symmetric_difference", &containers_frozenset_symmetric_difference_method) < 0 ||
        containers_replace_type_method(&PyFrozenSet_Type, "isdisjoint", &containers_frozenset_isdisjoint_method) < 0 ||
        containers_replace_type_method(&PyFrozenSet_Type, "issubset", &containers_frozenset_issubset_method) < 0 ||
        containers_replace_type_method(&PyFrozenSet_Type, "issuperset", &containers_frozenset_issuperset_method) < 0 ||
        containers_replace_type_method(&PyRange_Type, "count", &containers_range_count_method) < 0 ||
        containers_replace_type_method(&PyRange_Type, "index", &containers_range_index_method) < 0 ||
        containers_replace_type_method(&PySlice_Type, "indices", &containers_slice_indices_method) < 0
    ) {
        return -1;
    }
    return 0;
}
