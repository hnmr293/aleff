typedef enum {
    OPERATOR_PASSTHROUGH,
    OPERATOR_BOOL,
    OPERATOR_NOT,
    OPERATOR_INDEX,
    OPERATOR_LENGTH_HINT,
    OPERATOR_VOID,
} OperatorResumeKind;

typedef enum {
    OPERATOR_SEARCH_INDEX,
    OPERATOR_SEARCH_COUNT,
} OperatorSearchKind;

typedef struct {
    OperatorResumeKind kind;
} OperatorCallState;

static PyObject *operator_call_adapter(PyObject *self, PyObject *args);
static PyObject *operator_search_adapter(PyObject *self, PyObject *args);

typedef enum {
    OPERATOR_ACCESSOR_ATTR,
    OPERATOR_ACCESSOR_ITEM,
    OPERATOR_ACCESSOR_METHOD,
} OperatorAccessorKind;

typedef enum {
    OPERATOR_ACCESSOR_WAIT_ATTR,
    OPERATOR_ACCESSOR_WAIT_ITEM,
    OPERATOR_ACCESSOR_WAIT_METHOD_ATTR,
    OPERATOR_ACCESSOR_WAIT_METHOD_CALL,
} OperatorAccessorPhase;

typedef struct {
    PyObject *spec;
    PyObject *target;
    PyObject *current;
    PyObject *parts;
    PyObject *method;
    PyObject *results;
    Py_ssize_t index;
    Py_ssize_t part_index;
    OperatorAccessorPhase phase;
} OperatorAccessorState;

static PyObject *operator_accessor_constructor_adapter(
    PyObject *self,
    PyObject *args,
    PyObject *kwargs
);
static PyObject *operator_accessor_adapter(
    PyObject *self,
    PyObject *args,
    PyObject *kwargs
);

static PyMethodDef operator_call_method = {
    .ml_name = "operator",
    .ml_meth = operator_call_adapter,
    .ml_flags = METH_VARARGS,
    .ml_doc = NULL,
};

static PyMethodDef operator_search_method = {
    .ml_name = "operator_search",
    .ml_meth = operator_search_adapter,
    .ml_flags = METH_VARARGS,
    .ml_doc = NULL,
};

static PyMethodDef operator_accessor_constructor_method = {
    .ml_name = "operator_accessor_constructor",
    .ml_meth = _PyCFunction_CAST(operator_accessor_constructor_adapter),
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = NULL,
};

static PyMethodDef operator_accessor_method = {
    .ml_name = "operator_accessor",
    .ml_meth = _PyCFunction_CAST(operator_accessor_adapter),
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = NULL,
};

static void *
operator_call_copy_state(const void *raw_state)
{
    const OperatorCallState *state = raw_state;
    OperatorCallState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    return copy;
}

static void
operator_call_free_state(void *raw_state)
{
    PyMem_Free(raw_state);
}

static PyObject *
operator_call_resume(const void *raw_state, PyObject *value)
{
    const OperatorCallState *state = raw_state;
    if (value == NULL) {
        return NULL;
    }
    switch (state->kind) {
        case OPERATOR_PASSTHROUGH:
            return Py_NewRef(value);
        case OPERATOR_BOOL: {
            int truth = PyObject_IsTrue(value);
            if (truth < 0) {
                return NULL;
            }
            return PyBool_FromLong(truth);
        }
        case OPERATOR_NOT: {
            int truth = PyObject_IsTrue(value);
            if (truth < 0) {
                return NULL;
            }
            return PyBool_FromLong(!truth);
        }
        case OPERATOR_INDEX:
            return PyNumber_Index(value);
        case OPERATOR_LENGTH_HINT: {
            PyObject *index = PyNumber_Index(value);
            if (index == NULL) {
                return NULL;
            }
            Py_ssize_t result = PyLong_AsSsize_t(index);
            Py_DECREF(index);
            if (result == -1 && PyErr_Occurred()) {
                return NULL;
            }
            if (result < 0) {
                PyErr_SetString(
                    PyExc_ValueError,
                    "__length_hint__() should return >= 0"
                );
                return NULL;
            }
            return PyLong_FromSsize_t(result);
        }
        case OPERATOR_VOID:
            return Py_NewRef(Py_None);
    }
    PyErr_SetString(PyExc_RuntimeError, "unknown operator resume kind");
    return NULL;
}

static const AleffAdapterVTable operator_call_vtable = {
    .copy_state = operator_call_copy_state,
    .free_state = operator_call_free_state,
    .resume = operator_call_resume,
};

static PyObject *
operator_call_adapter(PyObject *self, PyObject *args)
{
    if (!PyTuple_Check(self) || PyTuple_GET_SIZE(self) != 2) {
        PyErr_SetString(PyExc_RuntimeError, "invalid operator adapter state");
        return NULL;
    }
    PyObject *original = PyTuple_GET_ITEM(self, 0);
    Py_ssize_t kind = PyLong_AsSsize_t(PyTuple_GET_ITEM(self, 1));
    if (kind < 0 && PyErr_Occurred()) {
        return NULL;
    }
    OperatorCallState state = {
        .kind = (OperatorResumeKind)kind,
    };
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &operator_call_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = PyObject_Call(original, args, NULL);
    adapter_leave(&frame);
    return result;
}

static int
operator_accessor_parse_target(
    PyObject *args,
    PyObject *kwargs,
    const char *name,
    PyObject **target
)
{
    if (kwargs != NULL && PyDict_Size(kwargs) != 0) {
        PyErr_Format(PyExc_TypeError, "%s() takes no keyword arguments", name);
        return -1;
    }
    if (!PyArg_UnpackTuple(args, name, 1, 1, target)) {
        return -1;
    }
    return 0;
}

static void *
operator_accessor_copy_state(const void *raw_state)
{
    const OperatorAccessorState *state = raw_state;
    OperatorAccessorState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->spec = Py_NewRef(state->spec);
    copy->target = Py_NewRef(state->target);
    copy->current = Py_XNewRef(state->current);
    copy->parts = Py_XNewRef(state->parts);
    copy->method = Py_XNewRef(state->method);
    copy->results = Py_XNewRef(state->results);
    if (state->results != NULL) {
        copy->results = PyList_GetSlice(state->results, 0, PyList_GET_SIZE(state->results));
        if (copy->results == NULL) {
            Py_DECREF(copy->spec);
            Py_DECREF(copy->target);
            Py_XDECREF(copy->current);
            Py_XDECREF(copy->parts);
            Py_XDECREF(copy->method);
            PyMem_Free(copy);
            return NULL;
        }
    }
    if (state->parts != NULL) {
        Py_XDECREF(copy->parts);
        copy->parts = PyList_GetSlice(state->parts, 0, PyList_GET_SIZE(state->parts));
        if (copy->parts == NULL) {
            Py_DECREF(copy->spec);
            Py_DECREF(copy->target);
            Py_XDECREF(copy->current);
            Py_XDECREF(copy->method);
            Py_XDECREF(copy->results);
            PyMem_Free(copy);
            return NULL;
        }
    }
    return copy;
}

static void
operator_accessor_free_state(void *raw_state)
{
    OperatorAccessorState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->spec);
    Py_DECREF(state->target);
    Py_XDECREF(state->current);
    Py_XDECREF(state->parts);
    Py_XDECREF(state->method);
    Py_XDECREF(state->results);
    PyMem_Free(state);
}

static PyObject *operator_accessor_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable operator_accessor_vtable = {
    .copy_state = operator_accessor_copy_state,
    .free_state = operator_accessor_free_state,
    .resume = operator_accessor_resume,
};

static PyObject *
operator_accessor_continue(
    OperatorAccessorState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    OperatorAccessorKind kind = (OperatorAccessorKind)
        PyLong_AsLong(PyTuple_GET_ITEM(state->spec, 0));
    if (PyErr_Occurred()) {
        return NULL;
    }

    if (is_resumed) {
        if (resumed_value == NULL) {
            return NULL;
        }
        switch (state->phase) {
            case OPERATOR_ACCESSOR_WAIT_ATTR:
                Py_XSETREF(state->current, Py_NewRef(resumed_value));
                state->part_index++;
                break;
            case OPERATOR_ACCESSOR_WAIT_ITEM:
                if (state->results == NULL) {
                    return Py_NewRef(resumed_value);
                }
                if (PyList_Append(state->results, resumed_value) < 0) {
                    return NULL;
                }
                state->index++;
                break;
            case OPERATOR_ACCESSOR_WAIT_METHOD_ATTR:
                state->method = Py_NewRef(resumed_value);
                state->phase = OPERATOR_ACCESSOR_WAIT_METHOD_CALL;
                break;
            case OPERATOR_ACCESSOR_WAIT_METHOD_CALL:
                return Py_NewRef(resumed_value);
        }
    }

    if (kind == OPERATOR_ACCESSOR_ATTR) {
        PyObject *names = PyTuple_GET_ITEM(state->spec, 1);
        for (;;) {
            if (state->parts == NULL) {
                if (state->index >= PyTuple_GET_SIZE(names)) {
                    PyObject *result = state->results == NULL
                        ? Py_NewRef(Py_None)
                        : PyList_AsTuple(state->results);
                    Py_CLEAR(state->results);
                    return result;
                }
                PyObject *separator = PyUnicode_FromString(".");
                if (separator == NULL) {
                    return NULL;
                }
                state->parts = PyUnicode_Split(
                    PyTuple_GET_ITEM(names, state->index),
                    separator,
                    -1
                );
                Py_DECREF(separator);
                if (state->parts == NULL) {
                    return NULL;
                }
                state->part_index = 0;
                state->current = Py_NewRef(state->target);
            }
            if (state->part_index < PyList_GET_SIZE(state->parts)) {
                state->phase = OPERATOR_ACCESSOR_WAIT_ATTR;
                PyObject *value = PyObject_GetAttr(
                    state->current,
                    PyList_GET_ITEM(state->parts, state->part_index)
                );
                if (value == NULL) {
                    return NULL;
                }
                Py_XSETREF(state->current, value);
                state->part_index++;
                continue;
            }
            PyObject *value = state->current;
            state->current = NULL;
            Py_CLEAR(state->parts);
            if (state->results == NULL) {
                if (state->index + 1 == PyTuple_GET_SIZE(names)) {
                    return value;
                }
                state->results = PyList_New(0);
            }
            if (state->results == NULL) {
                Py_DECREF(value);
                return NULL;
            }
            if (PyList_Append(state->results, value) < 0) {
                Py_DECREF(value);
                return NULL;
            }
            Py_DECREF(value);
            state->index++;
        }
    }

    if (kind == OPERATOR_ACCESSOR_ITEM) {
        PyObject *items = PyTuple_GET_ITEM(state->spec, 1);
        while (state->index < PyTuple_GET_SIZE(items)) {
            state->phase = OPERATOR_ACCESSOR_WAIT_ITEM;
            PyObject *value = PyObject_GetItem(
                state->target,
                PyTuple_GET_ITEM(items, state->index)
            );
            if (value == NULL) {
                return NULL;
            }
            if (state->index + 1 == PyTuple_GET_SIZE(items)) {
                if (state->results == NULL) {
                    return value;
                }
            }
            if (PyList_Append(state->results, value) < 0) {
                Py_DECREF(value);
                return NULL;
            }
            Py_DECREF(value);
            state->index++;
        }
        return PyList_AsTuple(state->results);
    }

    if (state->method == NULL) {
        state->phase = OPERATOR_ACCESSOR_WAIT_METHOD_ATTR;
        PyObject *method = PyObject_GetAttr(
            state->target,
            PyTuple_GET_ITEM(state->spec, 1)
        );
        if (method == NULL) {
            return NULL;
        }
        state->method = method;
    }
    state->phase = OPERATOR_ACCESSOR_WAIT_METHOD_CALL;
    PyObject *result = PyObject_Call(
        state->method,
        PyTuple_GET_ITEM(state->spec, 2),
        PyTuple_GET_ITEM(state->spec, 3)
    );
    if (result == NULL) {
        return NULL;
    }
    return result;
}

static PyObject *
operator_accessor_resume(const void *raw_state, PyObject *value)
{
    const OperatorAccessorState *source = raw_state;
    OperatorAccessorState *state = operator_accessor_copy_state(source);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &operator_accessor_vtable, state) < 0) {
        return NULL;
    }
    PyObject *result = operator_accessor_continue(state, value, 1);
    adapter_leave(&frame);
    operator_accessor_free_state(state);
    return result;
}

static PyObject *
operator_accessor_adapter(PyObject *self, PyObject *args, PyObject *kwargs)
{
    if (!PyTuple_Check(self) || PyTuple_GET_SIZE(self) < 2) {
        PyErr_SetString(PyExc_RuntimeError, "invalid operator accessor state");
        return NULL;
    }
    OperatorAccessorKind kind = (OperatorAccessorKind)
        PyLong_AsLong(PyTuple_GET_ITEM(self, 0));
    if (PyErr_Occurred()) {
        return NULL;
    }
    PyObject *target;
    const char *name = kind == OPERATOR_ACCESSOR_ATTR
        ? "attrgetter"
        : kind == OPERATOR_ACCESSOR_ITEM ? "itemgetter" : "methodcaller";
    if (operator_accessor_parse_target(args, kwargs, name, &target) < 0) {
        return NULL;
    }
    OperatorAccessorState state = {
        .spec = self,
        .target = target,
        .current = NULL,
        .parts = NULL,
        .method = NULL,
        .results = NULL,
        .index = 0,
        .part_index = 0,
        .phase = OPERATOR_ACCESSOR_WAIT_ATTR,
    };
    PyObject *items = PyTuple_GET_ITEM(self, 1);
    if (kind != OPERATOR_ACCESSOR_METHOD && PyTuple_GET_SIZE(items) > 1) {
        state.results = PyList_New(0);
        if (state.results == NULL) {
            return NULL;
        }
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &operator_accessor_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = operator_accessor_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.current);
    Py_XDECREF(state.parts);
    Py_XDECREF(state.method);
    Py_XDECREF(state.results);
    return result;
}

static PyObject *
operator_accessor_constructor_adapter(
    PyObject *self,
    PyObject *args,
    PyObject *kwargs
)
{
    if (!PyTuple_Check(self) || PyTuple_GET_SIZE(self) != 3) {
        PyErr_SetString(PyExc_RuntimeError, "invalid operator accessor constructor state");
        return NULL;
    }
    PyObject *original = PyTuple_GET_ITEM(self, 0);
    PyObject *module = PyTuple_GET_ITEM(self, 1);
    OperatorAccessorKind kind = (OperatorAccessorKind)
        PyLong_AsLong(PyTuple_GET_ITEM(self, 2));
    if (PyErr_Occurred()) {
        return NULL;
    }
    PyObject *validated = PyObject_Call(original, args, kwargs);
    if (validated == NULL) {
        return NULL;
    }
    Py_DECREF(validated);

    PyObject *tag = PyLong_FromLong(kind);
    PyObject *spec = NULL;
    if (tag == NULL) {
        return NULL;
    }
    if (kind == OPERATOR_ACCESSOR_METHOD) {
        PyObject *call_args = PyTuple_GetSlice(args, 1, PyTuple_GET_SIZE(args));
        PyObject *call_kwargs = kwargs == NULL ? PyDict_New() : PyDict_Copy(kwargs);
        if (call_args != NULL && call_kwargs != NULL) {
            spec = PyTuple_Pack(
                4,
                tag,
                PyTuple_GET_ITEM(args, 0),
                call_args,
                call_kwargs
            );
        }
        Py_XDECREF(call_args);
        Py_XDECREF(call_kwargs);
    }
    else {
        PyObject *items = PyTuple_GetSlice(args, 0, PyTuple_GET_SIZE(args));
        if (items != NULL) {
            spec = PyTuple_Pack(2, tag, items);
        }
        Py_XDECREF(items);
    }
    Py_DECREF(tag);
    if (spec == NULL) {
        return NULL;
    }
    PyObject *result = PyCFunction_NewEx(&operator_accessor_method, spec, module);
    Py_DECREF(spec);
    return result;
}

static int
operator_replace_accessor_constructor(
    PyObject *module,
    const char *name,
    OperatorAccessorKind kind
)
{
    PyObject *original = PyObject_GetAttrString(module, name);
    if (original == NULL) {
        return -1;
    }
    PyObject *tag = PyLong_FromLong(kind);
    PyObject *state = tag == NULL ? NULL : PyTuple_Pack(3, original, module, tag);
    Py_XDECREF(tag);
    Py_DECREF(original);
    if (state == NULL) {
        return -1;
    }
    PyObject *replacement = PyCFunction_NewEx(
        &operator_accessor_constructor_method,
        state,
        module
    );
    Py_DECREF(state);
    if (replacement == NULL) {
        return -1;
    }
    int result = PyObject_SetAttrString(module, name, replacement);
    Py_DECREF(replacement);
    return result;
}

typedef struct {
    PyObject *iterator;
    PyObject *target;
    PyObject *item;
    Py_ssize_t index;
    Py_ssize_t count;
    OperatorSearchKind kind;
    int waiting_for_equal;
} OperatorSearchState;

static void *
operator_search_copy_state(const void *raw_state)
{
    const OperatorSearchState *state = raw_state;
    OperatorSearchState *copy = PyMem_Malloc(sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *state;
    copy->iterator = Py_NewRef(state->iterator);
    copy->target = Py_NewRef(state->target);
    copy->item = Py_XNewRef(state->item);
    return copy;
}

static void
operator_search_free_state(void *raw_state)
{
    OperatorSearchState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_DECREF(state->iterator);
    Py_DECREF(state->target);
    Py_XDECREF(state->item);
    PyMem_Free(state);
}

static PyObject *operator_search_resume(const void *raw_state, PyObject *value);

static const AleffAdapterVTable operator_search_vtable = {
    .copy_state = operator_search_copy_state,
    .free_state = operator_search_free_state,
    .resume = operator_search_resume,
};

static PyObject *
operator_search_continue(
    OperatorSearchState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    int equal = -1;
    if (is_resumed) {
        equal = PyObject_IsTrue(resumed_value);
        if (equal < 0) {
            return NULL;
        }
    }
    for (;;) {
        if (state->item == NULL) {
            state->item = PyIter_Next(state->iterator);
            if (state->item == NULL) {
                if (PyErr_Occurred()) {
                    return NULL;
                }
                if (state->kind == OPERATOR_SEARCH_INDEX) {
                    PyErr_SetString(
                        PyExc_ValueError,
                        "sequence.index(x): x not in sequence"
                    );
                    return NULL;
                }
                return PyLong_FromSsize_t(state->count);
            }
            state->waiting_for_equal = 1;
            equal = state->item == state->target
                ? 1
                : PyObject_RichCompareBool(state->item, state->target, Py_EQ);
            if (equal < 0) {
                return NULL;
            }
        }
        if (equal) {
            if (state->kind == OPERATOR_SEARCH_INDEX) {
                return PyLong_FromSsize_t(state->index);
            }
            state->count++;
        }
        state->index++;
        Py_CLEAR(state->item);
        state->waiting_for_equal = 0;
        equal = -1;
    }
}

static PyObject *
operator_search_resume(const void *raw_state, PyObject *value)
{
    if (value == NULL) {
        return NULL;
    }
    OperatorSearchState *state = operator_search_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &operator_search_vtable, state) < 0) {
        return NULL;
    }
    PyObject *result = operator_search_continue(state, value, 1);
    adapter_leave(&frame);
    operator_search_free_state(state);
    return result;
}

static PyObject *
operator_search_adapter(PyObject *self, PyObject *args)
{
    PyObject *iterable;
    PyObject *target;
    if (!PyArg_ParseTuple(args, "OO:indexOf/countOf", &iterable, &target)) {
        return NULL;
    }
    PyObject *iterator = PyObject_GetIter(iterable);
    if (iterator == NULL) {
        return NULL;
    }
    OperatorSearchState state = {
        .iterator = iterator,
        .target = Py_NewRef(target),
        .item = NULL,
        .index = 0,
        .count = 0,
        .kind = (OperatorSearchKind)PyLong_AsLong(self),
        .waiting_for_equal = 0,
    };
    if (PyErr_Occurred()) {
        Py_DECREF(iterator);
        Py_DECREF(state.target);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &operator_search_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = operator_search_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_DECREF(state.iterator);
    Py_DECREF(state.target);
    Py_XDECREF(state.item);
    return result;
}

static int
operator_replace_function(
    PyObject *module,
    const char *name,
    OperatorResumeKind kind
)
{
    PyObject *original = PyObject_GetAttrString(module, name);
    if (original == NULL) {
        return -1;
    }
    PyObject *tag = PyLong_FromLong(kind);
    if (tag == NULL) {
        return -1;
    }
    PyObject *state = PyTuple_Pack(2, original, tag);
    Py_DECREF(tag);
    Py_DECREF(original);
    if (state == NULL) {
        return -1;
    }
    PyObject *replacement = PyCFunction_NewEx(&operator_call_method, state, module);
    Py_DECREF(state);
    if (replacement == NULL) {
        return -1;
    }
    int result = PyObject_SetAttrString(module, name, replacement);
    Py_DECREF(replacement);
    return result;
}

static int
adapter_operator_install(PyObject *operator_module)
{
    static const struct {
        const char *name;
        OperatorResumeKind kind;
    } functions[] = {
        {"abs", OPERATOR_PASSTHROUGH}, {"add", OPERATOR_PASSTHROUGH},
        {"and_", OPERATOR_PASSTHROUGH}, {"call", OPERATOR_PASSTHROUGH},
        {"concat", OPERATOR_PASSTHROUGH}, {"delitem", OPERATOR_VOID},
        {"eq", OPERATOR_PASSTHROUGH}, {"floordiv", OPERATOR_PASSTHROUGH},
        {"ge", OPERATOR_PASSTHROUGH}, {"getitem", OPERATOR_PASSTHROUGH},
        {"gt", OPERATOR_PASSTHROUGH}, {"iadd", OPERATOR_PASSTHROUGH},
        {"iand", OPERATOR_PASSTHROUGH}, {"iconcat", OPERATOR_PASSTHROUGH},
        {"ifloordiv", OPERATOR_PASSTHROUGH}, {"ilshift", OPERATOR_PASSTHROUGH},
        {"imatmul", OPERATOR_PASSTHROUGH}, {"imod", OPERATOR_PASSTHROUGH},
        {"imul", OPERATOR_PASSTHROUGH}, {"index", OPERATOR_INDEX},
        {"inv", OPERATOR_PASSTHROUGH}, {"invert", OPERATOR_PASSTHROUGH},
        {"ior", OPERATOR_PASSTHROUGH}, {"ipow", OPERATOR_PASSTHROUGH},
        {"irshift", OPERATOR_PASSTHROUGH}, {"isub", OPERATOR_PASSTHROUGH},
        {"itruediv", OPERATOR_PASSTHROUGH}, {"ixor", OPERATOR_PASSTHROUGH},
        {"le", OPERATOR_PASSTHROUGH}, {"lshift", OPERATOR_PASSTHROUGH},
        {"lt", OPERATOR_PASSTHROUGH}, {"matmul", OPERATOR_PASSTHROUGH},
        {"mod", OPERATOR_PASSTHROUGH}, {"mul", OPERATOR_PASSTHROUGH},
        {"ne", OPERATOR_PASSTHROUGH}, {"neg", OPERATOR_PASSTHROUGH},
        {"not_", OPERATOR_NOT}, {"or_", OPERATOR_PASSTHROUGH},
        {"pos", OPERATOR_PASSTHROUGH}, {"pow", OPERATOR_PASSTHROUGH},
        {"rshift", OPERATOR_PASSTHROUGH}, {"setitem", OPERATOR_VOID},
        {"sub", OPERATOR_PASSTHROUGH}, {"truediv", OPERATOR_PASSTHROUGH},
        {"truth", OPERATOR_BOOL}, {"xor", OPERATOR_PASSTHROUGH},
        {"length_hint", OPERATOR_LENGTH_HINT}, {"contains", OPERATOR_BOOL},
    };
    Py_ssize_t count = (Py_ssize_t)(sizeof(functions) / sizeof(*functions));
    for (Py_ssize_t index = 0; index < count; index++) {
        if (operator_replace_function(
                operator_module,
                functions[index].name,
                functions[index].kind
            ) < 0) {
            return -1;
        }
    }
    static const struct {
        const char *name;
        OperatorAccessorKind kind;
    } accessors[] = {
        {"attrgetter", OPERATOR_ACCESSOR_ATTR},
        {"itemgetter", OPERATOR_ACCESSOR_ITEM},
        {"methodcaller", OPERATOR_ACCESSOR_METHOD},
    };
    count = (Py_ssize_t)(sizeof(accessors) / sizeof(*accessors));
    for (Py_ssize_t index = 0; index < count; index++) {
        if (operator_replace_accessor_constructor(
                operator_module,
                accessors[index].name,
                accessors[index].kind
            ) < 0) {
            return -1;
        }
    }
    PyObject *index_tag = PyLong_FromLong(OPERATOR_SEARCH_INDEX);
    PyObject *count_tag = PyLong_FromLong(OPERATOR_SEARCH_COUNT);
    if (index_tag == NULL || count_tag == NULL) {
        Py_XDECREF(index_tag);
        Py_XDECREF(count_tag);
        return -1;
    }
    PyObject *index_function = PyCFunction_NewEx(
        &operator_search_method,
        index_tag,
        operator_module
    );
    PyObject *count_function = PyCFunction_NewEx(
        &operator_search_method,
        count_tag,
        operator_module
    );
    Py_DECREF(index_tag);
    Py_DECREF(count_tag);
    if (index_function == NULL || count_function == NULL) {
        Py_XDECREF(index_function);
        Py_XDECREF(count_function);
        return -1;
    }
    int result = PyObject_SetAttrString(operator_module, "indexOf", index_function);
    if (result == 0) {
        result = PyObject_SetAttrString(operator_module, "countOf", count_function);
    }
    Py_DECREF(index_function);
    Py_DECREF(count_function);
    return result;
}
