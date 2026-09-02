#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>

#include "internal.h"
#include "unsafe.h"

#if defined(__linux__) && defined(__x86_64__) && \
    !defined(Py_GIL_DISABLED) && PY_VERSION_HEX >= 0x030c0000 && \
    PY_VERSION_HEX < 0x030d0000
#  define ALEFF_UNSAFE_SUPPORTED 1
#else
#  define ALEFF_UNSAFE_SUPPORTED 0
#endif

#if ALEFF_UNSAFE_SUPPORTED

#define ALEFF_UNSAFE_ALT_STACK_SIZE (1024U * 1024U)
#define ALEFF_UNSAFE_MAX_SNAPSHOT_SIZE (8U * 1024U * 1024U)

typedef struct {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rsp;
    uint64_t rip;
    uint32_t mxcsr;
    uint16_t x87_control;
    uint16_t padding;
} AleffUnsafeContext;

_Static_assert(offsetof(AleffUnsafeContext, rsp) == 48, "context rsp offset");
_Static_assert(offsetof(AleffUnsafeContext, rip) == 56, "context rip offset");
_Static_assert(offsetof(AleffUnsafeContext, mxcsr) == 64, "context mxcsr offset");
_Static_assert(offsetof(AleffUnsafeContext, x87_control) == 68, "context x87 offset");

int aleff_unsafe_context_save(AleffUnsafeContext *context);
[[noreturn]] void aleff_unsafe_context_restore(const AleffUnsafeContext *context);
[[noreturn]] void aleff_unsafe_run_on_stack(
    void *stack_top,
    void (*function)(void *),
    void *argument
);

typedef struct AleffUnsafeCall AleffUnsafeCall;
typedef struct AleffUnsafeSnapshot AleffUnsafeSnapshot;

typedef enum {
    ALEFF_UNSAFE_SOURCE_LIVE = 1,
    ALEFF_UNSAFE_SOURCE_SNAPSHOT = 2,
} AleffUnsafeSourceKind;

typedef struct {
    AleffUnsafeSourceKind kind;
    AleffUnsafeCall *call;
} AleffUnsafeSource;

struct AleffUnsafeSnapshot {
    AleffUnsafeSource source;
    AleffUnsafeContext checkpoint;
    unsigned char *native_stack;
    size_t native_stack_size;
    uintptr_t native_stack_start;
    uintptr_t native_stack_end;

    AleffUnsafeContext return_context;
    unsigned char *return_stack;
    size_t return_stack_size;
    uintptr_t return_stack_start;
    unsigned char *alternate_stack;

    PyObject *completed_result;
    PyObject *completed_exception;
};

struct AleffUnsafeCall {
    _Atomic unsigned int references;
    PyThreadState *owner_thread;
    PyInterpreterState *owner_interpreter;
    _PyFrameEvalFunction previous_eval;
    AleffUnsafeContext checkpoint;
    uintptr_t boundary_top;
    int checkpoint_ready;
    int resuming;
    PyObject *resume_value;
    PyObject *resume_exception;
    AleffUnsafeSnapshot *active_snapshot;
};

#if defined(_MSC_VER)
#  define ALEFF_THREAD_LOCAL __declspec(thread)
#else
#  define ALEFF_THREAD_LOCAL _Thread_local
#endif

static ALEFF_THREAD_LOCAL AleffUnsafeCall *active_call = NULL;

static void
unsafe_zero(void *memory, size_t size)
{
    volatile unsigned char *bytes = memory;
    while (size > 0) {
        *bytes++ = 0;
        size--;
    }
}

static void
unsafe_call_retain(AleffUnsafeCall *call)
{
    atomic_fetch_add_explicit(&call->references, 1, memory_order_relaxed);
}

static void
unsafe_call_release(AleffUnsafeCall *call)
{
    if (atomic_fetch_sub_explicit(&call->references, 1, memory_order_acq_rel) != 1) {
        return;
    }
    Py_XDECREF(call->resume_value);
    Py_XDECREF(call->resume_exception);
    unsafe_zero(call, sizeof(*call));
    PyMem_Free(call);
}

static int
unsafe_stack_range(uintptr_t start, uintptr_t end, size_t *size)
{
    if (start >= end || end - start > ALEFF_UNSAFE_MAX_SNAPSHOT_SIZE) {
        PyErr_SetString(PyExc_RuntimeError, "aleffy encountered an invalid native stack range");
        return -1;
    }
    *size = (size_t)(end - start);
    return 0;
}

static void *
unsafe_copy_state(const void *state)
{
    const AleffUnsafeSource *source = state;
    AleffUnsafeCall *call = source->call;
    AleffUnsafeSnapshot *copy = PyMem_Calloc(1, sizeof(*copy));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    copy->source.kind = ALEFF_UNSAFE_SOURCE_SNAPSHOT;
    copy->source.call = call;
    unsafe_call_retain(call);

    if (source->kind == ALEFF_UNSAFE_SOURCE_LIVE) {
        if (!call->checkpoint_ready) {
            PyErr_SetString(PyExc_RuntimeError, "aleffy could not locate the C-to-Python callback boundary");
            goto error;
        }
        copy->checkpoint = call->checkpoint;
        copy->native_stack_start = (uintptr_t)call->checkpoint.rsp;
        copy->native_stack_end = call->boundary_top;
        if (unsafe_stack_range(
                copy->native_stack_start,
                copy->native_stack_end,
                &copy->native_stack_size
            ) < 0) {
            goto error;
        }
        copy->native_stack = PyMem_Malloc(copy->native_stack_size);
        if (copy->native_stack == NULL) {
            PyErr_NoMemory();
            goto error;
        }
        memcpy(
            copy->native_stack,
            (const void *)copy->native_stack_start,
            copy->native_stack_size
        );
    }
    else {
        const AleffUnsafeSnapshot *snapshot = (const AleffUnsafeSnapshot *)source;
        copy->checkpoint = snapshot->checkpoint;
        copy->native_stack_start = snapshot->native_stack_start;
        copy->native_stack_end = snapshot->native_stack_end;
        copy->native_stack_size = snapshot->native_stack_size;
        copy->native_stack = PyMem_Malloc(copy->native_stack_size);
        if (copy->native_stack == NULL) {
            PyErr_NoMemory();
            goto error;
        }
        memcpy(copy->native_stack, snapshot->native_stack, copy->native_stack_size);
    }
    return copy;

error:
    unsafe_call_release(call);
    PyMem_Free(copy->native_stack);
    PyMem_Free(copy);
    return NULL;
}

static void
unsafe_free_state(void *state)
{
    AleffUnsafeSnapshot *snapshot = state;
    if (snapshot == NULL) {
        return;
    }
    if (snapshot->native_stack != NULL) {
        unsafe_zero(snapshot->native_stack, snapshot->native_stack_size);
    }
    if (snapshot->return_stack != NULL) {
        unsafe_zero(snapshot->return_stack, snapshot->return_stack_size);
    }
    PyMem_Free(snapshot->native_stack);
    PyMem_Free(snapshot->return_stack);
    PyMem_Free(snapshot->alternate_stack);
    Py_XDECREF(snapshot->completed_result);
    Py_XDECREF(snapshot->completed_exception);
    unsafe_call_release(snapshot->source.call);
    unsafe_zero(snapshot, sizeof(*snapshot));
    PyMem_Free(snapshot);
}

static int
unsafe_prepare_resume(void *state)
{
    AleffUnsafeSnapshot *snapshot = state;
    AleffUnsafeCall *call = snapshot->source.call;
    PyThreadState *thread = PyThreadState_Get();
    if (thread != call->owner_thread || PyThreadState_GetInterpreter(thread) != call->owner_interpreter) {
        PyErr_SetString(PyExc_RuntimeError, "aleffy continuation belongs to another thread or interpreter");
        return -1;
    }
    if (call->active_snapshot != NULL) {
        PyErr_SetString(PyExc_RuntimeError, "aleffy continuation resume is reentrant");
        return -1;
    }
    return 0;
}

static PyObject *unsafe_eval_frame(
    PyThreadState *thread,
    struct _PyInterpreterFrame *frame,
    int throwflag
)
{
    AleffUnsafeCall *call = active_call;
    if (call == NULL) {
        return _PyEval_EvalFrameDefault(thread, frame, throwflag);
    }
    if (call->checkpoint_ready) {
        return call->previous_eval(thread, frame, throwflag);
    }
    int resumed = aleff_unsafe_context_save(&call->checkpoint);
    if (resumed) {
        PyObject *result = call->resume_value;
        PyObject *exception = call->resume_exception;
        call->checkpoint_ready = 0;
        call->resume_value = NULL;
        call->resume_exception = NULL;
        if (exception != NULL) {
            PyErr_SetRaisedException(exception);
            return NULL;
        }
        return result;
    }
    call->checkpoint_ready = 1;
    PyObject *result = call->previous_eval(thread, frame, throwflag);
    call->checkpoint_ready = 0;
    return result;
}

[[noreturn]] static void
unsafe_restore_original_stack(void *argument)
{
    AleffUnsafeSnapshot *snapshot = argument;
    if (snapshot->return_stack_size > 0) {
        memcpy(
            snapshot->return_stack,
            (const void *)snapshot->return_stack_start,
            snapshot->return_stack_size
        );
    }
    memcpy(
        (void *)snapshot->native_stack_start,
        snapshot->native_stack,
        snapshot->native_stack_size
    );
    aleff_unsafe_context_restore(&snapshot->checkpoint);
}

[[noreturn]] static void
unsafe_restore_return_stack(void *argument)
{
    AleffUnsafeSnapshot *snapshot = argument;
    if (snapshot->return_stack_size > 0) {
        memcpy(
            (void *)snapshot->return_stack_start,
            snapshot->return_stack,
            snapshot->return_stack_size
        );
    }
    aleff_unsafe_context_restore(&snapshot->return_context);
}

[[noreturn]] static void
unsafe_complete_restored_call(AleffUnsafeCall *call, PyObject *result)
{
    AleffUnsafeSnapshot *snapshot = call->active_snapshot;
    if (result == NULL) {
        snapshot->completed_exception = PyErr_GetRaisedException();
    }
    else {
        snapshot->completed_result = result;
    }
    aleff_unsafe_run_on_stack(
        snapshot->alternate_stack + ALEFF_UNSAFE_ALT_STACK_SIZE,
        unsafe_restore_return_stack,
        snapshot
    );
}

static PyObject *
unsafe_resume(const void *state, PyObject *value)
{
    AleffUnsafeSnapshot *snapshot = (AleffUnsafeSnapshot *)state;
    AleffUnsafeCall *call = snapshot->source.call;
    int resumed = aleff_unsafe_context_save(&snapshot->return_context);
    if (resumed) {
        PyObject *result = snapshot->completed_result;
        PyObject *exception = snapshot->completed_exception;
        snapshot->completed_result = NULL;
        snapshot->completed_exception = NULL;
        call->active_snapshot = NULL;
        call->resuming = 0;
        PyMem_Free(snapshot->return_stack);
        snapshot->return_stack = NULL;
        snapshot->return_stack_size = 0;
        PyMem_Free(snapshot->alternate_stack);
        snapshot->alternate_stack = NULL;
        if (exception != NULL) {
            PyErr_SetRaisedException(exception);
            return NULL;
        }
        return result;
    }

    call->active_snapshot = snapshot;
    call->resuming = 1;
    if (value == NULL) {
        call->resume_exception = PyErr_GetRaisedException();
    }
    else {
        call->resume_value = Py_NewRef(value);
    }

    uintptr_t return_start = (uintptr_t)snapshot->return_context.rsp;
    snapshot->return_stack_start = return_start;
    if (return_start < snapshot->native_stack_end) {
        if (unsafe_stack_range(
                return_start,
                snapshot->native_stack_end,
                &snapshot->return_stack_size
            ) < 0) {
            goto error;
        }
        snapshot->return_stack = PyMem_Malloc(snapshot->return_stack_size);
        if (snapshot->return_stack == NULL) {
            PyErr_NoMemory();
            goto error;
        }
    }
    snapshot->alternate_stack = PyMem_Malloc(ALEFF_UNSAFE_ALT_STACK_SIZE);
    if (snapshot->alternate_stack == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    aleff_unsafe_run_on_stack(
        snapshot->alternate_stack + ALEFF_UNSAFE_ALT_STACK_SIZE,
        unsafe_restore_original_stack,
        snapshot
    );

error:
    call->active_snapshot = NULL;
    call->resuming = 0;
    Py_CLEAR(call->resume_value);
    Py_CLEAR(call->resume_exception);
    PyMem_Free(snapshot->return_stack);
    snapshot->return_stack = NULL;
    snapshot->return_stack_size = 0;
    PyMem_Free(snapshot->alternate_stack);
    snapshot->alternate_stack = NULL;
    return NULL;
}

static const AleffAdapterVTable unsafe_vtable = {
    .copy_state = unsafe_copy_state,
    .free_state = unsafe_free_state,
    .resume = unsafe_resume,
    .prepare_resume = unsafe_prepare_resume,
};

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
PyObject *
aleff_unsafe_call(PyObject *Py_UNUSED(self), PyObject *args)
{
    PyObject *callable;
    PyObject *call_args;
    PyObject *kwargs;
    if (!PyArg_ParseTuple(args, "OOO:_unsafe_call", &callable, &call_args, &kwargs)) {
        return NULL;
    }
    if (active_call != NULL) {
        PyErr_SetString(PyExc_RuntimeError, "nested aleffy calls are not supported by the feasibility spike");
        return NULL;
    }

    AleffUnsafeCall *call = PyMem_Calloc(1, sizeof(*call));
    if (call == NULL) {
        return PyErr_NoMemory();
    }
    atomic_init(&call->references, 1);
    call->owner_thread = PyThreadState_Get();
    call->owner_interpreter = PyThreadState_GetInterpreter(call->owner_thread);
#if defined(__GNUC__) || defined(__clang__)
    call->boundary_top = (uintptr_t)__builtin_frame_address(0) + 2U * sizeof(void *);
#else
#  error "aleffy feasibility spike requires a compiler with __builtin_frame_address"
#endif

    AleffUnsafeSource source = {
        .kind = ALEFF_UNSAFE_SOURCE_LIVE,
        .call = call,
    };
    AleffAdapterFrame adapter_frame;
    if (adapter_enter(&adapter_frame, &unsafe_vtable, &source) < 0) {
        unsafe_call_release(call);
        return NULL;
    }

    call->previous_eval = _PyInterpreterState_GetEvalFrameFunc(call->owner_interpreter);
    active_call = call;
    _PyInterpreterState_SetEvalFrameFunc(call->owner_interpreter, unsafe_eval_frame);
    PyObject *result = PyObject_Call(callable, call_args, kwargs);

    if (call->resuming) {
        unsafe_complete_restored_call(call, result);
    }

    if (_PyInterpreterState_GetEvalFrameFunc(call->owner_interpreter) == unsafe_eval_frame) {
        _PyInterpreterState_SetEvalFrameFunc(call->owner_interpreter, call->previous_eval);
    }
    active_call = NULL;
    adapter_leave(&adapter_frame);
    unsafe_call_release(call);
    return result;
}

#else

PyObject *
aleff_unsafe_call(PyObject *Py_UNUSED(self), PyObject *Py_UNUSED(args))
{
    PyErr_SetString(
        PyExc_NotImplementedError,
        "aleffy feasibility spike requires Linux x86-64 with GIL-enabled CPython 3.12"
    );
    return NULL;
}

#endif
