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
#include <compile.h>
#include <frameobject.h>
#include <limits.h>
#include <stddef.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#if PY_VERSION_HEX < 0x030c0000
#error "_aleff requires Python 3.12 or later"
#endif

#if PY_VERSION_HEX >= 0x030f0000
#error "_aleff coroutine internals require review before supporting Python 3.15+"
#endif

/* MSVC /std:c17 does not support C23 [[maybe_unused]] or nullptr. */
#if defined(_MSC_VER) && !defined(__cplusplus)
#  define nullptr NULL
#  define _ALEFF_UNUSED
#else
#  define _ALEFF_UNUSED [[maybe_unused]]
#endif

/* Opcode metadata: we need a deopt table to map specialized opcodes
 * (e.g. CALL_PY_GENERAL) back to their generic base (CALL).
 *
 * _PyOpcode_Deopt[] is in internal headers and not exported from
 * libpython, so we include the header with Py_BUILD_CORE to get the
 * table definition, then copy it into a file-scoped static array. */
#if PY_VERSION_HEX >= 0x030d0000
#  include <opcode_ids.h>
#  define Py_BUILD_CORE
#  define NEED_OPCODE_METADATA
#  include <internal/pycore_opcode_metadata.h>
#  undef NEED_OPCODE_METADATA
#  undef Py_BUILD_CORE
#else
#  include <opcode.h>
#endif
#define CALL_OPCODE CALL

/* Build a local deopt table.  For opcodes that have no specialization
 * the entry maps to itself, so unknown opcodes pass through safely. */
static uint8_t _aleff_opcode_deopt[256];

static void
_aleff_init_opcode_deopt(void)
{
    for (int i = 0; i < 256; i++) {
        _aleff_opcode_deopt[i] = (uint8_t)i;
    }
    /* CALL specializations — all map back to generic CALL. */
#ifdef CALL_BOUND_METHOD_EXACT_ARGS
    _aleff_opcode_deopt[CALL_BOUND_METHOD_EXACT_ARGS] = CALL;
#endif
#ifdef CALL_PY_EXACT_ARGS
    _aleff_opcode_deopt[CALL_PY_EXACT_ARGS] = CALL;
#endif
#ifdef CALL_PY_GENERAL
    _aleff_opcode_deopt[CALL_PY_GENERAL] = CALL;
#endif
#ifdef CALL_PY_WITH_DEFAULTS
    _aleff_opcode_deopt[CALL_PY_WITH_DEFAULTS] = CALL;
#endif
#ifdef CALL_BOUND_METHOD_GENERAL
    _aleff_opcode_deopt[CALL_BOUND_METHOD_GENERAL] = CALL;
#endif
#ifdef CALL_BUILTIN_CLASS
    _aleff_opcode_deopt[CALL_BUILTIN_CLASS] = CALL;
#endif
#ifdef CALL_BUILTIN_FAST
    _aleff_opcode_deopt[CALL_BUILTIN_FAST] = CALL;
#endif
#ifdef CALL_BUILTIN_FAST_WITH_KEYWORDS
    _aleff_opcode_deopt[CALL_BUILTIN_FAST_WITH_KEYWORDS] = CALL;
#endif
#ifdef CALL_BUILTIN_O
    _aleff_opcode_deopt[CALL_BUILTIN_O] = CALL;
#endif
#ifdef CALL_ISINSTANCE
    _aleff_opcode_deopt[CALL_ISINSTANCE] = CALL;
#endif
#ifdef CALL_LEN
    _aleff_opcode_deopt[CALL_LEN] = CALL;
#endif
#ifdef CALL_LIST_APPEND
    _aleff_opcode_deopt[CALL_LIST_APPEND] = CALL;
#endif
#ifdef CALL_METHOD_DESCRIPTOR_FAST
    _aleff_opcode_deopt[CALL_METHOD_DESCRIPTOR_FAST] = CALL;
#endif
#ifdef CALL_METHOD_DESCRIPTOR_FAST_WITH_KEYWORDS
    _aleff_opcode_deopt[CALL_METHOD_DESCRIPTOR_FAST_WITH_KEYWORDS] = CALL;
#endif
#ifdef CALL_METHOD_DESCRIPTOR_NOARGS
    _aleff_opcode_deopt[CALL_METHOD_DESCRIPTOR_NOARGS] = CALL;
#endif
#ifdef CALL_METHOD_DESCRIPTOR_O
    _aleff_opcode_deopt[CALL_METHOD_DESCRIPTOR_O] = CALL;
#endif
#ifdef CALL_NON_PY_GENERAL
    _aleff_opcode_deopt[CALL_NON_PY_GENERAL] = CALL;
#endif
#ifdef CALL_STR_1
    _aleff_opcode_deopt[CALL_STR_1] = CALL;
#endif
#ifdef CALL_TUPLE_1
    _aleff_opcode_deopt[CALL_TUPLE_1] = CALL;
#endif
#ifdef CALL_TYPE_1
    _aleff_opcode_deopt[CALL_TYPE_1] = CALL;
#endif
#ifdef CALL_ALLOC_AND_ENTER_INIT
    _aleff_opcode_deopt[CALL_ALLOC_AND_ENTER_INIT] = CALL;
#endif
    /* 3.12 names */
#ifdef CALL_NO_KW_BUILTIN_FAST
    _aleff_opcode_deopt[CALL_NO_KW_BUILTIN_FAST] = CALL;
#endif
#ifdef CALL_NO_KW_BUILTIN_O
    _aleff_opcode_deopt[CALL_NO_KW_BUILTIN_O] = CALL;
#endif
#ifdef CALL_NO_KW_ISINSTANCE
    _aleff_opcode_deopt[CALL_NO_KW_ISINSTANCE] = CALL;
#endif
#ifdef CALL_NO_KW_LEN
    _aleff_opcode_deopt[CALL_NO_KW_LEN] = CALL;
#endif
#ifdef CALL_NO_KW_LIST_APPEND
    _aleff_opcode_deopt[CALL_NO_KW_LIST_APPEND] = CALL;
#endif
#ifdef CALL_NO_KW_METHOD_DESCRIPTOR_FAST
    _aleff_opcode_deopt[CALL_NO_KW_METHOD_DESCRIPTOR_FAST] = CALL;
#endif
#ifdef CALL_NO_KW_METHOD_DESCRIPTOR_NOARGS
    _aleff_opcode_deopt[CALL_NO_KW_METHOD_DESCRIPTOR_NOARGS] = CALL;
#endif
#ifdef CALL_NO_KW_METHOD_DESCRIPTOR_O
    _aleff_opcode_deopt[CALL_NO_KW_METHOD_DESCRIPTOR_O] = CALL;
#endif
#ifdef CALL_NO_KW_STR_1
    _aleff_opcode_deopt[CALL_NO_KW_STR_1] = CALL;
#endif
#ifdef CALL_NO_KW_TUPLE_1
    _aleff_opcode_deopt[CALL_NO_KW_TUPLE_1] = CALL;
#endif
#ifdef CALL_NO_KW_TYPE_1
    _aleff_opcode_deopt[CALL_NO_KW_TYPE_1] = CALL;
#endif
}

/* ========================================================================
 * _PyInterpreterFrame layout replica
 *
 * We define our own struct matching cpython/Include/internal/pycore_frame.h
 * because the internal headers are not installed.
 *
 * 3.14 changed several field types:
 *   - f_executable, f_funcobj: PyObject * → _PyStackRef (uintptr_t)
 *   - localsplus elements: PyObject * → _PyStackRef
 *   - stacktop (int) → stackpointer (_PyStackRef *)
 *   - FRAME_OWNED_BY_CSTACK: 3 → 4
 * ======================================================================== */

typedef uint16_t _aleff_codeunit;
#define ALEFF_CODE_CODE(code) ((_aleff_codeunit *)((code)->co_code_adaptive))

#if PY_VERSION_HEX >= 0x030e0000
/* Python 3.14+: _PyStackRef fields */

typedef uintptr_t _aleff_stackref;

typedef struct _aleff_frame {
    _aleff_stackref f_executable;        /* _PyStackRef: code object */
    struct _aleff_frame *previous;
    _aleff_stackref f_funcobj;           /* _PyStackRef: function object */
    PyObject *f_globals;
    PyObject *f_builtins;
    PyObject *f_locals;
    PyFrameObject *frame_obj;
    _aleff_codeunit *instr_ptr;          /* instruction pointer (3.13+ name) */
    _aleff_stackref *stackpointer;       /* pointer into localsplus */
#ifdef Py_GIL_DISABLED
    int32_t tlbc_index;                  /* thread-local bytecode index */
#endif
    uint16_t return_offset;
    char owner;
#ifdef Py_DEBUG
    uint8_t visited : 1;
    uint8_t lltrace : 7;
#else
    uint8_t visited;
#endif
    _aleff_stackref localsplus[1];       /* _PyStackRef elements */
} _aleff_frame_t;

/* CPython 3.14 stack slots can hold object references, null, or tagged
 * integers.  Only object references participate in reference counting. */
#define _ALEFF_REFCNT_TAG ((uintptr_t)1)
#define _ALEFF_INT_TAG ((uintptr_t)3)

static inline int
_aleff_stackref_is_tagged_int(_aleff_stackref ref)
{
    return (ref & _ALEFF_INT_TAG) == _ALEFF_INT_TAG;
}

static inline int
_aleff_stackref_is_null(_aleff_stackref ref)
{
    return ref == _ALEFF_REFCNT_TAG;
}

static inline int
_aleff_stackref_is_object(_aleff_stackref ref)
{
    return !_aleff_stackref_is_null(ref)
        && !_aleff_stackref_is_tagged_int(ref);
}

static inline PyObject *
_aleff_stackref_to_obj(_aleff_stackref ref)
{
    if (!_aleff_stackref_is_object(ref)) return nullptr;
    return (PyObject *)(ref & ~_ALEFF_REFCNT_TAG);
}

static inline _aleff_stackref
_aleff_obj_to_stackref(PyObject *obj)
{
    if (obj == nullptr) return _ALEFF_REFCNT_TAG;  /* PyStackRef_NULL.bits */
#ifdef Py_GIL_DISABLED
    /* A tag-0 stackref is a valid strong reference for every object.  The
     * deferred-refcount flag lives in private GC state on free-threaded
     * builds, so create an ordinary owned stackref instead. */
    return (_aleff_stackref)(uintptr_t)obj;
#else
    /* Replicate PyStackRef_FromPyObjectSteal: immortal objects get the
     * Py_TAG_REFCNT (1) tag bit set via ob_flags. */
    unsigned int tag = ((PyObject *)obj)->ob_flags & _ALEFF_REFCNT_TAG;
    return (_aleff_stackref)((uintptr_t)obj | tag);
#endif
}

static inline void
_aleff_stackref_retain(_aleff_stackref ref)
{
    Py_XINCREF(_aleff_stackref_to_obj(ref));
}

static inline void
_aleff_stackref_release(_aleff_stackref ref)
{
    Py_XDECREF(_aleff_stackref_to_obj(ref));
}

static inline void
_aleff_stackref_dup(_aleff_stackref ref)
{
    if (_aleff_stackref_is_object(ref)
        && (ref & _ALEFF_REFCNT_TAG) == 0) {
        Py_INCREF((PyObject *)ref);
    }
}

static inline void
_aleff_stackref_close(_aleff_stackref ref)
{
    if (_aleff_stackref_is_object(ref)
        && (ref & _ALEFF_REFCNT_TAG) == 0) {
        Py_DECREF((PyObject *)ref);
    }
}

static inline _aleff_stackref
_aleff_obj_to_new_stackref(PyObject *obj)
{
    _aleff_stackref ref = _aleff_obj_to_stackref(obj);
    _aleff_stackref_dup(ref);
    return ref;
}

static inline _aleff_stackref
_aleff_stackref_new_strong(_aleff_stackref ref)
{
    if (!_aleff_stackref_is_object(ref)) return ref;
    return _aleff_obj_to_new_stackref(_aleff_stackref_to_obj(ref));
}

static inline void
_aleff_stackref_dup_strong(_aleff_stackref *ref)
{
    *ref = _aleff_stackref_new_strong(*ref);
}

#define ALEFF_STACKREF_RETAIN(ref) _aleff_stackref_retain(ref)
#define ALEFF_STACKREF_RELEASE(ref) _aleff_stackref_release(ref)
#define ALEFF_STACKREF_DUP(ref) _aleff_stackref_dup_strong(&(ref))
#define ALEFF_STACKREF_CLOSE(ref) _aleff_stackref_close(ref)

#define ALEFF_LOCALSPLUS_GET(frame, i) \
    _aleff_stackref_to_obj((frame)->localsplus[i])
#define ALEFF_LOCALSPLUS_SET(frame, i, obj) \
    ((frame)->localsplus[i] = _aleff_obj_to_stackref(obj))
#define ALEFF_LOCALSPLUS_SET_NEW(frame, i, obj) \
    ((frame)->localsplus[i] = _aleff_obj_to_new_stackref(obj))
#define ALEFF_LOCALSPLUS_IS_OBJECT(frame, i) \
    _aleff_stackref_is_object((frame)->localsplus[i])
#define ALEFF_LOCALSPLUS_RETAIN(frame, i) \
    ALEFF_STACKREF_RETAIN((frame)->localsplus[i])
#define ALEFF_LOCALSPLUS_RELEASE(frame, i) \
    ALEFF_STACKREF_RELEASE((frame)->localsplus[i])
#define ALEFF_LOCALSPLUS_DUP(frame, i) \
    ALEFF_STACKREF_DUP((frame)->localsplus[i])
#define ALEFF_LOCALSPLUS_CLOSE(frame, i) \
    ALEFF_STACKREF_CLOSE((frame)->localsplus[i])

/* prev_instr accessor (3.14 uses instr_ptr) */
#define ALEFF_PREV_INSTR(frame) ((frame)->instr_ptr)

/* stacktop accessor: compute from stackpointer - localsplus base */
static inline int
_aleff_frame_stacktop(_aleff_frame_t *frame)
{
    if (frame->stackpointer == nullptr) return -1;
    return (int)(frame->stackpointer - frame->localsplus);
}

static inline void
_aleff_frame_set_stacktop(_aleff_frame_t *frame, int top)
{
    frame->stackpointer = frame->localsplus + top;
}

#define FRAME_OWNED_BY_THREAD 0
#define FRAME_OWNED_BY_GENERATOR 1
#define FRAME_OWNED_BY_FRAME_OBJECT 2
#define FRAME_OWNED_BY_INTERPRETER 3
#define FRAME_OWNED_BY_CSTACK 4

/* PyCoroObject became opaque in the public CPython 3.14 headers.  Mirror the
 * CPython 3.14 prefix through cr_frame_state; the explicit version guard near
 * the top of this file requires this layout to be reviewed for Python 3.15. */
typedef struct {
    PyObject_HEAD
    PyObject *cr_weakreflist;
    PyObject *cr_name;
    PyObject *cr_qualname;
    _PyErr_StackItem cr_exc_state;
    PyObject *cr_origin_or_finalizer;
    char cr_hooks_inited;
    char cr_closed;
    char cr_running_async;
    int8_t cr_frame_state;
} _aleff_coro_prefix_t;

#define ALEFF_SET_GEN_FRAME_STATE(owner, state) \
    (((_aleff_coro_prefix_t *)(owner))->cr_frame_state = (state))
#define ALEFF_GEN_EXC_STATE(owner) \
    (&((_aleff_coro_prefix_t *)(owner))->cr_exc_state)
#define ALEFF_GEN_FROM_FRAME(frame) \
    ((_aleff_coro_prefix_t *)((char *)(frame) - sizeof(_aleff_coro_prefix_t)))

#else
/* Python 3.12-3.13: PyObject* fields */

typedef struct _aleff_frame {
    PyObject *f_executable;              /* strong ref: code object */
    struct _aleff_frame *previous;
    PyObject *f_funcobj;                 /* strong ref: function object */
    PyObject *f_globals;
    PyObject *f_builtins;
    PyObject *f_locals;
    PyFrameObject *frame_obj;
    _aleff_codeunit *prev_instr;         /* instruction pointer */
    int stacktop;                        /* top of value stack */
    uint16_t return_offset;
    char owner;
    PyObject *localsplus[1];             /* variable-length */
} _aleff_frame_t;

#define ALEFF_LOCALSPLUS_GET(frame, i) ((frame)->localsplus[i])
#define ALEFF_LOCALSPLUS_SET(frame, i, obj) ((frame)->localsplus[i] = (obj))
#define ALEFF_LOCALSPLUS_SET_NEW(frame, i, obj) \
    (Py_INCREF(obj), ALEFF_LOCALSPLUS_SET(frame, i, obj))
#define ALEFF_LOCALSPLUS_IS_OBJECT(frame, i) \
    (ALEFF_LOCALSPLUS_GET(frame, i) != nullptr)
#define ALEFF_STACKREF_RETAIN(ref) Py_XINCREF(ref)
#define ALEFF_STACKREF_RELEASE(ref) Py_XDECREF(ref)
#define ALEFF_STACKREF_DUP(ref) Py_XINCREF(ref)
#define ALEFF_STACKREF_CLOSE(ref) Py_XDECREF(ref)
#define ALEFF_LOCALSPLUS_RETAIN(frame, i) \
    ALEFF_STACKREF_RETAIN((frame)->localsplus[i])
#define ALEFF_LOCALSPLUS_RELEASE(frame, i) \
    ALEFF_STACKREF_RELEASE((frame)->localsplus[i])
#define ALEFF_LOCALSPLUS_DUP(frame, i) \
    ALEFF_STACKREF_DUP((frame)->localsplus[i])
#define ALEFF_LOCALSPLUS_CLOSE(frame, i) \
    ALEFF_STACKREF_CLOSE((frame)->localsplus[i])
#define ALEFF_PREV_INSTR(frame) ((frame)->prev_instr)

static inline int
_aleff_frame_stacktop(_aleff_frame_t *frame)
{
    return frame->stacktop;
}

static inline void
_aleff_frame_set_stacktop(_aleff_frame_t *frame, int top)
{
    frame->stacktop = top;
}

#define FRAME_OWNED_BY_THREAD 0
#define FRAME_OWNED_BY_GENERATOR 1
#define FRAME_OWNED_BY_FRAME_OBJECT 2
#define FRAME_OWNED_BY_CSTACK 3

#define ALEFF_SET_GEN_FRAME_STATE(owner, state) \
    (((PyCoroObject *)(owner))->cr_frame_state = (state))
#define ALEFF_GEN_EXC_STATE(owner) \
    (&((PyCoroObject *)(owner))->cr_exc_state)
#define ALEFF_GEN_FROM_FRAME(frame) \
    ((PyCoroObject *)((char *)(frame) - offsetof(PyCoroObject, cr_iframe)))

#endif /* PY_VERSION_HEX >= 0x030e0000 */

static _aleff_frame_t *
_aleff_frame_from_pyframe(PyFrameObject *frame)
{
    #define F_FRAME_OFFSET (sizeof(PyObject) + sizeof(PyFrameObject *))
    _aleff_frame_t *internal = *(_aleff_frame_t **)(
        (char *)frame + F_FRAME_OFFSET
    );
    #undef F_FRAME_OFFSET
    return internal;
}

/* Accessors for f_executable and f_funcobj (PyObject* vs _PyStackRef) */
#if PY_VERSION_HEX >= 0x030e0000
#define ALEFF_GET_EXECUTABLE(frame) _aleff_stackref_to_obj((frame)->f_executable)
#define ALEFF_SET_EXECUTABLE(frame, obj) ((frame)->f_executable = _aleff_obj_to_stackref(obj))
#define ALEFF_GET_FUNCOBJ(frame) _aleff_stackref_to_obj((frame)->f_funcobj)
#define ALEFF_SET_FUNCOBJ(frame, obj) ((frame)->f_funcobj = _aleff_obj_to_stackref(obj))
#else
#define ALEFF_GET_EXECUTABLE(frame) ((frame)->f_executable)
#define ALEFF_SET_EXECUTABLE(frame, obj) ((frame)->f_executable = (obj))
#define ALEFF_GET_FUNCOBJ(frame) ((frame)->f_funcobj)
#define ALEFF_SET_FUNCOBJ(frame, obj) ((frame)->f_funcobj = (obj))
#endif

static inline PyCodeObject *
_aleff_frame_get_code(_aleff_frame_t *frame)
{
    return (PyCodeObject *)ALEFF_GET_EXECUTABLE(frame);
}

static PyObject *
_aleff_effective_handled_exception(_PyErr_StackItem *item)
{
    while (item != nullptr) {
        PyObject *exc = item->exc_value;
        if (exc != nullptr && exc != Py_None) {
            return exc;
        }
        item = item->previous_item;
    }
    return nullptr;
}

static inline int
_aleff_is_exception_instance(PyObject *obj)
{
    return obj != nullptr && PyExceptionInstance_Check(obj);
}

static PyObject *
_aleff_frame_handled_exception(_aleff_frame_t *frame, PyObject *fallback)
{
    if (frame->owner == FRAME_OWNED_BY_GENERATOR) {
        PyObject *owner = (PyObject *)ALEFF_GEN_FROM_FRAME(frame);
        return _aleff_effective_handled_exception(ALEFF_GEN_EXC_STATE(owner));
    }
    return _aleff_is_exception_instance(fallback) ? fallback : nullptr;
}

static PyObject *
_aleff_frame_generator_owner(_aleff_frame_t *frame)
{
    if (frame->owner != FRAME_OWNED_BY_GENERATOR) {
        return nullptr;
    }
    return (PyObject *)ALEFF_GEN_FROM_FRAME(frame);
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
    PyObject *handled_exception;  /* effective handled exception, or nullptr */
    PyObject *original_owner;  /* generator/coroutine owner, or nullptr */
    int send_stack_depth;  /* operand depth at an active SEND, or -1 */
    int send_target_offset;  /* bytecode offset of END_SEND, or -1 */
    int send_value_needs_pop;  /* C-level SEND still owns its sent value */
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

        ALEFF_STACKREF_RELEASE(f->f_executable);
        ALEFF_STACKREF_RELEASE(f->f_funcobj);
        /* f_globals and f_builtins are borrowed in live frames,
           but we hold strong refs in copies */
        Py_XDECREF(f->f_globals);
        Py_XDECREF(f->f_builtins);
        Py_XDECREF(f->f_locals);
        Py_XDECREF(fc->handled_exception);
        Py_XDECREF(fc->original_owner);
        /* Don't decref frame_obj — we set it to nullptr in copies */

        for (int j = 0; j < fc->num_slots; j++) {
            ALEFF_LOCALSPLUS_RELEASE(f, j);
        }
        PyMem_Free(f);
    }
    PyMem_Free(self->frames);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
FrameSnapshot_class_getitem(_ALEFF_UNUSED PyObject *cls, _ALEFF_UNUSED PyObject *args)
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
copy_single_frame(_aleff_frame_t *src, int send_stack_depth, int send_target_offset)
{
    _aleff_frame_copy_t result = {
        .frame = nullptr,
        .num_slots = 0,
        .handled_exception = nullptr,
        .original_owner = nullptr,
        .send_stack_depth = send_stack_depth,
        .send_target_offset = send_target_offset,
        .send_value_needs_pop = -1,
    };

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

#if PY_VERSION_HEX >= 0x030e0000
    /* Fix up stackpointer: it pointed into src->localsplus, now must
     * point into dst->localsplus at the same offset. */
    if (src->stackpointer != nullptr) {
        ptrdiff_t sp_offset = src->stackpointer - src->localsplus;
        dst->stackpointer = dst->localsplus + sp_offset;
    }
    /* GC traversal state belongs to the source stack walk, not the frame. */
    dst->visited = 0;
#endif

    /* Strong refs for objects */
    ALEFF_STACKREF_RETAIN(dst->f_executable);
    ALEFF_STACKREF_RETAIN(dst->f_funcobj);
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

    /* When stacktop >= 0: slots 0..stacktop-1 are valid.
     * When stacktop == -1 (active frame): only locals/cells/freevars
     * (0..co_nlocalsplus-1) are safe. The value stack portion may
     * contain stale pointers from the eval loop. */
    int stacktop = _aleff_frame_stacktop(dst);
    if (send_stack_depth >= 0) {
        int send_entry_top = code->co_nlocalsplus + send_stack_depth;
        if (send_entry_top > num_slots) {
            PyErr_SetString(PyExc_RuntimeError, "active SEND stack exceeds frame capacity");
            goto frame_header_error;
        }
        if (stacktop < 0) {
            stacktop = send_entry_top;
            _aleff_frame_set_stacktop(dst, stacktop);
            result.send_value_needs_pop = 1;
        }
        else if (stacktop == send_entry_top) {
            result.send_value_needs_pop = 1;
        }
        else if (stacktop == send_entry_top - 1) {
            result.send_value_needs_pop = 0;
        }
        else {
            PyErr_SetString(PyExc_RuntimeError, "operand stack does not match active SEND");
            goto frame_header_error;
        }
    }
    int valid_slots = stacktop >= 0
        ? stacktop
        : code->co_nlocalsplus;
    for (int i = 0; i < valid_slots; i++) {
        ALEFF_LOCALSPLUS_RETAIN(dst, i);
    }
    for (int i = valid_slots; i < num_slots; i++) {
        ALEFF_LOCALSPLUS_SET(dst, i, nullptr);
    }

    result.frame = dst;
    result.num_slots = num_slots;
    return result;

frame_header_error:
    ALEFF_STACKREF_RELEASE(dst->f_executable);
    ALEFF_STACKREF_RELEASE(dst->f_funcobj);
    Py_XDECREF(dst->f_globals);
    Py_XDECREF(dst->f_builtins);
    Py_XDECREF(dst->f_locals);
    PyMem_Free(dst);
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

    {
        PyObject *fallback_exception = _aleff_effective_handled_exception(
            PyThreadState_Get()->exc_info
        );
        PyFrameObject *f = py_frame;
        Py_INCREF(f);
        for (int i = 0; i < count; i++) {
            _aleff_frame_t *internal = _aleff_frame_from_pyframe(f);

            snapshot->frames[i] = copy_single_frame(internal, -1, -1);
            if (snapshot->frames[i].frame == nullptr) {
                snapshot->num_frames = i;
                Py_DECREF(f);
                Py_DECREF(snapshot);
                Py_DECREF(py_frame);
                return nullptr;
            }
            snapshot->frames[i].handled_exception = Py_XNewRef(
                _aleff_frame_handled_exception(internal, fallback_exception)
            );
            snapshot->frames[i].original_owner = Py_XNewRef(
                _aleff_frame_generator_owner(internal)
            );

            PyFrameObject *prev = PyFrame_GetBack(f);  /* new ref */
            Py_DECREF(f);
            f = prev;
        }
        Py_XDECREF(f);  /* may be nullptr if chain ended */
    }

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
 * Inject a resume value into a frame, simulating the completed operation
 * that dispatched into the captured Python continuation.
 *
 * Frames are normally suspended mid-CALL while invoking an effect.  CPython
 * 3.12 sets stacktop = -1 while such frames are active (the real stack
 * pointer lives in a register).  The opcode at prev_instr distinguishes the
 * CALL dispatch paths:
 *
 * - opcode == 171 (CALL): generic path. Stack has callable + args,
 *   prev_instr at the CALL instruction. Need to pop args and advance.
 *
 * - other opcode (CACHE=0, or specialized CALL variant): inline dispatch.
 *   Stack already shrunk, prev_instr past CACHE entries. Just push value.
 *
 * WITH_EXCEPT_START is a separate implicit call to __exit__.  From Python
 * 3.13 onward its result must resume at the following TO_BOOL instruction.
 */
static int
prepare_resume_frame(_aleff_frame_t *frame)
{
    PyCodeObject *code = _aleff_frame_get_code(frame);
    int value_stack_base = code->co_nlocalsplus;
    int stacktop = _aleff_frame_stacktop(frame);
    uint8_t raw_opcode = (*ALEFF_PREV_INSTR(frame)) & 0xFF;
    uint8_t base_opcode = _aleff_opcode_deopt[raw_opcode];

    /* CALL instruction size: 1 (CALL) + 3 (CACHE entries) = 4 codeunits.
     * Same in 3.12, 3.13, and 3.14. */
    #define CALL_TOTAL_SIZE 4

    if (base_opcode == CALL_OPCODE) {
        if (stacktop < 0) {
            /* Active frame (stacktop == -1): the frame is mid-CALL to a
             * C function via PyObject_Vectorcall.  The eval loop has NOT
             * shrunk the stack, so callable + self_or_null + args are still
             * on the value stack.  Pop them before pushing the resume value. */
            uint8_t oparg = (*ALEFF_PREV_INSTR(frame) >> 8) & 0xFF;
            int call_items = oparg + 2;  /* callable + self_or_null + args */
            stacktop = value_stack_base + call_items;
            int new_top = value_stack_base;
            for (int i = new_top; i < stacktop; i++) {
                ALEFF_LOCALSPLUS_CLOSE(frame, i);
                ALEFF_LOCALSPLUS_SET(frame, i, nullptr);
            }
            stacktop = new_top;
        }
        /* stacktop >= 0: the frame called a Python function via
         * CALL / CALL_PY_EXACT_ARGS / CALL_PY_GENERAL / etc.
         * The eval loop already executed STACK_SHRINK(oparg + 2) before
         * DISPATCH_INLINED, so stacktop is correct.  No cleanup needed. */

        /* Advance instruction pointer past CALL + CACHE entries.
         *
         * 3.12 (prev_instr): points to the LAST executed instruction.
         *   Eval loop resumes at prev_instr + 1, so advance by SIZE - 1.
         * 3.13+ (instr_ptr): points to the NEXT instruction to execute.
         *   Eval loop resumes at instr_ptr directly, so advance by SIZE.
         *
         * The width is hardcoded rather than read from frame->return_offset,
         * which is only meaningful while a call is in progress and is never
         * cleared afterwards.  From 3.14 the eval loop saves a stack pointer
         * even around escaping C calls, so stacktop >= 0 no longer implies
         * this frame dispatched inline, and return_offset may still hold the
         * width of some earlier call in the same frame. */
#if PY_VERSION_HEX >= 0x030d0000
        ALEFF_PREV_INSTR(frame) += CALL_TOTAL_SIZE;
#else
        ALEFF_PREV_INSTR(frame) += CALL_TOTAL_SIZE - 1;
#endif
    }
#if PY_VERSION_HEX >= 0x030d0000
    else if (base_opcode == CALL_FUNCTION_EX || base_opcode == CALL_KW) {
        /* Also calls, just not spelled CALL: f(*args) / f(**kwargs) compile to
         * CALL_FUNCTION_EX, and a keyword call compiles to CALL_KW.  A frame
         * suspended here must resume past the opcode, or it re-executes the
         * call with nothing but the injected value on the stack.
         *
         * 3.12 needs no branch of its own: prev_instr means "last executed"
         * and the eval loop resumes at prev_instr + 1, which is exactly past a
         * one-codeunit CALL_FUNCTION_EX.  From 3.13 instr_ptr means "about to
         * execute" and the resume point is instr_ptr + return_offset.
         *
         * return_offset is the width CPython itself recorded for this
         * dispatch, so it absorbs per-version cache sizes -- CALL_KW has no
         * cache entries on 3.13 and three on 3.14.  Reading it is sound here
         * precisely because the opcode identifies the frame as mid-call, so
         * the value belongs to the call being resumed.
         *
         * stacktop < 0 means the call escaped into C instead.  The C frame is
         * then missing from the captured chain entirely, which is a separate
         * unsupported case; treat it as any other escaping call. */
        if (stacktop < 0) {
            stacktop = value_stack_base;
        }
        else {
            ALEFF_PREV_INSTR(frame) += frame->return_offset;
        }
    }
    else if (base_opcode == WITH_EXCEPT_START && stacktop >= 0) {
        /* WITH_EXCEPT_START calls __exit__ without using a CALL opcode.  The
         * injected value is that call's result, so resume at the following
         * TO_BOOL instead of invoking __exit__ again with the result in the
         * exception slot. */
        ALEFF_PREV_INSTR(frame) += 1;
    }
#endif
    else {
        /* Not a call at all: the frame is suspended beneath an escaping C
         * call that dispatched back into Python -- operator, attribute or
         * subscript lookup.  That C frame is absent from the captured chain,
         * so the continuation cannot be replayed faithfully; this is a known
         * unsupported shape.  Reset an active frame's stack to base and leave
         * the instruction pointer alone. */
        if (stacktop < 0) {
            stacktop = value_stack_base;
        }
    }

    #undef CALL_TOTAL_SIZE

    _aleff_frame_set_stacktop(frame, stacktop);
    return stacktop;
}

static void
inject_resume_value(_aleff_frame_t *frame, PyObject *value)
{
    int stacktop = prepare_resume_frame(frame);

    /* Push the resume value */
    ALEFF_LOCALSPLUS_SET_NEW(frame, stacktop, value);
    stacktop++;
    _aleff_frame_set_stacktop(frame, stacktop);
}

static PyObject *
replacement_for(PyObject *obj, PyObject *replacements)
{
    if (obj == nullptr || replacements == nullptr || replacements == Py_None) {
        return obj;
    }

    PyObject *original;
    PyObject *replacement;
    Py_ssize_t pos = 0;
    while (PyDict_Next(replacements, &pos, &original, &replacement)) {
        if (obj == original) {
            return replacement;
        }
    }
    return obj;
}

static void
apply_frame_replacements(_aleff_frame_t *frame, int num_slots, PyObject *replacements)
{
    for (int i = 0; i < num_slots; i++) {
        if (!ALEFF_LOCALSPLUS_IS_OBJECT(frame, i)) {
            continue;
        }
        PyObject *obj = ALEFF_LOCALSPLUS_GET(frame, i);
        PyObject *replacement = replacement_for(obj, replacements);
        if (replacement != obj) {
            ALEFF_LOCALSPLUS_SET(frame, i, replacement);
        }
    }
}

/*
 * Copy a frame onto the thread data stack.
 * Returns a pointer to the frame on the data stack, or nullptr on error.
 * The caller must ensure there's enough space (or handle growth).
 */
static _aleff_frame_t *
push_frame_to_datastack(
    PyThreadState *tstate,
    _aleff_frame_t *src,
    int num_slots,
    PyObject *replacements
)
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

#if PY_VERSION_HEX >= 0x030e0000
    /* Fix up stackpointer after memcpy (points into src->localsplus). */
    if (src->stackpointer != nullptr) {
        ptrdiff_t sp_offset = src->stackpointer - src->localsplus;
        dst->stackpointer = dst->localsplus + sp_offset;
    }
    dst->visited = 0;
#endif

    apply_frame_replacements(dst, num_slots, replacements);

    /* Give the restored frame independent interpreter-owned references. */
    ALEFF_STACKREF_DUP(dst->f_executable);
    ALEFF_STACKREF_DUP(dst->f_funcobj);
    Py_XINCREF(dst->f_globals);
    Py_XINCREF(dst->f_builtins);
    Py_XINCREF(dst->f_locals);
    dst->frame_obj = nullptr;
    dst->owner = FRAME_OWNED_BY_THREAD;

    /* Source is from a snapshot where stale slots are already nullified.
     * Normalize object entries to independently owned stackrefs. */
    for (int i = 0; i < num_slots; i++) {
        ALEFF_LOCALSPLUS_DUP(dst, i);
    }

    return dst;
}

/* Build a real generator-family owner around a copied suspended frame.
 *
 * The public constructors accept a PyFrameObject.  Create a correctly-sized
 * frame object, replace its freshly initialized interpreter frame with the
 * snapshot state, then let the matching constructor move that state into
 * generator-owned storage.
 */
static PyObject *
generator_owner_from_frame_copy(
    PyThreadState *tstate,
    _aleff_frame_copy_t *src_copy,
    int resume_from_coroutine,
    PyObject *handled_exception,
    PyObject *replacements
)
{
    _aleff_frame_t *src = src_copy->frame;
    PyCodeObject *code = _aleff_frame_get_code(src);
    PyFrameObject *py_frame = PyFrame_New(tstate, code, src->f_globals, src->f_locals);
    if (py_frame == nullptr) {
        return nullptr;
    }

    _aleff_frame_t *dst = _aleff_frame_from_pyframe(py_frame);

    /* Release the empty frame state allocated by PyFrame_New().  Globals and
     * builtins are borrowed by live frames and therefore are not decref'd. */
    PyCodeObject *new_code = _aleff_frame_get_code(dst);
    int initialized_slots = _aleff_frame_stacktop(dst);
    if (initialized_slots < 0) {
        initialized_slots = new_code->co_nlocalsplus;
    }
    ALEFF_STACKREF_CLOSE(dst->f_executable);
    ALEFF_STACKREF_CLOSE(dst->f_funcobj);
    Py_XDECREF(dst->f_locals);
    for (int i = 0; i < initialized_slots; i++) {
        ALEFF_LOCALSPLUS_CLOSE(dst, i);
    }

    size_t frame_size = sizeof(_aleff_frame_t)
                      + (src_copy->num_slots - 1) * sizeof(PyObject *);
    memcpy(dst, src, frame_size);

#if PY_VERSION_HEX >= 0x030e0000
    if (src->stackpointer != nullptr) {
        ptrdiff_t sp_offset = src->stackpointer - src->localsplus;
        dst->stackpointer = dst->localsplus + sp_offset;
    }
    dst->visited = 0;
#endif

    apply_frame_replacements(dst, src_copy->num_slots, replacements);

    /* Match normal interpreter-frame ownership: executable, function,
     * locals, and localsplus are independently owned; globals and builtins
     * are borrowed. */
    ALEFF_STACKREF_DUP(dst->f_executable);
    ALEFF_STACKREF_DUP(dst->f_funcobj);
    Py_XINCREF(dst->f_locals);
    dst->frame_obj = nullptr;
    dst->previous = nullptr;
    dst->owner = FRAME_OWNED_BY_FRAME_OBJECT;
    for (int i = 0; i < src_copy->num_slots; i++) {
        ALEFF_LOCALSPLUS_DUP(dst, i);
    }

    /* coro.send(outcome) supplies the value that resumes this frame.  A value
     * returned by an inlined child coroutine must resume after SEND, exactly
     * as CPython's RETURN_VALUE path does via the caller's return_offset. */
    if (resume_from_coroutine) {
        if (src_copy->send_target_offset >= 0) {
            int stacktop = _aleff_frame_stacktop(dst);
            int minimum_top = code->co_nlocalsplus + 1 + src_copy->send_value_needs_pop;
            if (stacktop < minimum_top) {
                PyErr_SetString(PyExc_RuntimeError, "invalid operand stack for SEND completion");
                Py_DECREF(py_frame);
                return nullptr;
            }
            if (src_copy->send_value_needs_pop) {
                stacktop--;
                ALEFF_LOCALSPLUS_CLOSE(dst, stacktop);
                ALEFF_LOCALSPLUS_SET(dst, stacktop, nullptr);
                _aleff_frame_set_stacktop(dst, stacktop);
            }
#if PY_VERSION_HEX >= 0x030d0000
            ALEFF_PREV_INSTR(dst) = ALEFF_CODE_CODE(code) + src_copy->send_target_offset / 2;
#else
            ALEFF_PREV_INSTR(dst) = ALEFF_CODE_CODE(code) + src_copy->send_target_offset / 2 - 1;
#endif
        }
        else {
            ALEFF_PREV_INSTR(dst) += dst->return_offset;
        }
    }
    else {
        prepare_resume_frame(dst);
    }

    PyObject *owner;
    if (code->co_flags & CO_ASYNC_GENERATOR) {
        owner = PyAsyncGen_New(py_frame, code->co_name, code->co_qualname);
    }
    else if (code->co_flags & CO_COROUTINE) {
        owner = PyCoro_New(py_frame, code->co_name, code->co_qualname);
    }
    else {
        owner = PyGen_NewWithQualName(py_frame, code->co_name, code->co_qualname);
    }
    if (owner == nullptr) {
        return nullptr;
    }

#if PY_VERSION_HEX >= 0x030d0000
    ALEFF_SET_GEN_FRAME_STATE(owner, -2);  /* FRAME_SUSPENDED */
#else
    ALEFF_SET_GEN_FRAME_STATE(owner, -1);  /* FRAME_SUSPENDED */
#endif
    _PyErr_StackItem *exc_state = ALEFF_GEN_EXC_STATE(owner);
    Py_XSETREF(
        exc_state->exc_value,
        _aleff_is_exception_instance(handled_exception)
            ? Py_NewRef(handled_exception)
            : nullptr
    );
    exc_state->previous_item = nullptr;
    if (src_copy->original_owner != nullptr && replacements != nullptr) {
        if (PyDict_SetItem(replacements, src_copy->original_owner, owner) < 0) {
            Py_DECREF(owner);
            return nullptr;
        }
    }
    return owner;
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
_aleff_snapshot_frames(_ALEFF_UNUSED PyObject *self, PyObject *args)
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
_aleff_restore_continuation(_ALEFF_UNUSED PyObject *self, PyObject *args)
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
    for (int i = skip; i < snapshot->num_frames; i++) {
        PyCodeObject *code = _aleff_frame_get_code(snapshot->frames[i].frame);
        if (code->co_flags & CO_GENERATOR) {
            PyErr_SetString(
                PyExc_RuntimeError,
                "synchronous generator frames are not supported by multi-shot restoration"
            );
            return nullptr;
        }
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
        _aleff_frame_t *f = push_frame_to_datastack(
            tstate,
            src->frame,
            src->num_slots,
            Py_None
        );
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
    _PyErr_StackItem restored_exc_state = {
        .exc_value = Py_XNewRef(snapshot->frames[skip].handled_exception),
        .previous_item = tstate->exc_info,
    };
    tstate->exc_info = &restored_exc_state;

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

    tstate->exc_info = restored_exc_state.previous_item;
    restored_exc_state.previous_item = nullptr;
    Py_XDECREF(restored_exc_state.exc_value);

    /* Restore the data stack top. */
    tstate->datastack_top = saved_datastack_top;

    return result;
}

static PyObject *
make_async_restore_stage(
    int done,
    PyObject *payload,
    int next_frame,
    PyObject *initial,
    int is_exception
)
{
    PyObject *stage = PyTuple_New(5);
    if (stage == nullptr) {
        Py_DECREF(payload);
        Py_DECREF(initial);
        return nullptr;
    }

    PyTuple_SET_ITEM(stage, 0, PyBool_FromLong(done));
    PyTuple_SET_ITEM(stage, 1, payload);
    PyTuple_SET_ITEM(stage, 2, PyLong_FromLong(next_frame));
    PyTuple_SET_ITEM(stage, 3, initial);
    PyTuple_SET_ITEM(stage, 4, PyBool_FromLong(is_exception));

    if (PyErr_Occurred()) {
        Py_DECREF(stage);
        return nullptr;
    }
    return stage;
}

PyDoc_STRVAR(restore_async_continuation_doc,
"restore_async_continuation(\n"
"    snapshot, outcome, start=1, is_exception=False, from_coroutine=False, replacements=None\n"
")\n"
"--\n\n"
"Advance a restored frame chain until its next coroutine frame or completion.\n"
"\n"
"Coroutine frames are returned as independently-owned coroutine objects so\n"
"their yield/return/raise protocol remains managed by CPython.  The returned\n"
"tuple is (done, payload, next_frame, initial, is_exception).\n");

static PyObject *
_aleff_restore_async_continuation(_ALEFF_UNUSED PyObject *self, PyObject *args)
{
    FrameSnapshotObject *snapshot;
    PyObject *outcome;
    int start = 1;
    int is_exception = 0;
    int from_coroutine = 0;
    PyObject *replacements = Py_None;

    if (!PyArg_ParseTuple(
            args,
            "O!O|ippO",
            &FrameSnapshotType,
            &snapshot,
            &outcome,
            &start,
            &is_exception,
            &from_coroutine,
            &replacements
        )) {
        return nullptr;
    }

    if (_evalframe == nullptr) {
        PyErr_SetString(PyExc_RuntimeError,
            "_PyEval_EvalFrameDefault not available (dlsym failed at init)");
        return nullptr;
    }
    if (start < 0 || start > snapshot->num_frames) {
        PyErr_SetString(PyExc_ValueError, "invalid async continuation frame index");
        return nullptr;
    }
    if (replacements != Py_None && !PyDict_Check(replacements)) {
        PyErr_SetString(PyExc_TypeError, "replacements must be a dict or None");
        return nullptr;
    }

    PyObject *pending = Py_NewRef(outcome);
    int pending_is_exception = is_exception;
    PyThreadState *tstate = PyThreadState_Get();
    _PyErr_StackItem restored_exc_state = {
        .exc_value = start < snapshot->num_frames
            ? Py_XNewRef(snapshot->frames[start].handled_exception)
            : nullptr,
        .previous_item = tstate->exc_info,
    };
    tstate->exc_info = &restored_exc_state;
    PyObject *return_value = nullptr;

    for (int i = start; i < snapshot->num_frames; i++) {
        _aleff_frame_copy_t *src = &snapshot->frames[i];
        PyCodeObject *code = _aleff_frame_get_code(src->frame);

        if (code->co_flags & (CO_COROUTINE | CO_GENERATOR | CO_ASYNC_GENERATOR)) {
            PyObject *owner = generator_owner_from_frame_copy(
                tstate,
                src,
                from_coroutine && !pending_is_exception,
                restored_exc_state.exc_value,
                replacements
            );
            if (owner == nullptr) {
                Py_DECREF(pending);
                goto done;
            }

            if (code->co_flags & CO_ASYNC_GENERATOR) {
                PyObject *operation = PyObject_CallMethod(
                    owner,
                    pending_is_exception ? "athrow" : "asend",
                    "O",
                    pending
                );
                Py_DECREF(owner);
                Py_DECREF(pending);
                if (operation == nullptr) {
                    goto done;
                }
                return_value = make_async_restore_stage(
                    0,
                    operation,
                    i + 1,
                    Py_NewRef(Py_None),
                    0
                );
                goto done;
            }

            return_value = make_async_restore_stage(
                0,
                owner,
                i + 1,
                pending,
                pending_is_exception
            );
            goto done;
        }

        PyObject **saved_datastack_top = tstate->datastack_top;
        _aleff_frame_t *frame = push_frame_to_datastack(
            tstate,
            src->frame,
            src->num_slots,
            replacements
        );
        if (frame == nullptr) {
            tstate->datastack_top = saved_datastack_top;
            Py_DECREF(pending);
            goto done;
        }
        frame->previous = nullptr;

        PyObject *result;
        if (pending_is_exception) {
            prepare_resume_frame(frame);
            PyErr_SetRaisedException(pending);  /* steals pending */
            pending = nullptr;
            result = _evalframe(tstate, frame, 1);
        }
        else {
            inject_resume_value(frame, pending);
            Py_DECREF(pending);
            pending = nullptr;
            result = _evalframe(tstate, frame, 0);
        }
        tstate->datastack_top = saved_datastack_top;

        if (result == nullptr) {
            if (i + 1 >= snapshot->num_frames) {
                goto done;
            }
            pending = PyErr_GetRaisedException();
            if (pending == nullptr) {
                PyErr_SetString(PyExc_RuntimeError,
                    "restored frame failed without an active exception");
                goto done;
            }
            pending_is_exception = 1;
        }
        else {
            pending = result;
            pending_is_exception = 0;
        }
        from_coroutine = 0;
    }

    if (pending_is_exception) {
        PyErr_SetRaisedException(pending);  /* steals pending */
        goto done;
    }
    return_value = make_async_restore_stage(
        1,
        pending,
        snapshot->num_frames,
        Py_NewRef(Py_None),
        0
    );

done:
    tstate->exc_info = restored_exc_state.previous_item;
    restored_exc_state.previous_item = nullptr;
    Py_XDECREF(restored_exc_state.exc_value);
    return return_value;
}

PyDoc_STRVAR(snapshot_from_frame_doc,
"snapshot_from_frame(frame, depth=-1, handled_exception=None)\n"
"--\n\n"
"Capture a frame chain starting from the given frame object.\n"
"The frame should be from a suspended greenlet (gr_frame) so that\n"
"stacktop values are valid.\n"
"\n"
"Parameters:\n"
"  frame: A frame object (e.g. greenlet.gr_frame).\n"
"  depth: Maximum number of frames to capture. -1 for all.\n"
"  handled_exception: Active exception from the suspended caller, if any.\n");

static int
_aleff_read_exception_varint(
    const unsigned char **cursor,
    const unsigned char *end,
    int *value
)
{
    if (*cursor >= end) {
        PyErr_SetString(PyExc_RuntimeError, "truncated exception table");
        return -1;
    }
    unsigned int result = *(*cursor)++ & 63U;
    while ((*cursor)[-1] & 64U) {
        if (*cursor >= end || result > ((unsigned int)INT_MAX >> 6)) {
            PyErr_SetString(PyExc_RuntimeError, "invalid exception table varint");
            return -1;
        }
        result = (result << 6) | (*(*cursor)++ & 63U);
    }
    *value = (int)result;
    return 0;
}

typedef struct {
    int start;
    int end;
    int target;
    int depth;
    int lasti;
} _aleff_exception_entry_t;

static int
_aleff_record_stack_depth(
    int offset,
    int depth,
    int code_size,
    int stack_size,
    int *depths,
    int *queue,
    int *queue_end
)
{
    if (
        offset < 0 || offset >= code_size || offset % 2 != 0 ||
        depth < 0 || depth > stack_size
    ) {
        PyErr_SetString(PyExc_RuntimeError, "invalid bytecode stack-depth edge");
        return -1;
    }
    int index = offset / 2;
    if (depths[index] == INT_MIN) {
        depths[index] = depth;
        queue[(*queue_end)++] = offset;
    }
    else if (depths[index] != depth) {
        PyErr_SetString(PyExc_RuntimeError, "inconsistent bytecode stack depth");
        return -1;
    }
    return 0;
}

static int
_aleff_opcode_is_jump(int opcode)
{
    switch (opcode) {
        case FOR_ITER:
        case JUMP_FORWARD:
        case POP_JUMP_IF_FALSE:
        case POP_JUMP_IF_TRUE:
        case POP_JUMP_IF_NOT_NONE:
        case POP_JUMP_IF_NONE:
        case JUMP_BACKWARD_NO_INTERRUPT:
        case JUMP_BACKWARD:
        case SEND:
            return 1;
        default:
            return 0;
    }
}

static int
_aleff_opcode_is_backward_jump(int opcode)
{
    return opcode == JUMP_BACKWARD || opcode == JUMP_BACKWARD_NO_INTERRUPT;
}

static int
_aleff_opcode_is_unconditional_jump(int opcode)
{
    return opcode == JUMP_FORWARD || _aleff_opcode_is_backward_jump(opcode);
}

static int
_aleff_opcode_is_terminal(int opcode)
{
    switch (opcode) {
        case RETURN_VALUE:
#ifdef RETURN_CONST
        case RETURN_CONST:
#endif
        case RAISE_VARARGS:
        case RERAISE:
            return 1;
        default:
            return 0;
    }
}

static int
_aleff_instruction_arg(
    const unsigned char *bytecode,
    int offset,
    uint32_t *oparg
)
{
    uint32_t result = bytecode[offset + 1];
    int shift = 8;
    for (int previous = offset - 2; previous >= 0 && bytecode[previous] == EXTENDED_ARG;
         previous -= 2) {
        if (shift >= 32) {
            PyErr_SetString(PyExc_RuntimeError, "bytecode argument is too large");
            return -1;
        }
        result |= (uint32_t)bytecode[previous + 1] << shift;
        shift += 8;
    }
    *oparg = result;
    return 0;
}

static int
load_send_metadata(
    PyFrameObject *frame,
    int *stack_depth,
    int *target_offset
)
{
    int result = -1;
    PyCodeObject *code = nullptr;
    PyObject *code_bytes = nullptr;
    int *depths = nullptr;
    int *queue = nullptr;
    _aleff_exception_entry_t *exception_entries = nullptr;
    int code_size;
    int instruction_count;
    int exception_count = 0;

    *stack_depth = -1;
    *target_offset = -1;
    code = (PyCodeObject *)PyFrame_GetCode(frame);
    if (code == nullptr) {
        goto done;
    }
    code_bytes = PyCode_GetCode(code);
    if (code_bytes == nullptr) {
        goto done;
    }
    Py_ssize_t byte_count = PyBytes_GET_SIZE(code_bytes);
    if (byte_count <= 0 || byte_count > INT_MAX || byte_count % 2 != 0) {
        PyErr_SetString(PyExc_RuntimeError, "invalid frame bytecode size");
        goto done;
    }
    code_size = (int)byte_count;
    const unsigned char *bytecode = (const unsigned char *)PyBytes_AS_STRING(code_bytes);
    int send_offset = PyFrame_GetLasti(frame);
    if (
        send_offset < 0 || send_offset >= code_size || send_offset % 2 != 0 ||
        bytecode[send_offset] != SEND
    ) {
        result = 0;
        goto done;
    }

    uint32_t send_arg;
    if (_aleff_instruction_arg(bytecode, send_offset, &send_arg) < 0) {
        goto done;
    }
    int send_next = send_offset + 2;
    while (send_next < code_size && bytecode[send_next] == CACHE) {
        send_next += 2;
    }
    if (
        send_arg > (uint32_t)((code_size - send_next) / 2) ||
        send_next + (int)(send_arg * 2U) >= code_size
    ) {
        PyErr_SetString(PyExc_RuntimeError, "SEND jump exceeds frame bytecode");
        goto done;
    }
    int send_target = send_next + (int)(send_arg * 2U);
    if (bytecode[send_target] != END_SEND) {
        PyErr_SetString(PyExc_RuntimeError, "SEND jump does not target END_SEND");
        goto done;
    }

    instruction_count = code_size / 2;
    depths = PyMem_Malloc((size_t)instruction_count * sizeof(*depths));
    queue = PyMem_Malloc((size_t)instruction_count * sizeof(*queue));
    if (depths == nullptr || queue == nullptr) {
        PyErr_NoMemory();
        goto done;
    }
    for (int i = 0; i < instruction_count; i++) {
        depths[i] = INT_MIN;
    }

    PyObject *exception_table = code->co_exceptiontable;
    if (!PyBytes_Check(exception_table)) {
        PyErr_SetString(PyExc_RuntimeError, "invalid code exception table");
        goto done;
    }
    Py_ssize_t exception_size = PyBytes_GET_SIZE(exception_table);
    if (exception_size > INT_MAX) {
        PyErr_SetString(PyExc_RuntimeError, "exception table is too large");
        goto done;
    }
    if (exception_size > 0) {
        Py_ssize_t maximum_entries = exception_size / 4 + 1;
        exception_entries = PyMem_Malloc(
            (size_t)maximum_entries * sizeof(*exception_entries)
        );
        if (exception_entries == nullptr) {
            PyErr_NoMemory();
            goto done;
        }
        const unsigned char *cursor =
            (const unsigned char *)PyBytes_AS_STRING(exception_table);
        const unsigned char *exception_end = cursor + exception_size;
        while (cursor < exception_end) {
            int start_units;
            int length_units;
            int target_units;
            int depth_lasti;
            if (
                _aleff_read_exception_varint(&cursor, exception_end, &start_units) < 0 ||
                _aleff_read_exception_varint(&cursor, exception_end, &length_units) < 0 ||
                _aleff_read_exception_varint(&cursor, exception_end, &target_units) < 0 ||
                _aleff_read_exception_varint(&cursor, exception_end, &depth_lasti) < 0
            ) {
                goto done;
            }
            if (
                start_units > INT_MAX / 2 ||
                length_units > INT_MAX / 2 - start_units ||
                target_units > INT_MAX / 2
            ) {
                PyErr_SetString(PyExc_RuntimeError, "exception table entry is too large");
                goto done;
            }
            _aleff_exception_entry_t *entry = &exception_entries[exception_count++];
            entry->start = start_units * 2;
            entry->end = entry->start + length_units * 2;
            entry->target = target_units * 2;
            entry->depth = depth_lasti >> 1;
            entry->lasti = depth_lasti & 1;
            if (
                entry->end < entry->start || entry->end > code_size ||
                entry->target < 0 || entry->target >= code_size ||
                entry->depth < 0 ||
                entry->depth > code->co_stacksize - 1 - entry->lasti
            ) {
                PyErr_SetString(PyExc_RuntimeError, "invalid exception table entry");
                goto done;
            }
        }
    }

    int entry_offset = -1;
    for (int offset = 0; offset < code_size; offset += 2) {
        if (bytecode[offset] == RESUME && bytecode[offset + 1] == 0) {
            entry_offset = offset;
            break;
        }
    }
    if (entry_offset < 0) {
        PyErr_SetString(PyExc_RuntimeError, "frame bytecode has no entry RESUME");
        goto done;
    }

    int queue_start = 0;
    int queue_end = 0;
    if (
        _aleff_record_stack_depth(
            entry_offset,
            0,
            code_size,
            code->co_stacksize,
            depths,
            queue,
            &queue_end
        ) < 0
    ) {
        goto done;
    }
    while (queue_start < queue_end) {
        int offset = queue[queue_start++];
        int depth = depths[offset / 2];
        int opcode = bytecode[offset];
        uint32_t unsigned_arg;
        if (_aleff_instruction_arg(bytecode, offset, &unsigned_arg) < 0) {
            goto done;
        }
        if (unsigned_arg > INT_MAX) {
            PyErr_SetString(PyExc_RuntimeError, "bytecode argument exceeds stack-effect API");
            goto done;
        }
        int oparg = (int)unsigned_arg;
        int next_offset = offset + 2;
        while (next_offset < code_size && bytecode[next_offset] == CACHE) {
            next_offset += 2;
        }
        int is_jump = _aleff_opcode_is_jump(opcode);
        if (is_jump) {
            int jump_effect = PyCompile_OpcodeStackEffectWithJump(opcode, oparg, 1);
            if (jump_effect == PY_INVALID_STACK_EFFECT) {
                PyErr_SetString(PyExc_RuntimeError, "invalid jump opcode stack effect");
                goto done;
            }
            int64_t jump_distance = (int64_t)oparg * 2;
            int64_t jump_target_wide = _aleff_opcode_is_backward_jump(opcode)
                ? (int64_t)next_offset - jump_distance
                : (int64_t)next_offset + jump_distance;
            int64_t jump_depth_wide = (int64_t)depth + jump_effect;
            if (
                jump_target_wide < INT_MIN || jump_target_wide > INT_MAX ||
                jump_depth_wide < INT_MIN || jump_depth_wide > INT_MAX
            ) {
                PyErr_SetString(PyExc_RuntimeError, "jump bytecode edge overflows");
                goto done;
            }
            if (
                _aleff_record_stack_depth(
                    (int)jump_target_wide,
                    (int)jump_depth_wide,
                    code_size,
                    code->co_stacksize,
                    depths,
                    queue,
                    &queue_end
                ) < 0
            ) {
                goto done;
            }
        }
        if (!_aleff_opcode_is_terminal(opcode) && !_aleff_opcode_is_unconditional_jump(opcode)) {
            int fallthrough_effect = PyCompile_OpcodeStackEffectWithJump(
                opcode,
                oparg,
                is_jump ? 0 : -1
            );
            if (fallthrough_effect == PY_INVALID_STACK_EFFECT) {
                PyErr_SetString(PyExc_RuntimeError, "invalid opcode stack effect");
                goto done;
            }
            int64_t fallthrough_depth_wide = (int64_t)depth + fallthrough_effect;
            if (fallthrough_depth_wide < INT_MIN || fallthrough_depth_wide > INT_MAX) {
                PyErr_SetString(PyExc_RuntimeError, "fallthrough bytecode edge overflows");
                goto done;
            }
            if (
                next_offset < code_size &&
                _aleff_record_stack_depth(
                    next_offset,
                    (int)fallthrough_depth_wide,
                    code_size,
                    code->co_stacksize,
                    depths,
                    queue,
                    &queue_end
                ) < 0
            ) {
                goto done;
            }
        }
        for (int i = 0; i < exception_count; i++) {
            _aleff_exception_entry_t *entry = &exception_entries[i];
            if (entry->start <= offset && offset < entry->end) {
                if (
                    _aleff_record_stack_depth(
                        entry->target,
                        entry->depth + 1 + entry->lasti,
                        code_size,
                        code->co_stacksize,
                        depths,
                        queue,
                        &queue_end
                    ) < 0
                ) {
                    goto done;
                }
            }
        }
    }

    int depth = depths[send_offset / 2];
    if (depth < 2 || depth > code->co_stacksize) {
        PyErr_SetString(PyExc_RuntimeError, "cannot determine operand stack depth at SEND");
        goto done;
    }
    *stack_depth = depth;
    *target_offset = send_target;
    result = 0;

done:
    PyMem_Free(exception_entries);
    PyMem_Free(queue);
    PyMem_Free(depths);
    Py_XDECREF(code_bytes);
    Py_XDECREF(code);
    return result;
}

static PyObject *
_aleff_snapshot_from_frame(_ALEFF_UNUSED PyObject *self, PyObject *args)
{
    PyFrameObject *start_frame;
    int depth = -1;
    PyObject *fallback_exception = Py_None;
    if (!PyArg_ParseTuple(
            args,
            "O!|iO",
            &PyFrame_Type,
            &start_frame,
            &depth,
            &fallback_exception
        ))
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

    {
        PyFrameObject *f = start_frame;
        Py_INCREF(f);
        for (int i = 0; i < count; i++) {
            _aleff_frame_t *internal = _aleff_frame_from_pyframe(f);
            int send_stack_depth;
            int send_target_offset;
            if (load_send_metadata(f, &send_stack_depth, &send_target_offset) < 0) {
                snapshot->num_frames = i;
                Py_DECREF(f);
                Py_DECREF(snapshot);
                return nullptr;
            }

            snapshot->frames[i] = copy_single_frame(
                internal,
                send_stack_depth,
                send_target_offset
            );
            if (snapshot->frames[i].frame == nullptr) {
                snapshot->num_frames = i;
                Py_DECREF(f);
                Py_DECREF(snapshot);
                return nullptr;
            }
            snapshot->frames[i].handled_exception = Py_XNewRef(
                _aleff_frame_handled_exception(internal, fallback_exception)
            );
            snapshot->frames[i].original_owner = Py_XNewRef(
                _aleff_frame_generator_owner(internal)
            );

            PyFrameObject *prev = PyFrame_GetBack(f);
            Py_DECREF(f);
            f = prev;
        }
        Py_XDECREF(f);
    }

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
_aleff_snapshot_num_frames(_ALEFF_UNUSED PyObject *self, PyObject *arg)
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
_aleff_debug_frame_stacktop(_ALEFF_UNUSED PyObject *self, PyObject *arg)
{
    if (!PyFrame_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected a frame object");
        return nullptr;
    }
    _aleff_frame_t *iframe = _aleff_frame_from_pyframe((PyFrameObject *)arg);
    return PyLong_FromLong(_aleff_frame_stacktop(iframe));
}

/*
 * Test-only native boundary for X(func). It intentionally owns no state
 * across the callback beyond the argument borrowed from its Python caller.
 * Third-party functions with owned references or resources remain outside
 * X's safety guarantees.
 */
static PyObject *
_aleff_test_native_call(_ALEFF_UNUSED PyObject *self, PyObject *callback)
{
    if (!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "callback must be callable");
        return nullptr;
    }
    return PyObject_CallNoArgs(callback);
}

/* ========================================================================
 * Module definition
 * ======================================================================== */

static PyMethodDef _aleff_methods[] = {
    {"snapshot_frames", _aleff_snapshot_frames, METH_VARARGS, snapshot_frames_doc},
    {"snapshot_from_frame", _aleff_snapshot_from_frame, METH_VARARGS, snapshot_from_frame_doc},
    {"snapshot_num_frames", _aleff_snapshot_num_frames, METH_O, snapshot_num_frames_doc},
    {"restore_continuation", _aleff_restore_continuation, METH_VARARGS, restore_continuation_doc},
    {
        "restore_async_continuation",
        _aleff_restore_async_continuation,
        METH_VARARGS,
        restore_async_continuation_doc
    },
    {"_debug_frame_stacktop", _aleff_debug_frame_stacktop, METH_O, debug_frame_stacktop_doc},
    {"_test_native_call", _aleff_test_native_call, METH_O, nullptr},
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
    _aleff_init_opcode_deopt();

    /* Look up _PyEval_EvalFrameDefault.
     * Not fatal if not found — restore_continuation will raise at call time. */
#ifdef _WIN32
    {
        char dllname[32];
#ifdef Py_GIL_DISABLED
        snprintf(dllname, sizeof(dllname), "python%d%dt.dll",
                 PY_MAJOR_VERSION, PY_MINOR_VERSION);
#else
        snprintf(dllname, sizeof(dllname), "python%d%d.dll",
                 PY_MAJOR_VERSION, PY_MINOR_VERSION);
#endif
        HMODULE hmod = GetModuleHandleA(dllname);
        if (hmod) {
            _evalframe = (evalframe_fn_t)(void *)GetProcAddress(
                hmod, "_PyEval_EvalFrameDefault");
        }
    }
#else
    /* POSIX guarantees dlsym returns a valid function pointer via void*,
     * but ISO C forbids the cast. Suppress the warning here. */
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
    _evalframe = (evalframe_fn_t)dlsym(RTLD_DEFAULT, "_PyEval_EvalFrameDefault");
#  pragma GCC diagnostic pop
#endif

    if (PyType_Ready(&FrameSnapshotType) < 0)
        return nullptr;

    PyObject *m = PyModule_Create(&_aleff_module);
    if (m == nullptr)
        return nullptr;

#ifdef Py_GIL_DISABLED
    PyUnstable_Module_SetGIL(m, Py_MOD_GIL_NOT_USED);
#endif

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
