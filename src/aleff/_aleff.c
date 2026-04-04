/*
 * _aleff: C extension for multi-shot delimited continuations
 *
 * Provides frame chain snapshot/restore for Python 3.12+.
 * The continuation side must be pure Python (no C extension calls).
 *
 * Functions:
 *   snapshot_frames() -> FrameSnapshot
 *     Capture the current Python frame chain as a deep copy.
 *
 *   restore_continuation(snapshot, value) -> result
 *     Restore a frame chain from snapshot, push value onto the stack,
 *     and resume execution via PyEval_EvalFrame.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <frameobject.h>
#include <dlfcn.h>

#if PY_VERSION_HEX < 0x030c0000
#error "_aleff requires Python 3.12 or later"
#endif

/* ========================================================================
 * _PyInterpreterFrame layout for Python 3.12
 *
 * This is a replica of the internal structure from
 * cpython/Include/internal/pycore_frame.h
 * We define it here because the internal headers are not installed.
 * ======================================================================== */

typedef uint16_t _aleff_codeunit;

typedef struct _aleff_frame {
    PyObject *f_executable;              /* strong ref: code object */
    struct _aleff_frame *previous;       /* previous frame in chain */
    PyObject *f_funcobj;                 /* strong ref: function object */
    PyObject *f_globals;                 /* borrowed ref */
    PyObject *f_builtins;               /* borrowed ref */
    PyObject *f_locals;                  /* strong ref, may be NULL */
    PyFrameObject *frame_obj;            /* strong ref, may be NULL */
    _aleff_codeunit *prev_instr;        /* instruction pointer */
    int stacktop;                        /* top of value stack */
    uint16_t return_offset;
    char owner;
    PyObject *localsplus[1];            /* variable-length: locals + cells + stack */
} _aleff_frame_t;

/* Frame owner constants (from pycore_frame.h) */
#define FRAME_OWNED_BY_THREAD 0
#define FRAME_OWNED_BY_GENERATOR 1
#define FRAME_OWNED_BY_FRAME_OBJECT 2
#define FRAME_OWNED_BY_CSTACK 3

static inline PyCodeObject *
_aleff_frame_get_code(_aleff_frame_t *frame)
{
    return (PyCodeObject *)frame->f_executable;
}

static inline int
_aleff_frame_num_slots(PyCodeObject *code)
{
    return code->co_nlocalsplus + code->co_stacksize;
}

/* ========================================================================
 * FrameSnapshot: stores a deep copy of a frame chain
 * ======================================================================== */

typedef struct {
    _aleff_frame_t *frame;  /* deep-copied frame */
    int num_slots;          /* number of localsplus slots */
} _aleff_frame_copy_t;

typedef struct {
    PyObject_HEAD
    _aleff_frame_copy_t *frames;   /* array of frame copies */
    int num_frames;                /* number of frames in the chain */
} FrameSnapshotObject;

static void
FrameSnapshot_dealloc(FrameSnapshotObject *self)
{
    for (int i = 0; i < self->num_frames; i++) {
        _aleff_frame_copy_t *fc = &self->frames[i];
        _aleff_frame_t *f = fc->frame;
        if (f == NULL) continue;

        Py_XDECREF(f->f_executable);
        Py_XDECREF(f->f_funcobj);
        /* f_globals and f_builtins are borrowed in live frames,
           but we hold strong refs in copies */
        Py_XDECREF(f->f_globals);
        Py_XDECREF(f->f_builtins);
        Py_XDECREF(f->f_locals);
        /* Don't decref frame_obj — we set it to NULL in copies */

        for (int j = 0; j < fc->num_slots; j++) {
            Py_XDECREF(f->localsplus[j]);
        }
        PyMem_Free(f);
    }
    PyMem_Free(self->frames);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
FrameSnapshot_class_getitem([[maybe_unused]] PyObject *cls, [[maybe_unused]] PyObject *args)
{
    /* FrameSnapshot[R, V] */
    Py_INCREF(cls);
    return cls;
}

static PyMethodDef FrameSnapshot_methods[] = {
    {"__class_getitem__", FrameSnapshot_class_getitem, METH_O | METH_CLASS, NULL},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject FrameSnapshotType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_aleff.FrameSnapshot",
    .tp_doc = "Snapshot of a Python frame chain for multi-shot continuations.",
    .tp_basicsize = sizeof(FrameSnapshotObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)FrameSnapshot_dealloc,
    .tp_methods = FrameSnapshot_methods,
};

/* ========================================================================
 * Frame chain copying
 * ======================================================================== */

/*
 * Deep-copy a single _PyInterpreterFrame.
 * All PyObject* references in localsplus are Py_XINCREF'd.
 * f_globals and f_builtins are promoted from borrowed to strong refs.
 * frame_obj is set to NULL (not shared with the original).
 */
static _aleff_frame_copy_t
copy_single_frame(_aleff_frame_t *src)
{
    _aleff_frame_copy_t result = {NULL, 0};

    PyCodeObject *code = _aleff_frame_get_code(src);
    int num_slots = _aleff_frame_num_slots(code);

    size_t frame_size = sizeof(_aleff_frame_t)
                      + (num_slots - 1) * sizeof(PyObject *);

    _aleff_frame_t *dst = (_aleff_frame_t *)PyMem_Malloc(frame_size);
    if (dst == NULL) {
        PyErr_NoMemory();
        return result;
    }

    /* Bitwise copy first */
    memcpy(dst, src, frame_size);

    /* Strong refs for objects */
    Py_XINCREF(dst->f_executable);
    Py_XINCREF(dst->f_funcobj);
    /* Promote borrowed to strong */
    Py_XINCREF(dst->f_globals);
    Py_XINCREF(dst->f_builtins);
    Py_XINCREF(dst->f_locals);

    /* Don't share the PyFrameObject */
    dst->frame_obj = NULL;

    /* previous will be linked later */
    dst->previous = NULL;

    /* owner: mark as owned by thread (will be cleaned up manually) */
    dst->owner = FRAME_OWNED_BY_THREAD;

    /* INCREF only valid localsplus entries (0..stacktop-1).
     * Slots from stacktop onward are the unused portion of the value stack
     * and may contain stale/dangling pointers (CPython's POP() doesn't null them). */
    int stacktop = dst->stacktop;
    for (int i = 0; i < stacktop; i++) {
        Py_XINCREF(dst->localsplus[i]);
    }
    for (int i = stacktop; i < num_slots; i++) {
        dst->localsplus[i] = NULL;
    }

    result.frame = dst;
    result.num_slots = num_slots;
    return result;
}

/*
 * Snapshot the Python frame chain from the current thread state.
 *
 * Captures frames from the current frame up to (but not including)
 * the frame specified by `boundary` (or all frames if boundary is NULL).
 *
 * The `depth` parameter limits how many frames to capture.
 * Pass -1 for unlimited.
 */
static FrameSnapshotObject *
create_snapshot(PyFrameObject *boundary_frame, int max_depth)
{
    PyThreadState *tstate = PyThreadState_Get();
    PyFrameObject *py_frame = PyThreadState_GetFrame(tstate);
    if (py_frame == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "no current frame");
        return NULL;
    }

    /* Count frames.
     * PyFrame_GetBack returns a new reference, so we must DECREF. */
    int count = 0;
    {
        PyFrameObject *f = py_frame;
        Py_INCREF(f);  /* hold our own ref for the loop */
        while (f != NULL && f != boundary_frame) {
            if (max_depth >= 0 && count >= max_depth) {
                Py_DECREF(f);
                break;
            }
            count++;
            PyFrameObject *prev = PyFrame_GetBack(f);  /* new ref */
            Py_DECREF(f);
            f = prev;
        }
        /* If loop ended by boundary or NULL (not by max_depth break),
         * f was already DECREF'd inside the loop via prev/DECREF pattern,
         * or f is NULL. The boundary case: f == boundary_frame and we
         * exited the while condition, so f still has our ref. */
        if (f != NULL && !(max_depth >= 0 && count >= max_depth)) {
            Py_DECREF(f);
        }
    }

    if (count == 0) {
        PyErr_SetString(PyExc_RuntimeError, "no frames to snapshot");
        Py_DECREF(py_frame);
        return NULL;
    }

    FrameSnapshotObject *snapshot = PyObject_New(FrameSnapshotObject, &FrameSnapshotType);
    if (snapshot == NULL) {
        Py_DECREF(py_frame);
        return NULL;
    }

    snapshot->frames = (_aleff_frame_copy_t *)PyMem_Calloc(count, sizeof(_aleff_frame_copy_t));
    if (snapshot->frames == NULL) {
        PyErr_NoMemory();
        Py_DECREF(snapshot);
        Py_DECREF(py_frame);
        return NULL;
    }
    snapshot->num_frames = count;

    /* Copy frames from innermost to outermost.
     *
     * In CPython 3.12, PyFrameObject layout is:
     *   PyObject_HEAD           (16 bytes)
     *   PyFrameObject *f_back   (8 bytes)
     *   _PyInterpreterFrame *f_frame  (8 bytes)  <-- offset 24
     */
    #define F_FRAME_OFFSET (sizeof(PyObject) + sizeof(PyFrameObject *))

    {
        PyFrameObject *f = py_frame;
        Py_INCREF(f);
        for (int i = 0; i < count; i++) {
            _aleff_frame_t *internal = *(_aleff_frame_t **)(
                (char *)f + F_FRAME_OFFSET
            );

            snapshot->frames[i] = copy_single_frame(internal);
            if (snapshot->frames[i].frame == NULL) {
                snapshot->num_frames = i;
                Py_DECREF(f);
                Py_DECREF(snapshot);
                Py_DECREF(py_frame);
                return NULL;
            }

            PyFrameObject *prev = PyFrame_GetBack(f);  /* new ref */
            Py_DECREF(f);
            f = prev;
        }
        Py_XDECREF(f);  /* may be NULL if chain ended */
    }

    #undef F_FRAME_OFFSET

    Py_DECREF(py_frame);

    /* Link the copied frames */
    for (int i = 0; i < count - 1; i++) {
        snapshot->frames[i].frame->previous = snapshot->frames[i + 1].frame;
    }
    /* Outermost frame's previous = NULL (will be set during restore) */

    return snapshot;
}

/* ========================================================================
 * _PyEval_EvalFrameDefault lookup
 * ======================================================================== */

typedef PyObject *(*evalframe_fn_t)(PyThreadState *, void *, int);
static evalframe_fn_t _evalframe = NULL;

/* ========================================================================
 * Frame chain restoration and continuation resume
 * ======================================================================== */

/*
 * Inject resume value into a frame, simulating the return from a CALL.
 *
 * The frame was suspended mid-CALL (calling the effect).
 * Two CALL dispatch paths leave the frame in different states:
 *
 * 1. Inline dispatch (CALL_PY_EXACT_ARGS, for plain function calls):
 *    - Stack already shrunk (callable + args removed)
 *    - prev_instr at last CACHE entry (opcode 0)
 *    - Just push the resume value.
 *
 * 2. Generic CALL (for __call__ objects, bound methods, etc.):
 *    - Stack still has callable + self_or_null + args
 *    - prev_instr at the CALL instruction itself (opcode 171)
 *    - Pop args, advance prev_instr, push resume value.
 *
 * We distinguish the two by checking the opcode at prev_instr.
 */
static void
inject_resume_value(_aleff_frame_t *frame, PyObject *value)
{
    uint8_t opcode = (*frame->prev_instr) & 0xFF;

    /* CALL opcode in CPython 3.12 = 171 */
    /* CALL instruction size: 1 (CALL) + 3 (CACHE entries) = 4 codeunits */
    #define CALL_TOTAL_SIZE 4

    if (opcode == 171) {
        /* Generic CALL path: stack has callable + args, prev_instr at CALL */
        uint8_t oparg = (*frame->prev_instr >> 8) & 0xFF;
        int items_to_pop = oparg + 2;  /* callable + self_or_null + args */

        /* Pop CALL arguments from the stack */
        for (int i = 0; i < items_to_pop; i++) {
            int idx = frame->stacktop - items_to_pop + i;
            if (idx >= 0) {
                Py_XDECREF(frame->localsplus[idx]);
                frame->localsplus[idx] = NULL;
            }
        }
        frame->stacktop -= items_to_pop;

        /* Advance prev_instr past CALL + CACHE entries.
         * Generic CALL may not set return_offset, so we use the known
         * instruction size (CALL + 3 CACHE = 4 codeunits).
         * eval loop resumes at prev_instr + 1. */
        frame->prev_instr += CALL_TOTAL_SIZE - 1;
    }
    /* else: inline dispatch path — stack and prev_instr already correct */

    #undef CALL_TOTAL_SIZE

    /* Push the resume value */
    Py_INCREF(value);
    frame->localsplus[frame->stacktop] = value;
    frame->stacktop++;
}

/*
 * Copy a frame onto the thread data stack.
 * Returns a pointer to the frame on the data stack, or NULL on error.
 * The caller must ensure there's enough space (or handle growth).
 */
static _aleff_frame_t *
push_frame_to_datastack(PyThreadState *tstate, _aleff_frame_t *src, int num_slots)
{
    size_t frame_size = sizeof(_aleff_frame_t)
                      + (num_slots - 1) * sizeof(PyObject *);
    size_t nslots = (frame_size + sizeof(PyObject *) - 1) / sizeof(PyObject *);

    /* Check if we have space; if not, we can't easily grow the stack
     * from outside the interpreter. For now, check and error. */
    if (tstate->datastack_top + nslots > tstate->datastack_limit) {
        PyErr_SetString(PyExc_RuntimeError,
            "thread data stack too small for frame restoration");
        return NULL;
    }

    _aleff_frame_t *dst = (_aleff_frame_t *)tstate->datastack_top;
    memcpy(dst, src, frame_size);
    tstate->datastack_top += nslots;

    /* INCREF all references (the copy shares objects with the snapshot copy) */
    Py_XINCREF(dst->f_executable);
    Py_XINCREF(dst->f_funcobj);
    Py_XINCREF(dst->f_globals);
    Py_XINCREF(dst->f_builtins);
    Py_XINCREF(dst->f_locals);
    dst->frame_obj = NULL;
    dst->owner = FRAME_OWNED_BY_THREAD;

    int stacktop = dst->stacktop;
    for (int i = 0; i < stacktop; i++) {
        Py_XINCREF(dst->localsplus[i]);
    }
    for (int i = stacktop; i < num_slots; i++) {
        dst->localsplus[i] = NULL;
    }

    return dst;
}

/* ========================================================================
 * Python-facing functions
 * ======================================================================== */

PyDoc_STRVAR(snapshot_frames_doc,
"snapshot_frames(depth=-1)\n"
"--\n\n"
"Capture the current Python frame chain as a FrameSnapshot.\n"
"The snapshot can be used to create multi-shot continuations.\n"
"\n"
"Parameters:\n"
"  depth: Maximum number of frames to capture. -1 for all frames.\n");

static PyObject *
_aleff_snapshot_frames([[maybe_unused]] PyObject *self, PyObject *args)
{
    int depth = -1;
    if (!PyArg_ParseTuple(args, "|i", &depth)) {
        return NULL;
    }

    FrameSnapshotObject *snapshot = create_snapshot(NULL, depth);
    if (snapshot == NULL) {
        return NULL;
    }

    return (PyObject *)snapshot;
}

PyDoc_STRVAR(restore_continuation_doc,
"restore_continuation(snapshot, value, skip=1)\n"
"--\n\n"
"Restore a continuation from a FrameSnapshot and resume execution.\n"
"\n"
"Creates a fresh copy of the frame chain from the snapshot,\n"
"injects `value` as the return value of the effect call,\n"
"and resumes execution via _PyEval_EvalFrameDefault.\n"
"\n"
"Parameters:\n"
"  snapshot: A FrameSnapshot object.\n"
"  value: The value to resume the continuation with.\n"
"  skip: Number of innermost frames to skip (default 1 for _Effect.__call__).\n"
"\n"
"Returns the result of the resumed computation.\n"
"This function should be called inside a greenlet.\n");

static PyObject *
_aleff_restore_continuation([[maybe_unused]] PyObject *self, PyObject *args)
{
    FrameSnapshotObject *snapshot;
    PyObject *value;
    int skip = 1;

    if (!PyArg_ParseTuple(args, "O!O|i", &FrameSnapshotType, &snapshot, &value, &skip))
        return NULL;

    if (_evalframe == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
            "_PyEval_EvalFrameDefault not available (dlsym failed at init)");
        return NULL;
    }

    int num = snapshot->num_frames - skip;
    if (num <= 0) {
        PyErr_SetString(PyExc_ValueError, "no frames to restore");
        return NULL;
    }

    PyThreadState *tstate = PyThreadState_Get();

    /* Save the data stack top so we can restore it on cleanup.
     * All frames we push will be between saved_top and the new top. */
    PyObject **saved_datastack_top = tstate->datastack_top;

    /* Push frames onto the thread data stack from outermost to innermost.
     * This matches the stack growth direction: outermost at lower address. */
    _aleff_frame_t *frames_on_stack[128];  /* reasonable limit */
    if (num > 128) {
        PyErr_SetString(PyExc_RuntimeError, "frame chain too deep (>128)");
        return NULL;
    }

    for (int i = num - 1; i >= 0; i--) {
        _aleff_frame_copy_t *src = &snapshot->frames[i + skip];
        _aleff_frame_t *f = push_frame_to_datastack(tstate, src->frame, src->num_slots);
        if (f == NULL) {
            /* Restore data stack and bail */
            tstate->datastack_top = saved_datastack_top;
            return NULL;
        }
        frames_on_stack[i] = f;
    }

    /* Link previous pointers on the data-stack copies */
    for (int i = 0; i < num - 1; i++) {
        frames_on_stack[i]->previous = frames_on_stack[i + 1];
    }
    /* Outermost frame's previous = NULL (eval will set it to entry_frame) */
    frames_on_stack[num - 1]->previous = NULL;

    /* Inject the resume value into the innermost frame */
    inject_resume_value(frames_on_stack[0], value);

    /* Execute frames one at a time, from innermost to outermost.
     *
     * We can't pass the whole chain to a single eval call because
     * _PyEval_EvalFrameDefault overwrites frame->previous with its
     * internal entry_frame sentinel.
     *
     * Instead, we eval each frame individually and propagate the
     * return value to the next outer frame by pushing it onto
     * the outer frame's stack. */
    PyObject *result = NULL;

    for (int i = 0; i < num; i++) {
        _aleff_frame_t *frame = frames_on_stack[i];

        /* Disconnect from chain so eval's entry_frame doesn't conflict */
        frame->previous = NULL;

        result = _evalframe(tstate, frame, 0);

        if (result == NULL) {
            /* Exception — propagate it */
            break;
        }

        /* If there's a next (outer) frame, push result onto its stack */
        if (i + 1 < num) {
            _aleff_frame_t *outer = frames_on_stack[i + 1];
            inject_resume_value(outer, result);
            Py_DECREF(result);  /* inject_resume_value INCREFs */
            result = NULL;
        }
    }

    /* Restore the data stack top.
     * The eval loop already popped each frame via _PyThreadState_PopFrame,
     * but our frames were pushed contiguously, so we restore to the
     * saved position to be safe. */
    tstate->datastack_top = saved_datastack_top;

    return result;
}

PyDoc_STRVAR(snapshot_num_frames_doc,
"snapshot_num_frames(snapshot)\n"
"--\n\n"
"Return the number of frames in a FrameSnapshot.\n");

static PyObject *
_aleff_snapshot_num_frames([[maybe_unused]] PyObject *self, PyObject *arg)
{
    if (!Py_IS_TYPE(arg, &FrameSnapshotType)) {
        PyErr_SetString(PyExc_TypeError, "expected a FrameSnapshot");
        return NULL;
    }
    FrameSnapshotObject *snapshot = (FrameSnapshotObject *)arg;
    return PyLong_FromLong(snapshot->num_frames);
}

/* ========================================================================
 * Module definition
 * ======================================================================== */

static PyMethodDef _aleff_methods[] = {
    {"snapshot_frames", _aleff_snapshot_frames, METH_VARARGS, snapshot_frames_doc},
    {"snapshot_num_frames", _aleff_snapshot_num_frames, METH_O, snapshot_num_frames_doc},
    {"restore_continuation", _aleff_restore_continuation, METH_VARARGS, restore_continuation_doc},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef _aleff_module = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "_aleff",
    .m_doc = "C extension for multi-shot delimited continuations.\n"
             "Provides frame chain snapshot/restore for Python 3.12+.",
    .m_size = -1,
    .m_methods = _aleff_methods,
};

PyMODINIT_FUNC
PyInit__aleff(void)
{
    /* Look up _PyEval_EvalFrameDefault via dlsym.
     * POSIX guarantees dlsym returns a valid function pointer via void*,
     * but ISO C forbids the cast. Suppress the warning here. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    _evalframe = (evalframe_fn_t)dlsym(RTLD_DEFAULT, "_PyEval_EvalFrameDefault");
#pragma GCC diagnostic pop
    /* Not fatal if not found — restore_continuation will raise at call time */

    if (PyType_Ready(&FrameSnapshotType) < 0)
        return NULL;

    PyObject *m = PyModule_Create(&_aleff_module);
    if (m == NULL)
        return NULL;

    Py_INCREF(&FrameSnapshotType);
    if (PyModule_AddObject(m, "FrameSnapshot", (PyObject *)&FrameSnapshotType) < 0) {
        Py_DECREF(&FrameSnapshotType);
        Py_DECREF(m);
        return NULL;
    }

    /* Export whether restore_continuation is available */
    if (PyModule_AddIntConstant(m, "HAS_RESTORE", _evalframe != NULL) < 0) {
        Py_DECREF(m);
        return NULL;
    }

    return m;
}
