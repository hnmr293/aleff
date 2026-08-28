PyAPI_FUNC(int) _PySet_NextEntry(
    PyObject *set, Py_ssize_t *pos, PyObject **key, Py_hash_t *hash
);

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
    SET_OP_UPDATE,
    SET_OP_INTERSECTION_UPDATE,
    SET_OP_DIFFERENCE_UPDATE,
    SET_OP_SYMMETRIC_DIFFERENCE_UPDATE,
    SET_OP_UNION,
    SET_OP_INTERSECTION,
    SET_OP_DIFFERENCE,
    SET_OP_SYMMETRIC_DIFFERENCE,
    SET_OP_ISDISJOINT,
    SET_OP_ISSUBSET,
    SET_OP_ISSUPERSET,
} SetOperationKind;

typedef struct {
    PyObject *receiver;
    PyObject *args;
    PyObject *result;
    PyObject *items;
    PyObject *current_item;
    PyObject *candidate_item;
    Py_ssize_t index;
    Py_ssize_t candidate_position;
    Py_ssize_t receiver_position;
    Py_hash_t item_hash;
    Py_hash_t candidate_hash;
    SetOperationKind operation;
    int frozen;
    int relation_phase;
} SetOperationState;

static const AleffAdapterVTable set_operation_vtable;

static void *
set_operation_copy_state(const void *raw_state)
{
    const SetOperationState *state = raw_state;
    SetOperationState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->receiver = Py_NewRef(state->receiver);
    copy->args = Py_NewRef(state->args);
    copy->result = Py_XNewRef(state->result);
    copy->items = Py_XNewRef(state->items);
    copy->current_item = Py_XNewRef(state->current_item);
    copy->candidate_item = Py_XNewRef(state->candidate_item);
    copy->index = state->index;
    copy->candidate_position = state->candidate_position;
    copy->receiver_position = state->receiver_position;
    copy->item_hash = state->item_hash;
    copy->candidate_hash = state->candidate_hash;
    copy->operation = state->operation;
    copy->frozen = state->frozen;
    copy->relation_phase = state->relation_phase;
    return copy;
}

static void
set_operation_free_state(void *raw_state)
{
    SetOperationState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->receiver);
    Py_DECREF(state->args);
    Py_XDECREF(state->result);
    Py_XDECREF(state->items);
    Py_XDECREF(state->current_item);
    Py_XDECREF(state->candidate_item);
    PyMem_Free(state);
}

static int
set_operation_is_mutating(SetOperationKind operation)
{
    return operation <= SET_OP_SYMMETRIC_DIFFERENCE_UPDATE;
}

static int
set_operation_is_relation(SetOperationKind operation)
{
    return operation >= SET_OP_ISDISJOINT;
}

enum {
    SET_RELATION_NONE,
    SET_RELATION_WAIT_ITEM_HASH,
    SET_RELATION_WAIT_CANDIDATE_HASH,
    SET_RELATION_WAIT_EQUAL,
};

static int
set_normalize_hash(PyObject *value, Py_hash_t *hash)
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
set_relation_continue(
    SetOperationState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    int equal = -1;
    if (is_resumed) {
        switch (state->relation_phase) {
            case SET_RELATION_WAIT_ITEM_HASH:
                if (set_normalize_hash(resumed_value, &state->item_hash) < 0) {
                    return NULL;
                }
                break;
            case SET_RELATION_WAIT_CANDIDATE_HASH:
                if (set_normalize_hash(
                        resumed_value, &state->candidate_hash
                    ) < 0) {
                    return NULL;
                }
                break;
            case SET_RELATION_WAIT_EQUAL:
                equal = PyObject_IsTrue(resumed_value);
                if (equal < 0) {
                    return NULL;
                }
                break;
            default:
                PyErr_SetString(PyExc_RuntimeError, "invalid set relation resume phase");
                return NULL;
        }
    }

    int subset = state->operation == SET_OP_ISSUBSET;
    int disjoint = state->operation == SET_OP_ISDISJOINT;
    for (;;) {
        if (state->current_item == NULL) {
            if (subset) {
                PyObject *current;
                if (!_PySet_NextEntry(
                        state->receiver,
                        &state->receiver_position,
                        &current,
                        &state->item_hash
                    )) {
                    return PyBool_FromLong(1);
                }
                state->current_item = Py_NewRef(current);
            }
            else {
                if (state->items == NULL ||
                    state->index >= PyList_GET_SIZE(state->items)) {
                    return PyBool_FromLong(1);
                }
                state->current_item = Py_NewRef(
                    PyList_GET_ITEM(state->items, state->index)
                );
                state->relation_phase = SET_RELATION_WAIT_ITEM_HASH;
                state->item_hash = PyObject_Hash(state->current_item);
                if (state->item_hash == -1) {
                    return NULL;
                }
            }
            state->candidate_position = 0;
            state->candidate_hash = -1;
            state->candidate_item = NULL;
        }

        while (state->candidate_item == NULL) {
            PyObject *candidate;
            if (subset) {
                if (state->candidate_position >= PyList_GET_SIZE(state->items)) {
                    if (disjoint) {
                        Py_CLEAR(state->current_item);
                        state->index++;
                        break;
                    }
                    Py_CLEAR(state->current_item);
                    Py_RETURN_FALSE;
                }
                candidate = PyList_GET_ITEM(
                    state->items, state->candidate_position++
                );
                state->candidate_item = Py_NewRef(candidate);
                state->relation_phase = SET_RELATION_WAIT_CANDIDATE_HASH;
                state->candidate_hash = PyObject_Hash(candidate);
                if (state->candidate_hash == -1) {
                    return NULL;
                }
            }
            else if (!_PySet_NextEntry(
                         state->receiver,
                         &state->candidate_position,
                         &candidate,
                     &state->candidate_hash
                 )) {
                if (disjoint) {
                    Py_CLEAR(state->current_item);
                    state->index++;
                    break;
                }
                Py_CLEAR(state->current_item);
                Py_RETURN_FALSE;
            }
            else {
                state->candidate_item = Py_NewRef(candidate);
            }
            if (candidate == state->current_item) {
                equal = 1;
                break;
            }
            if (state->candidate_hash != state->item_hash) {
                Py_CLEAR(state->candidate_item);
                continue;
            }
            state->relation_phase = SET_RELATION_WAIT_EQUAL;
            equal = PyObject_RichCompareBool(candidate, state->current_item, Py_EQ);
            if (equal < 0) {
                return NULL;
            }
            break;
        }

        if (state->candidate_item != NULL && equal < 0 &&
            state->relation_phase == SET_RELATION_WAIT_CANDIDATE_HASH) {
            if (state->candidate_hash != state->item_hash) {
                Py_CLEAR(state->candidate_item);
                continue;
            }
            state->relation_phase = SET_RELATION_WAIT_EQUAL;
            equal = PyObject_RichCompareBool(
                state->candidate_item, state->current_item, Py_EQ
            );
            if (equal < 0) {
                return NULL;
            }
        }
        if (state->candidate_item != NULL && equal) {
            if (disjoint) {
                Py_CLEAR(state->candidate_item);
                Py_CLEAR(state->current_item);
                Py_RETURN_FALSE;
            }
            Py_CLEAR(state->candidate_item);
            Py_CLEAR(state->current_item);
            if (!subset) {
                state->index++;
            }
            equal = -1;
            continue;
        }
        if (state->candidate_item != NULL) {
            Py_CLEAR(state->candidate_item);
            equal = -1;
            continue;
        }
    }
}

static PyObject *
set_operation_apply(SetOperationState *state, PyObject *items)
{
    if (state->operation == SET_OP_ISDISJOINT) {
        Py_ssize_t size = PyList_GET_SIZE(items);
        for (Py_ssize_t i = 0; i < size; i++) {
            int contains = PySet_Contains(
                state->receiver,
                PyList_GET_ITEM(items, i)
            );
            if (contains < 0) {
                return NULL;
            }
            if (contains) {
                Py_RETURN_FALSE;
            }
        }
        Py_RETURN_TRUE;
    }

    if (set_operation_is_relation(state->operation)) {
        PyErr_SetString(PyExc_RuntimeError, "set relation must use element adapter");
        return NULL;
    }
    PyObject *other = state->frozen
        ? PyFrozenSet_New(items)
        : PySet_New(items);
    if (other == NULL) {
        return NULL;
    }

    if (set_operation_is_mutating(state->operation)) {
        PyObject *mutation;
        switch (state->operation) {
            case SET_OP_UPDATE:
                mutation = PyNumber_InPlaceOr(state->receiver, other);
                break;
            case SET_OP_INTERSECTION_UPDATE:
                mutation = PyNumber_InPlaceAnd(state->receiver, other);
                break;
            case SET_OP_DIFFERENCE_UPDATE:
                mutation = PyNumber_InPlaceSubtract(state->receiver, other);
                break;
            default:
                mutation = PyNumber_InPlaceXor(state->receiver, other);
                break;
        }
        Py_DECREF(other);
        if (mutation == NULL) {
            return NULL;
        }
        Py_DECREF(mutation);
        Py_RETURN_NONE;
    }

    PyObject *next;
    switch (state->operation) {
        case SET_OP_UNION:
            next = PyNumber_Or(state->result, other);
            break;
        case SET_OP_INTERSECTION:
            next = PyNumber_And(state->result, other);
            break;
        case SET_OP_DIFFERENCE:
            next = PyNumber_Subtract(state->result, other);
            break;
        default:
            next = PyNumber_Xor(state->result, other);
            break;
    }
    Py_DECREF(other);
    Py_CLEAR(state->result);
    state->result = next;
    return next == NULL ? NULL : Py_NewRef(Py_None);
}

static PyObject *
set_operation_continue(
    SetOperationState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (set_operation_is_relation(state->operation)) {
        if (state->items == NULL) {
            if (is_resumed) {
                state->items = Py_NewRef(resumed_value);
            }
            else {
                state->items = collect_iterable(
                    PyTuple_GET_ITEM(state->args, 0), COLLECT_LIST
                );
                if (state->items == NULL) {
                    return NULL;
                }
            }
        }
        return set_relation_continue(
            state,
            is_resumed && state->relation_phase != SET_RELATION_NONE
                ? resumed_value : NULL,
            is_resumed && state->relation_phase != SET_RELATION_NONE
        );
    }
    PyObject *items = is_resumed ? Py_NewRef(resumed_value) : NULL;
    Py_ssize_t count = PyTuple_GET_SIZE(state->args);
    while (state->index < count) {
        if (items == NULL) {
            items = collect_iterable(
                PyTuple_GET_ITEM(state->args, state->index),
                COLLECT_LIST
            );
            if (items == NULL) {
                return NULL;
            }
        }
        state->index++;
        PyObject *applied = set_operation_apply(state, items);
        Py_DECREF(items);
        items = NULL;
        if (applied == NULL) {
            return NULL;
        }
        if (set_operation_is_relation(state->operation)) {
            return applied;
        }
        Py_DECREF(applied);
    }
    if (set_operation_is_mutating(state->operation)) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(state->result);
}

static PyObject *
set_operation_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    SetOperationState *state = set_operation_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &set_operation_vtable, state) < 0) {
        return NULL;
    }
    PyObject *result = set_operation_continue(state, value, 1);
    adapter_leave(&frame);
    set_operation_free_state(state);
    return result;
}

static const AleffAdapterVTable set_operation_vtable = {
    .copy_state = set_operation_copy_state,
    .free_state = set_operation_free_state,
    .resume = set_operation_resume,
};

static PyObject *
adapter_set_operation(
    PyObject *self,
    PyObject *args,
    SetOperationKind operation,
    int frozen
)
{
    Py_ssize_t count = PyTuple_GET_SIZE(args);
    if (
        (operation == SET_OP_SYMMETRIC_DIFFERENCE && count != 1) ||
        (operation == SET_OP_SYMMETRIC_DIFFERENCE_UPDATE && count != 1) ||
        (set_operation_is_relation(operation) && count != 1)
    ) {
        PyErr_Format(
            PyExc_TypeError,
            "set operation received invalid argument count: %zd",
            count
        );
        return NULL;
    }
    if (count == 0) {
        if (set_operation_is_mutating(operation)) {
            Py_RETURN_NONE;
        }
        return frozen ? PyFrozenSet_New(self) : PySet_New(self);
    }
    SetOperationState state = {
        .receiver = self,
        .args = args,
        .result = set_operation_is_mutating(operation)
            ? NULL
            : Py_NewRef(self),
        .items = NULL,
        .current_item = NULL,
        .candidate_item = NULL,
        .index = 0,
        .candidate_position = 0,
        .receiver_position = 0,
        .item_hash = -1,
        .candidate_hash = -1,
        .operation = operation,
        .frozen = frozen,
        .relation_phase = SET_RELATION_NONE,
    };
    if (!set_operation_is_mutating(operation) && state.result == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &set_operation_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = set_operation_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.result);
    return result;
}

static PyObject *adapter_set_update(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_UPDATE, 0);
}

static PyObject *adapter_set_intersection_update(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_INTERSECTION_UPDATE, 0);
}

static PyObject *adapter_set_difference_update(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_DIFFERENCE_UPDATE, 0);
}

static PyObject *adapter_set_symmetric_difference_update(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_SYMMETRIC_DIFFERENCE_UPDATE, 0);
}

static PyObject *adapter_set_union(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_UNION, 0);
}

static PyObject *adapter_set_intersection(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_INTERSECTION, 0);
}

static PyObject *adapter_set_difference(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_DIFFERENCE, 0);
}

static PyObject *adapter_set_symmetric_difference(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_SYMMETRIC_DIFFERENCE, 0);
}

static PyObject *adapter_frozenset_union(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_UNION, 1);
}

static PyObject *adapter_frozenset_intersection(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_INTERSECTION, 1);
}

static PyObject *adapter_frozenset_difference(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_DIFFERENCE, 1);
}

static PyObject *adapter_frozenset_symmetric_difference(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_SYMMETRIC_DIFFERENCE, 1);
}

static PyObject *adapter_set_isdisjoint(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_ISDISJOINT, 0);
}

static PyObject *adapter_set_issubset(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_ISSUBSET, 0);
}

static PyObject *adapter_set_issuperset(PyObject *self, PyObject *args)
{
    return adapter_set_operation(self, args, SET_OP_ISSUPERSET, 0);
}

static PyMethodDef containers_set_update_method = {
    .ml_name = "update",
    .ml_meth = adapter_set_update,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Update the set with elements from all supplied iterables.",
};

static PyMethodDef containers_set_intersection_update_method = {
    .ml_name = "intersection_update",
    .ml_meth = adapter_set_intersection_update,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Update the set with the intersection of itself and all supplied iterables.",
};

static PyMethodDef containers_set_difference_update_method = {
    .ml_name = "difference_update",
    .ml_meth = adapter_set_difference_update,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Remove all elements found in any supplied iterable.",
};

static PyMethodDef containers_set_symmetric_difference_update_method = {
    .ml_name = "symmetric_difference_update",
    .ml_meth = adapter_set_symmetric_difference_update,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Update the set with the symmetric difference of itself and another iterable.",
};

static PyMethodDef containers_set_union_method = {
    .ml_name = "union",
    .ml_meth = adapter_set_union,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the union of the set and all supplied iterables.",
};

static PyMethodDef containers_set_intersection_method = {
    .ml_name = "intersection",
    .ml_meth = adapter_set_intersection,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the intersection of the set and all supplied iterables.",
};

static PyMethodDef containers_set_difference_method = {
    .ml_name = "difference",
    .ml_meth = adapter_set_difference,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the difference of the set and all supplied iterables.",
};

static PyMethodDef containers_set_symmetric_difference_method = {
    .ml_name = "symmetric_difference",
    .ml_meth = adapter_set_symmetric_difference,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the symmetric difference of the set and another iterable.",
};

static PyMethodDef containers_set_isdisjoint_method = {
    .ml_name = "isdisjoint",
    .ml_meth = adapter_set_isdisjoint,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return True if the set has no elements in common with other.",
};

static PyMethodDef containers_set_issubset_method = {
    .ml_name = "issubset",
    .ml_meth = adapter_set_issubset,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Report whether another set contains this set.",
};

static PyMethodDef containers_set_issuperset_method = {
    .ml_name = "issuperset",
    .ml_meth = adapter_set_issuperset,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Report whether this set contains another set.",
};

static PyMethodDef containers_frozenset_union_method = {
    .ml_name = "union",
    .ml_meth = adapter_frozenset_union,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the union of the frozenset and all supplied iterables.",
};

static PyMethodDef containers_frozenset_intersection_method = {
    .ml_name = "intersection",
    .ml_meth = adapter_frozenset_intersection,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the intersection of the frozenset and all supplied iterables.",
};

static PyMethodDef containers_frozenset_difference_method = {
    .ml_name = "difference",
    .ml_meth = adapter_frozenset_difference,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the difference of the frozenset and all supplied iterables.",
};

static PyMethodDef containers_frozenset_symmetric_difference_method = {
    .ml_name = "symmetric_difference",
    .ml_meth = adapter_frozenset_symmetric_difference,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the symmetric difference of the frozenset and another iterable.",
};

static PyMethodDef containers_frozenset_isdisjoint_method = {
    .ml_name = "isdisjoint",
    .ml_meth = adapter_set_isdisjoint,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return True if the frozenset has no elements in common with other.",
};

static PyMethodDef containers_frozenset_issubset_method = {
    .ml_name = "issubset",
    .ml_meth = adapter_set_issubset,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Report whether another set contains this frozenset.",
};

static PyMethodDef containers_frozenset_issuperset_method = {
    .ml_name = "issuperset",
    .ml_meth = adapter_set_issuperset,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Report whether this frozenset contains another set.",
};
