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
    PyObject *f_locals;                  /* strong ref, may be nullptr */
    PyFrameObject *frame_obj;            /* strong ref, may be nullptr */
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
        if (f == nullptr) continue;

        Py_XDECREF(f->f_executable);
        Py_XDECREF(f->f_funcobj);
        /* f_globals and f_builtins are borrowed in live frames,
           but we hold strong refs in copies */
        Py_XDECREF(f->f_globals);
        Py_XDECREF(f->f_builtins);
        Py_XDECREF(f->f_locals);
        /* Don't decref frame_obj — we set it to nullptr in copies */

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
    {"__class_getitem__", FrameSnapshot_class_getitem, METH_O | METH_CLASS, nullptr},
    {nullptr, nullptr, 0, nullptr}
};

static PyTypeObject FrameSnapshotType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
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
 * frame_obj is set to nullptr (not shared with the original).
 */
static _aleff_frame_copy_t
copy_single_frame(_aleff_frame_t *src)
{
    _aleff_frame_copy_t result = {nullptr, 0};

    PyCodeObject *code = _aleff_frame_get_code(src);
    int num_slots = _aleff_frame_num_slots(code);

    size_t frame_size = sizeof(_aleff_frame_t)
                      + (num_slots - 1) * sizeof(PyObject *);

    _aleff_frame_t *dst = (_aleff_frame_t *)PyMem_Malloc(frame_size);
    if (dst == nullptr) {
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
    dst->frame_obj = nullptr;

    /* previous will be linked later */
    dst->previous = nullptr;

    /* owner: mark as owned by thread (will be cleaned up manually) */
    dst->owner = FRAME_OWNED_BY_THREAD;

    /* INCREF localsplus entries.
     *
     * CPython 3.12 sets stacktop = -1 while a frame is actively executing
     * (the real stack pointer is kept in a register). stacktop is only
     * written with a valid value when the frame is deactivated (yield, etc.).
     *
     * Since snapshot_frames() is called from within the eval loop,
     * stacktop will be -1 for active frames. In this case we must
     * INCREF all slots (locals + cells + value stack).
     *
     * When stacktop >= 0 (deactivated frame), only slots 0..stacktop-1
     * are valid; the rest may contain stale pointers from POP(). */
    /* INCREF localsplus entries.
     *
     * CPython 3.12 sets stacktop = -1 while a frame is actively executing
     * (the real stack pointer lives in a register). In this case the
     * value stack slots still contain valid PyObject pointers.
     *
     * When stacktop >= 0 (deactivated frame), slots 0..stacktop-1 are
     * valid. Slots beyond stacktop may contain stale pointers left by
     * the eval loop's POP() macro (which doesn't null popped slots). */
    /* When stacktop >= 0: slots 0..stacktop-1 are valid.
     * When stacktop == -1 (active frame): only locals/cells/freevars
     * (0..co_nlocalsplus-1) are safe. The value stack portion may
     * contain stale pointers from the eval loop. */
    int valid_slots = dst->stacktop >= 0
        ? dst->stacktop
        : code->co_nlocalsplus;
    for (int i = 0; i < valid_slots; i++) {
        Py_XINCREF(dst->localsplus[i]);
    }
    for (int i = valid_slots; i < num_slots; i++) {
        dst->localsplus[i] = nullptr;
    }

    result.frame = dst;
    result.num_slots = num_slots;
    return result;
}

/*
 * Snapshot the Python frame chain from the current thread state.
 *
 * Captures frames from the current frame up to (but not including)
 * the frame specified by `boundary` (or all frames if boundary is nullptr).
 *
 * The `depth` parameter limits how many frames to capture.
 * Pass -1 for unlimited.
 */
static FrameSnapshotObject *
create_snapshot(PyFrameObject *boundary_frame, int max_depth)
{
    PyThreadState *tstate = PyThreadState_Get();
    PyFrameObject *py_frame = PyThreadState_GetFrame(tstate);
    if (py_frame == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "no current frame");
        return nullptr;
    }

    /* Count frames.
     * PyFrame_GetBack returns a new reference, so we must DECREF. */
    int count = 0;
    {
        PyFrameObject *f = py_frame;
        Py_INCREF(f);  /* hold our own ref for the loop */
        while (f != nullptr && f != boundary_frame) {
            if (max_depth >= 0 && count >= max_depth) {
                Py_DECREF(f);
                break;
            }
            count++;
            PyFrameObject *prev = PyFrame_GetBack(f);  /* new ref */
            Py_DECREF(f);
            f = prev;
        }
        /* If loop ended by boundary or nullptr (not by max_depth break),
         * f was already DECREF'd inside the loop via prev/DECREF pattern,
         * or f is nullptr. The boundary case: f == boundary_frame and we
         * exited the while condition, so f still has our ref. */
        if (f != nullptr && !(max_depth >= 0 && count >= max_depth)) {
            Py_DECREF(f);
        }
    }

    if (count == 0) {
        PyErr_SetString(PyExc_RuntimeError, "no frames to snapshot");
        Py_DECREF(py_frame);
        return nullptr;
    }

    FrameSnapshotObject *snapshot = PyObject_New(FrameSnapshotObject, &FrameSnapshotType);
    if (snapshot == nullptr) {
        Py_DECREF(py_frame);
        return nullptr;
    }

    snapshot->frames = (_aleff_frame_copy_t *)PyMem_Calloc(count, sizeof(_aleff_frame_copy_t));
    if (snapshot->frames == nullptr) {
        PyErr_NoMemory();
        Py_DECREF(snapshot);
        Py_DECREF(py_frame);
        return nullptr;
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
            if (snapshot->frames[i].frame == nullptr) {
                snapshot->num_frames = i;
                Py_DECREF(f);
                Py_DECREF(snapshot);
                Py_DECREF(py_frame);
                return nullptr;
            }

            PyFrameObject *prev = PyFrame_GetBack(f);  /* new ref */
            Py_DECREF(f);
            f = prev;
        }
        Py_XDECREF(f);  /* may be nullptr if chain ended */
    }

    #undef F_FRAME_OFFSET

    Py_DECREF(py_frame);

    /* Link the copied frames */
    for (int i = 0; i < count - 1; i++) {
        snapshot->frames[i].frame->previous = snapshot->frames[i + 1].frame;
    }
    /* Outermost frame's previous = nullptr (will be set during restore) */

    return snapshot;
}

/* ========================================================================
 * _PyEval_EvalFrameDefault lookup
 * ======================================================================== */

typedef PyObject *(*evalframe_fn_t)(PyThreadState *, void *, int);
static evalframe_fn_t _evalframe = nullptr;

/* ========================================================================
 * Frame chain restoration and continuation resume
 * ======================================================================== */

/*
 * Inject resume value into a frame, simulating the return from a CALL.
 *
 * The frame was suspended mid-CALL (calling the effect).
 *
 * CPython 3.12 sets stacktop = -1 while frames are active (the real
 * stack pointer lives in a register). We use the opcode at prev_instr
 * to determine the CALL dispatch path:
 *
 * - opcode == 171 (CALL): generic path. Stack has callable + args,
 *   prev_instr at the CALL instruction. Need to pop args and advance.
 *
 * - other opcode (CACHE=0, or specialized CALL variant): inline dispatch.
 *   Stack already shrunk, prev_instr past CACHE entries. Just push value.
 *
 * In both cases, we set stacktop to co_nlocalsplus (value stack base)
 * before pushing, since the original stacktop may be -1 (invalid).
 */
static void
inject_resume_value(_aleff_frame_t *frame, PyObject *value)
{
    PyCodeObject *code = _aleff_frame_get_code(frame);
    int value_stack_base = code->co_nlocalsplus;
    uint8_t opcode = (*frame->prev_instr) & 0xFF;

    /* CALL instruction size: 1 (CALL) + 3 (CACHE entries) = 4 codeunits */
    #define CALL_OPCODE 171
    #define CALL_TOTAL_SIZE 4

    if (opcode == CALL_OPCODE) {
        /* Generic CALL path: stack has callable + self_or_null + args
         * at the TOP of the value stack.  Items below (e.g. loop
         * iterators, partially-evaluated expressions) must be preserved.
         *
         * Pop only the CALL arguments from the top, not the whole stack. */
        uint8_t oparg = (*frame->prev_instr >> 8) & 0xFF;
        int call_items = oparg + 2;  /* callable + self_or_null + args */
        int new_top = frame->stacktop - call_items;
        if (new_top < value_stack_base) {
            new_top = value_stack_base;
        }
        for (int i = new_top; i < frame->stacktop; i++) {
            Py_XDECREF(frame->localsplus[i]);
            frame->localsplus[i] = nullptr;
        }
        frame->stacktop = new_top;

        /* Advance prev_instr past CALL + CACHE entries. */
        frame->prev_instr += CALL_TOTAL_SIZE - 1;
    } else {
        /* Non-CALL opcode (CACHE entry or specialised variant).
         * If stacktop is valid (>= 0), it was saved by the eval loop
         * when the frame called a Python function.  Preserve it.
         * Only reset to value_stack_base when stacktop is -1 (active
         * frame that went through a C-function call path). */
        if (frame->stacktop < 0) {
            frame->stacktop = value_stack_base;
        }
    }

    #undef CALL_OPCODE
    #undef CALL_TOTAL_SIZE

    /* Push the resume value */
    Py_INCREF(value);
    frame->localsplus[frame->stacktop] = value;
    frame->stacktop++;
}

/*
 * Copy a frame onto the thread data stack.
 * Returns a pointer to the frame on the data stack, or nullptr on error.
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
        return nullptr;
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
    dst->frame_obj = nullptr;
    dst->owner = FRAME_OWNED_BY_THREAD;

    /* Source is from a snapshot where stale slots are already nullified.
     * INCREF all non-null entries. */
    for (int i = 0; i < num_slots; i++) {
        Py_XINCREF(dst->localsplus[i]);
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
        return nullptr;
    }

    FrameSnapshotObject *snapshot = create_snapshot(nullptr, depth);
    if (snapshot == nullptr) {
        return nullptr;
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
        return nullptr;

    if (_evalframe == nullptr) {
        PyErr_SetString(PyExc_RuntimeError,
            "_PyEval_EvalFrameDefault not available (dlsym failed at init)");
        return nullptr;
    }

    int num = snapshot->num_frames - skip;
    if (num <= 0) {
        PyErr_SetString(PyExc_ValueError, "no frames to restore");
        return nullptr;
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
        return nullptr;
    }

    for (int i = num - 1; i >= 0; i--) {
        _aleff_frame_copy_t *src = &snapshot->frames[i + skip];
        _aleff_frame_t *f = push_frame_to_datastack(tstate, src->frame, src->num_slots);
        if (f == nullptr) {
            /* Restore data stack and bail */
            tstate->datastack_top = saved_datastack_top;
            return nullptr;
        }
        frames_on_stack[i] = f;
    }

    /* Link previous pointers on the data-stack copies */
    for (int i = 0; i < num - 1; i++) {
        frames_on_stack[i]->previous = frames_on_stack[i + 1];
    }
    /* Outermost frame's previous = nullptr (eval will set it to entry_frame) */
    frames_on_stack[num - 1]->previous = nullptr;

    /* Inject the resume value into the innermost frame */
    inject_resume_value(frames_on_stack[0], value);

    /* Execute frames one at a time, from innermost to outermost.
     *
     * _PyEval_EvalFrameDefault overwrites the passed frame's `previous`
     * with its internal entry_frame sentinel, so we can't pass the whole
     * chain. Instead we eval each frame individually.
     *
     * After each frame completes, we inject its return value into the
     * next outer frame using inject_resume_value, which handles both
     * inline dispatch and generic CALL stack states. */
    PyObject *result = nullptr;

    for (int i = 0; i < num; i++) {
        _aleff_frame_t *frame = frames_on_stack[i];
        frame->previous = nullptr;

        result = _evalframe(tstate, frame, 0);

        if (result == nullptr) {
            break;
        }

        if (i + 1 < num) {
            _aleff_frame_t *outer = frames_on_stack[i + 1];
            inject_resume_value(outer, result);
            Py_DECREF(result);
            result = nullptr;
        }
    }

    /* Restore the data stack top. */
    tstate->datastack_top = saved_datastack_top;

    return result;
}

PyDoc_STRVAR(snapshot_from_frame_doc,
"snapshot_from_frame(frame, depth=-1)\n"
"--\n\n"
"Capture a frame chain starting from the given frame object.\n"
"The frame should be from a suspended greenlet (gr_frame) so that\n"
"stacktop values are valid.\n"
"\n"
"Parameters:\n"
"  frame: A frame object (e.g. greenlet.gr_frame).\n"
"  depth: Maximum number of frames to capture. -1 for all.\n");

static PyObject *
_aleff_snapshot_from_frame([[maybe_unused]] PyObject *self, PyObject *args)
{
    PyFrameObject *start_frame;
    int depth = -1;
    if (!PyArg_ParseTuple(args, "O!|i", &PyFrame_Type, &start_frame, &depth))
        return nullptr;

    /* Count frames */
    int count = 0;
    {
        PyFrameObject *f = start_frame;
        Py_INCREF(f);
        while (f != nullptr) {
            if (depth >= 0 && count >= depth) {
                Py_DECREF(f);
                break;
            }
            count++;
            PyFrameObject *prev = PyFrame_GetBack(f);
            Py_DECREF(f);
            f = prev;
        }
        if (f != nullptr && !(depth >= 0 && count >= depth)) {
            Py_DECREF(f);
        }
    }

    if (count == 0) {
        PyErr_SetString(PyExc_RuntimeError, "no frames to snapshot");
        return nullptr;
    }

    FrameSnapshotObject *snapshot = PyObject_New(FrameSnapshotObject, &FrameSnapshotType);
    if (snapshot == nullptr)
        return nullptr;

    snapshot->frames = (_aleff_frame_copy_t *)PyMem_Calloc(count, sizeof(_aleff_frame_copy_t));
    if (snapshot->frames == nullptr) {
        PyErr_NoMemory();
        Py_DECREF(snapshot);
        return nullptr;
    }
    snapshot->num_frames = count;

    #define F_FRAME_OFFSET (sizeof(PyObject) + sizeof(PyFrameObject *))

    {
        PyFrameObject *f = start_frame;
        Py_INCREF(f);
        for (int i = 0; i < count; i++) {
            _aleff_frame_t *internal = *(_aleff_frame_t **)(
                (char *)f + F_FRAME_OFFSET
            );

            snapshot->frames[i] = copy_single_frame(internal);
            if (snapshot->frames[i].frame == nullptr) {
                snapshot->num_frames = i;
                Py_DECREF(f);
                Py_DECREF(snapshot);
                return nullptr;
            }

            PyFrameObject *prev = PyFrame_GetBack(f);
            Py_DECREF(f);
            f = prev;
        }
        Py_XDECREF(f);
    }

    #undef F_FRAME_OFFSET

    /* Link copied frames */
    for (int i = 0; i < count - 1; i++) {
        snapshot->frames[i].frame->previous = snapshot->frames[i + 1].frame;
    }

    return (PyObject *)snapshot;
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
        return nullptr;
    }
    FrameSnapshotObject *snapshot = (FrameSnapshotObject *)arg;
    return PyLong_FromLong(snapshot->num_frames);
}

/* Debug helper: return stacktop of internal frame behind a Python frame object */
PyDoc_STRVAR(debug_frame_stacktop_doc,
"_debug_frame_stacktop(frame)\n"
"--\n\n"
"Return the stacktop value of the internal frame (for debugging).\n");

static PyObject *
_aleff_debug_frame_stacktop([[maybe_unused]] PyObject *self, PyObject *arg)
{
    if (!PyFrame_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected a frame object");
        return nullptr;
    }
    #define F_FRAME_OFFSET (sizeof(PyObject) + sizeof(PyFrameObject *))
    _aleff_frame_t *iframe = *(_aleff_frame_t **)((char *)arg + F_FRAME_OFFSET);
    #undef F_FRAME_OFFSET
    return PyLong_FromLong(iframe->stacktop);
}

/* ========================================================================
 * Module definition
 * ======================================================================== */

static PyMethodDef _aleff_methods[] = {
    {"snapshot_frames", _aleff_snapshot_frames, METH_VARARGS, snapshot_frames_doc},
    {"snapshot_from_frame", _aleff_snapshot_from_frame, METH_VARARGS, snapshot_from_frame_doc},
    {"snapshot_num_frames", _aleff_snapshot_num_frames, METH_O, snapshot_num_frames_doc},
    {"restore_continuation", _aleff_restore_continuation, METH_VARARGS, restore_continuation_doc},
    {"_debug_frame_stacktop", _aleff_debug_frame_stacktop, METH_O, debug_frame_stacktop_doc},
    {nullptr, nullptr, 0, nullptr}
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
        return nullptr;

    PyObject *m = PyModule_Create(&_aleff_module);
    if (m == nullptr)
        return nullptr;

    Py_INCREF(&FrameSnapshotType);
    if (PyModule_AddObject(m, "FrameSnapshot", (PyObject *)&FrameSnapshotType) < 0) {
        Py_DECREF(&FrameSnapshotType);
        Py_DECREF(m);
        return nullptr;
    }

    /* Export whether restore_continuation is available */
    if (PyModule_AddIntConstant(m, "HAS_RESTORE", _evalframe != nullptr) < 0) {
        Py_DECREF(m);
        return nullptr;
    }

    return m;
}
