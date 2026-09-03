#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdatomic.h>
#include <stddef.h>

static int before_count = 0;
static int after_count = 0;
static _PyFrameEvalFunction previous_eval = NULL;
static unsigned long eval_hook_owner = 0;
static _Atomic long owner_thread_eval_count = 0;
static _Atomic long other_thread_eval_count = 0;

static PyObject *
aleff_test_counting_eval(
    PyThreadState *thread,
    struct _PyInterpreterFrame *frame,
    int throwflag
)
{
    if (PyThread_get_thread_ident() == eval_hook_owner) {
        atomic_fetch_add_explicit(
            &owner_thread_eval_count,
            1,
            memory_order_relaxed
        );
    }
    else {
        atomic_fetch_add_explicit(
            &other_thread_eval_count,
            1,
            memory_order_relaxed
        );
    }
    return previous_eval(thread, frame, throwflag);
}

static PyObject *
aleff_test_eval_hook_install(
    PyObject *Py_UNUSED(self),
    PyObject *Py_UNUSED(ignored)
)
{
    PyInterpreterState *interpreter = PyThreadState_GetInterpreter(PyThreadState_Get());
    previous_eval = _PyInterpreterState_GetEvalFrameFunc(interpreter);
    eval_hook_owner = PyThread_get_thread_ident();
    atomic_store_explicit(&owner_thread_eval_count, 0, memory_order_relaxed);
    atomic_store_explicit(&other_thread_eval_count, 0, memory_order_relaxed);
    _PyInterpreterState_SetEvalFrameFunc(interpreter, aleff_test_counting_eval);
    Py_RETURN_NONE;
}

static PyObject *
aleff_test_eval_hook_uninstall(
    PyObject *Py_UNUSED(self),
    PyObject *Py_UNUSED(ignored)
)
{
    PyInterpreterState *interpreter = PyThreadState_GetInterpreter(PyThreadState_Get());
    if (_PyInterpreterState_GetEvalFrameFunc(interpreter) == aleff_test_counting_eval) {
        _PyInterpreterState_SetEvalFrameFunc(interpreter, previous_eval);
    }
    previous_eval = NULL;
    Py_RETURN_NONE;
}

static PyObject *
aleff_test_eval_hook_reset(
    PyObject *Py_UNUSED(self),
    PyObject *Py_UNUSED(ignored)
)
{
    atomic_store_explicit(&owner_thread_eval_count, 0, memory_order_relaxed);
    atomic_store_explicit(&other_thread_eval_count, 0, memory_order_relaxed);
    Py_RETURN_NONE;
}

static PyObject *
aleff_test_eval_hook_owner_thread_count(
    PyObject *Py_UNUSED(self),
    PyObject *Py_UNUSED(ignored)
)
{
    return PyLong_FromLong(
        atomic_load_explicit(&owner_thread_eval_count, memory_order_relaxed)
    );
}

static PyObject *
aleff_test_eval_hook_other_thread_count(
    PyObject *Py_UNUSED(self),
    PyObject *Py_UNUSED(ignored)
)
{
    return PyLong_FromLong(
        atomic_load_explicit(&other_thread_eval_count, memory_order_relaxed)
    );
}

static PyObject *
aleff_test_eval_hook_is_current(
    PyObject *Py_UNUSED(self),
    PyObject *Py_UNUSED(ignored)
)
{
    PyInterpreterState *interpreter = PyThreadState_GetInterpreter(PyThreadState_Get());
    return PyBool_FromLong(
        _PyInterpreterState_GetEvalFrameFunc(interpreter) == aleff_test_counting_eval
    );
}

static PyObject *
aleff_test_aleffy_reset(
    PyObject *Py_UNUSED(self),
    PyObject *Py_UNUSED(ignored)
)
{
    before_count = 0;
    after_count = 0;
    Py_RETURN_NONE;
}

static PyObject *
aleff_test_aleffy_before_count(
    PyObject *Py_UNUSED(self),
    PyObject *Py_UNUSED(ignored)
)
{
    return PyLong_FromLong(before_count);
}

static PyObject *
aleff_test_aleffy_after_count(
    PyObject *Py_UNUSED(self),
    PyObject *Py_UNUSED(ignored)
)
{
    return PyLong_FromLong(after_count);
}

static PyObject *
aleff_test_aleffy_call(PyObject *Py_UNUSED(self), PyObject *callback)
{
    volatile long sentinel = 700;
    before_count++;
    PyObject *value = PyObject_CallNoArgs(callback);
    if (value == NULL) {
        return NULL;
    }
    long number = PyLong_AsLong(value);
    Py_DECREF(value);
    if (number == -1 && PyErr_Occurred()) {
        return NULL;
    }
    after_count++;
    return PyLong_FromLong(sentinel + number);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static PyObject *
aleff_test_deep_call(PyObject *callback, unsigned int depth)
{
    volatile unsigned char stack_padding[1024];
    stack_padding[0] = (unsigned char)depth;
    stack_padding[sizeof(stack_padding) - 1] = (unsigned char)(depth >> 8);

    PyObject *result;
    if (depth == 0) {
        result = PyObject_CallNoArgs(callback);
    }
    else {
        result = aleff_test_deep_call(callback, depth - 1);
    }

    if (stack_padding[0] != (unsigned char)depth ||
        stack_padding[sizeof(stack_padding) - 1] != (unsigned char)(depth >> 8)) {
        Py_XDECREF(result);
        PyErr_SetString(PyExc_RuntimeError, "native stack was not restored");
        return NULL;
    }
    return result;
}

static PyObject *
aleff_test_aleffy_deep_call(PyObject *Py_UNUSED(self), PyObject *args)
{
    PyObject *callback;
    unsigned int depth;
    if (!PyArg_ParseTuple(args, "OI:aleff_test_aleffy_deep_call", &callback, &depth)) {
        return NULL;
    }
    return aleff_test_deep_call(callback, depth);
}

static PyObject *
aleff_test_aleffy_call_twice(PyObject *Py_UNUSED(self), PyObject *callback)
{
    volatile long sentinel = 1000;
    before_count++;
    PyObject *first_value = PyObject_CallNoArgs(callback);
    if (first_value == NULL) {
        return NULL;
    }
    long first = PyLong_AsLong(first_value);
    Py_DECREF(first_value);
    if (first == -1 && PyErr_Occurred()) {
        return NULL;
    }

    PyObject *second_value = PyObject_CallNoArgs(callback);
    if (second_value == NULL) {
        return NULL;
    }
    long second = PyLong_AsLong(second_value);
    Py_DECREF(second_value);
    if (second == -1 && PyErr_Occurred()) {
        return NULL;
    }
    after_count++;
    return PyLong_FromLong(sentinel * first + second);
}

static PyMethodDef aleff_test_methods[] = {
    {"aleff_test_eval_hook_install", aleff_test_eval_hook_install, METH_NOARGS, NULL},
    {"aleff_test_eval_hook_uninstall", aleff_test_eval_hook_uninstall, METH_NOARGS, NULL},
    {"aleff_test_eval_hook_reset", aleff_test_eval_hook_reset, METH_NOARGS, NULL},
    {"aleff_test_eval_hook_owner_thread_count", aleff_test_eval_hook_owner_thread_count, METH_NOARGS, NULL},
    {"aleff_test_eval_hook_other_thread_count", aleff_test_eval_hook_other_thread_count, METH_NOARGS, NULL},
    {"aleff_test_eval_hook_is_current", aleff_test_eval_hook_is_current, METH_NOARGS, NULL},
    {"aleff_test_aleffy_reset", aleff_test_aleffy_reset, METH_NOARGS, NULL},
    {"aleff_test_aleffy_before_count", aleff_test_aleffy_before_count, METH_NOARGS, NULL},
    {"aleff_test_aleffy_after_count", aleff_test_aleffy_after_count, METH_NOARGS, NULL},
    {"aleff_test_aleffy_call", aleff_test_aleffy_call, METH_O, NULL},
    {"aleff_test_aleffy_deep_call", aleff_test_aleffy_deep_call, METH_VARARGS, NULL},
    {"aleff_test_aleffy_call_twice", aleff_test_aleffy_call_twice, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef aleff_test_module = {
    PyModuleDef_HEAD_INIT,
    "aleffy_test_helper",
    NULL,
    -1,
    aleff_test_methods,
};

PyMODINIT_FUNC
PyInit_aleffy_test_helper(void)
{
    return PyModule_Create(&aleff_test_module);
}
