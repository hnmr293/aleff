#include "binascii.h"
#include "buffers.h"
#include "module_functions.h"

#define ARRAY_SIZE(array) ((Py_ssize_t)(sizeof(array) / sizeof(*(array))))

static const char *const function_names[] = {
    "a2b_base64", "a2b_hex", "a2b_qp", "a2b_uu",
    "b2a_base64", "b2a_hex", "b2a_qp", "b2a_uu",
    "crc32", "crc_hqx", "hexlify", "unhexlify",
};
static const char *const argument_names[] = {
    NULL, NULL, "data", NULL,
    NULL, NULL, "data", NULL,
    NULL, NULL, NULL, NULL,
};
static PyObject *originals[ARRAY_SIZE(function_names)];
static PyMethodDef methods[ARRAY_SIZE(function_names)];
static PyObject *installed_module;
static int installed;

static PyObject *
binascii_call(Py_ssize_t index, PyObject *args, PyObject *kwargs)
{
    AleffBufferArgument argument = {
        .position = 0,
        .keyword = argument_names[index],
        .flags = PyBUF_SIMPLE,
    };
    return adapter_buffer_call(
        originals[index], NULL, args, kwargs, &argument, 1
    );
}

#define BINASCII_WRAPPER(number) \
    static PyObject *binascii_wrapper_##number( \
        PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs \
    ) { return binascii_call(number, args, kwargs); }

BINASCII_WRAPPER(0)
BINASCII_WRAPPER(1)
BINASCII_WRAPPER(2)
BINASCII_WRAPPER(3)
BINASCII_WRAPPER(4)
BINASCII_WRAPPER(5)
BINASCII_WRAPPER(6)
BINASCII_WRAPPER(7)
BINASCII_WRAPPER(8)
BINASCII_WRAPPER(9)
BINASCII_WRAPPER(10)
BINASCII_WRAPPER(11)

static PyCFunction wrappers[] = {
    _PyCFunction_CAST(binascii_wrapper_0),
    _PyCFunction_CAST(binascii_wrapper_1),
    _PyCFunction_CAST(binascii_wrapper_2),
    _PyCFunction_CAST(binascii_wrapper_3),
    _PyCFunction_CAST(binascii_wrapper_4),
    _PyCFunction_CAST(binascii_wrapper_5),
    _PyCFunction_CAST(binascii_wrapper_6),
    _PyCFunction_CAST(binascii_wrapper_7),
    _PyCFunction_CAST(binascii_wrapper_8),
    _PyCFunction_CAST(binascii_wrapper_9),
    _PyCFunction_CAST(binascii_wrapper_10),
    _PyCFunction_CAST(binascii_wrapper_11),
};

int
adapter_binascii_install(PyObject *module)
{
    if (installed) {
        return 0;
    }
    installed_module = Py_NewRef(module);
    if (adapter_module_functions_install(
            module,
            "binascii",
            function_names,
            originals,
            methods,
            wrappers,
            ARRAY_SIZE(function_names),
            ALEFF_MODULE_FUNCTION_REQUIRE_C
        ) < 0) {
        adapter_binascii_rollback();
        return -1;
    }
    installed = 1;
    return 0;
}

void
adapter_binascii_rollback(void)
{
    if (installed_module == NULL) {
        return;
    }
    PyObject *raised = PyErr_GetRaisedException();
    for (Py_ssize_t index = 0; index < ARRAY_SIZE(function_names); index++) {
        if (originals[index] != NULL) {
            if (PyObject_SetAttrString(
                    installed_module, function_names[index], originals[index]
                ) < 0) {
                PyErr_Clear();
            }
            Py_CLEAR(originals[index]);
        }
    }
    Py_CLEAR(installed_module);
    installed = 0;
    PyErr_SetRaisedException(raised);
}
