#include "bisect.h"

#include <stddef.h>

/* This is the continuation-aware equivalent of CPython's _bisectmodule.c.
 * The CPython implementation is covered by LICENSES/CPython.txt. */

typedef enum {
    BISECT_LEFT,
    BISECT_RIGHT,
} BisectDirection;

typedef enum {
    BISECT_WAIT_LO,
    BISECT_WAIT_HI,
    BISECT_WAIT_LENGTH,
    BISECT_WAIT_ITEM,
    BISECT_WAIT_KEY_ENTRY,
    BISECT_WAIT_KEY_PROBE,
    BISECT_WAIT_COMPARE,
    BISECT_WAIT_INSERT,
    BISECT_READY,
} BisectPhase;

typedef struct {
    PyObject *receiver;
    PyObject *item;
    PyObject *key;
    PyObject *lo_object;
    PyObject *hi_object;
    PyObject *probe;
    PyObject *item_key;
    PyObject *probe_key;

    PyObject *mutation_snapshot;

    Py_ssize_t lo;
    Py_ssize_t hi;
    Py_ssize_t mid;
    BisectDirection direction;
    BisectPhase phase;
    int insertion;
} BisectState;

static const AleffAdapterVTable bisect_vtable;

static PyObject *bisect_left_wrapper(
    PyObject *, PyObject *, PyObject *
);
static PyObject *bisect_right_wrapper(
    PyObject *, PyObject *, PyObject *
);
static PyObject *insort_left_wrapper(
    PyObject *, PyObject *, PyObject *
);
static PyObject *insort_right_wrapper(
    PyObject *, PyObject *, PyObject *
);

static int
bisect_capture_mutation_snapshot(BisectState *state)
{
    if (state->mutation_snapshot != NULL ||
        !PyList_CheckExact(state->receiver)) {
        return 0;
    }
    state->mutation_snapshot = PyList_GetSlice(
        state->receiver,
        0,
        PyList_GET_SIZE(state->receiver)
    );
    return state->mutation_snapshot == NULL ? -1 : 0;
}

static PyObject *
bisect_copy_mutation_snapshot(const BisectState *state)
{
    PyObject *source = state->mutation_snapshot;
    if (source == NULL && state->insertion &&
        PyList_CheckExact(state->receiver)) {
        source = state->receiver;
    }
    if (source == NULL) {
        return NULL;
    }
    return PyList_GetSlice(
        source,
        0,
        PyList_GET_SIZE(source)
    );
}

static int
bisect_restore_snapshot(BisectState *state)
{
    if (state->mutation_snapshot == NULL) {
        return 0;
    }
    return PyList_SetSlice(
        state->receiver,
        0,
        PyList_GET_SIZE(state->receiver),
        state->mutation_snapshot
    );
}

static void *
bisect_copy_state(const void *raw_state)
{
    const BisectState *source = raw_state;
    BisectState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->receiver = NULL;
    copy->item = NULL;
    copy->key = NULL;
    copy->lo_object = NULL;
    copy->hi_object = NULL;
    copy->probe = NULL;
    copy->item_key = NULL;
    copy->probe_key = NULL;
    copy->mutation_snapshot = NULL;

    copy->receiver = Py_NewRef(source->receiver);
    copy->item = Py_NewRef(source->item);
    copy->key = Py_XNewRef(source->key);
    copy->lo_object = Py_XNewRef(source->lo_object);
    copy->hi_object = Py_XNewRef(source->hi_object);
    copy->probe = Py_XNewRef(source->probe);
    copy->item_key = Py_XNewRef(source->item_key);
    copy->probe_key = Py_XNewRef(source->probe_key);
    if (source->mutation_snapshot != NULL ||
        (source->insertion && PyList_CheckExact(source->receiver))) {
        copy->mutation_snapshot = bisect_copy_mutation_snapshot(source);
    }
    if (copy->mutation_snapshot == NULL &&
        (source->mutation_snapshot != NULL ||
         (source->insertion && PyList_CheckExact(source->receiver)))) {
        Py_XDECREF(copy->receiver);
        Py_XDECREF(copy->item);
        Py_XDECREF(copy->key);
        Py_XDECREF(copy->lo_object);
        Py_XDECREF(copy->hi_object);
        Py_XDECREF(copy->probe);
        Py_XDECREF(copy->item_key);
        Py_XDECREF(copy->probe_key);
        PyMem_Free(copy);
        return NULL;
    }
    return copy;
}

static void
bisect_free_state(void *raw_state)
{
    BisectState *state = raw_state;
    if (state == NULL) {
        return;
    }
    Py_XDECREF(state->receiver);
    Py_XDECREF(state->item);
    Py_XDECREF(state->key);
    Py_XDECREF(state->lo_object);
    Py_XDECREF(state->hi_object);
    Py_XDECREF(state->probe);
    Py_XDECREF(state->item_key);
    Py_XDECREF(state->probe_key);
    Py_XDECREF(state->mutation_snapshot);
    PyMem_Free(state);
}

static int
bisect_ssize_from_index(PyObject *value, Py_ssize_t *result)
{
    PyObject *index = PyNumber_Index(value);
    if (index == NULL) {
        return -1;
    }
    *result = PyLong_AsSsize_t(index);
    Py_DECREF(index);
    if (*result == -1 && PyErr_Occurred()) {
        return -1;
    }
    return 0;
}

static int
bisect_ssize_from_resumed_index(PyObject *value, Py_ssize_t *result)
{
    if (!PyLong_Check(value)) {
        PyErr_Format(
            PyExc_TypeError,
            "__index__ returned non-int (type %.200s)",
            Py_TYPE(value)->tp_name
        );
        return -1;
    }
    return bisect_ssize_from_index(value, result);
}

static int
bisect_length_from_value(PyObject *value, Py_ssize_t *result)
{
    *result = PyNumber_AsSsize_t(value, PyExc_OverflowError);
    if (*result < 0) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_ValueError, "__len__() should return >= 0");
        }
        return -1;
    }
    return 0;
}

static int
bisect_enter_recursive(const BisectState *state)
{
#if PY_VERSION_HEX >= 0x030d0000
    return Py_EnterRecursiveCall(
        state->direction == BISECT_LEFT
            ? " in _bisect.bisect_left"
            : " in _bisect.bisect_right"
    );
#else
    (void)state;
    return 0;
#endif
}

static void
bisect_leave_recursive(void)
{
#if PY_VERSION_HEX >= 0x030d0000
    Py_LeaveRecursiveCall();
#endif
}

static int
bisect_apply_compare(BisectState *state, int comparison)
{
    if (comparison < 0) {
        return -1;
    }
    if (comparison) {
        if (state->direction == BISECT_LEFT) {
            state->lo = state->mid + 1;
        }
        else {
            state->hi = state->mid;
        }
    }
    else if (state->direction == BISECT_LEFT) {
        state->hi = state->mid;
    }
    else {
        state->lo = state->mid + 1;
    }
    Py_CLEAR(state->probe);
    Py_CLEAR(state->probe_key);
    state->phase = BISECT_READY;
    return 0;
}

static PyObject *bisect_continue(
    BisectState *state,
    PyObject *resumed_value,
    int is_resumed
);

static int
bisect_prepare_resume(void *raw_state)
{
    return bisect_restore_snapshot(raw_state);
}

static PyObject *
bisect_resume(const void *raw_state, PyObject *value)
{
    BisectState *state = bisect_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    Py_CLEAR(state->mutation_snapshot);

    if (value == NULL) {
        bisect_free_state(state);
        return NULL;
    }
    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &bisect_vtable, state) < 0) {
        bisect_free_state(state);
        return NULL;
    }
    PyObject *result = bisect_continue(state, value, 1);
    adapter_leave(&frame);
    bisect_free_state(state);
    return result;
}

static const AleffAdapterVTable bisect_vtable = {
    .copy_state = bisect_copy_state,
    .free_state = bisect_free_state,
    .resume = bisect_resume,
    .prepare_resume = bisect_prepare_resume,
};

static PyObject *
bisect_finish(BisectState *state)
{
    if (!state->insertion) {
        return PyLong_FromSsize_t(state->lo);
    }
    if (PyList_CheckExact(state->receiver)) {
        if (PyList_Insert(state->receiver, state->lo, state->item) < 0) {
            return NULL;
        }
        Py_RETURN_NONE;
    }
    if (bisect_capture_mutation_snapshot(state) < 0) {
        return NULL;
    }
    state->phase = BISECT_WAIT_INSERT;
    PyObject *result = PyObject_CallMethod(
        state->receiver,
        "insert",
        "nO",
        state->lo,
        state->item
    );
    if (result == NULL) {
        return NULL;
    }
    Py_DECREF(result);
    Py_RETURN_NONE;
}

static PyObject *
bisect_continue(
    BisectState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    if (is_resumed) {
        switch (state->phase) {
            case BISECT_WAIT_LO:
                if (bisect_ssize_from_resumed_index(resumed_value, &state->lo) < 0) {
                    return NULL;
                }
                state->phase = state->hi_object == NULL
                    ? BISECT_READY : BISECT_WAIT_HI;
                break;
            case BISECT_WAIT_HI:
                if (resumed_value == Py_None) {
                    state->hi = -1;
                }
                else if (bisect_ssize_from_resumed_index(
                        resumed_value,
                        &state->hi
                    ) < 0) {
                    return NULL;
                }
                state->phase = BISECT_READY;
                break;
            case BISECT_WAIT_LENGTH:
                if (bisect_length_from_value(resumed_value, &state->hi) < 0) {
                    return NULL;
                }
                state->phase = BISECT_READY;
                break;
            case BISECT_WAIT_ITEM:
                state->probe = Py_NewRef(resumed_value);
                state->phase = BISECT_READY;
                break;
            case BISECT_WAIT_KEY_ENTRY:
                state->item_key = Py_NewRef(resumed_value);
                state->phase = BISECT_READY;
                break;
            case BISECT_WAIT_KEY_PROBE:
                state->probe_key = Py_NewRef(resumed_value);
                state->phase = BISECT_READY;
                break;
            case BISECT_WAIT_COMPARE: {
                if (bisect_enter_recursive(state) < 0) {
                    return NULL;
                }
                int comparison = PyObject_IsTrue(resumed_value);
                bisect_leave_recursive();
                if (bisect_apply_compare(state, comparison) < 0) {
                    return NULL;
                }
                break;
            }
            case BISECT_WAIT_INSERT:
                Py_RETURN_NONE;
            case BISECT_READY:
                PyErr_SetString(PyExc_RuntimeError, "invalid bisect resume phase");
                return NULL;
        }
    }

    if (state->phase == BISECT_WAIT_LO) {
        state->phase = BISECT_WAIT_LO;
        Py_ssize_t result;
        if (bisect_ssize_from_index(state->lo_object, &result) < 0) {
            return NULL;
        }
        state->lo = result;
        state->phase = state->hi_object == NULL
            ? BISECT_READY : BISECT_WAIT_HI;
    }
    if (state->phase == BISECT_WAIT_HI) {
        if (state->hi_object == Py_None) {
            state->hi = -1;
            state->phase = BISECT_READY;
        }
        else {
            Py_ssize_t result;
            if (bisect_ssize_from_index(state->hi_object, &result) < 0) {
                return NULL;
            }
            state->hi = result;
            state->phase = BISECT_READY;
        }
    }
    if (state->phase == BISECT_READY && state->insertion &&
        state->key != NULL && state->item_key == NULL) {
        state->phase = BISECT_WAIT_KEY_ENTRY;
        state->item_key = PyObject_CallOneArg(state->key, state->item);
        if (state->item_key == NULL) {
            return NULL;
        }
        state->phase = BISECT_READY;
    }
    if (state->lo < 0) {
        PyErr_SetString(PyExc_ValueError, "lo must be non-negative");
        return NULL;
    }
    if (state->hi == -1) {
        state->phase = BISECT_WAIT_LENGTH;
        state->hi = PySequence_Size(state->receiver);
        if (state->hi < 0) {
            return NULL;
        }
        state->phase = BISECT_READY;
    }

    if (bisect_enter_recursive(state) < 0) {
        return NULL;
    }
    for (;;) {
        if (state->lo >= state->hi) {
            bisect_leave_recursive();
            return bisect_finish(state);
        }
        state->mid = ((size_t)state->lo + state->hi) / 2;
        if (state->probe == NULL) {
            state->phase = BISECT_WAIT_ITEM;
            state->probe = PySequence_GetItem(state->receiver, state->mid);
            if (state->probe == NULL) {
                break;
            }
        }
        if (state->key != NULL && state->probe_key == NULL) {
            state->phase = BISECT_WAIT_KEY_PROBE;
            state->probe_key = PyObject_CallOneArg(state->key, state->probe);
            if (state->probe_key == NULL) {
                break;
            }
            state->phase = BISECT_READY;
        }

        PyObject *needle = state->key != NULL && state->insertion
            ? state->item_key
            : state->item;
        PyObject *left = state->direction == BISECT_LEFT
            ? (state->key == NULL ? state->probe : state->probe_key)
            : needle;
        PyObject *right = state->direction == BISECT_LEFT
            ? needle
            : (state->key == NULL ? state->probe : state->probe_key);
        state->phase = BISECT_WAIT_COMPARE;
        int comparison = PyObject_RichCompareBool(left, right, Py_LT);
        if (comparison < 0) {
            break;
        }
        if (bisect_apply_compare(state, comparison) < 0) {
            break;
        }
    }
    bisect_leave_recursive();
    return NULL;
}

static PyObject *
bisect_call(
    PyObject *self,
    PyObject *args,
    PyObject *kwargs,
    BisectDirection direction,
    int insertion,
    const char *name
)
{
    (void)self;
    PyObject *receiver;
    PyObject *item;
    PyObject *lo_object = NULL;
    PyObject *hi_object = NULL;
    PyObject *key = Py_None;
    static char *keywords[] = {"a", "x", "lo", "hi", "key", NULL};
    char format[64];
    (void)PyOS_snprintf(format, sizeof(format), "OO|OO$O:%s", name);
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            format,
            keywords,
            &receiver,
            &item,
            &lo_object,
            &hi_object,
            &key
        )) {
        return NULL;
    }

    BisectState state = {
        .receiver = receiver,
        .item = item,
        .key = key == Py_None ? NULL : key,
        .lo_object = lo_object,
        .hi_object = hi_object,
        .probe = NULL,
        .item_key = NULL,
        .probe_key = NULL,
        .mutation_snapshot = NULL,
        .lo = 0,
        .hi = -1,
        .mid = 0,
        .direction = direction,
        .phase = lo_object == NULL
            ? (hi_object == NULL ? BISECT_READY : BISECT_WAIT_HI)
            : BISECT_WAIT_LO,
        .insertion = insertion,
    };
    if (insertion && bisect_capture_mutation_snapshot(&state) < 0) {
        return NULL;
    }

    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &bisect_vtable, &state) < 0) {
        Py_XDECREF(state.mutation_snapshot);
        return NULL;
    }
    PyObject *result = bisect_continue(&state, NULL, 0);
    adapter_leave(&frame);
    Py_XDECREF(state.probe);
    Py_XDECREF(state.item_key);
    Py_XDECREF(state.probe_key);
    Py_XDECREF(state.mutation_snapshot);
    return result;
}

static PyObject *
bisect_left_wrapper(PyObject *self, PyObject *args, PyObject *kwargs)
{
    return bisect_call(self, args, kwargs, BISECT_LEFT, 0, "bisect_left");
}

static PyObject *
bisect_right_wrapper(PyObject *self, PyObject *args, PyObject *kwargs)
{
    return bisect_call(self, args, kwargs, BISECT_RIGHT, 0, "bisect_right");
}

static PyObject *
insort_left_wrapper(PyObject *self, PyObject *args, PyObject *kwargs)
{
    return bisect_call(self, args, kwargs, BISECT_LEFT, 1, "insort_left");
}

static PyObject *
insort_right_wrapper(PyObject *self, PyObject *args, PyObject *kwargs)
{
    return bisect_call(self, args, kwargs, BISECT_RIGHT, 1, "insort_right");
}

typedef struct {
    const char *name;
    PyCFunction function;
} BisectReplacement;

static const BisectReplacement replacements[] = {
    {"bisect_left", _PyCFunction_CAST(bisect_left_wrapper)},
    {"bisect_right", _PyCFunction_CAST(bisect_right_wrapper)},
    {"insort_left", _PyCFunction_CAST(insort_left_wrapper)},
    {"insort_right", _PyCFunction_CAST(insort_right_wrapper)},
};
static PyMethodDef replacement_methods[4];

static const char *const public_names[] = {
    "bisect_left", "bisect_right", "insort_left", "insort_right",
    "bisect", "insort",
};
static PyObject *original_public[6];
static PyObject *installed_module;
static int bisect_installed;

static PyObject *
make_replacement(
    PyObject *original,
    const BisectReplacement *replacement
)
{
    if (!PyCFunction_Check(original)) {
        PyErr_Format(
            PyExc_RuntimeError,
            "bisect.%s is not a C function",
            replacement->name
        );
        return NULL;
    }
    PyMethodDef *method = &replacement_methods[replacement - replacements];
    *method = *((PyCFunctionObject *)original)->m_ml;
    method->ml_name = replacement->name;
    method->ml_meth = replacement->function;
    method->ml_flags = METH_VARARGS | METH_KEYWORDS;
    PyObject *module_name = PyObject_GetAttrString(original, "__module__");
    if (module_name == NULL) {
        PyErr_Clear();
        module_name = PyUnicode_FromString("bisect");
    }
    PyObject *result = module_name == NULL
        ? NULL
        : PyCFunction_NewEx(
            method,
            PyCFunction_GET_SELF(original),
            module_name
        );
    Py_XDECREF(module_name);
    return result;
}

int
adapter_bisect_install(PyObject *bisect_module)
{
    if (bisect_installed) {
        return 0;
    }
    installed_module = Py_NewRef(bisect_module);
    for (int index = 0; index < 6; index++) {
        original_public[index] = PyObject_GetAttrString(
            bisect_module,
            public_names[index]
        );
        if (original_public[index] == NULL) {
            adapter_bisect_rollback();
            return -1;
        }
    }

    PyObject *new_functions[4] = {NULL, NULL, NULL, NULL};
    for (int index = 0; index < 4; index++) {
        new_functions[index] = make_replacement(
            original_public[index],
            &replacements[index]
        );
        if (new_functions[index] == NULL) {
            for (int item = 0; item < 4; item++) {
                Py_XDECREF(new_functions[item]);
            }
            adapter_bisect_rollback();
            return -1;
        }
    }

    int status = 0;
    for (int index = 0; index < 4; index++) {
        if (PyObject_SetAttrString(
                bisect_module,
                public_names[index],
                new_functions[index]
            ) < 0) {
            status = -1;
            break;
        }
    }
    if (status == 0 && PyObject_SetAttrString(
            bisect_module, "bisect", new_functions[1]
        ) < 0) {
        status = -1;
    }
    if (status == 0 && PyObject_SetAttrString(
            bisect_module, "insort", new_functions[3]
        ) < 0) {
        status = -1;
    }
    for (int index = 0; index < 4; index++) {
        Py_DECREF(new_functions[index]);
    }
    if (status < 0) {
        adapter_bisect_rollback();
        return -1;
    }
    bisect_installed = 1;
    return 0;
}

void
adapter_bisect_rollback(void)
{
    if (installed_module == NULL) {
        return;
    }
    for (int index = 0; index < 6; index++) {
        if (original_public[index] != NULL) {
            if (PyObject_SetAttrString(
                    installed_module,
                    public_names[index],
                    original_public[index]
                ) < 0) {
                PyErr_Clear();
            }
            Py_CLEAR(original_public[index]);
        }
    }
    Py_CLEAR(installed_module);
    bisect_installed = 0;
}
