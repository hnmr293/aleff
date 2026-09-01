/*
 * The heap algorithms below follow CPython's heapq implementation.  The
 * corresponding CPython sources are distributed under the PSF license; the
 * terms are included in this repository's LICENSES/CPython.txt and NOTICE.
 */

#include "internal.h"
#include "heapq.h"

typedef enum {
    HEAPQ_HEAPIFY,
    HEAPQ_PUSH,
    HEAPQ_POP,
    HEAPQ_PUSHPOP,
    HEAPQ_REPLACE,
} HeapqOperation;

typedef enum {
    HEAPQ_STAGE_HEAPIFY,
    HEAPQ_STAGE_PUSH,
    HEAPQ_STAGE_POP,
    HEAPQ_STAGE_PUSHPOP,
    HEAPQ_STAGE_REPLACE,
    HEAPQ_STAGE_SIFT_DOWN,
    HEAPQ_STAGE_SIFT_UP,
    HEAPQ_STAGE_DONE,
} HeapqStage;

/* A request is live while PyObject_RichCompareBool may suspend.  Resume
 * consumes this request; it never calls the comparison again. */
typedef enum {
    HEAPQ_COMPARE_NONE,
    HEAPQ_COMPARE_CHILDREN,
    HEAPQ_COMPARE_PARENT,
    HEAPQ_COMPARE_PUSHPOP,
} HeapqComparisonKind;

typedef struct {
    HeapqComparisonKind kind;
    PyObject *left;
    PyObject *right;
    Py_ssize_t heap_size;
} HeapqComparisonRequest;

typedef struct {
    PyObject *heap;
    PyObject *item;
    PyObject *newitem;
    PyObject *result;
    PyObject *snapshot;

    HeapqOperation operation;
    HeapqStage stage;
    HeapqComparisonRequest comparison;

    Py_ssize_t start;
    Py_ssize_t pos;
    Py_ssize_t end;
    Py_ssize_t child;
    Py_ssize_t next_root;
    Py_ssize_t root;
    int is_max;
} HeapqState;

static void *heapq_copy_state(const void *raw_state);
static void heapq_free_state(void *raw_state);
static PyObject *heapq_resume(const void *raw_state, PyObject *value);
static int heapq_prepare_resume(void *raw_state);

static const AleffAdapterVTable heapq_vtable = {
    .copy_state = heapq_copy_state,
    .free_state = heapq_free_state,
    .resume = heapq_resume,
    .prepare_resume = heapq_prepare_resume,
};

static int
heapq_set_item(PyObject *heap, Py_ssize_t index, PyObject *value)
{
    Py_INCREF(value);
    return PyList_SetItem(heap, index, value);
}

static void
heapq_clear_comparison(HeapqState *state)
{
    Py_XDECREF(state->comparison.left);
    Py_XDECREF(state->comparison.right);
    state->comparison.left = NULL;
    state->comparison.right = NULL;
    state->comparison.kind = HEAPQ_COMPARE_NONE;
}

static void
heapq_clear_state_refs(HeapqState *state)
{
    Py_XDECREF(state->heap);
    Py_XDECREF(state->item);
    Py_XDECREF(state->newitem);
    Py_XDECREF(state->result);
    Py_XDECREF(state->snapshot);
    heapq_clear_comparison(state);
    state->heap = NULL;
    state->item = NULL;
    state->newitem = NULL;
    state->result = NULL;
    state->snapshot = NULL;
}

static int
heapq_restore_snapshot(HeapqState *state)
{
    if (state->snapshot == NULL) {
        return 0;
    }
    int result;
#if PY_VERSION_HEX >= 0x030d0000
    Py_BEGIN_CRITICAL_SECTION(state->heap);
#endif
    result = PyList_SetSlice(
        state->heap,
        0,
        PyList_GET_SIZE(state->heap),
        state->snapshot
    );
#if PY_VERSION_HEX >= 0x030d0000
    Py_END_CRITICAL_SECTION();
#endif
    return result;
}

static int heapq_apply_comparison(HeapqState *state, int comparison);

static int
heapq_request_comparison(
    HeapqState *state,
    HeapqComparisonKind kind,
    PyObject *left,
    PyObject *right
)
{
    state->comparison.kind = kind;
    state->comparison.left = Py_NewRef(left);
    state->comparison.right = Py_NewRef(right);
    state->comparison.heap_size = PyList_GET_SIZE(state->heap);

    int result = PyObject_RichCompareBool(left, right, Py_LT);
    if (result < 0) {
        heapq_clear_comparison(state);
        return -1;
    }
    return heapq_apply_comparison(state, result != 0);
}

static int
heapq_consume_comparison(HeapqState *state, PyObject *value)
{
    if (state->comparison.kind == HEAPQ_COMPARE_NONE) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "heapq continuation is not awaiting a comparison"
        );
        return -1;
    }
    int result = PyObject_IsTrue(value);
    if (result < 0) {
        return -1;
    }
    return heapq_apply_comparison(state, result != 0);
}

static int
heapq_finish_sift(HeapqState *state)
{
    Py_CLEAR(state->newitem);
    switch (state->operation) {
        case HEAPQ_HEAPIFY:
            state->next_root = state->root - 1;
            state->stage = HEAPQ_STAGE_HEAPIFY;
            return 0;
        case HEAPQ_PUSH:
        case HEAPQ_POP:
        case HEAPQ_PUSHPOP:
        case HEAPQ_REPLACE:
            state->stage = HEAPQ_STAGE_DONE;
            return 0;
        default:
            PyErr_SetString(PyExc_RuntimeError, "invalid heapq operation");
            return -1;
    }
}

static int
heapq_apply_comparison(HeapqState *state, int comparison)
{
    HeapqComparisonKind kind = state->comparison.kind;
    Py_ssize_t expected_size = state->comparison.heap_size;
    if (kind == HEAPQ_COMPARE_NONE) {
        PyErr_SetString(PyExc_RuntimeError, "invalid heapq comparison request");
        return -1;
    }
    heapq_clear_comparison(state);
    if (kind != HEAPQ_COMPARE_PUSHPOP &&
        PyList_GET_SIZE(state->heap) != expected_size) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "list changed size during iteration"
        );
        return -1;
    }

    switch (kind) {
        case HEAPQ_COMPARE_CHILDREN:
            /* CPython's C heapq compares left < right for min heaps and
             * right < left for max heaps.  A false result selects right. */
            if (!comparison) {
                state->child++;
            }
            if (heapq_set_item(
                    state->heap,
                    state->pos,
                    PyList_GET_ITEM(state->heap, state->child)
                ) < 0) {
                return -1;
            }
            state->pos = state->child;
            state->stage = HEAPQ_STAGE_SIFT_DOWN;
            return 0;

        case HEAPQ_COMPARE_PARENT:
            if (comparison) {
                Py_ssize_t parent = (state->pos - 1) >> 1;
                if (heapq_set_item(
                        state->heap,
                        state->pos,
                        PyList_GET_ITEM(state->heap, parent)
                    ) < 0) {
                    return -1;
                }
                state->pos = parent;
                state->stage = HEAPQ_STAGE_SIFT_UP;
                return 0;
            }
            if (heapq_set_item(state->heap, state->pos, state->newitem) < 0) {
                return -1;
            }
            return heapq_finish_sift(state);

        case HEAPQ_COMPARE_PUSHPOP:
            if (!comparison) {
                Py_XSETREF(state->result, Py_NewRef(state->item));
                state->stage = HEAPQ_STAGE_DONE;
                return 0;
            }
            else {
                if (PyList_GET_SIZE(state->heap) == 0) {
                    PyErr_SetString(PyExc_IndexError, "index out of range");
                    return -1;
                }
                PyObject *old = Py_NewRef(PyList_GET_ITEM(state->heap, 0));
                if (heapq_set_item(state->heap, 0, state->item) < 0) {
                    Py_DECREF(old);
                    return -1;
                }
                Py_XSETREF(state->result, old);
                Py_XSETREF(state->newitem, Py_NewRef(state->item));
                state->start = 0;
                state->pos = 0;
                state->end = PyList_GET_SIZE(state->heap);
                state->stage = HEAPQ_STAGE_SIFT_DOWN;
                return 0;
            }

        case HEAPQ_COMPARE_NONE:
        default:
            PyErr_SetString(PyExc_RuntimeError, "invalid heapq comparison request");
            return -1;
    }
}

static int
heapq_sift_down_step(HeapqState *state)
{
    Py_ssize_t left = state->pos * 2 + 1;
    if (left >= state->end) {
        if (heapq_set_item(state->heap, state->pos, state->newitem) < 0) {
            return -1;
        }
        state->stage = HEAPQ_STAGE_SIFT_UP;
        return 0;
    }

    state->child = left;
    Py_ssize_t right = left + 1;
    if (right < state->end) {
        PyObject *left_value = PyList_GET_ITEM(state->heap, left);
        PyObject *right_value = PyList_GET_ITEM(state->heap, right);
        if (state->is_max) {
            return heapq_request_comparison(
                state,
                HEAPQ_COMPARE_CHILDREN,
                right_value,
                left_value
            );
        }
        return heapq_request_comparison(
            state,
            HEAPQ_COMPARE_CHILDREN,
            left_value,
            right_value
        );
    }

    if (heapq_set_item(
            state->heap,
            state->pos,
            PyList_GET_ITEM(state->heap, state->child)
        ) < 0) {
        return -1;
    }
    state->pos = state->child;
    return 0;
}

static int
heapq_sift_up_step(HeapqState *state)
{
    if (state->pos <= state->start) {
        if (heapq_set_item(state->heap, state->pos, state->newitem) < 0) {
            return -1;
        }
        return heapq_finish_sift(state);
    }

    Py_ssize_t parent = (state->pos - 1) >> 1;
    PyObject *parent_value = PyList_GET_ITEM(state->heap, parent);
    if (state->is_max) {
        return heapq_request_comparison(
            state,
            HEAPQ_COMPARE_PARENT,
            parent_value,
            state->newitem
        );
    }
    return heapq_request_comparison(
        state,
        HEAPQ_COMPARE_PARENT,
        state->newitem,
        parent_value
    );
}

static PyObject *
heapq_continue(HeapqState *state, PyObject *resumed_value, int is_resumed)
{
    if (is_resumed) {
        if (resumed_value == NULL) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(
                    PyExc_RuntimeError,
                    "heapq continuation resumed without a value or exception"
                );
            }
            return NULL;
        }
        if (heapq_consume_comparison(state, resumed_value) < 0) {
            return NULL;
        }
    }

    for (;;) {
        switch (state->stage) {
            case HEAPQ_STAGE_HEAPIFY:
                if (state->next_root < 0) {
                    state->stage = HEAPQ_STAGE_DONE;
                    continue;
                }
                state->root = state->next_root;
                state->start = state->root;
                state->pos = state->root;
                state->end = PyList_GET_SIZE(state->heap);
                Py_XSETREF(
                    state->newitem,
                    Py_NewRef(PyList_GET_ITEM(state->heap, state->root))
                );
                state->stage = HEAPQ_STAGE_SIFT_DOWN;
                continue;

            case HEAPQ_STAGE_PUSH:
                if (PyList_Append(state->heap, state->item) < 0) {
                    return NULL;
                }
                Py_XSETREF(state->newitem, Py_NewRef(state->item));
                state->start = 0;
                state->pos = PyList_GET_SIZE(state->heap) - 1;
                state->end = PyList_GET_SIZE(state->heap);
                state->stage = HEAPQ_STAGE_SIFT_UP;
                continue;

            case HEAPQ_STAGE_POP: {
                Py_ssize_t size = PyList_GET_SIZE(state->heap);
                if (size == 0) {
                    PyErr_SetString(PyExc_IndexError, "index out of range");
                    return NULL;
                }
                PyObject *last = Py_NewRef(PyList_GET_ITEM(state->heap, size - 1));
                if (PyList_SetSlice(
                        state->heap,
                        size - 1,
                        size,
                        NULL
                    ) < 0) {
                    Py_DECREF(last);
                    return NULL;
                }
                if (size == 1) {
                    Py_XSETREF(state->result, last);
                    state->stage = HEAPQ_STAGE_DONE;
                    continue;
                }
                Py_XSETREF(
                    state->result,
                    Py_NewRef(PyList_GET_ITEM(state->heap, 0))
                );
                if (heapq_set_item(state->heap, 0, last) < 0) {
                    Py_DECREF(last);
                    return NULL;
                }
                Py_XSETREF(state->newitem, last);
                state->start = 0;
                state->pos = 0;
                state->end = size - 1;
                state->stage = HEAPQ_STAGE_SIFT_DOWN;
                continue;
            }

            case HEAPQ_STAGE_PUSHPOP:
                if (PyList_GET_SIZE(state->heap) == 0) {
                    Py_XSETREF(state->result, Py_NewRef(state->item));
                    state->stage = HEAPQ_STAGE_DONE;
                    continue;
                }
                if (state->is_max) {
                    if (heapq_request_comparison(
                            state,
                            HEAPQ_COMPARE_PUSHPOP,
                            state->item,
                            PyList_GET_ITEM(state->heap, 0)
                        ) < 0) {
                        return NULL;
                    }
                }
                else if (heapq_request_comparison(
                        state,
                        HEAPQ_COMPARE_PUSHPOP,
                        PyList_GET_ITEM(state->heap, 0),
                        state->item
                    ) < 0) {
                    return NULL;
                }
                continue;

            case HEAPQ_STAGE_REPLACE:
                if (PyList_GET_SIZE(state->heap) == 0) {
                    PyErr_SetString(PyExc_IndexError, "index out of range");
                    return NULL;
                }
                Py_XSETREF(
                    state->result,
                    Py_NewRef(PyList_GET_ITEM(state->heap, 0))
                );
                if (heapq_set_item(state->heap, 0, state->item) < 0) {
                    return NULL;
                }
                Py_XSETREF(state->newitem, Py_NewRef(state->item));
                state->start = 0;
                state->pos = 0;
                state->end = PyList_GET_SIZE(state->heap);
                state->stage = HEAPQ_STAGE_SIFT_DOWN;
                continue;

            case HEAPQ_STAGE_SIFT_DOWN:
                if (heapq_sift_down_step(state) < 0) {
                    return NULL;
                }
                continue;

            case HEAPQ_STAGE_SIFT_UP:
                if (heapq_sift_up_step(state) < 0) {
                    return NULL;
                }
                continue;

            case HEAPQ_STAGE_DONE:
                if (state->operation == HEAPQ_HEAPIFY ||
                    state->operation == HEAPQ_PUSH) {
                    return Py_NewRef(Py_None);
                }
                return Py_NewRef(state->result);

            default:
                PyErr_SetString(PyExc_RuntimeError, "invalid heapq stage");
                return NULL;
        }
    }
}

static PyObject *
heapq_continue_locked(
    HeapqState *state,
    PyObject *resumed_value,
    int is_resumed
)
{
    PyObject *result;
#if PY_VERSION_HEX >= 0x030d0000
    Py_BEGIN_CRITICAL_SECTION(state->heap);
#endif
    if (!is_resumed && state->operation == HEAPQ_HEAPIFY) {
        state->next_root = PyList_GET_SIZE(state->heap) / 2 - 1;
    }
    result = heapq_continue(state, resumed_value, is_resumed);
#if PY_VERSION_HEX >= 0x030d0000
    Py_END_CRITICAL_SECTION();
#endif
    return result;
}

static void *
heapq_copy_state(const void *raw_state)
{
    const HeapqState *source = raw_state;
    HeapqState *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *copy = *source;
    copy->heap = NULL;
    copy->item = NULL;
    copy->newitem = NULL;
    copy->result = NULL;
    copy->snapshot = NULL;
    copy->comparison.left = NULL;
    copy->comparison.right = NULL;
    copy->comparison.kind = source->comparison.kind;

    copy->heap = Py_NewRef(source->heap);
    copy->item = Py_XNewRef(source->item);
    copy->newitem = Py_XNewRef(source->newitem);
    copy->result = Py_XNewRef(source->result);
    copy->comparison.left = Py_XNewRef(source->comparison.left);
    copy->comparison.right = Py_XNewRef(source->comparison.right);

    /* A live state has no snapshot.  A cloned state already has the exact
     * save-point snapshot and must clone that, never the live heap. */
    PyObject *snapshot_source = source->snapshot == NULL
        ? source->heap
        : source->snapshot;
    copy->snapshot = PyList_GetSlice(
        snapshot_source,
        0,
        PyList_GET_SIZE(snapshot_source)
    );
    if (copy->snapshot == NULL) {
        heapq_free_state(copy);
        return NULL;
    }
    return copy;
}

static void
heapq_free_state(void *raw_state)
{
    HeapqState *state = raw_state;
    if (state == NULL) {
        return;
    }
    heapq_clear_state_refs(state);
    PyMem_Free(state);
}

static int
heapq_prepare_resume(void *raw_state)
{
    return heapq_restore_snapshot(raw_state);
}

static PyObject *
heapq_resume(const void *raw_state, PyObject *value)
{
    HeapqState *state = heapq_copy_state(raw_state);
    if (state == NULL) {
        return NULL;
    }
    Py_CLEAR(state->snapshot);

    if (value == NULL) {
        heapq_free_state(state);
        return NULL;
    }

    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &heapq_vtable, state) < 0) {
        heapq_free_state(state);
        return NULL;
    }
    PyObject *result = heapq_continue_locked(state, value, 1);
    adapter_leave(&frame);
    heapq_free_state(state);
    return result;
}

static PyObject *
heapq_call(
    HeapqOperation operation,
    int is_max,
    PyObject *args,
    PyObject *kwargs
)
{
    const char *name;
    if (operation == HEAPQ_HEAPIFY) {
        name = is_max ? "heapify_max" : "heapify";
    }
    else if (operation == HEAPQ_PUSH) {
        name = is_max ? "heappush_max" : "heappush";
    }
    else if (operation == HEAPQ_POP) {
        name = is_max ? "heappop_max" : "heappop";
    }
    else if (operation == HEAPQ_PUSHPOP) {
        name = is_max ? "heappushpop_max" : "heappushpop";
    }
    else {
        name = is_max ? "heapreplace_max" : "heapreplace";
    }

    if (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0) {
        PyErr_Format(
            PyExc_TypeError,
            "_heapq.%s() takes no keyword arguments",
            name
        );
        return NULL;
    }

    int one_argument = operation == HEAPQ_HEAPIFY || operation == HEAPQ_POP;
    Py_ssize_t expected = one_argument ? 1 : 2;
    Py_ssize_t given = PyTuple_GET_SIZE(args);
    if (given != expected) {
        if (one_argument) {
            PyErr_Format(
                PyExc_TypeError,
                "_heapq.%s() takes exactly one argument (%zd given)",
                name,
                given
            );
        }
        else {
            PyErr_Format(
                PyExc_TypeError,
                "%s expected 2 arguments, got %zd",
                name,
                given
            );
        }
        return NULL;
    }

    PyObject *heap = PyTuple_GET_ITEM(args, 0);
    PyObject *item = one_argument ? NULL : PyTuple_GET_ITEM(args, 1);
    if (!PyList_Check(heap)) {
        if (one_argument) {
            PyErr_Format(
                PyExc_TypeError,
                "%s() argument must be list, not %.200s",
                name,
                Py_TYPE(heap)->tp_name
            );
        }
        else {
            PyErr_Format(
                PyExc_TypeError,
                "%s() argument 1 must be list, not %.200s",
                name,
                Py_TYPE(heap)->tp_name
            );
        }
        return NULL;
    }

    HeapqState state = {
        .heap = Py_NewRef(heap),
        .item = Py_XNewRef(item),
        .operation = operation,
        .stage = operation == HEAPQ_HEAPIFY
            ? HEAPQ_STAGE_HEAPIFY
            : operation == HEAPQ_PUSH
                ? HEAPQ_STAGE_PUSH
                : operation == HEAPQ_POP
                    ? HEAPQ_STAGE_POP
                    : operation == HEAPQ_PUSHPOP
                        ? HEAPQ_STAGE_PUSHPOP
                        : HEAPQ_STAGE_REPLACE,
        .comparison = {
            .kind = HEAPQ_COMPARE_NONE,
            .left = NULL,
            .right = NULL,
        },
        .is_max = is_max,
    };

    AleffAdapterFrame frame;
    if (adapter_enter(&frame, &heapq_vtable, &state) < 0) {
        heapq_clear_state_refs(&state);
        return NULL;
    }
    PyObject *result = heapq_continue_locked(&state, NULL, 0);
    adapter_leave(&frame);
    heapq_clear_state_refs(&state);
    return result;
}

static PyObject *
adapter_heapify(PyObject *self, PyObject *args, PyObject *kwargs)
{
    (void)self;
    return heapq_call(HEAPQ_HEAPIFY, 0, args, kwargs);
}

static PyObject *
adapter_heappush(PyObject *self, PyObject *args, PyObject *kwargs)
{
    (void)self;
    return heapq_call(HEAPQ_PUSH, 0, args, kwargs);
}

static PyObject *
adapter_heappop(PyObject *self, PyObject *args, PyObject *kwargs)
{
    (void)self;
    return heapq_call(HEAPQ_POP, 0, args, kwargs);
}

static PyObject *
adapter_heappushpop(PyObject *self, PyObject *args, PyObject *kwargs)
{
    (void)self;
    return heapq_call(HEAPQ_PUSHPOP, 0, args, kwargs);
}

static PyObject *
adapter_heapreplace(PyObject *self, PyObject *args, PyObject *kwargs)
{
    (void)self;
    return heapq_call(HEAPQ_REPLACE, 0, args, kwargs);
}

#if PY_VERSION_HEX >= 0x030e0000
static PyObject *
adapter_heapify_max(PyObject *self, PyObject *args, PyObject *kwargs)
{
    (void)self;
    return heapq_call(HEAPQ_HEAPIFY, 1, args, kwargs);
}

static PyObject *
adapter_heappush_max(PyObject *self, PyObject *args, PyObject *kwargs)
{
    (void)self;
    return heapq_call(HEAPQ_PUSH, 1, args, kwargs);
}

static PyObject *
adapter_heappop_max(PyObject *self, PyObject *args, PyObject *kwargs)
{
    (void)self;
    return heapq_call(HEAPQ_POP, 1, args, kwargs);
}

static PyObject *
adapter_heappushpop_max(PyObject *self, PyObject *args, PyObject *kwargs)
{
    (void)self;
    return heapq_call(HEAPQ_PUSHPOP, 1, args, kwargs);
}

static PyObject *
adapter_heapreplace_max(PyObject *self, PyObject *args, PyObject *kwargs)
{
    (void)self;
    return heapq_call(HEAPQ_REPLACE, 1, args, kwargs);
}
#endif

typedef struct {
    const char *name;
    PyMethodDef *definition;
} HeapqMethod;

/* PyCFunctionObject retains m_ml for the replacement's whole lifetime. */
static PyMethodDef heapq_method_defs[] = {
    {
        "heapify",
        _PyCFunction_CAST(adapter_heapify),
        METH_VARARGS | METH_KEYWORDS,
        NULL,
    },
    {
        "heappush",
        _PyCFunction_CAST(adapter_heappush),
        METH_VARARGS | METH_KEYWORDS,
        NULL,
    },
    {
        "heappop",
        _PyCFunction_CAST(adapter_heappop),
        METH_VARARGS | METH_KEYWORDS,
        NULL,
    },
    {
        "heappushpop",
        _PyCFunction_CAST(adapter_heappushpop),
        METH_VARARGS | METH_KEYWORDS,
        NULL,
    },
    {
        "heapreplace",
        _PyCFunction_CAST(adapter_heapreplace),
        METH_VARARGS | METH_KEYWORDS,
        NULL,
    },
#if PY_VERSION_HEX >= 0x030e0000
    {
        "heapify_max",
        _PyCFunction_CAST(adapter_heapify_max),
        METH_VARARGS | METH_KEYWORDS,
        NULL,
    },
    {
        "heappush_max",
        _PyCFunction_CAST(adapter_heappush_max),
        METH_VARARGS | METH_KEYWORDS,
        NULL,
    },
    {
        "heappop_max",
        _PyCFunction_CAST(adapter_heappop_max),
        METH_VARARGS | METH_KEYWORDS,
        NULL,
    },
    {
        "heappushpop_max",
        _PyCFunction_CAST(adapter_heappushpop_max),
        METH_VARARGS | METH_KEYWORDS,
        NULL,
    },
    {
        "heapreplace_max",
        _PyCFunction_CAST(adapter_heapreplace_max),
        METH_VARARGS | METH_KEYWORDS,
        NULL,
    },
#endif
};

static const HeapqMethod heapq_methods[] = {
    {"heapify", &heapq_method_defs[0]},
    {"heappush", &heapq_method_defs[1]},
    {"heappop", &heapq_method_defs[2]},
    {"heappushpop", &heapq_method_defs[3]},
    {"heapreplace", &heapq_method_defs[4]},
#if PY_VERSION_HEX >= 0x030e0000
    {"heapify_max", &heapq_method_defs[5]},
    {"heappush_max", &heapq_method_defs[6]},
    {"heappop_max", &heapq_method_defs[7]},
    {"heappushpop_max", &heapq_method_defs[8]},
    {"heapreplace_max", &heapq_method_defs[9]},
#endif
};

typedef struct {
    PyObject *module;
    PyObject *key;
    PyObject *original;
    PyObject *replacement;
} HeapqInstalledMethod;

static HeapqInstalledMethod heapq_installed[
    sizeof(heapq_methods) / sizeof(*heapq_methods)
];
static Py_ssize_t heapq_installed_count;

int
adapter_heapq_install(PyObject *heapq)
{
    if (heapq_installed_count != 0) {
        return 0;
    }
    for (Py_ssize_t i = 0;
         i < (Py_ssize_t)(sizeof(heapq_methods) / sizeof(*heapq_methods));
         i++) {
        PyObject *original = PyObject_GetAttrString(heapq, heapq_methods[i].name);
        if (original == NULL) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                PyErr_Clear();
                continue;
            }
            goto error;
        }
        if (!PyCFunction_Check(original)) {
            Py_DECREF(original);
            PyErr_Format(
                PyExc_RuntimeError,
                "heapq.%s is not a C function",
                heapq_methods[i].name
            );
            goto error;
        }

        heapq_methods[i].definition->ml_doc =
            ((PyCFunctionObject *)original)->m_ml->ml_doc;
        PyObject *module_name = PyObject_GetAttrString(
            original,
            "__module__"
        );
        if (module_name == NULL) {
            PyErr_Clear();
            module_name = PyUnicode_FromString("_heapq");
        }
        PyObject *replacement = module_name == NULL
            ? NULL
            : PyCFunction_NewEx(
                heapq_methods[i].definition,
                PyCFunction_GET_SELF(original),
                module_name
            );
        Py_XDECREF(module_name);
        if (replacement == NULL) {
            Py_DECREF(original);
            goto error;
        }
        PyObject *key = PyUnicode_FromString(heapq_methods[i].name);
        if (key == NULL || PyDict_SetItem(
                PyModule_GetDict(heapq), key, replacement
            ) < 0) {
            Py_XDECREF(key);
            Py_DECREF(replacement);
            Py_DECREF(original);
            goto error;
        }
        heapq_installed[heapq_installed_count++] = (HeapqInstalledMethod){
            .module = Py_NewRef(heapq),
            .key = key,
            .original = original,
            .replacement = replacement,
        };
    }
    return 0;

error:
    adapter_heapq_rollback();
    return -1;
}

void
adapter_heapq_rollback(void)
{
    while (heapq_installed_count > 0) {
        HeapqInstalledMethod *entry = &heapq_installed[--heapq_installed_count];
        if (entry->module != NULL && entry->key != NULL && entry->original != NULL) {
            PyDict_SetItem(
                PyModule_GetDict(entry->module),
                entry->key,
                entry->original
            );
        }
        Py_XDECREF(entry->module);
        Py_XDECREF(entry->key);
        Py_XDECREF(entry->original);
        Py_XDECREF(entry->replacement);
        entry->module = NULL;
        entry->key = NULL;
        entry->original = NULL;
        entry->replacement = NULL;
    }
}
