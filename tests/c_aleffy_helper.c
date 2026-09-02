#define PY_SSIZE_T_CLEAN
#include <Python.h>

static int before_count = 0;
static int after_count = 0;

void
aleff_test_aleffy_reset(void)
{
    before_count = 0;
    after_count = 0;
}

int
aleff_test_aleffy_before_count(void)
{
    return before_count;
}

int
aleff_test_aleffy_after_count(void)
{
    return after_count;
}

PyObject *
aleff_test_aleffy_call(PyObject *callback)
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
