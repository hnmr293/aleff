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
    if (adapter_enter(&frame, &dict_get_vtable, state) < 0) {
        return NULL;
    }
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
    if (adapter_enter(&frame, &dict_get_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = dict_get_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.candidate_key);
    Py_XDECREF(state.candidate_value);
    return result;
}

typedef enum {
    DICT_ITEM_GET,
    DICT_ITEM_SET,
    DICT_ITEM_DELETE,
    DICT_ITEM_CONTAINS,
} DictItemOperation;

typedef struct {
    PyObject *receiver;
    PyObject *key;
    PyObject *value;
    Py_hash_t hash;
    DictItemOperation operation;
} DictItemState;

static const AleffAdapterVTable dict_item_vtable;

static void *
dict_item_copy_state(const void *raw_state)
{
    const DictItemState *state = raw_state;
    DictItemState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->receiver = Py_NewRef(state->receiver);
    copy->key = Py_NewRef(state->key);
    copy->value = Py_XNewRef(state->value);
    copy->hash = state->hash;
    copy->operation = state->operation;
    return copy;
}

static void
dict_item_free_state(void *raw_state)
{
    DictItemState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->receiver);
    Py_DECREF(state->key);
    Py_XDECREF(state->value);
    PyMem_Free(state);
}

static PyObject *
dict_item_apply(DictItemState *state)
{
    PyObject *candidate_key = NULL;
    PyObject *candidate_value = NULL;
    Py_ssize_t position = 0;
    while (PyDict_Next(
        state->receiver,
        &position,
        &candidate_key,
        &candidate_value
    )) {
        if (PyLong_Check(candidate_key)) {
            long candidate_hash = PyLong_AsLong(candidate_key);
            if (candidate_hash == -1 && PyErr_Occurred()) {
                return NULL;
            }
            if ((Py_hash_t)candidate_hash != state->hash) {
                continue;
            }
        }
        int equal = PyObject_RichCompareBool(
            candidate_key,
            state->key,
            Py_EQ
        );
        if (equal < 0) {
            return NULL;
        }
        if (!equal) {
            continue;
        }
        switch (state->operation) {
            case DICT_ITEM_GET:
                return Py_NewRef(candidate_value);
            case DICT_ITEM_CONTAINS:
                Py_RETURN_TRUE;
            case DICT_ITEM_SET:
                if (PyDict_SetItem(state->receiver, candidate_key, state->value) < 0) {
                    return NULL;
                }
                Py_RETURN_NONE;
            case DICT_ITEM_DELETE:
                if (PyDict_DelItem(state->receiver, candidate_key) < 0) {
                    return NULL;
                }
                Py_RETURN_NONE;
        }
    }
    if (state->operation == DICT_ITEM_CONTAINS) {
        Py_RETURN_FALSE;
    }
    if (state->operation == DICT_ITEM_SET) {
        if (PyDict_SetItem(state->receiver, state->key, state->value) < 0) {
            return NULL;
        }
        Py_RETURN_NONE;
    }
    if (state->operation == DICT_ITEM_DELETE) {
        PyErr_SetObject(PyExc_KeyError, state->key);
        return NULL;
    }
    PyErr_SetObject(PyExc_KeyError, state->key);
    return NULL;
}

static PyObject *
dict_item_resume(const void *raw_state, PyObject *value)
{
    DictItemState *state = (DictItemState *)raw_state;
    if (value == NULL) {
        return NULL;
    }
    state->hash = PyLong_AsLong(value);
    if (state->hash == -1 && PyErr_Occurred()) {
        return NULL;
    }
    return dict_item_apply(state);
}

static const AleffAdapterVTable dict_item_vtable = {
    .copy_state = dict_item_copy_state,
    .free_state = dict_item_free_state,
    .resume = dict_item_resume,
};

static PyObject *
adapter_dict_item_operation(
    PyObject *self,
    PyObject *key,
    PyObject *value,
    DictItemOperation operation
)
{
    if (!dict_item_has_python_hash(key)) {
        if (operation == DICT_ITEM_GET && original_dict_subscript != NULL) {
            return original_dict_subscript(self, key);
        }
        if (operation == DICT_ITEM_CONTAINS && original_dict_subscript != NULL) {
            int contains = PyDict_Contains(self, key);
            if (contains < 0) {
                return NULL;
            }
            return PyBool_FromLong(contains);
        }
        if (operation == DICT_ITEM_SET) {
            if (PyDict_SetItem(self, key, value) < 0) {
                return NULL;
            }
            Py_RETURN_NONE;
        }
        if (operation == DICT_ITEM_DELETE) {
            if (PyDict_DelItem(self, key) < 0) {
                return NULL;
            }
            Py_RETURN_NONE;
        }
    }
    DictItemState state = {
        .receiver = self,
        .key = key,
        .value = value,
        .hash = -1,
        .operation = operation,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &dict_item_vtable, &state) < 0) {
        return NULL;
    }
    state.hash = PyObject_Hash(key);
    PyObject *result = state.hash == -1 && PyErr_Occurred()
        ? NULL
        : dict_item_apply(&state);
    adapter_leave(&frame);
    return result;
}

static PyObject *adapter_dict_getitem(PyObject *self, PyObject *key)
{
    return adapter_dict_item_operation(self, key, NULL, DICT_ITEM_GET);
}

static PyObject *adapter_dict_contains(PyObject *self, PyObject *key)
{
    return adapter_dict_item_operation(self, key, NULL, DICT_ITEM_CONTAINS);
}

static PyObject *adapter_dict_setitem(PyObject *self, PyObject *args)
{
    PyObject *key;
    PyObject *value;
    if (!PyArg_ParseTuple(args, "OO:__setitem__", &key, &value)) {
        return NULL;
    }
    return adapter_dict_item_operation(self, key, value, DICT_ITEM_SET);
}

static PyObject *adapter_dict_delitem(PyObject *self, PyObject *key)
{
    return adapter_dict_item_operation(self, key, NULL, DICT_ITEM_DELETE);
}

typedef enum {
    DICT_POP_WAIT_HASH,
    DICT_POP_WAIT_CANDIDATE_HASH,
    DICT_POP_WAIT_EQUAL,
} DictPopPhase;

typedef struct {
    PyObject *receiver;
    PyObject *key;
    PyObject *default_value;
    PyObject *candidate_key;
    PyObject *candidate_value;
    Py_ssize_t position;
    Py_hash_t hash;
    Py_hash_t candidate_hash;
    DictPopPhase phase;
    int has_default;
} DictPopState;

static const AleffAdapterVTable dict_pop_vtable;

static void *
dict_pop_copy_state(const void *raw_state)
{
    const DictPopState *state = raw_state;
    DictPopState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = (DictPopState){
        .receiver = Py_NewRef(state->receiver),
        .key = Py_NewRef(state->key),
        .default_value = Py_NewRef(state->default_value),
        .candidate_key = Py_XNewRef(state->candidate_key),
        .candidate_value = Py_XNewRef(state->candidate_value),
        .position = state->position,
        .hash = state->hash,
        .candidate_hash = state->candidate_hash,
        .phase = state->phase,
        .has_default = state->has_default,
    };
    return copy;
}

static void
dict_pop_free_state(void *raw_state)
{
    DictPopState *state = raw_state;
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

static PyObject *
dict_pop_continue(
    DictPopState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    int equal = -1;
    if (is_resumed) {
        if (state->phase == DICT_POP_WAIT_HASH) {
            if (dict_get_normalize_hash(resumed_value, &state->hash) < 0) {
                return NULL;
            }
        }
        else if (state->phase == DICT_POP_WAIT_CANDIDATE_HASH) {
            if (dict_get_normalize_hash(
                    resumed_value, &state->candidate_hash
                ) < 0) {
                return NULL;
            }
        }
        else {
            equal = PyObject_IsTrue(resumed_value);
            if (equal < 0) {
                return NULL;
            }
        }
    }
    if (!is_resumed) {
        state->phase = DICT_POP_WAIT_HASH;
        state->hash = PyObject_Hash(state->key);
        if (state->hash == -1) {
            return NULL;
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
                break;
            }
            state->candidate_key = Py_NewRef(candidate_key);
            state->candidate_value = Py_NewRef(candidate_value);
            if (candidate_key == state->key) {
                state->candidate_hash = state->hash;
            }
            else {
                state->phase = DICT_POP_WAIT_CANDIDATE_HASH;
                state->candidate_hash = PyObject_Hash(candidate_key);
                if (state->candidate_hash == -1) {
                    return NULL;
                }
            }
#endif
            if (state->candidate_key == NULL) {
                break;
            }
        }
        if (state->candidate_hash != state->hash) {
            Py_CLEAR(state->candidate_key);
            Py_CLEAR(state->candidate_value);
            equal = -1;
            continue;
        }
        if (equal < 0 && state->candidate_key != state->key) {
            state->phase = DICT_POP_WAIT_EQUAL;
            equal = PyObject_RichCompareBool(
                state->candidate_key, state->key, Py_EQ
            );
            if (equal < 0) {
                return NULL;
            }
        }
        if (state->candidate_key == state->key || equal) {
            PyObject *result = Py_NewRef(state->candidate_value);
            if (_PyDict_DelItem_KnownHash(
                    state->receiver, state->candidate_key, state->hash
                ) < 0) {
                Py_DECREF(result);
                return NULL;
            }
            Py_CLEAR(state->candidate_key);
            Py_CLEAR(state->candidate_value);
            return result;
        }
        Py_CLEAR(state->candidate_key);
        Py_CLEAR(state->candidate_value);
        equal = -1;
    }

    if (state->has_default) {
        return Py_NewRef(state->default_value);
    }
    PyErr_SetObject(PyExc_KeyError, state->key);
    return NULL;
}

static PyObject *
dict_pop_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    DictPopState *state = dict_pop_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &dict_pop_vtable, state) < 0) {
        return NULL;
    }
    PyObject *result = dict_pop_continue(state, value, 1);
    adapter_leave(&frame);
    dict_pop_free_state(state);
    return result;
}

static const AleffAdapterVTable dict_pop_vtable = {
    .copy_state = dict_pop_copy_state,
    .free_state = dict_pop_free_state,
    .resume = dict_pop_resume,
};

static PyObject *
adapter_dict_pop(PyObject *self, PyObject *args)
{
    PyObject *key;
    PyObject *default_value = Py_None;
    if (!PyArg_ParseTuple(args, "O|O:pop", &key, &default_value)) {
        return NULL;
    }
    DictPopState state = {
        .receiver = self,
        .key = key,
        .default_value = default_value,
        .position = 0,
        .hash = -1,
        .candidate_hash = -1,
        .phase = DICT_POP_WAIT_HASH,
        .has_default = PyTuple_GET_SIZE(args) == 2,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &dict_pop_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = dict_pop_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.candidate_key);
    Py_XDECREF(state.candidate_value);
    return result;
}

typedef struct {
    PyObject *left;
    PyObject *right;
    int operation;
} DictCompareState;

static const AleffAdapterVTable dict_compare_vtable;

static void *
dict_compare_copy_state(const void *raw_state)
{
    const DictCompareState *state = raw_state;
    DictCompareState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->left = Py_NewRef(state->left);
    copy->right = Py_NewRef(state->right);
    copy->operation = state->operation;
    return copy;
}

static void
dict_compare_free_state(void *raw_state)
{
    DictCompareState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->left);
    Py_DECREF(state->right);
    PyMem_Free(state);
}

static PyObject *
dict_compare_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    int equal = PyObject_IsTrue(value);
    if (equal < 0) {
        return NULL;
    }
    if (raw_state != NULL && ((const DictCompareState *)raw_state)->operation) {
        equal = !equal;
    }
    return PyBool_FromLong(equal);
}

static const AleffAdapterVTable dict_compare_vtable = {
    .copy_state = dict_compare_copy_state,
    .free_state = dict_compare_free_state,
    .resume = dict_compare_resume,
};

static PyObject *
adapter_dict_compare(PyObject *self, PyObject *other, int operation)
{
    if (!PyDict_Check(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    if (PyDict_GET_SIZE(self) != PyDict_GET_SIZE(other)) {
        return PyBool_FromLong(operation ? 1 : 0);
    }
    DictCompareState state = {
        .left = self,
        .right = other,
        .operation = operation,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &dict_compare_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *left_key;
    PyObject *left_value;
    Py_ssize_t position = 0;
    PyObject *result = Py_NewRef(Py_True);
    while (result != NULL && PyDict_Next(self, &position, &left_key, &left_value)) {
        PyObject *right_value = PyDict_GetItemWithError(other, left_key);
        if (right_value == NULL) {
            Py_DECREF(result);
            result = Py_NewRef(Py_False);
            break;
        }
        int equal = PyObject_RichCompareBool(left_value, right_value, Py_EQ);
        if (equal < 0) {
            Py_CLEAR(result);
            break;
        }
        if (!equal) {
            Py_DECREF(result);
            result = Py_NewRef(Py_False);
            break;
        }
    }
    adapter_leave(&frame);
    if (result == NULL) {
        return NULL;
    }
    if (operation) {
        int equal = PyObject_IsTrue(result);
        Py_DECREF(result);
        return PyBool_FromLong(!equal);
    }
    return result;
}

static PyObject *adapter_dict_eq(PyObject *self, PyObject *other)
{
    return adapter_dict_compare(self, other, 0);
}

static PyObject *adapter_dict_ne(PyObject *self, PyObject *other)
{
    return adapter_dict_compare(self, other, 1);
}

static PyObject *
adapter_dict_richcompare(PyObject *self, PyObject *other, int operation)
{
    if (operation == Py_EQ) {
        return adapter_dict_compare(self, other, 0);
    }
    if (operation == Py_NE) {
        return adapter_dict_compare(self, other, 1);
    }
    Py_RETURN_NOTIMPLEMENTED;
}

typedef struct {
    PyObject *type;
    PyObject *value;
} DictFromKeysState;

static const AleffAdapterVTable dict_fromkeys_vtable;

static void *
dict_fromkeys_copy_state(const void *raw_state)
{
    const DictFromKeysState *state = raw_state;
    DictFromKeysState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->type = Py_NewRef(state->type);
    copy->value = Py_NewRef(state->value);
    return copy;
}

static void
dict_fromkeys_free_state(void *raw_state)
{
    DictFromKeysState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->type);
    Py_DECREF(state->value);
    PyMem_Free(state);
}

static PyObject *
dict_fromkeys_apply(DictFromKeysState *state, PyObject *items)
{
    PyObject *result;
    if (state->type == (PyObject *)&PyDict_Type) {
        result = PyDict_New();
        if (result != NULL) {
            Py_ssize_t size = PyList_GET_SIZE(items);
            for (Py_ssize_t index = 0; index < size; index++) {
                if (PyDict_SetItem(
                    result,
                    PyList_GET_ITEM(items, index),
                    state->value
                ) < 0) {
                    Py_CLEAR(result);
                    break;
                }
            }
        }
    }
    else {
        result = PyObject_CallMethod(
            state->type,
            "fromkeys",
            "OO",
            items,
            state->value
        );
    }
    return result;
}

static PyObject *
dict_fromkeys_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    return dict_fromkeys_apply((DictFromKeysState *)raw_state, value);
}

static const AleffAdapterVTable dict_fromkeys_vtable = {
    .copy_state = dict_fromkeys_copy_state,
    .free_state = dict_fromkeys_free_state,
    .resume = dict_fromkeys_resume,
};

static PyObject *
adapter_dict_fromkeys(PyObject *type, PyObject *args)
{
    PyObject *iterable;
    PyObject *value = Py_None;
    if (!PyArg_ParseTuple(args, "O|O:fromkeys", &iterable, &value)) {
        return NULL;
    }
    DictFromKeysState state = {
        .type = type,
        .value = value,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &dict_fromkeys_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *items = collect_iterable(iterable, COLLECT_LIST);
    PyObject *result = items == NULL ? NULL : dict_fromkeys_apply(&state, items);
    Py_XDECREF(items);
    adapter_leave(&frame);
    return result;
}

typedef enum {
    DICT_UPDATE_WAIT_SEQUENCE,
    DICT_UPDATE_WAIT_KEYS,
    DICT_UPDATE_WAIT_KEY_ITERATION,
    DICT_UPDATE_WAIT_VALUE,
} DictUpdatePhase;

typedef struct {
    PyObject *receiver;
    PyObject *baseline;
    PyObject *mapping;
    PyObject *kwargs;
    PyObject *keys;
    Py_ssize_t index;
    DictUpdatePhase phase;
    int is_mapping;
} DictUpdateState;

static const AleffAdapterVTable dict_update_vtable;

static void *
dict_update_copy_state(const void *raw_state)
{
    const DictUpdateState *state = raw_state;
    DictUpdateState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->receiver = Py_NewRef(state->receiver);
    copy->baseline = state->baseline == NULL
        ? PyDict_Copy(state->receiver)
        : Py_NewRef(state->baseline);
    if (copy->baseline == NULL) {
        Py_DECREF(copy->receiver);
        PyMem_Free(copy);
        return NULL;
    }
    copy->mapping = Py_XNewRef(state->mapping);
    copy->kwargs = Py_XNewRef(state->kwargs);
    copy->keys = Py_XNewRef(state->keys);
    copy->index = state->index;
    copy->phase = state->phase;
    copy->is_mapping = state->is_mapping;
    return copy;
}

static void
dict_update_free_state(void *raw_state)
{
    DictUpdateState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->receiver);
    Py_XDECREF(state->baseline);
    Py_XDECREF(state->mapping);
    Py_XDECREF(state->kwargs);
    Py_XDECREF(state->keys);
    PyMem_Free(state);
}

static int
dict_update_restore_receiver(DictUpdateState *state)
{
    if (state->baseline == NULL) {
        return 0;
    }
    PyDict_Clear(state->receiver);
    return PyDict_Update(state->receiver, state->baseline);
}

static PyObject *
dict_update_apply(DictUpdateState *state, PyObject *items)
{
    if (PyDict_MergeFromSeq2(state->receiver, items, 1) < 0) {
        return NULL;
    }
    if (state->kwargs != NULL && PyDict_GET_SIZE(state->kwargs) != 0) {
        if (PyDict_Update(state->receiver, state->kwargs) < 0) {
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

static PyObject *
dict_update_mapping_continue(
    DictUpdateState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (resumed_value == NULL) {
            return NULL;
        }
        if (dict_update_restore_receiver(state) < 0) {
            return NULL;
        }
        if (state->phase == DICT_UPDATE_WAIT_KEYS) {
            state->phase = DICT_UPDATE_WAIT_KEY_ITERATION;
            PyObject *keys = collect_iterable(resumed_value, COLLECT_LIST);
            if (keys == NULL) {
                return NULL;
            }
            state->keys = keys;
            state->index = 0;
        }
        else if (state->phase == DICT_UPDATE_WAIT_KEY_ITERATION) {
            Py_CLEAR(state->keys);
            state->keys = Py_NewRef(resumed_value);
            state->index = 0;
        }
        else if (state->phase == DICT_UPDATE_WAIT_VALUE) {
            PyObject *key = PyList_GET_ITEM(state->keys, state->index);
            if (PyDict_SetItem(state->receiver, key, resumed_value) < 0) {
                return NULL;
            }
            state->index++;
        }
    }

    if (state->keys == NULL) {
        state->phase = DICT_UPDATE_WAIT_KEYS;
        PyObject *keys_result = PyObject_CallMethod(
            state->mapping,
            "keys",
            NULL
        );
        if (keys_result == NULL) {
            return NULL;
        }
        state->phase = DICT_UPDATE_WAIT_KEY_ITERATION;
        PyObject *keys = collect_iterable(keys_result, COLLECT_LIST);
        Py_DECREF(keys_result);
        if (keys == NULL) {
            return NULL;
        }
        state->keys = keys;
        state->index = 0;
    }

    Py_ssize_t size = PyList_GET_SIZE(state->keys);
    while (state->index < size) {
        PyObject *key = PyList_GET_ITEM(state->keys, state->index);
        state->phase = DICT_UPDATE_WAIT_VALUE;
        PyObject *value = PyObject_GetItem(state->mapping, key);
        if (value == NULL) {
            return NULL;
        }
        if (PyDict_SetItem(state->receiver, key, value) < 0) {
            Py_DECREF(value);
            return NULL;
        }
        Py_DECREF(value);
        state->index++;
    }

    if (state->kwargs != NULL && PyDict_GET_SIZE(state->kwargs) != 0) {
        if (PyDict_Update(state->receiver, state->kwargs) < 0) {
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

static PyObject *
dict_update_continue(
    DictUpdateState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (state->is_mapping) {
        return dict_update_mapping_continue(
            state,
            resumed_value,
            is_resumed
        );
    }
    if (is_resumed) {
        if (resumed_value == NULL) {
            return NULL;
        }
        return dict_update_apply(state, resumed_value);
    }
    state->phase = DICT_UPDATE_WAIT_SEQUENCE;
    PyObject *items = collect_iterable(state->mapping, COLLECT_LIST);
    if (items == NULL) {
        return NULL;
    }
    PyObject *result = dict_update_apply(state, items);
    Py_DECREF(items);
    return result;
}

static PyObject *
dict_update_resume(const void *raw_state, PyObject *value)
{
    DictUpdateState *state = dict_update_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &dict_update_vtable, state) < 0) {
        return NULL;
    }
    PyObject *result = dict_update_continue(state, value, 1);
    adapter_leave(&frame);
    dict_update_free_state(state);
    return result;
}

static const AleffAdapterVTable dict_update_vtable = {
    .copy_state = dict_update_copy_state,
    .free_state = dict_update_free_state,
    .resume = dict_update_resume,
};

static PyObject *
adapter_dict_update(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *iterable;
    Py_ssize_t argument_count = PyTuple_GET_SIZE(args);
    if (argument_count > 1) {
        PyErr_Format(
            PyExc_TypeError,
            "update expected at most 1 argument, got %zd",
            argument_count
        );
        return NULL;
    }
    if (argument_count == 1) {
        iterable = PyTuple_GET_ITEM(args, 0);
        int is_mapping = 0;
        if (PyMapping_Check(iterable)) {
            is_mapping = PyObject_HasAttrString(iterable, "keys");
            if (is_mapping < 0) {
                return NULL;
            }
        }
        DictUpdateState state = {
            .receiver = self,
            .baseline = NULL,
            .mapping = Py_NewRef(iterable),
            .kwargs = Py_XNewRef(kwargs),
            .keys = NULL,
            .index = 0,
            .phase = is_mapping
                ? DICT_UPDATE_WAIT_KEYS
                : DICT_UPDATE_WAIT_SEQUENCE,
            .is_mapping = is_mapping,
        };
        AleffAdapterFrame frame;
        if (adapter_enter(&frame, &dict_update_vtable, &state) < 0) {
            return NULL;
        }
        PyObject *result = dict_update_continue(&state, NULL, 0);
        adapter_leave(&frame);
        Py_DECREF(state.mapping);
        Py_XDECREF(state.kwargs);
        Py_XDECREF(state.keys);
        if (result == NULL) {
            return NULL;
        }
        return result;
    }
    if (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0) {
        if (PyDict_Update(self, kwargs) < 0) {
            return NULL;
        }
    }
    Py_RETURN_NONE;
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

static PyMethodDef containers_dict_fromkeys_method = {
    .ml_name = "fromkeys",
    .ml_meth = adapter_dict_fromkeys,
    .ml_flags = METH_VARARGS | METH_CLASS,
    .ml_doc = "Create a new dictionary with keys from iterable and values from value.",
};

static PyMethodDef containers_dict_update_method = {
    .ml_name = "update",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_dict_update,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Update the dictionary with elements from another mapping or iterable.",
};

static PyMethodDef containers_dict_getitem_method = {
    .ml_name = "__getitem__",
    .ml_meth = adapter_dict_getitem,
    .ml_flags = METH_O,
    .ml_doc = "Return the value for key.",
};

static PyMethodDef containers_dict_setitem_method = {
    .ml_name = "__setitem__",
    .ml_meth = adapter_dict_setitem,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Set the value for key.",
};

static PyMethodDef containers_dict_delitem_method = {
    .ml_name = "__delitem__",
    .ml_meth = adapter_dict_delitem,
    .ml_flags = METH_O,
    .ml_doc = "Delete the value for key.",
};

static PyMethodDef containers_dict_contains_method = {
    .ml_name = "__contains__",
    .ml_meth = adapter_dict_contains,
    .ml_flags = METH_O,
    .ml_doc = "Return whether key is present.",
};

static PyMethodDef containers_dict_eq_method = {
    .ml_name = "__eq__",
    .ml_meth = adapter_dict_eq,
    .ml_flags = METH_O,
    .ml_doc = "Compare dictionaries for equality.",
};

static PyMethodDef containers_dict_ne_method = {
    .ml_name = "__ne__",
    .ml_meth = adapter_dict_ne,
    .ml_flags = METH_O,
    .ml_doc = "Compare dictionaries for inequality.",
};
