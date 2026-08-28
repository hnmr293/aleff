PyAPI_FUNC(int) _PyDict_SetItem_KnownHash(
    PyObject *mapping, PyObject *key, PyObject *value, Py_hash_t hash
);
PyAPI_FUNC(int) _PyDict_DelItem_KnownHash(
    PyObject *mapping, PyObject *key, Py_hash_t hash
);

#if PY_VERSION_HEX >= 0x030d0000
typedef struct {
    Py_ssize_t refcount;
    uint8_t log2_size;
    uint8_t log2_index_bytes;
    uint8_t kind;
#ifdef Py_GIL_DISABLED
    PyMutex mutex;
#endif
    uint32_t version;
    Py_ssize_t usable;
    Py_ssize_t entry_count;
    char indices[];
} AleffDictKeysHeader;

typedef struct {
    Py_hash_t hash;
    PyObject *key;
    PyObject *value;
} AleffDictKeyEntry;
#endif

static int
aleff_dict_next_with_hash(
    PyObject *mapping,
    Py_ssize_t *position,
    PyObject **key,
    PyObject **value,
    Py_hash_t *hash
)
{
#if PY_VERSION_HEX < 0x030d0000
    return _PyDict_Next(mapping, position, key, value, hash);
#else
    if (!PyDict_Next(mapping, position, key, value)) {
        return 0;
    }
    AleffDictKeysHeader *header = (AleffDictKeysHeader *)(
        (PyDictObject *)mapping
    )->ma_keys;
    if (header->kind == 0) {
        char *entries_start = header->indices + (
            (size_t)1 << header->log2_index_bytes
        );
        AleffDictKeyEntry *entries = (AleffDictKeyEntry *)entries_start;
        *hash = entries[*position - 1].hash;
    }
    else {
        *hash = PyObject_Hash(*key);
        if (*hash == -1) {
            return -1;
        }
    }
    return 1;
#endif
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
            Py_hash_t candidate_hash;
            int found;
            while ((found = aleff_dict_next_with_hash(
                state->receiver,
                &state->position,
                &candidate_key,
                &candidate_value,
                &candidate_hash
            )) > 0) {
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
            if (found < 0) {
                return NULL;
            }
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
    PyObject *candidate_key;
    PyObject *candidate_value;
    Py_ssize_t position;
    Py_hash_t hash;
    Py_hash_t candidate_hash;
    DictItemOperation operation;
    int phase;
} DictItemState;

enum {
    DICT_ITEM_WAIT_HASH,
    DICT_ITEM_WAIT_EQUAL,
};

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
    copy->candidate_key = Py_XNewRef(state->candidate_key);
    copy->candidate_value = Py_XNewRef(state->candidate_value);
    copy->position = state->position;
    copy->hash = state->hash;
    copy->candidate_hash = state->candidate_hash;
    copy->operation = state->operation;
    copy->phase = state->phase;
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
    Py_XDECREF(state->candidate_key);
    Py_XDECREF(state->candidate_value);
    PyMem_Free(state);
}

static PyObject *
dict_item_apply_match(DictItemState *state)
{
    switch (state->operation) {
        case DICT_ITEM_GET:
            return Py_NewRef(state->candidate_value);
        case DICT_ITEM_CONTAINS:
            Py_RETURN_TRUE;
        case DICT_ITEM_SET:
            if (_PyDict_SetItem_KnownHash(
                    state->receiver,
                    state->candidate_key,
                    state->value,
                    state->hash
                ) < 0) {
                return NULL;
            }
            Py_RETURN_NONE;
        case DICT_ITEM_DELETE:
            if (_PyDict_DelItem_KnownHash(
                    state->receiver,
                    state->candidate_key,
                    state->hash
                ) < 0) {
                return NULL;
            }
            Py_RETURN_NONE;
    }
    PyErr_SetString(PyExc_RuntimeError, "invalid dictionary item operation");
    return NULL;
}

static PyObject *
dict_item_continue(DictItemState *state, PyObject *resumed_value, int is_resumed)
{
    int equal = -1;
    if (is_resumed) {
        if (state->phase == DICT_ITEM_WAIT_HASH) {
            if (dict_get_normalize_hash(resumed_value, &state->hash) < 0) {
                return NULL;
            }
        }
        else if (state->phase == DICT_ITEM_WAIT_EQUAL) {
            equal = PyObject_IsTrue(resumed_value);
            if (equal < 0) {
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "invalid dictionary item phase");
            return NULL;
        }
    }
    else {
        state->phase = DICT_ITEM_WAIT_HASH;
        state->hash = PyObject_Hash(state->key);
        if (state->hash == -1) {
            return NULL;
        }
    }

    for (;;) {
        if (state->candidate_key == NULL) {
            PyObject *candidate_key;
            PyObject *candidate_value;
            int found;
            while ((found = aleff_dict_next_with_hash(
                state->receiver,
                &state->position,
                &candidate_key,
                &candidate_value,
                &state->candidate_hash
            )) > 0) {
                if (state->candidate_hash != state->hash) {
                    continue;
                }
                state->candidate_key = Py_NewRef(candidate_key);
                state->candidate_value = Py_NewRef(candidate_value);
                if (candidate_key == state->key) {
                    return dict_item_apply_match(state);
                }
                break;
            }
            if (found < 0) {
                return NULL;
            }
            if (state->candidate_key == NULL) {
                break;
            }
        }
        if (equal < 0) {
            state->phase = DICT_ITEM_WAIT_EQUAL;
            equal = PyObject_RichCompareBool(
                state->candidate_key,
                state->key,
                Py_EQ
            );
            if (equal < 0) {
                return NULL;
            }
        }
        if (equal) {
            return dict_item_apply_match(state);
        }
        Py_CLEAR(state->candidate_key);
        Py_CLEAR(state->candidate_value);
        equal = -1;
    }

    if (state->operation == DICT_ITEM_CONTAINS) {
        Py_RETURN_FALSE;
    }
    if (state->operation == DICT_ITEM_SET) {
        if (_PyDict_SetItem_KnownHash(
                state->receiver, state->key, state->value, state->hash
            ) < 0) {
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
    if (value == NULL) {
        return NULL;
    }
    DictItemState *state = dict_item_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &dict_item_vtable, state) < 0) {
        dict_item_free_state(state);
        return NULL;
    }
    PyObject *result = dict_item_continue(state, value, 1);
    adapter_leave(&frame);
    dict_item_free_state(state);
    return result;
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
        .candidate_key = NULL,
        .candidate_value = NULL,
        .position = 0,
        .hash = -1,
        .candidate_hash = -1,
        .operation = operation,
        .phase = DICT_ITEM_WAIT_HASH,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &dict_item_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = dict_item_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.candidate_key);
    Py_XDECREF(state.candidate_value);
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
            Py_hash_t candidate_hash;
            int found;
            while ((found = aleff_dict_next_with_hash(
                state->receiver,
                &state->position,
                &candidate_key,
                &candidate_value,
                &candidate_hash
            )) > 0) {
                if (candidate_hash != state->hash) {
                    continue;
                }
                state->candidate_key = Py_NewRef(candidate_key);
                state->candidate_value = Py_NewRef(candidate_value);
                state->candidate_hash = candidate_hash;
                break;
            }
            if (found < 0) {
                return NULL;
            }
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
    PyObject *left_key;
    PyObject *left_value;
    PyObject *right_key;
    PyObject *right_value;
    Py_ssize_t left_position;
    Py_ssize_t right_position;
    Py_hash_t left_hash;
    Py_hash_t right_hash;
    int operation;
    int phase;
} DictCompareState;

enum {
    DICT_COMPARE_WAIT_KEY_EQUAL,
    DICT_COMPARE_WAIT_VALUE_EQUAL,
};

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
    copy->left_key = Py_XNewRef(state->left_key);
    copy->left_value = Py_XNewRef(state->left_value);
    copy->right_key = Py_XNewRef(state->right_key);
    copy->right_value = Py_XNewRef(state->right_value);
    copy->left_position = state->left_position;
    copy->right_position = state->right_position;
    copy->left_hash = state->left_hash;
    copy->right_hash = state->right_hash;
    copy->operation = state->operation;
    copy->phase = state->phase;
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
    Py_XDECREF(state->left_key);
    Py_XDECREF(state->left_value);
    Py_XDECREF(state->right_key);
    Py_XDECREF(state->right_value);
    PyMem_Free(state);
}

static PyObject *
dict_compare_result(const DictCompareState *state, int equal)
{
    if (state->operation) {
        equal = !equal;
    }
    return PyBool_FromLong(equal);
}

static PyObject *
dict_compare_continue(
    DictCompareState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    int key_equal = -1;
    int value_equal = -1;
    if (is_resumed) {
        int equal = PyObject_IsTrue(resumed_value);
        if (equal < 0) {
            return NULL;
        }
        if (state->phase == DICT_COMPARE_WAIT_KEY_EQUAL) {
            key_equal = equal;
        }
        else if (state->phase == DICT_COMPARE_WAIT_VALUE_EQUAL) {
            value_equal = equal;
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "invalid dictionary comparison phase");
            return NULL;
        }
    }

    for (;;) {
        if (state->left_key == NULL) {
            PyObject *key;
            PyObject *value;
            int found = aleff_dict_next_with_hash(
                state->left,
                &state->left_position,
                &key,
                &value,
                &state->left_hash
            );
            if (found < 0) {
                return NULL;
            }
            if (!found) {
                return dict_compare_result(state, 1);
            }
            state->left_key = Py_NewRef(key);
            state->left_value = Py_NewRef(value);
            state->right_position = 0;
        }

        if (state->right_key == NULL) {
            PyObject *key;
            PyObject *value;
            int found;
            while ((found = aleff_dict_next_with_hash(
                state->right,
                &state->right_position,
                &key,
                &value,
                &state->right_hash
            )) > 0) {
                if (state->right_hash != state->left_hash) {
                    continue;
                }
                state->right_key = Py_NewRef(key);
                state->right_value = Py_NewRef(value);
                if (key == state->left_key) {
                    key_equal = 1;
                }
                break;
            }
            if (found < 0) {
                return NULL;
            }
            if (state->right_key == NULL) {
                return dict_compare_result(state, 0);
            }
        }

        if (key_equal < 0) {
            state->phase = DICT_COMPARE_WAIT_KEY_EQUAL;
            key_equal = PyObject_RichCompareBool(
                state->right_key,
                state->left_key,
                Py_EQ
            );
            if (key_equal < 0) {
                return NULL;
            }
        }
        if (!key_equal) {
            Py_CLEAR(state->right_key);
            Py_CLEAR(state->right_value);
            key_equal = -1;
            continue;
        }

        if (value_equal < 0) {
            state->phase = DICT_COMPARE_WAIT_VALUE_EQUAL;
            value_equal = PyObject_RichCompareBool(
                state->left_value,
                state->right_value,
                Py_EQ
            );
            if (value_equal < 0) {
                return NULL;
            }
        }
        if (!value_equal) {
            return dict_compare_result(state, 0);
        }
        Py_CLEAR(state->left_key);
        Py_CLEAR(state->left_value);
        Py_CLEAR(state->right_key);
        Py_CLEAR(state->right_value);
        key_equal = -1;
        value_equal = -1;
    }
}

static PyObject *
dict_compare_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    DictCompareState *state = dict_compare_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &dict_compare_vtable, state) < 0) {
        dict_compare_free_state(state);
        return NULL;
    }
    PyObject *result = dict_compare_continue(state, value, 1);
    adapter_leave(&frame);
    dict_compare_free_state(state);
    return result;
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
        .left_key = NULL,
        .left_value = NULL,
        .right_key = NULL,
        .right_value = NULL,
        .left_position = 0,
        .right_position = 0,
        .left_hash = -1,
        .right_hash = -1,
        .operation = operation,
        .phase = DICT_COMPARE_WAIT_KEY_EQUAL,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &dict_compare_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = dict_compare_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.left_key);
    Py_XDECREF(state.left_value);
    Py_XDECREF(state.right_key);
    Py_XDECREF(state.right_value);
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
    PyObject *result;
    PyObject *items;
    Py_ssize_t index;
    int phase;
    DictItemState insertion;
    int insertion_active;
} DictFromKeysState;

enum {
    DICT_FROMKEYS_WAIT_COLLECT,
    DICT_FROMKEYS_WAIT_INSERT,
};

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
    copy->result = NULL;
    if (state->result != NULL) {
        copy->result = PyObject_CallNoArgs(state->type);
        if (copy->result == NULL || PyDict_Update(
                copy->result, state->result
            ) < 0) {
            Py_XDECREF(copy->result);
            Py_DECREF(copy->type);
            Py_DECREF(copy->value);
            PyMem_Free(copy);
            return NULL;
        }
    }
    copy->items = Py_XNewRef(state->items);
    copy->index = state->index;
    copy->phase = state->phase;
    copy->insertion = state->insertion;
    copy->insertion.receiver = copy->result;
    copy->insertion.key = state->insertion_active
        ? PyList_GET_ITEM(copy->items, copy->index)
        : NULL;
    copy->insertion.value = copy->value;
    copy->insertion.candidate_key = Py_XNewRef(
        state->insertion.candidate_key
    );
    copy->insertion.candidate_value = Py_XNewRef(
        state->insertion.candidate_value
    );
    copy->insertion_active = state->insertion_active;
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
    Py_XDECREF(state->result);
    Py_XDECREF(state->items);
    Py_XDECREF(state->insertion.candidate_key);
    Py_XDECREF(state->insertion.candidate_value);
    PyMem_Free(state);
}

static PyObject *
dict_fromkeys_continue(
    DictFromKeysState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        if (resumed_value == NULL) {
            return NULL;
        }
        if (state->phase == DICT_FROMKEYS_WAIT_COLLECT) {
            if (!PyList_Check(resumed_value)) {
                PyErr_SetString(PyExc_RuntimeError, "invalid fromkeys collection");
                return NULL;
            }
            state->items = Py_NewRef(resumed_value);
        }
        else if (state->phase == DICT_FROMKEYS_WAIT_INSERT) {
            PyObject *inserted = dict_item_continue(
                &state->insertion, resumed_value, 1
            );
            if (inserted == NULL) {
                return NULL;
            }
            Py_DECREF(inserted);
            Py_CLEAR(state->insertion.candidate_key);
            Py_CLEAR(state->insertion.candidate_value);
            state->insertion_active = 0;
            state->index++;
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "invalid fromkeys phase");
            return NULL;
        }
    }
    if (state->items == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "fromkeys items are unavailable");
        return NULL;
    }
    if (state->result == NULL) {
        state->result = PyObject_CallNoArgs(state->type);
        if (state->result == NULL) {
            return NULL;
        }
    }
    Py_ssize_t size = PyList_GET_SIZE(state->items);
    while (state->index < size) {
        PyObject *key = PyList_GET_ITEM(state->items, state->index);
        state->phase = DICT_FROMKEYS_WAIT_INSERT;
        state->insertion = (DictItemState){
            .receiver = state->result,
            .key = key,
            .value = state->value,
            .candidate_key = NULL,
            .candidate_value = NULL,
            .position = 0,
            .hash = -1,
            .candidate_hash = -1,
            .operation = DICT_ITEM_SET,
            .phase = DICT_ITEM_WAIT_HASH,
        };
        state->insertion_active = 1;
        PyObject *inserted = dict_item_continue(
            &state->insertion, NULL, 0
        );
        if (inserted == NULL) {
            return NULL;
        }
        Py_DECREF(inserted);
        Py_CLEAR(state->insertion.candidate_key);
        Py_CLEAR(state->insertion.candidate_value);
        state->insertion_active = 0;
        state->index++;
    }
    return Py_NewRef(state->result);
}

static PyObject *
dict_fromkeys_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    DictFromKeysState *state = dict_fromkeys_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &dict_fromkeys_vtable, state) < 0) {
        dict_fromkeys_free_state(state);
        return NULL;
    }
    PyObject *result = dict_fromkeys_continue(state, value, 1);
    adapter_leave(&frame);
    dict_fromkeys_free_state(state);
    return result;
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
        .result = NULL,
        .items = NULL,
        .index = 0,
        .phase = DICT_FROMKEYS_WAIT_COLLECT,
        .insertion = {0},
        .insertion_active = 0,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &dict_fromkeys_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *items = collect_iterable(iterable, COLLECT_LIST);
    PyObject *result = NULL;
    if (items != NULL) {
        state.items = items;
        result = dict_fromkeys_continue(&state, NULL, 0);
    }
    adapter_leave(&frame);
    Py_XDECREF(state.result);
    Py_XDECREF(state.items);
    Py_XDECREF(state.insertion.candidate_key);
    Py_XDECREF(state.insertion.candidate_value);
    return result;
}

typedef enum {
    DICT_UPDATE_WAIT_SEQUENCE,
    DICT_UPDATE_WAIT_SEQUENCE_NEXT,
    DICT_UPDATE_WAIT_KEYS,
    DICT_UPDATE_WAIT_KEY_ITERATION,
    DICT_UPDATE_WAIT_VALUE,
    DICT_UPDATE_WAIT_PAIR_COLLECTION,
    DICT_UPDATE_WAIT_INSERT,
} DictUpdatePhase;

typedef struct {
    PyObject *receiver;
    PyObject *baseline;
    PyObject *mapping;
    PyObject *kwargs;
    PyObject *keys;
    PyObject *iterator;
    PyObject *item;
    PyObject *pair;
    PyObject *pending_value;
    Py_ssize_t index;
    DictUpdatePhase phase;
    int is_mapping;
    DictItemState insertion;
    int insertion_active;
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
    copy->iterator = Py_XNewRef(state->iterator);
    copy->item = Py_XNewRef(state->item);
    copy->pair = Py_XNewRef(state->pair);
    copy->pending_value = Py_XNewRef(state->pending_value);
    copy->index = state->index;
    copy->phase = state->phase;
    copy->is_mapping = state->is_mapping;
    copy->insertion = state->insertion;
    copy->insertion.receiver = copy->receiver;
    copy->insertion.key = state->insertion_active
        ? state->insertion.key
        : NULL;
    copy->insertion.value = state->insertion_active
        ? copy->pending_value != NULL
            ? copy->pending_value
            : state->insertion.value
        : NULL;
    copy->insertion.candidate_key = Py_XNewRef(
        state->insertion.candidate_key
    );
    copy->insertion.candidate_value = Py_XNewRef(
        state->insertion.candidate_value
    );
    copy->insertion_active = state->insertion_active;
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
    Py_XDECREF(state->iterator);
    Py_XDECREF(state->item);
    Py_XDECREF(state->pair);
    Py_XDECREF(state->pending_value);
    Py_XDECREF(state->insertion.candidate_key);
    Py_XDECREF(state->insertion.candidate_value);
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

static void
dict_update_clear_insertion(DictUpdateState *state)
{
    Py_CLEAR(state->insertion.candidate_key);
    Py_CLEAR(state->insertion.candidate_value);
    state->insertion_active = 0;
}

static PyObject *
dict_update_insert(
    DictUpdateState *state,
    PyObject *key,
    PyObject *value,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (!is_resumed) {
        state->insertion = (DictItemState){
            .receiver = state->receiver,
            .key = key,
            .value = value,
            .candidate_key = NULL,
            .candidate_value = NULL,
            .position = 0,
            .hash = -1,
            .candidate_hash = -1,
            .operation = DICT_ITEM_SET,
            .phase = DICT_ITEM_WAIT_HASH,
        };
        state->insertion_active = 1;
    }
    state->phase = DICT_UPDATE_WAIT_INSERT;
    return dict_item_continue(
        &state->insertion,
        resumed_value,
        is_resumed
    );
}

static PyObject *
dict_update_sequence_continue(
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
        if (state->phase == DICT_UPDATE_WAIT_SEQUENCE) {
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
        else if (state->phase == DICT_UPDATE_WAIT_SEQUENCE_NEXT) {
            state->item = Py_NewRef(resumed_value);
        }
        else if (state->phase == DICT_UPDATE_WAIT_PAIR_COLLECTION) {
            state->pair = Py_NewRef(resumed_value);
        }
        else if (state->phase == DICT_UPDATE_WAIT_INSERT) {
            PyObject *inserted = dict_update_insert(
                state,
                state->insertion.key,
                state->insertion.value,
                resumed_value,
                1
            );
            if (inserted == NULL) {
                return NULL;
            }
            Py_DECREF(inserted);
            dict_update_clear_insertion(state);
            Py_CLEAR(state->item);
            Py_CLEAR(state->pair);
            state->index++;
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "invalid dictionary update phase");
            return NULL;
        }
    }

    if (state->iterator == NULL) {
        state->phase = DICT_UPDATE_WAIT_SEQUENCE;
        state->iterator = PyObject_GetIter(state->mapping);
        if (state->iterator == NULL) {
            return NULL;
        }
    }
    for (;;) {
        if (state->item == NULL) {
            state->phase = DICT_UPDATE_WAIT_SEQUENCE_NEXT;
            state->item = Py_TYPE(state->iterator)->tp_iternext(state->iterator);
            if (state->item == NULL) {
                if (PyErr_Occurred()) {
                    if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
                        return NULL;
                    }
                    PyErr_Clear();
                }
                break;
            }
        }
        if (state->pair == NULL) {
            state->phase = DICT_UPDATE_WAIT_PAIR_COLLECTION;
            state->pair = collect_iterable(state->item, COLLECT_LIST);
            if (state->pair == NULL) {
                if (PyErr_ExceptionMatches(PyExc_TypeError)) {
                    PyErr_Clear();
                    PyErr_Format(
                        PyExc_TypeError,
                        "cannot convert dictionary update sequence element #%zd to a sequence",
                        state->index
                    );
                }
                return NULL;
            }
        }
        Py_ssize_t pair_size = PyList_GET_SIZE(state->pair);
        if (pair_size != 2) {
            PyErr_Format(
                PyExc_ValueError,
                "dictionary update sequence element #%zd has length %zd; 2 is required",
                state->index,
                pair_size
            );
            return NULL;
        }
        PyObject *inserted = dict_update_insert(
            state,
            PyList_GET_ITEM(state->pair, 0),
            PyList_GET_ITEM(state->pair, 1),
            NULL,
            0
        );
        if (inserted == NULL) {
            return NULL;
        }
        Py_DECREF(inserted);
        dict_update_clear_insertion(state);
        Py_CLEAR(state->item);
        Py_CLEAR(state->pair);
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
            state->pending_value = Py_NewRef(resumed_value);
        }
        else if (state->phase == DICT_UPDATE_WAIT_INSERT) {
            PyObject *inserted = dict_update_insert(
                state,
                state->insertion.key,
                state->insertion.value,
                resumed_value,
                1
            );
            if (inserted == NULL) {
                return NULL;
            }
            Py_DECREF(inserted);
            dict_update_clear_insertion(state);
            Py_CLEAR(state->pending_value);
            state->index++;
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "invalid dictionary update phase");
            return NULL;
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
        if (state->pending_value == NULL) {
            state->phase = DICT_UPDATE_WAIT_VALUE;
            state->pending_value = PyObject_GetItem(state->mapping, key);
            if (state->pending_value == NULL) {
                return NULL;
            }
        }
        PyObject *inserted = dict_update_insert(
            state,
            key,
            state->pending_value,
            NULL,
            0
        );
        if (inserted == NULL) {
            return NULL;
        }
        Py_DECREF(inserted);
        dict_update_clear_insertion(state);
        Py_CLEAR(state->pending_value);
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
    return dict_update_sequence_continue(state, resumed_value, is_resumed);
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
        int is_mapping = PyObject_HasAttrString(iterable, "keys");
        if (is_mapping < 0) {
            return NULL;
        }
        DictUpdateState state = {
            .receiver = self,
            .baseline = NULL,
            .mapping = Py_NewRef(iterable),
            .kwargs = Py_XNewRef(kwargs),
            .keys = NULL,
            .iterator = NULL,
            .item = NULL,
            .pair = NULL,
            .pending_value = NULL,
            .index = 0,
            .phase = is_mapping
                ? DICT_UPDATE_WAIT_KEYS
                : DICT_UPDATE_WAIT_SEQUENCE,
            .is_mapping = is_mapping,
            .insertion = {0},
            .insertion_active = 0,
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
        Py_XDECREF(state.iterator);
        Py_XDECREF(state.item);
        Py_XDECREF(state.pair);
        Py_XDECREF(state.pending_value);
        Py_XDECREF(state.insertion.candidate_key);
        Py_XDECREF(state.insertion.candidate_value);
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
    if (
        callable == (PyObject *)&PyDict_Type &&
        PyVectorcall_NARGS(nargsf) == 1 &&
        (kwnames == NULL || PyTuple_GET_SIZE(kwnames) == 0)
    ) {
        int has_keys = PyObject_HasAttrString(args[0], "keys");
        if (has_keys < 0) {
            return NULL;
        }
        if (has_keys) {
            return original_dict_vectorcall(callable, args, nargsf, kwnames);
        }
    }
    return adapter_collect_vectorcall(
        callable, args, nargsf, kwnames,
        &PyDict_Type, COLLECT_DICT, original_dict_vectorcall
    );
}

static int
adapter_dict_init(PyObject *self, PyObject *args, PyObject *kwargs)
{
    int has_keys = 0;
    if (PyTuple_GET_SIZE(args) == 1) {
        has_keys = PyObject_HasAttrString(PyTuple_GET_ITEM(args, 0), "keys");
        if (has_keys < 0) {
            return -1;
        }
    }
    if (
        PyTuple_GET_SIZE(args) != 1 ||
        (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0) ||
        has_keys
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
