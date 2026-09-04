#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>

static PyMemAllocatorEx previous_allocator;
static PyMemAllocatorEx failing_allocator;
static int allocator_domain = -1;
static int allocator_installed = 0;
static int fail_malloc_enabled = 0;
static int fail_malloc_once = 0;
static int fail_calloc_enabled = 0;
static int fail_allocation_once = 0;
static size_t malloc_calls = 0;
static size_t fail_malloc_after = 0;
static PyObject *tracked_reference = NULL;
static Py_ssize_t tracked_reference_count = 0;
static int tracked_reference_acquired = 0;

static void *test_realloc(void *context, void *pointer, size_t size);
static void test_free(void *context, void *pointer);
void aleff_test_allocator_restore(void);

static int
test_allocation_should_fail(size_t size)
{
    if (fail_allocation_once && size != 0) {
        fail_allocation_once = 0;
        return 1;
    }
    return 0;
}

static int
test_tracked_reference_should_fail(void)
{
    if (tracked_reference == NULL ||
        Py_REFCNT(tracked_reference) <= tracked_reference_count) {
        return 0;
    }
    tracked_reference_acquired = 1;
    if (fail_malloc_once) {
        fail_malloc_enabled = 0;
    }
    return 1;
}

static void *
test_malloc(void *context, size_t size)
{
    malloc_calls++;
    if (test_allocation_should_fail(size)) {
        return NULL;
    }
    if (fail_malloc_enabled) {
        int should_fail = tracked_reference == NULL
            ? malloc_calls >= fail_malloc_after
            : size != 0 && test_tracked_reference_should_fail();
        if (should_fail) {
            return NULL;
        }
    }
    return previous_allocator.malloc(context, size);
}

static void *
test_calloc(void *context, size_t nelem, size_t elsize)
{
    if (test_allocation_should_fail(nelem != 0 && elsize != 0) ||
        fail_calloc_enabled ||
        (fail_malloc_enabled && nelem != 0 && elsize != 0 &&
         test_tracked_reference_should_fail())) {
        return NULL;
    }
    return previous_allocator.calloc(context, nelem, elsize);
}

static void
test_allocator_install(int domain)
{
    if (!allocator_installed) {
        allocator_domain = domain;
        PyMem_GetAllocator(domain, &previous_allocator);
        failing_allocator.ctx = previous_allocator.ctx;
        failing_allocator.malloc = test_malloc;
        failing_allocator.calloc = test_calloc;
        failing_allocator.realloc = test_realloc;
        failing_allocator.free = test_free;
        PyMem_SetAllocator(domain, &failing_allocator);
        allocator_installed = 1;
    }
}

static void *
test_realloc(void *context, void *pointer, size_t size)
{
    if (test_allocation_should_fail(size)) {
        return NULL;
    }
    if (fail_malloc_enabled && size != 0 &&
        test_tracked_reference_should_fail()) {
        return NULL;
    }
    return previous_allocator.realloc(context, pointer, size);
}

static void
test_free(void *context, void *pointer)
{
    previous_allocator.free(context, pointer);
}

int
aleff_test_call_len(PyObject *object)
{
    PyObject *builtins = PyEval_GetBuiltins();
    PyObject *callable = PyDict_GetItemString(builtins, "len");
    test_allocator_install(PYMEM_DOMAIN_MEM);
    fail_malloc_enabled = 0;
    fail_calloc_enabled = 1;
    PyObject *result = PyObject_CallOneArg(callable, object);
    int had_error = PyErr_Occurred() != NULL;
    int is_memory_error = PyErr_ExceptionMatches(PyExc_MemoryError);
    int is_system_error = PyErr_ExceptionMatches(PyExc_SystemError);
    int result_is_null = result == NULL;
    Py_XDECREF(result);
    PyErr_Clear();
    aleff_test_allocator_restore();
    return (had_error ? 1 : 0) | (result_is_null ? 2 : 0) |
        (is_memory_error ? 4 : 0) | (is_system_error ? 8 : 0);
}

int
aleff_test_call_install(
    void *function_pointer,
    PyObject *original_dir,
    PyObject *dir_key,
    size_t call_number
)
{
    typedef int (*install_function)(void);
    _Static_assert(sizeof(install_function) == sizeof(void *), "function pointers must fit in void * for this test");
    install_function install;
    memcpy(&install, &function_pointer, sizeof(install));
    PyObject *builtins = PyEval_GetBuiltins();
    test_allocator_install(PYMEM_DOMAIN_OBJ);
    fail_malloc_enabled = 1;
    fail_malloc_after = call_number;
    fail_calloc_enabled = 0;
    malloc_calls = 0;
    int result = install();
    int had_error = PyErr_Occurred() != NULL;
    int is_memory_error = PyErr_ExceptionMatches(PyExc_MemoryError);
    PyObject *current_dir = PyDict_GetItemWithError(builtins, dir_key);
    int mutated = current_dir != original_dir;
    PyErr_Clear();
    aleff_test_allocator_restore();
    return (result < 0 ? 1 : 0) | (had_error ? 2 : 0) |
        (mutated ? 4 : 0) | (is_memory_error ? 8 : 0);
}

static void
test_tracked_failure_start(PyObject *tracked)
{
    test_allocator_install(PYMEM_DOMAIN_OBJ);
    fail_malloc_enabled = 1;
    fail_malloc_once = 1;
    fail_calloc_enabled = 0;
    malloc_calls = 0;
    tracked_reference = tracked;
    tracked_reference_count = Py_REFCNT(tracked);
    tracked_reference_acquired = 0;
}

static int
test_tracked_failure_finish(int result)
{
    int had_error = PyErr_Occurred() != NULL;
    int is_memory_error = PyErr_ExceptionMatches(PyExc_MemoryError);
    int leaked = Py_REFCNT(tracked_reference) > tracked_reference_count;
    int status = (result < 0 ? 1 : 0) | (had_error ? 2 : 0) |
        (tracked_reference_acquired ? 4 : 0) | (leaked ? 8 : 0) |
        (is_memory_error ? 16 : 0);
    tracked_reference = NULL;
    fail_malloc_once = 0;
    PyErr_Clear();
    aleff_test_allocator_restore();
    return status;
}

int
aleff_test_call_hashing_install(
    void *function_pointer,
    PyObject *hashlib_module,
    PyObject *hmac_module,
    PyObject *tracked
)
{
    typedef int (*install_function)(PyObject *, PyObject *);
    _Static_assert(
        sizeof(install_function) == sizeof(void *),
        "function pointers must fit in void * for this test"
    );
    install_function install;
    memcpy(&install, &function_pointer, sizeof(install));
    test_tracked_failure_start(tracked);
    int result = install(hashlib_module, hmac_module);
    return test_tracked_failure_finish(result);
}

int
aleff_test_call_compression_install(
    void *function_pointer,
    PyObject *zlib_module,
    PyObject *bz2_module,
    PyObject *lzma_module,
    PyObject *tracked
)
{
    typedef int (*install_function)(
        PyObject *, PyObject *, PyObject *, PyObject *
    );
    _Static_assert(
        sizeof(install_function) == sizeof(void *),
        "function pointers must fit in void * for this test"
    );
    install_function install;
    memcpy(&install, &function_pointer, sizeof(install));
    test_tracked_failure_start(tracked);
    int result = install(zlib_module, bz2_module, lzma_module, NULL);
    return test_tracked_failure_finish(result);
}

int
aleff_test_registry_allocation_failure(
    void *register_pointer,
    void *predicate_pointer,
    void *clear_pointer,
    PyObject *callable,
    int mode
)
{
    typedef int (*register_function)(PyObject *);
    typedef int (*predicate_function)(PyObject *);
    typedef void (*clear_function)(void);
    _Static_assert(
        sizeof(register_function) == sizeof(void *) &&
        sizeof(predicate_function) == sizeof(void *) &&
        sizeof(clear_function) == sizeof(void *),
        "function pointers must fit in void * for this test"
    );
    register_function register_callable;
    predicate_function is_registered;
    clear_function clear_registry;
    memcpy(&register_callable, &register_pointer, sizeof(register_callable));
    memcpy(&is_registered, &predicate_pointer, sizeof(is_registered));
    memcpy(&clear_registry, &clear_pointer, sizeof(clear_registry));

    clear_registry();
    if (mode == 0) {
        PyGC_Collect();
        test_allocator_install(PYMEM_DOMAIN_OBJ);
        fail_malloc_enabled = 1;
        fail_malloc_after = 1;
        malloc_calls = 0;
    }
    else {
        test_allocator_install(PYMEM_DOMAIN_MEM);
        fail_allocation_once = 1;
    }

    int result = register_callable(callable);
    int had_error = PyErr_Occurred() != NULL;
    int is_memory_error = PyErr_ExceptionMatches(PyExc_MemoryError);
    PyErr_Clear();
    aleff_test_allocator_restore();

    int registered_after_failure = is_registered(callable);
    int retry = register_callable(callable);
    int retry_error = PyErr_Occurred() != NULL;
    PyErr_Clear();
    int registered_after_retry = is_registered(callable);
    clear_registry();

    return (result < 0 ? 1 : 0) | (had_error ? 2 : 0) |
        (is_memory_error ? 4 : 0) | (registered_after_failure ? 8 : 0) |
        (retry < 0 ? 16 : 0) | (retry_error ? 32 : 0) |
        (!registered_after_retry ? 64 : 0);
}

int
aleff_test_call_install_registry_failure(
    void *function_pointer,
    PyObject *tracked,
    PyObject *original_dir,
    PyObject *dir_key
)
{
    typedef int (*install_function)(void);
    _Static_assert(
        sizeof(install_function) == sizeof(void *),
        "function pointers must fit in void * for this test"
    );
    install_function install;
    memcpy(&install, &function_pointer, sizeof(install));
    PyObject *builtins = PyEval_GetBuiltins();

    test_allocator_install(PYMEM_DOMAIN_MEM);
    fail_malloc_enabled = 1;
    fail_malloc_once = 1;
    fail_calloc_enabled = 0;
    malloc_calls = 0;
    tracked_reference = tracked;
    tracked_reference_count = Py_REFCNT(tracked);
    tracked_reference_acquired = 0;

    int result = install();
    int had_error = PyErr_Occurred() != NULL;
    int is_memory_error = PyErr_ExceptionMatches(PyExc_MemoryError);
    PyObject *current_dir = PyDict_GetItemWithError(builtins, dir_key);
    int mutated = current_dir != original_dir;
    int acquired = tracked_reference_acquired;
    tracked_reference = NULL;
    PyErr_Clear();
    aleff_test_allocator_restore();

    return (result < 0 ? 1 : 0) | (had_error ? 2 : 0) |
        (mutated ? 4 : 0) | (is_memory_error ? 8 : 0) |
        (acquired ? 16 : 0);
}

void
aleff_test_allocator_restore(void)
{
    if (allocator_installed) {
        fail_malloc_enabled = 0;
        fail_malloc_once = 0;
        fail_calloc_enabled = 0;
        fail_allocation_once = 0;
        PyMem_SetAllocator(allocator_domain, &previous_allocator);
        allocator_installed = 0;
    }
}
