#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>

static PyMemAllocatorEx previous_allocator;
static PyMemAllocatorEx failing_allocator;
static int allocator_domain = -1;
static int allocator_installed = 0;
static int fail_malloc_enabled = 0;
static int fail_calloc_enabled = 0;
static size_t malloc_calls = 0;
static size_t fail_malloc_after = 0;

static void *test_realloc(void *context, void *pointer, size_t size);
static void test_free(void *context, void *pointer);
void aleff_test_allocator_restore(void);

static void *
test_malloc(void *context, size_t size)
{
    malloc_calls++;
    if (fail_malloc_enabled && malloc_calls >= fail_malloc_after) {
        return NULL;
    }
    return previous_allocator.malloc(context, size);
}

static void *
test_calloc(void *context, size_t nelem, size_t elsize)
{
    if (fail_calloc_enabled) {
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

void
aleff_test_allocator_restore(void)
{
    if (allocator_installed) {
        fail_malloc_enabled = 0;
        fail_calloc_enabled = 0;
        PyMem_SetAllocator(allocator_domain, &previous_allocator);
        allocator_installed = 0;
    }
}
