#include "api.h"
#include "internal.h"
#include "operator.h"

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

static PyObject *operator_call_adapter(
    PyObject *self,
    PyObject *args,
    PyObject *kwargs
);
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

static PyObject *operator_accessor_adapter(
    PyObject *self,
    PyObject *args,
    PyObject *kwargs
);

static PyTypeObject *operator_accessor_types[3] = {NULL, NULL, NULL};

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
operator_call_adapter(PyObject *self, PyObject *args, PyObject *kwargs)
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
    PyObject *result = PyObject_Call(original, args, kwargs);
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
                for (
                    Py_ssize_t part = 0;
                    part < PyList_GET_SIZE(state->parts);
                    part++
                ) {
                    PyObject *name = Py_NewRef(PyList_GET_ITEM(state->parts, part));
                    PyUnicode_InternInPlace(&name);
                    if (name == NULL) {
                        return NULL;
                    }
                    if (PyList_SetItem(state->parts, part, name) < 0) {
                        return NULL;
                    }
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
operator_accessor_spec(PyObject *accessor, OperatorAccessorKind kind)
{
    PyObject *reduced = PyObject_CallMethod(accessor, "__reduce__", NULL);
    if (reduced == NULL) {
        return NULL;
    }
    if (!PyTuple_Check(reduced) || PyTuple_GET_SIZE(reduced) != 2) {
        Py_DECREF(reduced);
        PyErr_SetString(PyExc_RuntimeError, "invalid operator accessor reduction");
        return NULL;
    }
    PyObject *constructor = PyTuple_GET_ITEM(reduced, 0);
    PyObject *reduced_args = PyTuple_GET_ITEM(reduced, 1);
    if (!PyTuple_Check(reduced_args)) {
        Py_DECREF(reduced);
        PyErr_SetString(PyExc_RuntimeError, "invalid operator accessor arguments");
        return NULL;
    }
    PyObject *tag = PyLong_FromLong(kind);
    if (tag == NULL) {
        Py_DECREF(reduced);
        return NULL;
    }
    PyObject *spec = NULL;
    if (kind != OPERATOR_ACCESSOR_METHOD) {
        spec = PyTuple_Pack(2, tag, reduced_args);
    }
    else if (PyType_Check(constructor)) {
        if (PyTuple_GET_SIZE(reduced_args) < 1) {
            PyErr_SetString(PyExc_RuntimeError, "invalid methodcaller reduction");
        }
        else {
            PyObject *call_args = PyTuple_GetSlice(
                reduced_args,
                1,
                PyTuple_GET_SIZE(reduced_args)
            );
            PyObject *call_kwargs = PyDict_New();
            if (call_args != NULL && call_kwargs != NULL) {
                spec = PyTuple_Pack(
                    4,
                    tag,
                    PyTuple_GET_ITEM(reduced_args, 0),
                    call_args,
                    call_kwargs
                );
            }
            Py_XDECREF(call_args);
            Py_XDECREF(call_kwargs);
        }
    }
    else {
        PyObject *bound_args = PyObject_GetAttrString(constructor, "args");
        PyObject *call_kwargs = PyObject_GetAttrString(constructor, "keywords");
        if (
            bound_args != NULL && PyTuple_Check(bound_args) &&
            PyTuple_GET_SIZE(bound_args) >= 1 &&
            call_kwargs != NULL && PyDict_Check(call_kwargs)
        ) {
            spec = PyTuple_Pack(
                4,
                tag,
                PyTuple_GET_ITEM(bound_args, 0),
                reduced_args,
                call_kwargs
            );
        }
        else if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_RuntimeError, "invalid methodcaller reduction");
        }
        Py_XDECREF(bound_args);
        Py_XDECREF(call_kwargs);
    }
    Py_DECREF(tag);
    Py_DECREF(reduced);
    return spec;
}

static PyObject *
operator_accessor_type_call(PyObject *self, PyObject *args, PyObject *kwargs)
{
    OperatorAccessorKind kind;
    if (Py_TYPE(self) == operator_accessor_types[OPERATOR_ACCESSOR_ATTR]) {
        kind = OPERATOR_ACCESSOR_ATTR;
    }
    else if (Py_TYPE(self) == operator_accessor_types[OPERATOR_ACCESSOR_ITEM]) {
        kind = OPERATOR_ACCESSOR_ITEM;
    }
    else if (Py_TYPE(self) == operator_accessor_types[OPERATOR_ACCESSOR_METHOD]) {
        kind = OPERATOR_ACCESSOR_METHOD;
    }
    else {
        PyErr_SetString(PyExc_RuntimeError, "unknown operator accessor type");
        return NULL;
    }
    PyObject *spec = operator_accessor_spec(self, kind);
    if (spec == NULL) {
        return NULL;
    }
    PyObject *result = operator_accessor_adapter(spec, args, kwargs);
    Py_DECREF(spec);
    return result;
}

static int
operator_install_accessor_type(
    PyObject *module,
    const char *name,
    OperatorAccessorKind kind
)
{
    PyObject *object = PyObject_GetAttrString(module, name);
    if (object == NULL) {
        return -1;
    }
    if (!PyType_Check(object)) {
        Py_DECREF(object);
        PyErr_Format(PyExc_RuntimeError, "operator.%s is not a type", name);
        return -1;
    }
    PyTypeObject *type = (PyTypeObject *)object;
    if (aleff_adapter_register_callable(object) < 0) {
        Py_DECREF(object);
        return -1;
    }
    operator_accessor_types[kind] = type;
    type->tp_call = operator_accessor_type_call;
    type->tp_flags &= ~Py_TPFLAGS_HAVE_VECTORCALL;
    PyType_Modified(type);
    Py_DECREF(object);
    return 0;
}

typedef struct {
    PyObject *sequence;
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
    copy->sequence = Py_XNewRef(state->sequence);
    copy->iterator = Py_XNewRef(state->iterator);
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
    Py_XDECREF(state->sequence);
    Py_XDECREF(state->iterator);
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
            if (state->sequence != NULL) {
                Py_ssize_t size = PyTuple_CheckExact(state->sequence)
                    ? PyTuple_GET_SIZE(state->sequence)
                    : PyList_GET_SIZE(state->sequence);
                state->item = state->index < size
                    ? Py_NewRef(
                        PyTuple_CheckExact(state->sequence)
                            ? PyTuple_GET_ITEM(state->sequence, state->index)
                            : PyList_GET_ITEM(state->sequence, state->index)
                    )
                    : NULL;
            }
            else {
                state->item = PyIter_Next(state->iterator);
            }
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
    PyObject *sequence = (
        PyTuple_CheckExact(iterable) || PyList_CheckExact(iterable)
    ) ? Py_NewRef(iterable) : NULL;
    PyObject *iterator = sequence == NULL ? PyObject_GetIter(iterable) : NULL;
    if (sequence == NULL && iterator == NULL) {
        return NULL;
    }
    OperatorSearchState state = {
        .sequence = sequence,
        .iterator = iterator,
        .target = Py_NewRef(target),
        .item = NULL,
        .index = 0,
        .count = 0,
        .kind = (OperatorSearchKind)PyLong_AsLong(self),
        .waiting_for_equal = 0,
    };
    if (PyErr_Occurred()) {
        Py_XDECREF(sequence);
        Py_XDECREF(iterator);
        Py_DECREF(state.target);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &operator_search_vtable, &state) < 0) {
        return NULL;
    }
    PyObject *result = operator_search_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.sequence);
    Py_XDECREF(state.iterator);
    Py_DECREF(state.target);
    Py_XDECREF(state.item);
    return result;
}

static int
operator_replace_function(
    PyObject *module,
    const char *name,
    OperatorResumeKind kind,
    PyMethodDef *method
)
{
    PyObject *original = PyObject_GetAttrString(module, name);
    if (original == NULL) {
        return -1;
    }
    if (!PyCFunction_Check(original)) {
        Py_DECREF(original);
        PyErr_Format(PyExc_RuntimeError, "operator.%s is not a C function", name);
        return -1;
    }
    *method = *(((PyCFunctionObject *)original)->m_ml);
    method->ml_meth = _PyCFunction_CAST(operator_call_adapter);
    method->ml_flags = METH_VARARGS | METH_KEYWORDS;
    PyObject *module_name = PyObject_GetAttrString(original, "__module__");
    if (module_name == NULL) {
        Py_DECREF(original);
        return -1;
    }
    PyObject *tag = PyLong_FromLong(kind);
    if (tag == NULL) {
        Py_DECREF(module_name);
        Py_DECREF(original);
        return -1;
    }
    PyObject *state = PyTuple_Pack(2, original, tag);
    Py_DECREF(tag);
    Py_DECREF(original);
    if (state == NULL) {
        Py_DECREF(module_name);
        return -1;
    }
    PyObject *replacement = PyCFunction_NewEx(method, state, module_name);
    Py_DECREF(module_name);
    Py_DECREF(state);
    if (replacement == NULL) {
        return -1;
    }
    int result = aleff_adapter_register_callable(replacement);
    if (result == 0) {
        result = PyObject_SetAttrString(module, name, replacement);
    }
    Py_DECREF(replacement);
    return result;
}

static int
operator_restore_public_aliases(PyObject *module)
{
    static const struct {
        const char *alias;
        const char *canonical;
    } aliases[] = {
        {"__abs__", "abs"}, {"__add__", "add"}, {"__and__", "and_"},
        {"__call__", "call"}, {"__concat__", "concat"},
        {"__contains__", "contains"}, {"__delitem__", "delitem"},
        {"__eq__", "eq"}, {"__floordiv__", "floordiv"}, {"__ge__", "ge"},
        {"__getitem__", "getitem"}, {"__gt__", "gt"},
        {"__iadd__", "iadd"}, {"__iand__", "iand"},
        {"__iconcat__", "iconcat"}, {"__ifloordiv__", "ifloordiv"},
        {"__ilshift__", "ilshift"}, {"__imatmul__", "imatmul"},
        {"__imod__", "imod"}, {"__imul__", "imul"},
        {"__index__", "index"}, {"__inv__", "inv"},
        {"__invert__", "invert"}, {"__ior__", "ior"},
        {"__ipow__", "ipow"}, {"__irshift__", "irshift"},
        {"__isub__", "isub"}, {"__itruediv__", "itruediv"},
        {"__ixor__", "ixor"}, {"__le__", "le"},
        {"__lshift__", "lshift"}, {"__lt__", "lt"},
        {"__matmul__", "matmul"}, {"__mod__", "mod"},
        {"__mul__", "mul"}, {"__ne__", "ne"}, {"__neg__", "neg"},
        {"__not__", "not_"}, {"__or__", "or_"}, {"__pos__", "pos"},
        {"__pow__", "pow"}, {"__rshift__", "rshift"},
        {"__setitem__", "setitem"}, {"__sub__", "sub"},
        {"__truediv__", "truediv"}, {"__xor__", "xor"},
    };
    for (
        Py_ssize_t index = 0;
        index < (Py_ssize_t)(sizeof(aliases) / sizeof(*aliases));
        index++
    ) {
        PyObject *function = PyObject_GetAttrString(
            module,
            aliases[index].canonical
        );
        if (function == NULL) {
            return -1;
        }
        int status = PyObject_SetAttrString(
            module,
            aliases[index].alias,
            function
        );
        Py_DECREF(function);
        if (status < 0) {
            return -1;
        }
    }
    return 0;
}

static int
operator_replace_search(
    PyObject *module,
    const char *name,
    OperatorSearchKind kind,
    PyMethodDef *method
)
{
    PyObject *original = PyObject_GetAttrString(module, name);
    if (original == NULL) {
        return -1;
    }
    if (!PyCFunction_Check(original)) {
        Py_DECREF(original);
        PyErr_Format(PyExc_RuntimeError, "operator.%s is not a C function", name);
        return -1;
    }
    *method = *(((PyCFunctionObject *)original)->m_ml);
    method->ml_meth = operator_search_adapter;
    method->ml_flags = METH_VARARGS;
    PyObject *module_name = PyObject_GetAttrString(original, "__module__");
    Py_DECREF(original);
    if (module_name == NULL) {
        return -1;
    }
    PyObject *tag = PyLong_FromLong(kind);
    if (tag == NULL) {
        Py_DECREF(module_name);
        return -1;
    }
    PyObject *replacement = PyCFunction_NewEx(method, tag, module_name);
    Py_DECREF(tag);
    Py_DECREF(module_name);
    if (replacement == NULL) {
        return -1;
    }
    int status = aleff_adapter_register_callable(replacement);
    if (status == 0) {
        status = PyObject_SetAttrString(module, name, replacement);
    }
    Py_DECREF(replacement);
    return status;
}

int
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
    static PyMethodDef methods[sizeof(functions) / sizeof(*functions)];
    for (Py_ssize_t index = 0; index < count; index++) {
        if (operator_replace_function(
                operator_module,
                functions[index].name,
                functions[index].kind,
                &methods[index]
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
        if (operator_install_accessor_type(
                operator_module,
                accessors[index].name,
                accessors[index].kind
            ) < 0) {
            return -1;
        }
    }
    static PyMethodDef search_methods[2];
    int result = operator_replace_search(
        operator_module,
        "indexOf",
        OPERATOR_SEARCH_INDEX,
        &search_methods[0]
    );
    if (result == 0) {
        result = operator_replace_search(
            operator_module,
            "countOf",
            OPERATOR_SEARCH_COUNT,
            &search_methods[1]
        );
    }
    if (result == 0) {
        result = operator_restore_public_aliases(operator_module);
    }
    return result;
}
