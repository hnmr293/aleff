#include "internal.h"
#include "containers.h"
#include "iterators.h"
#include "mappings.h"
#include "sets.h"
#include "sort_engine.h"
#include "text.h"

typedef enum {
    COLLECT_WAIT_ITER,
    COLLECT_WAIT_HINT,
    COLLECT_WAIT_NEXT,
    COLLECT_WAIT_INDEX,
} CollectPhase;

typedef struct {
    PyObject *iterable;
    PyObject *iterator;
    PyObject *items;
    CollectKind kind;
    CollectPhase phase;
} CollectState;

initproc original_bytearray_init;
 binaryfunc original_dict_subscript;

int
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
    if (state->kind == COLLECT_DICT) {
        copy->items = PyDict_Copy(state->items);
    }
    else if (state->kind == COLLECT_SET || state->kind == COLLECT_FROZENSET) {
        copy->items = PySet_New(state->items);
    }
    else {
        Py_ssize_t item_count = PyList_GET_SIZE(state->items);
        copy->items = PyList_GetSlice(state->items, 0, item_count);
    }
    if (copy->items == NULL) {
        PyMem_Free(copy);
        return NULL;
    }
    copy->iterator = Py_XNewRef(state->iterator);
    copy->iterable = Py_NewRef(state->iterable);
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
    Py_DECREF(state->iterable);
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
            return PyDict_Copy(state->items);
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
collect_byte_index(CollectState *state, PyObject *value)
{
    PyObject *index = PyNumber_Index(value);
    if (index == NULL) {
        return NULL;
    }
    long byte = PyLong_AsLong(index);
    if (byte == -1 && PyErr_Occurred()) {
        if (!PyErr_ExceptionMatches(PyExc_OverflowError)) {
            Py_DECREF(index);
            return NULL;
        }
        PyErr_Clear();
        byte = -1;
    }
    if (byte < 0 || byte > 255) {
        PyErr_SetString(
            PyExc_ValueError,
            state->kind == COLLECT_BYTES
                ? "bytes must be in range(0, 256)"
                : "byte must be in range(0, 256)"
        );
        Py_DECREF(index);
        return NULL;
    }
    return index;
}

static PyObject *
collect_continue(CollectState *state, PyObject *resumed_value, int is_resumed)
{
    PyObject *item = NULL;
    int byte_index_ready = 0;
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
        else if (state->phase == COLLECT_WAIT_HINT) {
            Py_ssize_t hint = PyNumber_AsSsize_t(
                resumed_value, PyExc_OverflowError
            );
            if (hint < 0 && PyErr_Occurred()) {
                return NULL;
            }
            if (hint < 0) {
                PyErr_SetString(
                    PyExc_ValueError,
                    "__length_hint__() should return >= 0"
                );
                return NULL;
            }
        }
        else if (state->phase == COLLECT_WAIT_INDEX) {
            item = collect_byte_index(state, resumed_value);
            if (item == NULL) {
                return NULL;
            }
            byte_index_ready = 1;
        }
        else {
            item = Py_NewRef(resumed_value);
        }
    }

    if (state->phase == COLLECT_WAIT_ITER) {
        if (
            (state->kind == COLLECT_LIST || state->kind == COLLECT_BYTES) &&
            Py_TYPE(state->iterable)->tp_iternext != adapter_reversed_next
        ) {
            state->phase = COLLECT_WAIT_HINT;
            Py_ssize_t hint = PyObject_LengthHint(state->iterable, 8);
            if (hint < 0) {
                return NULL;
            }
        }
        else {
            state->phase = COLLECT_WAIT_NEXT;
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
        if (
            !byte_index_ready &&
            (state->kind == COLLECT_BYTES || state->kind == COLLECT_BYTEARRAY)
        ) {
            state->phase = COLLECT_WAIT_INDEX;
            PyObject *index = collect_byte_index(state, item);
            Py_DECREF(item);
            if (index == NULL) {
                return NULL;
            }
            item = index;
        }
        byte_index_ready = 0;
        if (state->kind == COLLECT_DICT) {
            PyObject *single = PyTuple_Pack(1, item);
            if (single == NULL) {
                Py_DECREF(item);
                return NULL;
            }
            int status = PyDict_MergeFromSeq2(state->items, single, 1);
            Py_DECREF(single);
            if (status < 0) {
                Py_DECREF(item);
                return NULL;
            }
        }
        else if (
            state->kind == COLLECT_SET || state->kind == COLLECT_FROZENSET
        ) {
            if (PySet_Add(state->items, item) < 0) {
                Py_DECREF(item);
                return NULL;
            }
        }
        else if (PyList_Append(state->items, item) < 0) {
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

PyObject *
collect_iterable(PyObject *iterable, CollectKind kind)
{
    if (kind == COLLECT_SET || kind == COLLECT_FROZENSET) {
        return collect_set_iterable(iterable, kind);
    }
    CollectState state = {
        .iterable = iterable,
        .iterator = NULL,
        .items = kind == COLLECT_DICT
            ? PyDict_New()
            : (kind == COLLECT_SET || kind == COLLECT_FROZENSET)
                ? PySet_New(NULL)
                : PyList_New(0),
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
    PyObject *iterable;
    PyObject *iterator;
    CollectPhase phase;
} ListExtendState;

static const AleffAdapterVTable list_extend_vtable;

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
    copy->iterable = Py_NewRef(state->iterable);
    copy->iterator = Py_XNewRef(state->iterator);
    copy->phase = state->phase;
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
    Py_DECREF(state->iterable);
    Py_XDECREF(state->iterator);
    PyMem_Free(state);
}

static PyObject *
list_extend_continue(
    ListExtendState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
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
        else if (state->phase == COLLECT_WAIT_HINT) {
            Py_ssize_t hint = PyNumber_AsSsize_t(
                resumed_value, PyExc_OverflowError
            );
            if (hint < 0 && PyErr_Occurred()) {
                return NULL;
            }
            if (hint < 0) {
                PyErr_SetString(
                    PyExc_ValueError,
                    "__length_hint__() should return >= 0"
                );
                return NULL;
            }
        }
        else if (PyList_Append(state->receiver, resumed_value) < 0) {
            return NULL;
        }
    }

    if (state->iterator == NULL) {
        state->phase = COLLECT_WAIT_ITER;
        state->iterator = PyObject_GetIter(state->iterable);
        if (state->iterator == NULL) {
            return NULL;
        }
        state->phase = COLLECT_WAIT_HINT;
        if (PyObject_LengthHint(state->iterable, 8) < 0) {
            return NULL;
        }
    }

    for (;;) {
        state->phase = COLLECT_WAIT_NEXT;
        PyObject *item = Py_TYPE(state->iterator)->tp_iternext(state->iterator);
        if (item == NULL) {
            if (PyErr_Occurred()) {
                if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                    return NULL;
                }
                PyErr_Clear();
            }
            Py_RETURN_NONE;
        }
        int status = PyList_Append(state->receiver, item);
        Py_DECREF(item);
        if (status < 0) {
            return NULL;
        }
    }
}

static PyObject *
list_extend_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    ListExtendState *copy = list_extend_copy_state(raw_state);
    if (copy == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &list_extend_vtable, copy) < 0) {
        list_extend_free_state(copy);
        return NULL;
    }
    PyObject *result = list_extend_continue(copy, value, 1);
    adapter_leave(&frame);
    list_extend_free_state(copy);
    return result;
}

static const AleffAdapterVTable list_extend_vtable = {
    .copy_state = list_extend_copy_state,
    .free_state = list_extend_free_state,
    .resume = list_extend_resume,
};

PyObject *
adapter_list_extend(PyObject *self, PyObject *iterable)
{
    if (iterable == self || PyList_CheckExact(iterable) || PyTuple_CheckExact(iterable)) {
        Py_ssize_t size = PyList_GET_SIZE(self);
        if (PyList_SetSlice(self, size, size, iterable) < 0) {
            return NULL;
        }
        Py_RETURN_NONE;
    }
    ListExtendState state = {
        .receiver = self,
        .iterable = iterable,
        .iterator = NULL,
        .phase = COLLECT_WAIT_ITER,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &list_extend_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = list_extend_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.iterator);
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

PyObject *
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

typedef enum {
    SEQUENCE_SEARCH_WAIT_EQUAL,
    SEQUENCE_SEARCH_WAIT_NOT_FOUND_REPR,
} SequenceSearchPhase;

typedef struct {
    PyObject *receiver;
    PyObject *target;
    PyObject *item;
    Py_ssize_t index;
    Py_ssize_t stop;
    Py_ssize_t count;
    SequenceSearchKind kind;
    SequenceSearchPhase phase;
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
sequence_search_list_index_not_found(
    SequenceSearchState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
#if PY_VERSION_HEX < 0x030e0000
    PyObject *representation;
    if (is_resumed) {
        if (!PyUnicode_Check(resumed_value)) {
            PyErr_Format(
                PyExc_TypeError,
                "__repr__ returned non-string (type %.200s)",
                Py_TYPE(resumed_value)->tp_name
            );
            return NULL;
        }
        representation = Py_NewRef(resumed_value);
    }
    else {
        state->phase = SEQUENCE_SEARCH_WAIT_NOT_FOUND_REPR;
        representation = PyObject_Repr(state->target);
        if (representation == NULL) {
            return NULL;
        }
    }
    PyErr_Format(PyExc_ValueError, "%U is not in list", representation);
    Py_DECREF(representation);
#else
    (void)state;
    (void)resumed_value;
    (void)is_resumed;
    PyErr_SetString(PyExc_ValueError, "list.index(x): x not in list");
#endif
    return NULL;
}

static PyObject *
sequence_search_not_found(SequenceSearchState *state)
{
    if (state->kind == SEQUENCE_SEARCH_COUNT) {
        return PyLong_FromSsize_t(state->count);
    }
    if (state->kind == SEQUENCE_SEARCH_CONTAINS) {
        return Py_NewRef(Py_False);
    }
    if (state->kind == SEQUENCE_SEARCH_REMOVE) {
        PyErr_SetString(PyExc_ValueError, "list.remove(x): x not in list");
    }
    else if (PyList_Check(state->receiver)) {
        return sequence_search_list_index_not_found(state, NULL, 0);
    }
    else {
        PyErr_SetString(PyExc_ValueError, "tuple.index(x): x not in tuple");
    }
    return NULL;
}

static PyObject *
sequence_search_continue(
    SequenceSearchState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed &&
        state->phase == SEQUENCE_SEARCH_WAIT_NOT_FOUND_REPR) {
        return sequence_search_list_index_not_found(
            state, resumed_value, 1
        );
    }
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
            state->phase = SEQUENCE_SEARCH_WAIT_EQUAL;
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
                if (PyList_SetSlice(
                        state->receiver,
                        state->index,
                        state->index + 1,
                        NULL
                    ) < 0) {
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
        .phase = SEQUENCE_SEARCH_WAIT_EQUAL,
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

PyObject *
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

PyObject *
adapter_sequence_index(PyObject *self, PyObject *args)
{
    PyObject *target;
    Py_ssize_t start = 0;
    Py_ssize_t stop = PY_SSIZE_T_MAX;
    Py_ssize_t argument_count = PyTuple_GET_SIZE(args);
    for (Py_ssize_t index = 1; index < argument_count && index < 3; index++) {
        if (!PyIndex_Check(PyTuple_GET_ITEM(args, index))) {
            PyErr_SetString(
                PyExc_TypeError,
                "slice indices must be integers or have an __index__ method"
            );
            return NULL;
        }
    }
    if (!PyArg_ParseTuple(args, "O|nn:index", &target, &start, &stop)) {
        return NULL;
    }
    return sequence_search(self, target, start, stop, SEQUENCE_SEARCH_INDEX);
}

PyObject *
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

int
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

typedef struct {
    PyObject *receiver;
    PyObject *parts;
    Py_ssize_t index;
} ListReprState;

static void *
list_repr_copy_state(const void *raw_state)
{
    const ListReprState *state = raw_state;
    ListReprState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->receiver = Py_NewRef(state->receiver);
    copy->parts = PyList_GetSlice(
        state->parts, 0, PyList_GET_SIZE(state->parts)
    );
    copy->index = state->index;
    if (copy->parts == NULL) {
        Py_DECREF(copy->receiver);
        PyMem_Free(copy);
        return NULL;
    }
    return copy;
}

static void
list_repr_free_state(void *raw_state)
{
    ListReprState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->receiver);
    Py_DECREF(state->parts);
    PyMem_Free(state);
}

static PyObject *list_repr_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable list_repr_vtable = {
    .copy_state = list_repr_copy_state,
    .free_state = list_repr_free_state,
    .resume = list_repr_resume,
};

static PyObject *
list_repr_finish(ListReprState *state)
{
    PyObject *separator = PyUnicode_FromString(", ");
    if (separator == NULL) {
        return NULL;
    }
    PyObject *body = PyUnicode_Join(separator, state->parts);
    Py_DECREF(separator);
    if (body == NULL) {
        return NULL;
    }
    PyObject *result = PyUnicode_FromFormat("[%U]", body);
    Py_DECREF(body);
    return result;
}

static PyObject *
list_repr_continue(ListReprState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed) {
        if (!PyUnicode_Check(resumed_value)) {
            PyErr_Format(
                PyExc_TypeError,
                "__repr__ returned non-string (type %.200s)",
                Py_TYPE(resumed_value)->tp_name
            );
            return NULL;
        }
        if (PyList_Append(state->parts, resumed_value) < 0) {
            return NULL;
        }
        state->index++;
    }
    while (state->index < PyList_GET_SIZE(state->receiver)) {
        PyObject *item = PyList_GET_ITEM(state->receiver, state->index);
        PyObject *part = PyObject_Repr(item);
        if (part == NULL) {
            return NULL;
        }
        if (PyList_Append(state->parts, part) < 0) {
            Py_DECREF(part);
            return NULL;
        }
        Py_DECREF(part);
        state->index++;
    }
    return list_repr_finish(state);
}

static PyObject *
list_repr_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    ListReprState *state = list_repr_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    int recursive = Py_ReprEnter(state->receiver);
    if (recursive != 0) {
        list_repr_free_state(state);
        return recursive < 0
            ? NULL
            : PyUnicode_FromString("[...]");
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &list_repr_vtable, state) < 0) {
        Py_ReprLeave(state->receiver);
        list_repr_free_state(state);
        return NULL;
    }
    PyObject *result = list_repr_continue(state, value, 1);
    adapter_leave(&frame);
    Py_ReprLeave(state->receiver);
    list_repr_free_state(state);
    return result;
}

PyObject *
adapter_list_repr(PyObject *receiver)
{
    int recursive = Py_ReprEnter(receiver);
    if (recursive != 0) {
        return recursive < 0
            ? NULL
            : PyUnicode_FromString("[...]");
    }
    ListReprState state = {
        .receiver = receiver,
        .parts = PyList_New(0),
        .index = 0,
    };
    if (state.parts == NULL) {
        Py_ReprLeave(receiver);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &list_repr_vtable, &state) < 0) {
        Py_DECREF(state.parts);
        Py_ReprLeave(receiver);
        return NULL;
    }
    PyObject *result = list_repr_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_DECREF(state.parts);
    Py_ReprLeave(receiver);
    return result;
}

typedef struct {
    PyObject *receiver;
    Py_hash_t *hashes;
    Py_ssize_t size;
    Py_ssize_t index;
} TupleHashState;

hashfunc original_tuple_hash;

static void *
tuple_hash_copy_state(const void *raw_state)
{
    const TupleHashState *state = raw_state;
    TupleHashState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->receiver = Py_NewRef(state->receiver);
    copy->hashes = PyMem_Malloc((size_t)state->size * sizeof(*copy->hashes));
    if (copy->hashes == NULL && state->size != 0) {
        Py_DECREF(copy->receiver);
        PyMem_Free(copy);
        PyErr_NoMemory();
        return NULL;
    }
    if (state->size != 0) {
        memcpy(
            copy->hashes,
            state->hashes,
            (size_t)state->size * sizeof(*copy->hashes)
        );
    }
    return copy;
}

static void
tuple_hash_free_state(void *raw_state)
{
    TupleHashState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->receiver);
    PyMem_Free(state->hashes);
    PyMem_Free(state);
}

static PyObject *tuple_hash_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable tuple_hash_vtable = {
    .copy_state = tuple_hash_copy_state,
    .free_state = tuple_hash_free_state,
    .resume = tuple_hash_resume,
};

static PyObject *
tuple_hash_finish(TupleHashState *state)
{
    PyObject *proxy = PyTuple_New(state->size);
    if (proxy == NULL) {
        return NULL;
    }
    for (Py_ssize_t index = 0; index < state->size; index++) {
        PyObject *hash = PyLong_FromSsize_t(state->hashes[index]);
        if (hash == NULL) {
            Py_DECREF(proxy);
            return NULL;
        }
        PyTuple_SET_ITEM(proxy, index, hash);
    }
    Py_hash_t result = original_tuple_hash(proxy);
    Py_DECREF(proxy);
    return result == -1 ? NULL : PyLong_FromSsize_t(result);
}

static PyObject *
tuple_hash_continue(TupleHashState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed) {
        if (!PyLong_Check(resumed_value)) {
            PyErr_SetString(PyExc_TypeError, "__hash__ method should return an integer");
            return NULL;
        }
        state->hashes[state->index] = PyObject_Hash(resumed_value);
        if (state->hashes[state->index] == -1) {
            return NULL;
        }
        state->index++;
    }
    while (state->index < state->size) {
        Py_hash_t hash = PyObject_Hash(
            PyTuple_GET_ITEM(state->receiver, state->index)
        );
        if (hash == -1) {
            return NULL;
        }
        state->hashes[state->index++] = hash;
    }
    return tuple_hash_finish(state);
}

static PyObject *
tuple_hash_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    TupleHashState *state = tuple_hash_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &tuple_hash_vtable, state) < 0) {
        tuple_hash_free_state(state);
        return NULL;
    }
    PyObject *result = tuple_hash_continue(state, value, 1);
    adapter_leave(&frame);
    tuple_hash_free_state(state);
    return result;
}

 Py_hash_t
adapter_tuple_hash(PyObject *receiver)
{
    TupleHashState state = {
        .receiver = receiver,
        .hashes = NULL,
        .size = PyTuple_GET_SIZE(receiver),
        .index = 0,
    };
    state.hashes = PyMem_Malloc((size_t)state.size * sizeof(*state.hashes));
    if (state.hashes == NULL && state.size != 0) {
        PyErr_NoMemory();
        return -1;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &tuple_hash_vtable, &state) < 0) {
        PyMem_Free(state.hashes);
        return -1;
    }
    PyObject *result = tuple_hash_continue(&state, NULL, 0);
    adapter_leave(&frame);
    PyMem_Free(state.hashes);
    if (result == NULL) {
        return -1;
    }
    Py_hash_t hash = PyLong_AsSsize_t(result);
    Py_DECREF(result);
    return hash;
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

richcmpfunc original_list_richcompare = NULL;
richcmpfunc original_tuple_richcompare = NULL;

PyObject *
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
    if ((operation == Py_EQ || operation == Py_NE) &&
        sequence_compare_size(left) != sequence_compare_size(right)) {
        return PyBool_FromLong(operation == Py_NE);
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
    SORT_ADAPTER_LIST,
    SORT_ADAPTER_BUILTIN,
} SortAdapterKind;

typedef enum {
    SORT_ADAPTER_WAIT_COLLECT,
    SORT_ADAPTER_WAIT_REVERSE,
    SORT_ADAPTER_WAIT_KEY,
    SORT_ADAPTER_WAIT_COMPARE,
} SortAdapterPhase;

typedef struct {
    SortAdapterKind kind;
    SortAdapterPhase phase;
    PyObject *receiver;
    PyObject *iterable;
    PyObject *items;
    PyObject *key_function;
    PyObject *reverse_object;
    AleffSortEngine *engine;
    int reverse;
    int reverse_ready;
    int detached;
    int mutated;
    int snapshot_state;
} SortAdapterState;

static int
sort_receiver_mutated_unlocked(PyObject *receiver)
{
    PyListObject *list = (PyListObject *)receiver;
    return list->allocated != -1;
}

static int
sort_receiver_mutated(PyObject *receiver)
{
    int mutated;
#if PY_VERSION_HEX >= 0x030d0000
    Py_BEGIN_CRITICAL_SECTION(receiver);
#endif
    mutated = sort_receiver_mutated_unlocked(receiver);
#if PY_VERSION_HEX >= 0x030d0000
    Py_END_CRITICAL_SECTION();
#endif
    return mutated;
}

static PyObject *
sort_receiver_detach(PyObject *receiver)
{
    PyObject *items;
#if PY_VERSION_HEX >= 0x030d0000
    Py_BEGIN_CRITICAL_SECTION(receiver);
#endif
    Py_ssize_t size = PyList_GET_SIZE(receiver);
    items = PyList_GetSlice(receiver, 0, size);
    if (items != NULL && PyList_SetSlice(receiver, 0, size, NULL) < 0) {
        Py_CLEAR(items);
    }
    if (items != NULL) {
        ((PyListObject *)receiver)->allocated = -1;
    }
#if PY_VERSION_HEX >= 0x030d0000
    Py_END_CRITICAL_SECTION();
#endif
    return items;
}

static int
sort_receiver_reset(PyObject *receiver)
{
    int status;
#if PY_VERSION_HEX >= 0x030d0000
    Py_BEGIN_CRITICAL_SECTION(receiver);
#endif
    status = PyList_SetSlice(
        receiver,
        0,
        PyList_GET_SIZE(receiver),
        NULL
    );
    if (status == 0) {
        ((PyListObject *)receiver)->allocated = -1;
    }
#if PY_VERSION_HEX >= 0x030d0000
    Py_END_CRITICAL_SECTION();
#endif
    return status;
}

static int
sort_receiver_restore(
    PyObject *receiver,
    PyObject *items,
    int *mutated
)
{
    int status;
#if PY_VERSION_HEX >= 0x030d0000
    Py_BEGIN_CRITICAL_SECTION(receiver);
#endif
    if (mutated != NULL && sort_receiver_mutated_unlocked(receiver)) {
        *mutated = 1;
    }
    PyListObject *list = (PyListObject *)receiver;
    if (list->allocated == -1) {
        list->allocated = 0;
    }
    status = PyList_SetSlice(
        receiver,
        0,
        PyList_GET_SIZE(receiver),
        items
    );
#if PY_VERSION_HEX >= 0x030d0000
    Py_END_CRITICAL_SECTION();
#endif
    return status;
}

static void *
sort_adapter_copy_state(const void *raw_state)
{
    const SortAdapterState *state = raw_state;
    SortAdapterState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->receiver = Py_XNewRef(state->receiver);
    copy->iterable = Py_XNewRef(state->iterable);
    copy->items = Py_XNewRef(state->items);
    copy->key_function = Py_XNewRef(state->key_function);
    copy->reverse_object = Py_NewRef(state->reverse_object);
    copy->engine = state->engine == NULL
        ? NULL
        : aleff_sort_engine_copy(state->engine);
    if (state->engine != NULL && copy->engine == NULL) {
        Py_XDECREF(copy->receiver);
        Py_XDECREF(copy->iterable);
        Py_XDECREF(copy->items);
        Py_XDECREF(copy->key_function);
        Py_DECREF(copy->reverse_object);
        PyMem_Free(copy);
        return NULL;
    }
    if (state->kind == SORT_ADAPTER_LIST && state->detached &&
        !state->snapshot_state && sort_receiver_mutated(state->receiver)) {
        copy->mutated = 1;
    }
    copy->snapshot_state = 1;
    return copy;
}

static void
sort_adapter_free_state(void *raw_state)
{
    SortAdapterState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->receiver);
    Py_XDECREF(state->iterable);
    Py_XDECREF(state->items);
    Py_XDECREF(state->key_function);
    Py_DECREF(state->reverse_object);
    aleff_sort_engine_free(state->engine);
    PyMem_Free(state);
}

static PyObject *sort_adapter_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable sort_adapter_vtable = {
    .copy_state = sort_adapter_copy_state,
    .free_state = sort_adapter_free_state,
    .resume = sort_adapter_resume,
};


static void
sort_adapter_restore_after_error(SortAdapterState *state)
{
    if (state->engine == NULL &&
        !(state->kind == SORT_ADAPTER_LIST && state->detached)) {
        return;
    }
    PyObject *exception = PyErr_Occurred()
        ? PyErr_GetRaisedException()
        : NULL;
    if (state->engine != NULL) {
        aleff_sort_engine_abort(state->engine);
    }
    if (state->kind == SORT_ADAPTER_LIST && state->detached) {
        PyObject *items = state->engine == NULL
            ? Py_XNewRef(state->items)
            : aleff_sort_engine_materialize(state->engine);
        if (items != NULL) {
            sort_receiver_restore(state->receiver, items, NULL);
            Py_DECREF(items);
        }
        state->detached = 0;
    }
    if (exception != NULL) {
        PyErr_SetRaisedException(exception);
    }
}

static PyObject *
sort_adapter_finish(SortAdapterState *state)
{
    PyObject *items = aleff_sort_engine_materialize(state->engine);
    if (items == NULL) {
        return NULL;
    }
    if (state->kind == SORT_ADAPTER_BUILTIN) {
        return items;
    }
    int mutated = state->mutated;
    int status = sort_receiver_restore(state->receiver, items, &mutated);
    Py_DECREF(items);
    if (status < 0) {
        return NULL;
    }
    state->detached = 0;
    if (mutated) {
        PyErr_SetString(PyExc_ValueError, "list modified during sort");
        return NULL;
    }
    return Py_NewRef(Py_None);
}

static PyObject *
sort_adapter_continue(
    SortAdapterState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (resumed_value == NULL) {
            return NULL;
        }
        switch (state->phase) {
            case SORT_ADAPTER_WAIT_COLLECT:
                if (!PyList_Check(resumed_value)) {
                    PyErr_SetString(
                        PyExc_RuntimeError,
                        "sorted collector returned a non-list"
                    );
                    return NULL;
                }
                Py_XSETREF(state->items, Py_NewRef(resumed_value));
                break;
            case SORT_ADAPTER_WAIT_REVERSE:
                state->reverse = PyObject_IsTrue(resumed_value);
                if (state->reverse < 0) {
                    return NULL;
                }
                state->reverse_ready = 1;
                break;
            case SORT_ADAPTER_WAIT_KEY:
                if (aleff_sort_engine_accept_key(
                        state->engine,
                        resumed_value
                    ) < 0) {
                    return NULL;
                }
                break;
            case SORT_ADAPTER_WAIT_COMPARE: {
                int comparison = PyObject_IsTrue(resumed_value);
                if (comparison < 0 ||
                    aleff_sort_engine_accept_lt(
                        state->engine,
                        comparison
                    ) < 0) {
                    return NULL;
                }
                break;
            }
        }
    }

    if (state->kind == SORT_ADAPTER_BUILTIN && state->items == NULL) {
        state->phase = SORT_ADAPTER_WAIT_COLLECT;
        state->items = collect_iterable(state->iterable, COLLECT_LIST);
        if (state->items == NULL) {
            return NULL;
        }
    }

    if (!state->reverse_ready) {
        state->phase = SORT_ADAPTER_WAIT_REVERSE;
        state->reverse = PyObject_IsTrue(state->reverse_object);
        if (state->reverse < 0) {
            return NULL;
        }
        state->reverse_ready = 1;
    }

    if (state->engine == NULL) {
        if (state->kind == SORT_ADAPTER_LIST) {
            state->items = sort_receiver_detach(state->receiver);
            if (state->items == NULL) {
                return NULL;
            }
            state->detached = 1;
        }
        state->engine = aleff_sort_engine_new(
            state->items,
            state->key_function != NULL,
            state->reverse
        );
        if (state->engine == NULL) {
            return NULL;
        }
    }

    for (;;) {
        AleffSortRequest request;
        int status = aleff_sort_engine_advance(state->engine, &request);
        if (status < 0) {
            return NULL;
        }
        if (status == 0) {
            return sort_adapter_finish(state);
        }
        if (request.kind == ALEFF_SORT_REQUEST_NONE) {
            continue;
        }
        if (request.kind == ALEFF_SORT_REQUEST_KEY) {
            state->phase = SORT_ADAPTER_WAIT_KEY;
            PyObject *key = PyObject_CallOneArg(
                state->key_function,
                request.left
            );
            if (key == NULL) {
                return NULL;
            }
            status = aleff_sort_engine_accept_key(state->engine, key);
            Py_DECREF(key);
            if (status < 0) {
                return NULL;
            }
        }
        else {
            state->phase = SORT_ADAPTER_WAIT_COMPARE;
            int comparison = PyObject_RichCompareBool(
                request.left,
                request.right,
                Py_LT
            );
            if (comparison < 0 ||
                aleff_sort_engine_accept_lt(
                    state->engine,
                    comparison
                ) < 0) {
                return NULL;
            }
        }
    }
}

static PyObject *
sort_adapter_resume(const void *raw_state, PyObject *value)
{
    SortAdapterState *state = sort_adapter_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    if (state->kind == SORT_ADAPTER_LIST && state->detached) {
        if (sort_receiver_reset(state->receiver) < 0) {
            sort_adapter_free_state(state);
            return NULL;
        }
        state->snapshot_state = 0;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &sort_adapter_vtable, state) < 0) {
        sort_adapter_restore_after_error(state);
        sort_adapter_free_state(state);
        return NULL;
    }
    PyObject *result = sort_adapter_continue(state, value, 1);
    adapter_leave(&frame);
    if (result == NULL) {
        sort_adapter_restore_after_error(state);
    }
    sort_adapter_free_state(state);
    return result;
}

PyObject *
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
    SortAdapterState state = {
        .kind = SORT_ADAPTER_LIST,
        .phase = SORT_ADAPTER_WAIT_REVERSE,
        .receiver = self,
        .iterable = NULL,
        .items = NULL,
        .key_function = key_function,
        .reverse_object = reverse_object,
        .engine = NULL,
        .reverse = 0,
        .reverse_ready = 0,
        .detached = 0,
        .mutated = 0,
        .snapshot_state = 0,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &sort_adapter_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = sort_adapter_continue(&state, NULL, 0);
    adapter_leave(&frame);
    if (result == NULL) {
        sort_adapter_restore_after_error(&state);
    }
    Py_XDECREF(state.items);
    aleff_sort_engine_free(state.engine);
    return result;
}

typedef struct {
    PyObject *components[3];
    Py_hash_t hashes[3];
    int index;
} SliceHashState;

hashfunc original_slice_hash;
PyObject *original_slice_indices;

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

 Py_hash_t
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


PyObject *
adapter_sorted(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    Py_ssize_t positional_count = PyTuple_GET_SIZE(args);
    if (positional_count != 1) {
        PyErr_Format(
            PyExc_TypeError,
            "sorted expected 1 argument, got %zd",
            positional_count
        );
        return NULL;
    }
    static char *keywords[] = {"", "key", "reverse", NULL};
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
    if (key_function == Py_None) {
        key_function = NULL;
    }
    SortAdapterState state = {
        .kind = SORT_ADAPTER_BUILTIN,
        .phase = SORT_ADAPTER_WAIT_COLLECT,
        .receiver = NULL,
        .iterable = iterable,
        .items = NULL,
        .key_function = key_function,
        .reverse_object = reverse_object,
        .engine = NULL,
        .reverse = 0,
        .reverse_ready = 0,
        .detached = 0,
        .mutated = 0,
        .snapshot_state = 0,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &sort_adapter_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = sort_adapter_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.items);
    aleff_sort_engine_free(state.engine);
    return result;
}

initproc original_list_init;
vectorcallfunc original_list_vectorcall;
newfunc original_tuple_new;
vectorcallfunc original_tuple_vectorcall;
initproc original_dict_init;
vectorcallfunc original_dict_vectorcall;
initproc original_set_init;
vectorcallfunc original_set_vectorcall;
newfunc original_frozenset_new;
vectorcallfunc original_frozenset_vectorcall;
newfunc original_bytes_new;

PyObject *
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
        ((kind == COLLECT_SET || kind == COLLECT_FROZENSET) &&
            PyAnySet_Check(args[0])) ||
        (kind == COLLECT_DICT && PyMapping_Check(args[0]))
    ) {
        return original(callable, args, nargsf, kwnames);
    }
    return collect_iterable(args[0], kind);
}

PyObject *
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

PyObject *
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

int
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

PyObject *
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
    RANGE_SEARCH_WAIT_EQUAL,
    RANGE_SEARCH_READY,
} RangeSearchPhase;

typedef struct {
    PyObject *receiver;
    PyObject *target;
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
        int equal = PyObject_IsTrue(resumed_value);
        if (equal < 0) {
            return NULL;
        }
        state->phase = RANGE_SEARCH_READY;
        if (equal) {
            if (state->find_index) {
                return PyLong_FromSsize_t(state->index - 1);
            }
            state->count++;
        }
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
                PyErr_SetString(
                    PyExc_ValueError,
                    "sequence.index(x): x not in sequence"
                );
                return NULL;
            }
            return PyLong_FromSsize_t(state->count);
        }
        state->index++;
        state->phase = RANGE_SEARCH_WAIT_EQUAL;
        int equal = PyObject_RichCompareBool(item, state->target, Py_EQ);
        Py_DECREF(item);
        if (equal < 0) {
            return NULL;
        }
        state->phase = RANGE_SEARCH_READY;
        if (equal) {
            if (state->find_index) {
                return PyLong_FromSsize_t(state->index - 1);
            }
            state->count++;
        }
    }
}

static PyObject *
range_exact_int_index(PyObject *self, PyObject *target)
{
    int contains = PySequence_Contains(self, target);
    if (contains < 0) {
        return NULL;
    }
    if (!contains) {
#if PY_VERSION_HEX < 0x030e0000
        PyErr_Format(PyExc_ValueError, "%R is not in range", target);
#else
        PyErr_SetString(PyExc_ValueError, "range.index(x): x not in range");
#endif
        return NULL;
    }
    PyObject *start = PyObject_GetAttrString(self, "start");
    PyObject *step = PyObject_GetAttrString(self, "step");
    if (start == NULL || step == NULL) {
        Py_XDECREF(start);
        Py_XDECREF(step);
        return NULL;
    }
    PyObject *difference = PyNumber_Subtract(target, start);
    Py_DECREF(start);
    if (difference == NULL) {
        Py_DECREF(step);
        return NULL;
    }
    PyObject *index = PyNumber_FloorDivide(difference, step);
    Py_DECREF(difference);
    Py_DECREF(step);
    return index;
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
    if (PyLong_CheckExact(target) || PyBool_Check(target)) {
        int contains = PySequence_Contains(self, target);
        if (contains < 0) {
            return NULL;
        }
        return PyLong_FromLong(contains);
    }
    RangeSearchState state = {
        .receiver = self,
        .target = target,
        .iterator = NULL,
        .index = 0,
        .count = 0,
        .find_index = 0,
        .phase = RANGE_SEARCH_READY,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &range_search_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = range_search_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.iterator);
    return result;
}

static PyObject *
adapter_range_index(PyObject *self, PyObject *target)
{
    if (PyLong_CheckExact(target) || PyBool_Check(target)) {
        return range_exact_int_index(self, target);
    }
    RangeSearchState state = {
        .receiver = self,
        .target = target,
        .iterator = NULL,
        .index = 0,
        .count = 0,
        .find_index = 1,
        .phase = RANGE_SEARCH_READY,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &range_search_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = range_search_continue(&state, NULL, 0);
    adapter_leave(&frame);
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
    PyObject *length;
    PyObject *components[3];
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
    copy->length = Py_XNewRef(state->length);
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
    Py_XDECREF(state->length);
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
    PyObject *result = PyObject_CallFunctionObjArgs(
        original_slice_indices,
        normalized,
        state->length,
        NULL
    );
    Py_DECREF(normalized);
    return result;
}

static int
slice_component_slot(int component)
{
    static const int order[] = {2, 0, 1};
    return order[component];
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
            Py_XSETREF(state->length, length);
        }
        else {
            PyObject *component = PyNumber_Index(resumed_value);
            if (component == NULL) {
                return NULL;
            }
            int slot = slice_component_slot(state->component);
            Py_XSETREF(state->components[slot], component);
            state->component++;
        }
    }
    if (!is_resumed) {
        state->phase = SLICE_INDICES_WAIT_LENGTH;
        PyObject *length = PyNumber_Index(state->length_object);
        if (length == NULL) {
            return NULL;
        }
        state->length = length;
    }
    while (state->component < 3) {
        int slot = slice_component_slot(state->component);
        PyObject *value = ((PySliceObject *)state->slice)->start;
        if (slot == 1) value = ((PySliceObject *)state->slice)->stop;
        if (slot == 2) value = ((PySliceObject *)state->slice)->step;
        if (value == Py_None) {
            state->component++;
            continue;
        }
        if (!PyIndex_Check(value)) {
            PyErr_SetString(
                PyExc_TypeError,
                "slice indices must be integers or None or have an __index__ method"
            );
            return NULL;
        }
        state->phase = SLICE_INDICES_WAIT_COMPONENT;
        PyObject *component = PyNumber_Index(value);
        if (component == NULL) {
            return NULL;
        }
        state->components[slot] = component;
        state->component++;
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
    if (
        PyLong_Check(length_object) &&
        (slice->start == Py_None || PyLong_Check(slice->start)) &&
        (slice->stop == Py_None || PyLong_Check(slice->stop)) &&
        (slice->step == Py_None || PyLong_Check(slice->step))
    ) {
        return PyObject_CallFunctionObjArgs(
            original_slice_indices,
            self,
            length_object,
            NULL
        );
    }
    SliceIndicesState state = {
        .slice = self,
        .length_object = length_object,
        .length = NULL,
        .components = {NULL, NULL, NULL},
        .component = 0,
        .phase = SLICE_INDICES_WAIT_LENGTH,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &slice_indices_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = slice_indices_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.length);
    for (int i = 0; i < 3; i++) Py_XDECREF(state.components[i]);
    (void)slice;
    return result;
}


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
    PyObject *type_dict = PyType_GetDict(type);
    if (type_dict == NULL) {
        return -1;
    }
    PyObject *original = PyDict_GetItemString(type_dict, name);
    if (original != NULL && Py_IS_TYPE(original, &PyMethodDescr_Type)) {
        method->ml_doc = ((PyMethodDescrObject *)original)->d_method->ml_doc;
    }
    PyObject *descriptor = (method->ml_flags & METH_CLASS) != 0
        ? PyDescr_NewClassMethod(type, method)
        : PyDescr_NewMethod(type, method);
    if (descriptor == NULL) {
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
    if (original_slice_indices == NULL) {
        PyObject *slice_dict = PyType_GetDict(&PySlice_Type);
        PyObject *descriptor = slice_dict == NULL
            ? NULL : PyDict_GetItemString(slice_dict, "indices");
        if (descriptor == NULL) {
            PyErr_SetString(PyExc_RuntimeError, "cannot access slice.indices");
            return -1;
        }
        original_slice_indices = Py_NewRef(descriptor);
    }
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
