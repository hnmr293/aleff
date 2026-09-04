#if defined(__linux__)
#  define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>

#include "internal.h"
#include "unsafe.h"

#if (defined(__linux__) && defined(__x86_64__)) || \
    (defined(__APPLE__) && (defined(__x86_64__) || defined(__aarch64__))) || \
    defined(_M_X64)
#  define ALEFF_UNSAFE_SUPPORTED_PLATFORM 1
#else
#  define ALEFF_UNSAFE_SUPPORTED_PLATFORM 0
#endif

#if ALEFF_UNSAFE_SUPPORTED_PLATFORM && PY_VERSION_HEX >= 0x030c0000 && \
    PY_VERSION_HEX < 0x030f0000
#  define ALEFF_UNSAFE_SUPPORTED 1
#else
#  define ALEFF_UNSAFE_SUPPORTED 0
#endif

#if ALEFF_UNSAFE_SUPPORTED

#if defined(_WIN32)
#  include <intrin.h>
#  include <windows.h>
#  pragma intrinsic(_AddressOfReturnAddress)
#else
#  include <errno.h>
#  include <pthread.h>
#  include <sys/mman.h>
#  include <unistd.h>
#  if defined(__APPLE__) && !defined(MAP_ANONYMOUS)
#    define MAP_ANONYMOUS MAP_ANON
#  endif
#endif

#if defined(__clang__)
#  if __has_feature(address_sanitizer)
#    define ALEFF_UNSAFE_ADDRESS_SANITIZER 1
#  endif
#elif defined(__GNUC__) && defined(__SANITIZE_ADDRESS__)
#  define ALEFF_UNSAFE_ADDRESS_SANITIZER 1
#endif

#ifndef ALEFF_UNSAFE_ADDRESS_SANITIZER
#  define ALEFF_UNSAFE_ADDRESS_SANITIZER 0
#endif

#if ALEFF_UNSAFE_ADDRESS_SANITIZER
#  include <sanitizer/asan_interface.h>
#  include <sanitizer/common_interface_defs.h>
#  define ALEFF_UNSAFE_NO_ASAN __attribute__((no_sanitize_address))
#else
#  define ALEFF_UNSAFE_NO_ASAN
#endif

#if defined(_MSC_VER)
#  define ALEFF_UNSAFE_NOINLINE __declspec(noinline)
#  define ALEFF_UNSAFE_NORETURN __declspec(noreturn)
#else
#  define ALEFF_UNSAFE_NOINLINE __attribute__((noinline))
#  define ALEFF_UNSAFE_NORETURN [[noreturn]]
#endif

#define ALEFF_UNSAFE_ALT_STACK_SIZE (1024U * 1024U)

#if defined(_M_X64)
typedef struct {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rsp;
    uint64_t rip;
    uint32_t mxcsr;
    uint16_t x87_control;
    uint16_t padding;
    uint64_t reserved;
    unsigned char xmm6[16];
    unsigned char xmm7[16];
    unsigned char xmm8[16];
    unsigned char xmm9[16];
    unsigned char xmm10[16];
    unsigned char xmm11[16];
    unsigned char xmm12[16];
    unsigned char xmm13[16];
    unsigned char xmm14[16];
    unsigned char xmm15[16];
} AleffUnsafeContext;

_Static_assert(offsetof(AleffUnsafeContext, rsp) == 64, "context rsp offset");
_Static_assert(offsetof(AleffUnsafeContext, rip) == 72, "context rip offset");
_Static_assert(offsetof(AleffUnsafeContext, mxcsr) == 80, "context mxcsr offset");
_Static_assert(offsetof(AleffUnsafeContext, x87_control) == 84, "context x87 offset");
_Static_assert(offsetof(AleffUnsafeContext, xmm6) == 96, "context xmm6 offset");
#elif defined(__APPLE__) && defined(__aarch64__)
typedef struct {
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t x29;
    uint64_t x30;
    uint64_t sp;
    uint32_t fpcr;
    uint32_t fpsr;
    uint64_t d8;
    uint64_t d9;
    uint64_t d10;
    uint64_t d11;
    uint64_t d12;
    uint64_t d13;
    uint64_t d14;
    uint64_t d15;
} AleffUnsafeContext;

_Static_assert(offsetof(AleffUnsafeContext, x29) == 80, "context x29 offset");
_Static_assert(offsetof(AleffUnsafeContext, x30) == 88, "context x30 offset");
_Static_assert(offsetof(AleffUnsafeContext, sp) == 96, "context sp offset");
_Static_assert(offsetof(AleffUnsafeContext, fpcr) == 104, "context fpcr offset");
_Static_assert(offsetof(AleffUnsafeContext, fpsr) == 108, "context fpsr offset");
_Static_assert(offsetof(AleffUnsafeContext, d8) == 112, "context d8 offset");
#else
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
#endif

#if defined(__GNUC__) || defined(__clang__)
__attribute__((returns_twice))
#endif
int aleff_unsafe_context_save(AleffUnsafeContext *context);
ALEFF_UNSAFE_NORETURN void aleff_unsafe_context_restore(const AleffUnsafeContext *context);
ALEFF_UNSAFE_NORETURN void aleff_unsafe_run_on_stack(
    void *stack_top,
    void (*function)(void *),
    void *argument
);
void aleff_unsafe_stack_copy(void *destination, const void *source, size_t size);

typedef struct AleffUnsafeCall AleffUnsafeCall;
typedef struct AleffUnsafeSnapshot AleffUnsafeSnapshot;
typedef struct AleffUnsafeHookManager AleffUnsafeHookManager;

typedef enum {
    ALEFF_UNSAFE_SOURCE_LIVE = 1,
    ALEFF_UNSAFE_SOURCE_SNAPSHOT = 2,
} AleffUnsafeSourceKind;

typedef enum {
    ALEFF_UNSAFE_EVENT_NONE = 0,
    ALEFF_UNSAFE_EVENT_CALLBACK = 1,
    ALEFF_UNSAFE_EVENT_COMPLETE = 2,
} AleffUnsafeEvent;

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
    void *alternate_stack_mapping;
    size_t alternate_stack_mapping_size;
    void *alternate_stack_bottom;
    size_t alternate_stack_size;
    void *alternate_stack_top;
#if PY_VERSION_HEX < 0x030d0000
    _PyCFrame *return_cframe;
    _PyCFrame bridge_cframe;
#else
    struct _PyInterpreterFrame *return_current_frame;
#endif

    AleffUnsafeEvent event;
    struct _PyInterpreterFrame *callback_frame;
    int callback_throwflag;
    PyObject *completed_result;
    PyObject *completed_exception;
};

struct AleffUnsafeCall {
    _Atomic unsigned int references;
    AleffUnsafeSource source;
    PyThreadState *owner_thread;
    PyInterpreterState *owner_interpreter;
    AleffUnsafeContext checkpoint;
    uintptr_t stack_low;
    uintptr_t stack_high;
    uintptr_t boundary_top;
    int checkpoint_ready;
    int resuming;
    PyObject *resume_value;
    PyObject *resume_exception;
    AleffUnsafeSnapshot *active_snapshot;
};

struct AleffUnsafeHookManager {
    PyInterpreterState *interpreter;
    _PyFrameEvalFunction downstream_eval;
    Py_ssize_t active_boundaries;
    AleffUnsafeHookManager *next;
};

#if defined(_MSC_VER)
#  define ALEFF_THREAD_LOCAL __declspec(thread)
#else
#  define ALEFF_THREAD_LOCAL _Thread_local
#endif

static ALEFF_THREAD_LOCAL AleffUnsafeCall *active_call = NULL;
static AleffUnsafeHookManager *hook_managers = NULL;

static void
unsafe_sanitizer_start_switch(const void *stack_bottom, size_t stack_size)
{
#if ALEFF_UNSAFE_ADDRESS_SANITIZER
    __sanitizer_start_switch_fiber(NULL, stack_bottom, stack_size);
#else
    (void)stack_bottom;
    (void)stack_size;
#endif
}

static void
unsafe_sanitizer_finish_switch(void)
{
#if ALEFF_UNSAFE_ADDRESS_SANITIZER
    __sanitizer_finish_switch_fiber(NULL, NULL, NULL);
#endif
}

static void
unsafe_capture_stack(void *destination, const void *source, size_t size)
{
#if ALEFF_UNSAFE_ADDRESS_SANITIZER
    aleff_unsafe_stack_copy(destination, source, size);
    __asan_unpoison_memory_region(source, size);
#else
    memcpy(destination, source, size);
#endif
}

static void
unsafe_restore_stack(void *destination, const void *source, size_t size)
{
#if ALEFF_UNSAFE_ADDRESS_SANITIZER
    aleff_unsafe_stack_copy(destination, source, size);
    __asan_unpoison_memory_region(destination, size);
#else
    memcpy(destination, source, size);
#endif
}

#if PY_VERSION_HEX < 0x030d0000
static ALEFF_UNSAFE_NO_ASAN struct _PyInterpreterFrame *
unsafe_read_current_frame(const _PyCFrame *cframe)
{
    return cframe->current_frame;
}

static int
unsafe_get_current_frame(
    const AleffUnsafeCall *call,
    struct _PyInterpreterFrame **frame
)
{
    const _PyCFrame *cframe = call->owner_thread->cframe;
    if (cframe != &call->owner_thread->root_cframe) {
        uintptr_t address = (uintptr_t)cframe;
        if (address < call->stack_low ||
            address > call->stack_high ||
            sizeof(*cframe) > call->stack_high - address) {
            PyErr_SetString(
                PyExc_RuntimeError,
                "aleffy encountered a C frame outside the native thread stack"
            );
            return -1;
        }
    }
    *frame = unsafe_read_current_frame(cframe);
    return 0;
}
#endif

static PyObject *unsafe_eval_frame(
    PyThreadState *thread,
    struct _PyInterpreterFrame *frame,
    int throwflag
);
ALEFF_UNSAFE_NORETURN static void unsafe_restore_return_stack(void *argument);
static const AleffAdapterVTable unsafe_vtable;

static AleffUnsafeHookManager *
unsafe_find_hook_manager(PyInterpreterState *interpreter)
{
    for (AleffUnsafeHookManager *manager = hook_managers;
         manager != NULL;
         manager = manager->next) {
        if (manager->interpreter == interpreter) {
            return manager;
        }
    }
    return NULL;
}

static int
unsafe_hook_enter(PyInterpreterState *interpreter)
{
    AleffUnsafeHookManager *manager = unsafe_find_hook_manager(interpreter);
    if (manager != NULL) {
        if (_PyInterpreterState_GetEvalFrameFunc(interpreter) != unsafe_eval_frame) {
            PyErr_SetString(
                PyExc_RuntimeError,
                "the interpreter eval-frame hook changed while aleffy was active"
            );
            return -1;
        }
        manager->active_boundaries++;
        return 0;
    }

    _PyFrameEvalFunction downstream = _PyInterpreterState_GetEvalFrameFunc(interpreter);
    if (downstream == unsafe_eval_frame) {
        PyErr_SetString(PyExc_RuntimeError, "aleffy eval-frame hook state is inconsistent");
        return -1;
    }
    manager = PyMem_Calloc(1, sizeof(*manager));
    if (manager == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    manager->interpreter = interpreter;
    manager->downstream_eval = downstream;
    manager->active_boundaries = 1;
    manager->next = hook_managers;
    hook_managers = manager;
    _PyInterpreterState_SetEvalFrameFunc(interpreter, unsafe_eval_frame);
    return 0;
}

static void
unsafe_hook_leave(PyInterpreterState *interpreter)
{
    AleffUnsafeHookManager **link = &hook_managers;
    while (*link != NULL && (*link)->interpreter != interpreter) {
        link = &(*link)->next;
    }
    AleffUnsafeHookManager *manager = *link;
    if (manager == NULL) {
        return;
    }
    manager->active_boundaries--;
    if (manager->active_boundaries != 0) {
        return;
    }
    if (_PyInterpreterState_GetEvalFrameFunc(interpreter) == unsafe_eval_frame) {
        _PyInterpreterState_SetEvalFrameFunc(interpreter, manager->downstream_eval);
    }
    *link = manager->next;
    PyMem_Free(manager);
}

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
    aleff_adapter_defer_node_frees_leave();
    unsafe_zero(call, sizeof(*call));
    PyMem_Free(call);
}

static int
unsafe_get_stack_bounds(AleffUnsafeCall *call)
{
#if defined(__linux__)
    pthread_attr_t attributes;
    int error = pthread_getattr_np(pthread_self(), &attributes);
    if (error != 0) {
        PyErr_Format(
            PyExc_RuntimeError,
            "aleffy could not query the native thread stack: %s",
            strerror(error)
        );
        return -1;
    }
    void *stack_address = NULL;
    size_t stack_size = 0;
    error = pthread_attr_getstack(&attributes, &stack_address, &stack_size);
    pthread_attr_destroy(&attributes);
    if (error != 0) {
        PyErr_Format(
            PyExc_RuntimeError,
            "aleffy could not determine the native thread stack bounds: %s",
            strerror(error)
        );
        return -1;
    }
    uintptr_t low = (uintptr_t)stack_address;
    if (stack_size == 0 || stack_size > UINTPTR_MAX - low) {
        PyErr_SetString(PyExc_RuntimeError, "aleffy received invalid native thread stack bounds");
        return -1;
    }
    call->stack_low = low;
    call->stack_high = low + stack_size;
    return 0;
#elif defined(__APPLE__)
    void *stack_address = pthread_get_stackaddr_np(pthread_self());
    size_t stack_size = pthread_get_stacksize_np(pthread_self());
    uintptr_t high = (uintptr_t)stack_address;
    if (stack_size == 0 || high < stack_size) {
        PyErr_SetString(PyExc_RuntimeError, "aleffy received invalid native thread stack bounds");
        return -1;
    }
    call->stack_low = high - stack_size;
    call->stack_high = high;
    return 0;
#elif defined(_WIN32)
    ULONG_PTR low;
    ULONG_PTR high;
    GetCurrentThreadStackLimits(&low, &high);
    if (low == 0 || high <= low) {
        PyErr_SetString(PyExc_RuntimeError, "aleffy received invalid native thread stack bounds");
        return -1;
    }
    call->stack_low = (uintptr_t)low;
    call->stack_high = (uintptr_t)high;
    return 0;
#endif
}

static int
unsafe_stack_range(
    const AleffUnsafeCall *call,
    uintptr_t start,
    uintptr_t end,
    size_t *size
)
{
    if (start < call->stack_low || end > call->stack_high || start >= end) {
        PyErr_SetString(PyExc_RuntimeError, "aleffy encountered an invalid native stack range");
        return -1;
    }
    *size = (size_t)(end - start);
    return 0;
}

static int
unsafe_allocate_alternate_stack(AleffUnsafeSnapshot *snapshot)
{
#if defined(_WIN32)
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    size_t page_size = (size_t)system_info.dwPageSize;
    size_t usable_size = ALEFF_UNSAFE_ALT_STACK_SIZE;
    size_t remainder = usable_size % page_size;
    if (remainder != 0) {
        usable_size += page_size - remainder;
    }
    void *mapping = VirtualAlloc(
        NULL,
        usable_size,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE
    );
    if (mapping == NULL) {
        PyErr_SetFromWindowsErr(0);
        return -1;
    }
    snapshot->alternate_stack_mapping = mapping;
    snapshot->alternate_stack_mapping_size = usable_size;
    snapshot->alternate_stack_bottom = mapping;
    snapshot->alternate_stack_size = usable_size;
    snapshot->alternate_stack_top = (unsigned char *)mapping + usable_size;
    return 0;
#else
    long page_size_value = sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0) {
        PyErr_SetString(PyExc_RuntimeError, "aleffy could not determine the system page size");
        return -1;
    }
    size_t page_size = (size_t)page_size_value;
    size_t usable_size = ALEFF_UNSAFE_ALT_STACK_SIZE;
    size_t remainder = usable_size % page_size;
    if (remainder != 0) {
        usable_size += page_size - remainder;
    }
    if (usable_size > SIZE_MAX - 2U * page_size) {
        PyErr_NoMemory();
        return -1;
    }
    size_t mapping_size = usable_size + 2U * page_size;
    void *mapping = mmap(
        NULL,
        mapping_size,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );
    if (mapping == MAP_FAILED) {
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
    unsigned char *usable = (unsigned char *)mapping + page_size;
    if (mprotect(usable, usable_size, PROT_READ | PROT_WRITE) < 0) {
        int saved_errno = errno;
        munmap(mapping, mapping_size);
        errno = saved_errno;
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
    snapshot->alternate_stack_mapping = mapping;
    snapshot->alternate_stack_mapping_size = mapping_size;
    snapshot->alternate_stack_bottom = usable;
    snapshot->alternate_stack_size = usable_size;
    snapshot->alternate_stack_top = usable + usable_size;
    return 0;
#endif
}

static void
unsafe_free_alternate_stack(AleffUnsafeSnapshot *snapshot)
{
    if (snapshot->alternate_stack_mapping != NULL) {
#if defined(_WIN32)
        VirtualFree(snapshot->alternate_stack_mapping, 0, MEM_RELEASE);
#else
        munmap(
            snapshot->alternate_stack_mapping,
            snapshot->alternate_stack_mapping_size
        );
#endif
    }
    snapshot->alternate_stack_mapping = NULL;
    snapshot->alternate_stack_mapping_size = 0;
    snapshot->alternate_stack_bottom = NULL;
    snapshot->alternate_stack_size = 0;
    snapshot->alternate_stack_top = NULL;
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

    if (source->kind == ALEFF_UNSAFE_SOURCE_LIVE &&
        !(call->resuming && call->active_snapshot != NULL)) {
        if (!call->checkpoint_ready) {
            PyErr_SetString(PyExc_RuntimeError, "aleffy could not locate the C-to-Python callback boundary");
            goto error;
        }
        copy->checkpoint = call->checkpoint;
        copy->native_stack_start = (uintptr_t)call->checkpoint.rsp;
        copy->native_stack_end = call->boundary_top;
        if (unsafe_stack_range(
                call,
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
        unsafe_capture_stack(
            copy->native_stack,
            (const void *)copy->native_stack_start,
            copy->native_stack_size
        );
    }
    else {
        const AleffUnsafeSnapshot *snapshot = source->kind == ALEFF_UNSAFE_SOURCE_LIVE
            ? call->active_snapshot
            : (const AleffUnsafeSnapshot *)source;
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
    unsafe_free_alternate_stack(snapshot);
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
    if (call->active_snapshot == snapshot) {
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
    AleffUnsafeHookManager *manager = unsafe_find_hook_manager(thread->interp);
    if (manager == NULL) {
        return _PyEval_EvalFrameDefault(thread, frame, throwflag);
    }
    AleffUnsafeCall *call = active_call;
    if (call == NULL || call->owner_interpreter != thread->interp) {
        return manager->downstream_eval(thread, frame, throwflag);
    }
    if (call->checkpoint_ready) {
        return manager->downstream_eval(thread, frame, throwflag);
    }
    int resumed = aleff_unsafe_context_save(&call->checkpoint);
    if (resumed) {
        unsafe_sanitizer_finish_switch();
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
    if (call->resuming) {
        AleffUnsafeSnapshot *snapshot = call->active_snapshot;
        if (snapshot == NULL) {
            call->checkpoint_ready = 0;
            PyErr_SetString(PyExc_RuntimeError, "aleffy callback bridge is not active");
            return NULL;
        }

        uintptr_t native_start = (uintptr_t)call->checkpoint.rsp;
        size_t native_size;
        if (unsafe_stack_range(
                call,
                native_start,
                call->boundary_top,
                &native_size
            ) < 0) {
            call->checkpoint_ready = 0;
            return NULL;
        }
        unsigned char *native_stack = PyMem_Malloc(native_size);
        if (native_stack == NULL) {
            call->checkpoint_ready = 0;
            PyErr_NoMemory();
            return NULL;
        }
        unsafe_capture_stack(native_stack, (const void *)native_start, native_size);
        if (snapshot->native_stack != NULL) {
            unsafe_zero(snapshot->native_stack, snapshot->native_stack_size);
        }
        PyMem_Free(snapshot->native_stack);
        snapshot->checkpoint = call->checkpoint;
        snapshot->native_stack = native_stack;
        snapshot->native_stack_size = native_size;
        snapshot->native_stack_start = native_start;
        snapshot->native_stack_end = call->boundary_top;
        snapshot->callback_frame = frame;
        snapshot->callback_throwflag = throwflag;
        snapshot->event = ALEFF_UNSAFE_EVENT_CALLBACK;
        unsafe_sanitizer_start_switch(
            snapshot->alternate_stack_bottom,
            snapshot->alternate_stack_size
        );
        aleff_unsafe_run_on_stack(
            snapshot->alternate_stack_top,
            unsafe_restore_return_stack,
            snapshot
        );
    }
    PyObject *result = manager->downstream_eval(thread, frame, throwflag);
    call->checkpoint_ready = 0;
    return result;
}

ALEFF_UNSAFE_NORETURN static void
unsafe_restore_original_stack(void *argument)
{
    unsafe_sanitizer_finish_switch();
    AleffUnsafeSnapshot *snapshot = argument;
    if (snapshot->return_stack_size > 0) {
        unsafe_capture_stack(
            snapshot->return_stack,
            (const void *)snapshot->return_stack_start,
            snapshot->return_stack_size
        );
    }
    unsafe_restore_stack(
        (void *)snapshot->native_stack_start,
        snapshot->native_stack,
        snapshot->native_stack_size
    );
    AleffUnsafeCall *call = snapshot->source.call;
#if PY_VERSION_HEX < 0x030d0000
    call->owner_thread->cframe = &snapshot->bridge_cframe;
#else
    call->owner_thread->current_frame = snapshot->return_current_frame;
#endif
    unsafe_sanitizer_start_switch(
        (const void *)call->stack_low,
        (size_t)(call->stack_high - call->stack_low)
    );
    aleff_unsafe_context_restore(&snapshot->checkpoint);
}

ALEFF_UNSAFE_NORETURN static void
unsafe_restore_return_stack(void *argument)
{
    unsafe_sanitizer_finish_switch();
    AleffUnsafeSnapshot *snapshot = argument;
    if (snapshot->return_stack_size > 0) {
        unsafe_restore_stack(
            (void *)snapshot->return_stack_start,
            snapshot->return_stack,
            snapshot->return_stack_size
        );
    }
    AleffUnsafeCall *call = snapshot->source.call;
#if PY_VERSION_HEX < 0x030d0000
    call->owner_thread->cframe = snapshot->return_cframe;
#else
    call->owner_thread->current_frame = snapshot->return_current_frame;
#endif
    unsafe_sanitizer_start_switch(
        (const void *)call->stack_low,
        (size_t)(call->stack_high - call->stack_low)
    );
    aleff_unsafe_context_restore(&snapshot->return_context);
}

ALEFF_UNSAFE_NORETURN static void
unsafe_complete_restored_call(AleffUnsafeCall *call, PyObject *result)
{
    AleffUnsafeSnapshot *snapshot = call->active_snapshot;
    if (result == NULL) {
        snapshot->completed_exception = PyErr_GetRaisedException();
    }
    else {
        snapshot->completed_result = result;
    }
    snapshot->event = ALEFF_UNSAFE_EVENT_COMPLETE;
    unsafe_sanitizer_start_switch(
        snapshot->alternate_stack_bottom,
        snapshot->alternate_stack_size
    );
    aleff_unsafe_run_on_stack(
        snapshot->alternate_stack_top,
        unsafe_restore_return_stack,
        snapshot
    );
}

static int
unsafe_switch_to_native(AleffUnsafeSnapshot *snapshot)
{
    AleffUnsafeCall *call = snapshot->source.call;
    int resumed = aleff_unsafe_context_save(&snapshot->return_context);
    if (resumed) {
        unsafe_sanitizer_finish_switch();
        PyMem_Free(snapshot->return_stack);
        snapshot->return_stack = NULL;
        snapshot->return_stack_size = 0;
        unsafe_free_alternate_stack(snapshot);
        return 0;
    }

    uintptr_t return_start = (uintptr_t)snapshot->return_context.rsp;
    snapshot->return_stack_start = return_start;
    if (return_start < snapshot->native_stack_end) {
        if (unsafe_stack_range(
                call,
                return_start,
                snapshot->native_stack_end,
                &snapshot->return_stack_size
            ) < 0) {
            return -1;
        }
        snapshot->return_stack = PyMem_Malloc(snapshot->return_stack_size);
        if (snapshot->return_stack == NULL) {
            snapshot->return_stack_size = 0;
            PyErr_NoMemory();
            return -1;
        }
    }
    if (unsafe_allocate_alternate_stack(snapshot) < 0) {
        PyMem_Free(snapshot->return_stack);
        snapshot->return_stack = NULL;
        snapshot->return_stack_size = 0;
        return -1;
    }

    PyThreadState *thread = call->owner_thread;
#if PY_VERSION_HEX < 0x030d0000
    snapshot->return_cframe = thread->cframe;
    if (unsafe_get_current_frame(call, &snapshot->bridge_cframe.current_frame) < 0) {
        PyMem_Free(snapshot->return_stack);
        snapshot->return_stack = NULL;
        snapshot->return_stack_size = 0;
        unsafe_free_alternate_stack(snapshot);
        return -1;
    }
    snapshot->bridge_cframe.previous = &thread->root_cframe;
#else
    snapshot->return_current_frame = thread->current_frame;
#endif
    snapshot->event = ALEFF_UNSAFE_EVENT_NONE;
    unsafe_sanitizer_start_switch(
        snapshot->alternate_stack_bottom,
        snapshot->alternate_stack_size
    );
    aleff_unsafe_run_on_stack(
        snapshot->alternate_stack_top,
        unsafe_restore_original_stack,
        snapshot
    );
}

static PyObject *
unsafe_resume(const void *state, PyObject *value)
{
    AleffUnsafeSnapshot *snapshot = (AleffUnsafeSnapshot *)state;
    AleffUnsafeCall *call = snapshot->source.call;
    AleffUnsafeCall *previous_call = active_call;
    AleffUnsafeSnapshot *previous_snapshot = call->active_snapshot;
    int hook_entered = 0;

    if (unsafe_hook_enter(call->owner_interpreter) < 0) {
        return NULL;
    }
    hook_entered = 1;
    active_call = call;
    call->active_snapshot = snapshot;
    call->resuming = 1;
    if (value == NULL) {
        call->resume_exception = PyErr_GetRaisedException();
    }
    else {
        call->resume_value = Py_NewRef(value);
    }

    while (1) {
        if (unsafe_switch_to_native(snapshot) < 0) {
            goto error;
        }
        if (snapshot->event == ALEFF_UNSAFE_EVENT_COMPLETE) {
            break;
        }
        if (snapshot->event != ALEFF_UNSAFE_EVENT_CALLBACK ||
            snapshot->callback_frame == NULL) {
            PyErr_SetString(PyExc_RuntimeError, "aleffy callback bridge returned an invalid event");
            goto error;
        }

        struct _PyInterpreterFrame *callback_frame = snapshot->callback_frame;
        int callback_throwflag = snapshot->callback_throwflag;
        snapshot->callback_frame = NULL;
        snapshot->callback_throwflag = 0;
        PyObject *callback_result;
        AleffUnsafeHookManager *manager = unsafe_find_hook_manager(
            call->owner_interpreter
        );
        if (manager == NULL) {
            PyErr_SetString(PyExc_RuntimeError, "aleffy eval-frame hook is not active");
            callback_result = NULL;
        }
        else {
            AleffAdapterFrame adapter_frame;
            if (adapter_enter(
                    &adapter_frame,
                    &unsafe_vtable,
                    &call->source
                ) < 0) {
                callback_result = NULL;
            }
            else {
                callback_result = manager->downstream_eval(
                    call->owner_thread,
                    callback_frame,
                    callback_throwflag
                );
                adapter_leave(&adapter_frame);
            }
        }
        if (callback_result == NULL) {
            call->resume_exception = PyErr_GetRaisedException();
            if (call->resume_exception == NULL) {
                PyErr_SetString(
                    PyExc_RuntimeError,
                    "aleffy callback failed without an active exception"
                );
                call->resume_exception = PyErr_GetRaisedException();
            }
        }
        else {
            call->resume_value = callback_result;
        }
    }

    PyObject *result = snapshot->completed_result;
    PyObject *exception = snapshot->completed_exception;
    snapshot->completed_result = NULL;
    snapshot->completed_exception = NULL;
    call->active_snapshot = previous_snapshot;
    call->resuming = previous_snapshot != NULL;
    active_call = previous_call;
    unsafe_hook_leave(call->owner_interpreter);
    if (exception != NULL) {
        PyErr_SetRaisedException(exception);
        return NULL;
    }
    return result;

error:
    active_call = previous_call;
    if (hook_entered) {
        unsafe_hook_leave(call->owner_interpreter);
    }
    call->active_snapshot = previous_snapshot;
    call->resuming = previous_snapshot != NULL;
    Py_CLEAR(call->resume_value);
    Py_CLEAR(call->resume_exception);
    PyMem_Free(snapshot->return_stack);
    snapshot->return_stack = NULL;
    snapshot->return_stack_size = 0;
    unsafe_free_alternate_stack(snapshot);
    return NULL;
}

static const AleffAdapterVTable unsafe_vtable = {
    .copy_state = unsafe_copy_state,
    .free_state = unsafe_free_state,
    .resume = unsafe_resume,
    .prepare_resume = unsafe_prepare_resume,
};

ALEFF_UNSAFE_NOINLINE
PyObject *
aleff_unsafe_call(PyObject *Py_UNUSED(self), PyObject *args)
{
    PyObject *callable;
    PyObject *call_args;
    PyObject *kwargs;
    if (!PyArg_ParseTuple(args, "OOO:_unsafe_call", &callable, &call_args, &kwargs)) {
        return NULL;
    }
    AleffUnsafeCall *call = PyMem_Calloc(1, sizeof(*call));
    if (call == NULL) {
        return PyErr_NoMemory();
    }
    atomic_init(&call->references, 1);
    if (aleff_adapter_defer_node_frees_enter() < 0) {
        PyMem_Free(call);
        return NULL;
    }
    call->source.kind = ALEFF_UNSAFE_SOURCE_LIVE;
    call->source.call = call;
    call->owner_thread = PyThreadState_Get();
    call->owner_interpreter = PyThreadState_GetInterpreter(call->owner_thread);
    if (unsafe_get_stack_bounds(call) < 0) {
        unsafe_call_release(call);
        return NULL;
    }
#if defined(_MSC_VER)
    call->boundary_top = (uintptr_t)_AddressOfReturnAddress() + sizeof(void *);
#elif defined(__GNUC__) || defined(__clang__)
    call->boundary_top = (uintptr_t)__builtin_frame_address(0) + 2U * sizeof(void *);
#else
#  error "aleffy feasibility spike requires a compiler with __builtin_frame_address"
#endif
    if (call->boundary_top < call->stack_low || call->boundary_top > call->stack_high) {
        PyErr_SetString(PyExc_RuntimeError, "aleffy boundary is outside the native thread stack");
        unsafe_call_release(call);
        return NULL;
    }

    AleffAdapterFrame adapter_frame;
    if (adapter_enter(&adapter_frame, &unsafe_vtable, &call->source) < 0) {
        unsafe_call_release(call);
        return NULL;
    }

    if (unsafe_hook_enter(call->owner_interpreter) < 0) {
        adapter_leave(&adapter_frame);
        unsafe_call_release(call);
        return NULL;
    }
    AleffUnsafeCall *previous_call = active_call;
    active_call = call;
    PyObject *result = PyObject_Call(callable, call_args, kwargs);

    if (call->resuming) {
        unsafe_complete_restored_call(call, result);
    }

    active_call = previous_call;
    unsafe_hook_leave(call->owner_interpreter);
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
        "aleffy feasibility spike requires Linux x86-64, macOS x86-64/arm64, or Windows x64 with CPython 3.12 through 3.14"
    );
    return NULL;
}

#endif
