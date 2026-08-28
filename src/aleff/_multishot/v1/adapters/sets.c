PyAPI_FUNC(int) _PySet_NextEntry(
    PyObject *set, Py_ssize_t *pos, PyObject **key, Py_hash_t *hash
);
PyAPI_FUNC(int) _PySet_Update(PyObject *set, PyObject *iterable);
PyAPI_DATA(PyObject *) _PySet_Dummy;

#define ALEFF_SET_LINEAR_PROBES 9
#define ALEFF_SET_PERTURB_SHIFT 5

typedef struct {
    PyObject *candidate;
    size_t perturb;
    size_t index;
    int offset;
    int probes;
    Py_ssize_t free_index;
    Py_ssize_t candidate_index;
    int active;
    int waiting_equal;
} AleffSetLookupState;

static void
aleff_set_lookup_reset(AleffSetLookupState *state)
{
    Py_CLEAR(state->candidate);
    state->perturb = 0;
    state->index = 0;
    state->offset = 0;
    state->probes = 0;
    state->free_index = -1;
    state->candidate_index = -1;
    state->active = 0;
    state->waiting_equal = 0;
}

static void
aleff_set_lookup_start(
    AleffSetLookupState *state,
    PySetObject *set,
    Py_hash_t hash
)
{
    aleff_set_lookup_reset(state);
    state->perturb = (size_t)hash;
    state->index = (size_t)hash & (size_t)set->mask;
    state->probes = state->index + ALEFF_SET_LINEAR_PROBES <= (size_t)set->mask
        ? ALEFF_SET_LINEAR_PROBES
        : 0;
    state->active = 1;
}

static void
aleff_set_lookup_advance(AleffSetLookupState *state, PySetObject *set)
{
    if (state->offset < state->probes) {
        state->offset++;
        return;
    }
    state->perturb >>= ALEFF_SET_PERTURB_SHIFT;
    state->index = (
        state->index * 5 + 1 + state->perturb
    ) & (size_t)set->mask;
    state->offset = 0;
    state->probes = state->index + ALEFF_SET_LINEAR_PROBES <= (size_t)set->mask
        ? ALEFF_SET_LINEAR_PROBES
        : 0;
}

static setentry *
aleff_set_lookup_continue(
    PyObject *object,
    PyObject *key,
    Py_hash_t hash,
    AleffSetLookupState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    PySetObject *set = (PySetObject *)object;
    if (!state->active) {
        aleff_set_lookup_start(state, set, hash);
    }
    if (is_resumed) {
        if (!state->waiting_equal || state->candidate == NULL) {
            PyErr_SetString(PyExc_RuntimeError, "invalid set lookup resume state");
            return NULL;
        }
        int equal = PyObject_IsTrue(resumed_value);
        if (equal < 0) {
            return NULL;
        }
        setentry *entry = state->candidate_index <= set->mask
            ? &set->table[state->candidate_index]
            : NULL;
        if (entry == NULL || entry->key != state->candidate) {
            aleff_set_lookup_start(state, set, hash);
        }
        else {
            Py_CLEAR(state->candidate);
            state->waiting_equal = 0;
            if (equal) {
                state->active = 0;
                return entry;
            }
            aleff_set_lookup_advance(state, set);
        }
    }

    for (;;) {
        Py_ssize_t entry_index = (Py_ssize_t)(state->index + state->offset);
        setentry *entry = &set->table[entry_index];
        if (entry->hash == 0 && entry->key == NULL) {
            Py_ssize_t result_index = state->free_index >= 0
                ? state->free_index
                : entry_index;
            state->active = 0;
            return &set->table[result_index];
        }
        if (entry->hash == hash && entry->key != _PySet_Dummy) {
            if (entry->key == key) {
                state->active = 0;
                return entry;
            }
            Py_XSETREF(state->candidate, Py_NewRef(entry->key));
            state->candidate_index = entry_index;
            state->waiting_equal = 1;
            int equal = PyObject_RichCompareBool(entry->key, key, Py_EQ);
            if (equal < 0) {
                return NULL;
            }
            Py_CLEAR(state->candidate);
            state->waiting_equal = 0;
            if (equal) {
                state->active = 0;
                return entry;
            }
        }
        else if (entry->hash == -1 && state->free_index < 0) {
            state->free_index = entry_index;
        }
        aleff_set_lookup_advance(state, set);
    }
}

static setentry *
aleff_set_lookkey(PySetObject *set, PyObject *key, Py_hash_t hash)
{
restart:
    size_t perturb = (size_t)hash;
    size_t index = (size_t)hash & (size_t)set->mask;
    setentry *free_slot = NULL;

    for (;;) {
        setentry *table = set->table;
        setentry *entry = &set->table[index];
        int probes = index + ALEFF_SET_LINEAR_PROBES <= (size_t)set->mask
            ? ALEFF_SET_LINEAR_PROBES
            : 0;
        do {
            if (entry->hash == 0 && entry->key == NULL) {
                return free_slot == NULL ? entry : free_slot;
            }
            if (entry->hash == hash && entry->key != _PySet_Dummy) {
                PyObject *candidate = entry->key;
                if (candidate == key) {
                    return entry;
                }
                Py_INCREF(candidate);
                int equal = PyObject_RichCompareBool(candidate, key, Py_EQ);
                Py_DECREF(candidate);
                if (equal < 0) {
                    return NULL;
                }
                if (set->table != table || entry->key != candidate) {
                    goto restart;
                }
                if (equal) {
                    return entry;
                }
            }
            else if (entry->hash == -1 && free_slot == NULL) {
                free_slot = entry;
            }
            entry++;
        } while (probes--);
        perturb >>= ALEFF_SET_PERTURB_SHIFT;
        index = (index * 5 + 1 + perturb) & (size_t)set->mask;
    }
}

static void
aleff_set_insert_clean(setentry *table, Py_ssize_t mask, PyObject *key, Py_hash_t hash)
{
    size_t perturb = (size_t)hash;
    size_t index = (size_t)hash & (size_t)mask;
    for (;;) {
        setentry *entry = &table[index];
        int probes = index + ALEFF_SET_LINEAR_PROBES <= (size_t)mask
            ? ALEFF_SET_LINEAR_PROBES
            : 0;
        do {
            if (entry->key == NULL) {
                entry->key = key;
                entry->hash = hash;
                return;
            }
            entry++;
        } while (probes--);
        perturb >>= ALEFF_SET_PERTURB_SHIFT;
        index = (index * 5 + 1 + perturb) & (size_t)mask;
    }
}

static int
aleff_set_resize(PySetObject *set, Py_ssize_t minimum_used)
{
    Py_ssize_t size = PySet_MINSIZE;
    while (size <= minimum_used * 2) {
        if (size > PY_SSIZE_T_MAX / 2) {
            PyErr_NoMemory();
            return -1;
        }
        size <<= 1;
    }
    setentry *table = PyMem_Calloc((size_t)size, sizeof(*table));
    if (table == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    setentry *old_table = set->table;
    Py_ssize_t old_mask = set->mask;
    for (Py_ssize_t index = 0; index <= old_mask; index++) {
        setentry *entry = &old_table[index];
        if (entry->key != NULL && entry->key != _PySet_Dummy) {
            aleff_set_insert_clean(table, size - 1, entry->key, entry->hash);
        }
    }
    set->table = table;
    set->mask = size - 1;
    set->fill = set->used;
    if (old_table != set->smalltable) {
        PyMem_Free(old_table);
    }
    return 0;
}

static int
aleff_set_add_known_hash(PyObject *object, PyObject *key, Py_hash_t hash)
{
    PySetObject *set = (PySetObject *)object;
    setentry *entry = aleff_set_lookkey(set, key, hash);
    if (entry == NULL) {
        return -1;
    }
    if (entry->key != NULL && entry->key != _PySet_Dummy) {
        return 0;
    }
    if ((set->fill + 1) * 5 >= set->mask * 3) {
        if (aleff_set_resize(set, (set->used + 1) * 2) < 0) {
            return -1;
        }
        entry = aleff_set_lookkey(set, key, hash);
        if (entry == NULL) {
            return -1;
        }
    }
    if (entry->key == NULL) {
        set->fill++;
    }
    entry->key = Py_NewRef(key);
    entry->hash = hash;
    set->used++;
    return 0;
}

static int
aleff_set_insert_at_entry(
    PyObject *object,
    setentry *entry,
    PyObject *key,
    Py_hash_t hash
)
{
    PySetObject *set = (PySetObject *)object;
    if (entry->key == NULL) {
        set->fill++;
    }
    entry->key = Py_NewRef(key);
    entry->hash = hash;
    set->used++;
    if (set->fill * 5 >= set->mask * 3) {
        return aleff_set_resize(set, set->used * 2);
    }
    return 0;
}

static void
aleff_set_discard_entry(PyObject *object, setentry *entry)
{
    PySetObject *set = (PySetObject *)object;
    PyObject *removed = entry->key;
    entry->key = _PySet_Dummy;
    entry->hash = -1;
    set->used--;
    Py_DECREF(removed);
}

static int
aleff_set_contains_known_hash(PyObject *object, PyObject *key, Py_hash_t hash)
{
    setentry *entry = aleff_set_lookkey((PySetObject *)object, key, hash);
    if (entry == NULL) {
        return -1;
    }
    return entry->key != NULL && entry->key != _PySet_Dummy;
}

static int set_normalize_hash(PyObject *value, Py_hash_t *hash);

static Py_hash_t
set_element_hash(PyObject *item)
{
#if PY_VERSION_HEX >= 0x030e0000
    if (Py_TYPE(item)->tp_hash == PyObject_HashNotImplemented) {
        PyErr_Format(
            PyExc_TypeError,
            "cannot use '%.200s' as a set element (unhashable type: '%.200s')",
            Py_TYPE(item)->tp_name,
            Py_TYPE(item)->tp_name
        );
        return -1;
    }
#endif
    return PyObject_Hash(item);
}

typedef enum {
    SET_COLLECT_WAIT_ITER,
    SET_COLLECT_WAIT_NEXT,
    SET_COLLECT_WAIT_HASH,
    SET_COLLECT_WAIT_EQUAL,
} SetCollectPhase;

typedef struct {
    PyObject *iterable;
    PyObject *iterator;
    PyObject *result;
    PyObject *item;
    CollectKind kind;
    SetCollectPhase phase;
    Py_hash_t hash;
    AleffSetLookupState lookup;
} SetCollectState;

static void *
set_collect_copy_state(const void *raw_state)
{
    const SetCollectState *state = raw_state;
    SetCollectState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->iterable = Py_NewRef(state->iterable);
    copy->iterator = Py_XNewRef(state->iterator);
    copy->result = PySet_New(state->result);
    copy->item = Py_XNewRef(state->item);
    if (copy->result == NULL) {
        Py_DECREF(copy->iterable);
        Py_XDECREF(copy->iterator);
        Py_XDECREF(copy->item);
        PyMem_Free(copy);
        return NULL;
    }
    copy->lookup.candidate = Py_XNewRef(state->lookup.candidate);
    return copy;
}

static void
set_collect_free_state(void *raw_state)
{
    SetCollectState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->iterable);
    Py_XDECREF(state->iterator);
    Py_DECREF(state->result);
    Py_XDECREF(state->item);
    Py_XDECREF(state->lookup.candidate);
    PyMem_Free(state);
}

static PyObject *set_collect_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable set_collect_vtable = {
    .copy_state = set_collect_copy_state,
    .free_state = set_collect_free_state,
    .resume = set_collect_resume,
};

static PyObject *
set_collect_finish(SetCollectState *state)
{
    return state->kind == COLLECT_FROZENSET
        ? PyFrozenSet_New(state->result)
        : PySet_New(state->result);
}

static PyObject *
set_collect_continue(
    SetCollectState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (state->phase == SET_COLLECT_WAIT_ITER) {
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
        else if (state->phase == SET_COLLECT_WAIT_NEXT) {
            state->item = Py_NewRef(resumed_value);
        }
        else if (state->phase == SET_COLLECT_WAIT_HASH) {
            if (set_normalize_hash(resumed_value, &state->hash) < 0) {
                return NULL;
            }
        }
        else if (state->phase != SET_COLLECT_WAIT_EQUAL) {
            PyErr_SetString(PyExc_RuntimeError, "invalid set collector phase");
            return NULL;
        }
    }

    if (state->iterator == NULL) {
        state->phase = SET_COLLECT_WAIT_ITER;
        state->iterator = PyObject_GetIter(state->iterable);
        if (state->iterator == NULL) {
            return NULL;
        }
    }
    int resumed_lookup = is_resumed && state->phase == SET_COLLECT_WAIT_EQUAL;
    for (;;) {
        if (state->item == NULL) {
            state->phase = SET_COLLECT_WAIT_NEXT;
            state->item = Py_TYPE(state->iterator)->tp_iternext(state->iterator);
            if (state->item == NULL) {
                if (PyErr_Occurred()) {
                    if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                        return NULL;
                    }
                    PyErr_Clear();
                }
                return set_collect_finish(state);
            }
            state->phase = SET_COLLECT_WAIT_HASH;
            state->hash = set_element_hash(state->item);
            if (state->hash == -1) {
                return NULL;
            }
        }
        state->phase = SET_COLLECT_WAIT_EQUAL;
        setentry *entry = aleff_set_lookup_continue(
            state->result,
            state->item,
            state->hash,
            &state->lookup,
            resumed_lookup ? resumed_value : NULL,
            resumed_lookup
        );
        resumed_lookup = 0;
        if (entry == NULL) {
            return NULL;
        }
        if ((entry->key == NULL || entry->key == _PySet_Dummy) &&
            aleff_set_insert_at_entry(
                state->result,
                entry,
                state->item,
                state->hash
            ) < 0) {
            return NULL;
        }
        Py_CLEAR(state->item);
        state->hash = -1;
        aleff_set_lookup_reset(&state->lookup);
    }
}

static PyObject *
set_collect_resume(const void *raw_state, PyObject *value)
{
    const SetCollectState *source = raw_state;
    if (value == NULL) {
        if (source->phase != SET_COLLECT_WAIT_NEXT) {
            return NULL;
        }
        if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration)) {
            return NULL;
        }
    }
    SetCollectState *state = set_collect_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &set_collect_vtable, state) < 0) {
        set_collect_free_state(state);
        return NULL;
    }
    PyObject *result;
    if (value == NULL) {
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        result = set_collect_finish(state);
    }
    else {
        result = set_collect_continue(state, value, 1);
    }
    adapter_leave(&frame);
    set_collect_free_state(state);
    return result;
}

static PyObject *
collect_set_iterable(PyObject *iterable, CollectKind kind)
{
    if (PyAnySet_Check(iterable)) {
        return kind == COLLECT_FROZENSET
            ? PyFrozenSet_New(iterable)
            : PySet_New(iterable);
    }
    SetCollectState state = {
        .iterable = iterable,
        .iterator = NULL,
        .result = PySet_New(NULL),
        .item = NULL,
        .kind = kind,
        .phase = SET_COLLECT_WAIT_ITER,
        .hash = -1,
        .lookup = {
            .free_index = -1,
            .candidate_index = -1,
        },
    };
    if (state.result == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &set_collect_vtable, &state) < 0) {
        Py_DECREF(state.result);
        return NULL;
    }
    PyObject *result = set_collect_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.iterator);
    Py_DECREF(state.result);
    Py_XDECREF(state.item);
    Py_XDECREF(state.lookup.candidate);
    return result;
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
adapter_set_init(PyObject *self, PyObject *args, PyObject *kwargs)
{
    if (
        PyTuple_GET_SIZE(args) != 1 ||
        (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0) ||
        PyAnySet_Check(PyTuple_GET_ITEM(args, 0))
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
        PyAnySet_Check(PyTuple_GET_ITEM(args, 0))
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
    PyObject *apply_result;
    Py_ssize_t index;
    Py_ssize_t candidate_position;
    Py_ssize_t receiver_position;
    Py_hash_t item_hash;
    Py_hash_t candidate_hash;
    SetOperationKind operation;
    int frozen;
    int relation_phase;
    int operation_phase;
    int apply_source_is_target;
    Py_ssize_t operand_position;
    AleffSetLookupState lookup;
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
    copy->result = state->result == NULL ? NULL : PySet_New(state->result);
    if (state->result != NULL && copy->result == NULL) {
        Py_DECREF(copy->receiver);
        Py_DECREF(copy->args);
        PyMem_Free(copy);
        return NULL;
    }
    copy->items = Py_XNewRef(state->items);
    copy->current_item = Py_XNewRef(state->current_item);
    copy->candidate_item = Py_XNewRef(state->candidate_item);
    copy->apply_result = state->apply_result == NULL
        ? NULL
        : PySet_New(state->apply_result);
    if (state->apply_result != NULL && copy->apply_result == NULL) {
        Py_DECREF(copy->receiver);
        Py_DECREF(copy->args);
        Py_XDECREF(copy->result);
        Py_XDECREF(copy->items);
        Py_XDECREF(copy->current_item);
        Py_XDECREF(copy->candidate_item);
        PyMem_Free(copy);
        return NULL;
    }
    copy->index = state->index;
    copy->candidate_position = state->candidate_position;
    copy->receiver_position = state->receiver_position;
    copy->item_hash = state->item_hash;
    copy->candidate_hash = state->candidate_hash;
    copy->operation = state->operation;
    copy->frozen = state->frozen;
    copy->relation_phase = state->relation_phase;
    copy->operation_phase = state->operation_phase;
    copy->apply_source_is_target = state->apply_source_is_target;
    copy->operand_position = state->operand_position;
    copy->lookup = state->lookup;
    copy->lookup.candidate = Py_XNewRef(state->lookup.candidate);
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
    Py_XDECREF(state->apply_result);
    Py_XDECREF(state->lookup.candidate);
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

static const char *
set_operation_name(SetOperationKind operation)
{
    switch (operation) {
        case SET_OP_SYMMETRIC_DIFFERENCE_UPDATE:
            return "symmetric_difference_update";
        case SET_OP_SYMMETRIC_DIFFERENCE:
            return "symmetric_difference";
        case SET_OP_ISDISJOINT:
            return "isdisjoint";
        case SET_OP_ISSUBSET:
            return "issubset";
        case SET_OP_ISSUPERSET:
            return "issuperset";
        default:
            return NULL;
    }
}

enum {
    SET_RELATION_NONE,
    SET_RELATION_WAIT_ITEM_HASH,
    SET_RELATION_WAIT_CANDIDATE_HASH,
    SET_RELATION_WAIT_EQUAL,
};

enum {
    SET_OPERATION_WAIT_COLLECT,
    SET_OPERATION_APPLY,
    SET_OPERATION_WAIT_EQUAL,
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
        if (state->relation_phase != SET_RELATION_WAIT_EQUAL) {
            PyErr_SetString(PyExc_RuntimeError, "invalid set relation resume phase");
            return NULL;
        }
        equal = PyObject_IsTrue(resumed_value);
        if (equal < 0) {
            return NULL;
        }
    }

    int subset = state->operation == SET_OP_ISSUBSET;
    for (;;) {
        if (state->current_item == NULL) {
            PyObject *current;
            int found = subset
                ? _PySet_NextEntry(
                    state->receiver,
                    &state->receiver_position,
                    &current,
                    &state->item_hash
                )
                : _PySet_NextEntry(
                    state->items,
                    &state->index,
                    &current,
                    &state->item_hash
                );
            if (!found) {
                Py_RETURN_TRUE;
            }
            state->current_item = Py_NewRef(current);
            state->candidate_position = 0;
            state->candidate_hash = -1;
        }

        if (state->candidate_item == NULL) {
            PyObject *candidate;
            int found = subset
                ? _PySet_NextEntry(
                    state->items,
                    &state->candidate_position,
                    &candidate,
                    &state->candidate_hash
                )
                : _PySet_NextEntry(
                    state->receiver,
                    &state->candidate_position,
                    &candidate,
                    &state->candidate_hash
                );
            if (!found) {
                Py_CLEAR(state->current_item);
                Py_RETURN_FALSE;
            }
            state->candidate_item = Py_NewRef(candidate);
            if (candidate == state->current_item) {
                equal = 1;
            }
            else if (state->candidate_hash != state->item_hash) {
                Py_CLEAR(state->candidate_item);
                continue;
            }
            else {
                state->relation_phase = SET_RELATION_WAIT_EQUAL;
                equal = PyObject_RichCompareBool(
                    candidate,
                    state->current_item,
                    Py_EQ
                );
                if (equal < 0) {
                    return NULL;
                }
            }
        }

        if (equal) {
            Py_CLEAR(state->candidate_item);
            Py_CLEAR(state->current_item);
            equal = -1;
            continue;
        }
        Py_CLEAR(state->candidate_item);
        equal = -1;
    }
}

static PyObject *
set_operation_apply(
    SetOperationState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (set_operation_is_relation(state->operation)) {
        PyErr_SetString(PyExc_RuntimeError, "set relation must use element adapter");
        return NULL;
    }
    PyObject *target = set_operation_is_mutating(state->operation)
        ? state->receiver
        : state->result;
    SetOperationKind operation = state->operation;
    if (operation == SET_OP_UNION) operation = SET_OP_UPDATE;
    if (operation == SET_OP_INTERSECTION) operation = SET_OP_INTERSECTION_UPDATE;
    if (operation == SET_OP_DIFFERENCE) operation = SET_OP_DIFFERENCE_UPDATE;
    if (operation == SET_OP_SYMMETRIC_DIFFERENCE) {
        operation = SET_OP_SYMMETRIC_DIFFERENCE_UPDATE;
    }

    if (operation == SET_OP_INTERSECTION_UPDATE) {
        if (state->apply_result == NULL) {
            PyObject *original_operand = PyTuple_GET_ITEM(
                state->args,
                state->index
            );
            state->apply_source_is_target = (
                PyAnySet_Check(original_operand) &&
                PySet_GET_SIZE(target) <= PySet_GET_SIZE(state->items)
            );
            state->apply_result = PySet_New(NULL);
            if (state->apply_result == NULL) {
                return NULL;
            }
        }
    }

    int resumed_lookup = is_resumed;
    for (;;) {
        if (state->current_item == NULL) {
            PyObject *source = operation == SET_OP_INTERSECTION_UPDATE &&
                state->apply_source_is_target
                ? target
                : state->items;
            PyObject *item;
            if (!_PySet_NextEntry(
                    source,
                    &state->operand_position,
                    &item,
                    &state->item_hash
                )) {
                if (operation == SET_OP_INTERSECTION_UPDATE) {
                    if (PySet_Clear(target) < 0 ||
                        _PySet_Update(target, state->apply_result) < 0) {
                        return NULL;
                    }
                    Py_CLEAR(state->apply_result);
                    state->apply_source_is_target = 0;
                }
                state->operand_position = 0;
                state->operation_phase = SET_OPERATION_WAIT_COLLECT;
                Py_RETURN_NONE;
            }
            state->current_item = Py_NewRef(item);
            aleff_set_lookup_reset(&state->lookup);
        }

        PyObject *lookup = operation == SET_OP_INTERSECTION_UPDATE &&
            !state->apply_source_is_target
            ? target
            : operation == SET_OP_INTERSECTION_UPDATE
                ? state->items
                : target;
        state->operation_phase = SET_OPERATION_WAIT_EQUAL;
        setentry *entry = aleff_set_lookup_continue(
            lookup,
            state->current_item,
            state->item_hash,
            &state->lookup,
            resumed_lookup ? resumed_value : NULL,
            resumed_lookup
        );
        resumed_lookup = 0;
        if (entry == NULL) {
            return NULL;
        }
        int found = entry->key != NULL && entry->key != _PySet_Dummy;
        if (operation == SET_OP_INTERSECTION_UPDATE) {
            if (found && aleff_set_add_known_hash(
                    state->apply_result,
                    state->current_item,
                    state->item_hash
                ) < 0) {
                return NULL;
            }
        }
        else if (operation == SET_OP_UPDATE) {
            if (!found && aleff_set_insert_at_entry(
                    target,
                    entry,
                    state->current_item,
                    state->item_hash
                ) < 0) {
                return NULL;
            }
        }
        else if (operation == SET_OP_DIFFERENCE_UPDATE) {
            if (found) {
                aleff_set_discard_entry(target, entry);
            }
        }
        else if (found) {
            aleff_set_discard_entry(target, entry);
        }
        else if (aleff_set_insert_at_entry(
                target,
                entry,
                state->current_item,
                state->item_hash
            ) < 0) {
            return NULL;
        }
        Py_CLEAR(state->current_item);
        aleff_set_lookup_reset(&state->lookup);
        state->operation_phase = SET_OPERATION_APPLY;
    }
}

static PyObject *
set_collect_operand_as_set(PyObject *operand)
{
    return collect_set_iterable(operand, COLLECT_SET);
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
                state->items = set_collect_operand_as_set(
                    PyTuple_GET_ITEM(state->args, 0)
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
    PyObject *lookup_resume = NULL;
    int resume_lookup = 0;
    if (is_resumed) {
        if (state->operation_phase == SET_OPERATION_WAIT_COLLECT) {
            state->items = Py_NewRef(resumed_value);
        }
        else if (state->operation_phase == SET_OPERATION_WAIT_EQUAL) {
            lookup_resume = resumed_value;
            resume_lookup = 1;
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "invalid set operation resume phase");
            return NULL;
        }
    }
    Py_ssize_t count = PyTuple_GET_SIZE(state->args);
    while (state->index < count) {
        if (state->items == NULL) {
            state->operation_phase = SET_OPERATION_WAIT_COLLECT;
            state->items = set_collect_operand_as_set(
                PyTuple_GET_ITEM(state->args, state->index)
            );
            if (state->items == NULL) {
                return NULL;
            }
        }
        PyObject *applied = set_operation_apply(
            state,
            lookup_resume,
            resume_lookup
        );
        lookup_resume = NULL;
        resume_lookup = 0;
        if (applied == NULL) {
            return NULL;
        }
        Py_DECREF(applied);
        Py_CLEAR(state->items);
        state->index++;
    }
    if (set_operation_is_mutating(state->operation)) {
        Py_RETURN_NONE;
    }
    return state->frozen
        ? PyFrozenSet_New(state->result)
        : Py_NewRef(state->result);
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
        const char *operation_name = set_operation_name(operation);
        PyErr_Format(
            PyExc_TypeError,
            "%s.%s() takes exactly one argument (%zd given)",
            frozen ? "frozenset" : "set",
            operation_name,
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
            : PySet_New(self),
        .items = NULL,
        .current_item = NULL,
        .candidate_item = NULL,
        .apply_result = NULL,
        .index = 0,
        .candidate_position = 0,
        .receiver_position = 0,
        .item_hash = -1,
        .candidate_hash = -1,
        .operation = operation,
        .frozen = frozen,
        .relation_phase = SET_RELATION_NONE,
        .operation_phase = SET_OPERATION_WAIT_COLLECT,
        .apply_source_is_target = 0,
        .operand_position = 0,
        .lookup = {
            .candidate = NULL,
            .free_index = -1,
            .candidate_index = -1,
        },
    };
    if (!set_operation_is_mutating(operation) && state.result == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &set_operation_vtable, &state) < 0) {
        Py_XDECREF(state.result);
        return NULL;
    }
    PyObject *result = set_operation_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.result);
    Py_XDECREF(state.items);
    Py_XDECREF(state.current_item);
    Py_XDECREF(state.candidate_item);
    Py_XDECREF(state.apply_result);
    Py_XDECREF(state.lookup.candidate);
    return result;
}

typedef enum {
    SET_UPDATE_WAIT_ITER,
    SET_UPDATE_WAIT_NEXT,
} SetUpdatePhase;

typedef struct {
    PyObject *receiver;
    PyObject *args;
    PyObject *iterator;
    Py_ssize_t index;
    SetUpdatePhase phase;
} SetUpdateState;

static const AleffAdapterVTable set_update_vtable;

static void *
set_update_copy_state(const void *raw_state)
{
    const SetUpdateState *state = raw_state;
    SetUpdateState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->receiver = Py_NewRef(state->receiver);
    copy->args = Py_NewRef(state->args);
    copy->iterator = Py_XNewRef(state->iterator);
    copy->index = state->index;
    copy->phase = state->phase;
    return copy;
}

static void
set_update_free_state(void *raw_state)
{
    SetUpdateState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->receiver);
    Py_DECREF(state->args);
    Py_XDECREF(state->iterator);
    PyMem_Free(state);
}

static PyObject *
set_update_continue(
    SetUpdateState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (state->phase == SET_UPDATE_WAIT_ITER) {
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
        else if (PySet_Add(state->receiver, resumed_value) < 0) {
            return NULL;
        }
    }

    Py_ssize_t count = PyTuple_GET_SIZE(state->args);
    while (state->index < count) {
        if (state->iterator == NULL) {
            state->phase = SET_UPDATE_WAIT_ITER;
            state->iterator = PyObject_GetIter(
                PyTuple_GET_ITEM(state->args, state->index)
            );
            if (state->iterator == NULL) {
                return NULL;
            }
        }
        for (;;) {
            state->phase = SET_UPDATE_WAIT_NEXT;
            PyObject *item = Py_TYPE(state->iterator)->tp_iternext(
                state->iterator
            );
            if (item == NULL) {
                if (PyErr_Occurred()) {
                    if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                        return NULL;
                    }
                    PyErr_Clear();
                }
                Py_CLEAR(state->iterator);
                state->index++;
                break;
            }
            int status = PySet_Add(state->receiver, item);
            Py_DECREF(item);
            if (status < 0) {
                return NULL;
            }
        }
    }
    Py_RETURN_NONE;
}

static PyObject *
set_update_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    SetUpdateState *state = set_update_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &set_update_vtable, state) < 0) {
        set_update_free_state(state);
        return NULL;
    }
    PyObject *result = set_update_continue(state, value, 1);
    adapter_leave(&frame);
    set_update_free_state(state);
    return result;
}

static const AleffAdapterVTable set_update_vtable = {
    .copy_state = set_update_copy_state,
    .free_state = set_update_free_state,
    .resume = set_update_resume,
};

static PyObject *adapter_set_update(PyObject *self, PyObject *args)
{
    SetUpdateState state = {
        .receiver = self,
        .args = args,
        .iterator = NULL,
        .index = 0,
        .phase = SET_UPDATE_WAIT_ITER,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &set_update_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = set_update_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.iterator);
    return result;
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

typedef struct {
    PyObject *receiver;
    PyObject *iterable;
    PyObject *iterator;
    SetUpdatePhase phase;
} SetIsDisjointState;

static const AleffAdapterVTable set_isdisjoint_vtable;

static void *
set_isdisjoint_copy_state(const void *raw_state)
{
    const SetIsDisjointState *state = raw_state;
    SetIsDisjointState *copy = PyMem_Malloc(sizeof(*copy));
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
set_isdisjoint_free_state(void *raw_state)
{
    SetIsDisjointState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->receiver);
    Py_DECREF(state->iterable);
    Py_XDECREF(state->iterator);
    PyMem_Free(state);
}

static PyObject *
set_isdisjoint_continue(
    SetIsDisjointState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    PyObject *item = NULL;
    if (is_resumed) {
        if (state->phase == SET_UPDATE_WAIT_ITER) {
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
    if (state->iterator == NULL) {
        state->phase = SET_UPDATE_WAIT_ITER;
        state->iterator = PyObject_GetIter(state->iterable);
        if (state->iterator == NULL) {
            return NULL;
        }
    }
    for (;;) {
        if (item == NULL) {
            state->phase = SET_UPDATE_WAIT_NEXT;
            item = Py_TYPE(state->iterator)->tp_iternext(state->iterator);
            if (item == NULL) {
                if (PyErr_Occurred()) {
                    if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                        return NULL;
                    }
                    PyErr_Clear();
                }
                Py_RETURN_TRUE;
            }
        }
        int contains = PySet_Contains(state->receiver, item);
        Py_CLEAR(item);
        if (contains < 0) {
            return NULL;
        }
        if (contains) {
            Py_RETURN_FALSE;
        }
    }
}

static PyObject *
set_isdisjoint_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    SetIsDisjointState *state = set_isdisjoint_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &set_isdisjoint_vtable, state) < 0) {
        set_isdisjoint_free_state(state);
        return NULL;
    }
    PyObject *result = set_isdisjoint_continue(state, value, 1);
    adapter_leave(&frame);
    set_isdisjoint_free_state(state);
    return result;
}

static const AleffAdapterVTable set_isdisjoint_vtable = {
    .copy_state = set_isdisjoint_copy_state,
    .free_state = set_isdisjoint_free_state,
    .resume = set_isdisjoint_resume,
};

static PyObject *adapter_set_isdisjoint(PyObject *self, PyObject *args)
{
    Py_ssize_t count = PyTuple_GET_SIZE(args);
    if (count != 1) {
        PyErr_Format(
            PyExc_TypeError,
            "%s.isdisjoint() takes exactly one argument (%zd given)",
            PyFrozenSet_Check(self) ? "frozenset" : "set",
            count
        );
        return NULL;
    }
    PyObject *iterable = PyTuple_GET_ITEM(args, 0);
    if (iterable == self) {
        return PyBool_FromLong(PySet_Size(self) == 0);
    }
    if (PySet_CheckExact(iterable) || PyFrozenSet_CheckExact(iterable)) {
        Py_ssize_t position = 0;
        PyObject *item;
        Py_hash_t hash;
        while (_PySet_NextEntry(iterable, &position, &item, &hash)) {
            int contains = aleff_set_contains_known_hash(self, item, hash);
            if (contains < 0) {
                return NULL;
            }
            if (contains) {
                Py_RETURN_FALSE;
            }
        }
        Py_RETURN_TRUE;
    }
    SetIsDisjointState state = {
        .receiver = self,
        .iterable = iterable,
        .iterator = NULL,
        .phase = SET_UPDATE_WAIT_ITER,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &set_isdisjoint_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = set_isdisjoint_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.iterator);
    return result;
}

static PyObject *adapter_set_issubset(PyObject *self, PyObject *args)
{
    return adapter_set_operation(
        self,
        args,
        SET_OP_ISSUBSET,
        PyFrozenSet_Check(self)
    );
}

static PyObject *adapter_set_issuperset(PyObject *self, PyObject *args)
{
    return adapter_set_operation(
        self,
        args,
        SET_OP_ISSUPERSET,
        PyFrozenSet_Check(self)
    );
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
