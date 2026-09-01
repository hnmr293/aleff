#include "buffers.h"
#include "hashing.h"
#include "module_functions.h"

#define ARRAY_SIZE(array) ((Py_ssize_t)(sizeof(array) / sizeof(*(array))))
#define NO_POSITION PY_SSIZE_T_MAX

static const char *const hash_names[] = {
    "new", "md5", "sha1", "sha224", "sha256", "sha384", "sha512",
    "sha3_224", "sha3_256", "sha3_384", "sha3_512", "shake_128",
    "shake_256",
};
static PyObject *hash_originals[ARRAY_SIZE(hash_names)];
static PyMethodDef hash_methods[ARRAY_SIZE(hash_names)];

static const AleffBufferArgument hash_new_arguments[] = {
#if PY_VERSION_HEX >= 0x030d0000
    {
        .position = 1,
        .keyword = "data",
        .exclusive_keyword = "string",
        .flags = PyBUF_SIMPLE,
    },
    {.position = NO_POSITION, .keyword = "string", .flags = PyBUF_SIMPLE},
#else
    {.position = 1, .keyword = "data", .flags = PyBUF_SIMPLE},
#endif
};
static const AleffBufferArgument hash_data_arguments[] = {
#if PY_VERSION_HEX >= 0x030d0000
    {
        .position = 0,
        .keyword = "data",
        .exclusive_keyword = "string",
        .flags = PyBUF_SIMPLE,
    },
    {.position = NO_POSITION, .keyword = "string", .flags = PyBUF_SIMPLE},
#else
    {.position = 0, .keyword = "string", .flags = PyBUF_SIMPLE},
#endif
};
static const AleffBufferArgument hash_update_arguments[] = {
    {.position = 0, .keyword = NULL, .flags = PyBUF_SIMPLE},
};
static const AleffBufferArgument blake_arguments[] = {
#if PY_VERSION_HEX >= 0x030d0000
    {
        .position = 0,
        .keyword = "data",
        .exclusive_keyword = "string",
        .flags = PyBUF_SIMPLE,
    },
    {.position = NO_POSITION, .keyword = "string", .flags = PyBUF_SIMPLE},
#else
    {.position = 0, .keyword = NULL, .flags = PyBUF_SIMPLE},
#endif
    {.position = NO_POSITION, .keyword = "key", .flags = PyBUF_SIMPLE},
    {.position = NO_POSITION, .keyword = "salt", .flags = PyBUF_SIMPLE},
    {.position = NO_POSITION, .keyword = "person", .flags = PyBUF_SIMPLE},
};
static const AleffBufferArgument hmac_arguments[] = {
    {
        .position = 0,
        .keyword = "key",
        .flags = PyBUF_SIMPLE,
        .make_bytearray = 1,
    },
    {.position = 1, .keyword = "msg", .flags = PyBUF_SIMPLE},
};

static PyObject *hashlib_module;
static PyObject *hmac_module;
static PyObject *hmac_originals[2];
static PyMethodDef hmac_methods[2];
static const char *const hmac_names[] = {"new", "digest"};

static PyTypeObject *hash_types[16];
static PyObject *hash_update_originals[16];
static PyMethodDef hash_update_methods[16];
static Py_ssize_t hash_type_count;

static PyTypeObject *blake_types[2];
static newfunc blake_original_new[2];

static int hashing_installed;

static PyObject *
hash_call(Py_ssize_t index, PyObject *args, PyObject *kwargs)
{
    const AleffBufferArgument *arguments = index == 0
        ? hash_new_arguments : hash_data_arguments;
    Py_ssize_t argument_count = index == 0
        ? ARRAY_SIZE(hash_new_arguments) : ARRAY_SIZE(hash_data_arguments);
    return adapter_buffer_function(
        hash_originals[index], args, kwargs,
        arguments, argument_count
    );
}

#define HASH_WRAPPER(number) \
    static PyObject *hash_wrapper_##number( \
        PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs \
    ) { return hash_call(number, args, kwargs); }

HASH_WRAPPER(0)
HASH_WRAPPER(1)
HASH_WRAPPER(2)
HASH_WRAPPER(3)
HASH_WRAPPER(4)
HASH_WRAPPER(5)
HASH_WRAPPER(6)
HASH_WRAPPER(7)
HASH_WRAPPER(8)
HASH_WRAPPER(9)
HASH_WRAPPER(10)
HASH_WRAPPER(11)
HASH_WRAPPER(12)

static PyCFunction hash_wrappers[] = {
    _PyCFunction_CAST(hash_wrapper_0), _PyCFunction_CAST(hash_wrapper_1),
    _PyCFunction_CAST(hash_wrapper_2), _PyCFunction_CAST(hash_wrapper_3),
    _PyCFunction_CAST(hash_wrapper_4), _PyCFunction_CAST(hash_wrapper_5),
    _PyCFunction_CAST(hash_wrapper_6), _PyCFunction_CAST(hash_wrapper_7),
    _PyCFunction_CAST(hash_wrapper_8), _PyCFunction_CAST(hash_wrapper_9),
    _PyCFunction_CAST(hash_wrapper_10), _PyCFunction_CAST(hash_wrapper_11),
    _PyCFunction_CAST(hash_wrapper_12),
};

static PyObject *
hmac_call(Py_ssize_t index, PyObject *args, PyObject *kwargs)
{
    return adapter_buffer_function(
        hmac_originals[index], args, kwargs,
        hmac_arguments, ARRAY_SIZE(hmac_arguments)
    );
}

static PyObject *
hmac_new_wrapper(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    return hmac_call(0, args, kwargs);
}

static PyObject *
hmac_digest_wrapper(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    return hmac_call(1, args, kwargs);
}

static PyObject *
hash_update_wrapper(PyObject *self, PyObject *args, PyObject *kwargs)
{
    for (Py_ssize_t index = 0; index < hash_type_count; index++) {
        if (PyObject_TypeCheck(self, hash_types[index])) {
            return adapter_buffer_method(
                hash_update_originals[index], self, args, kwargs,
                hash_update_arguments, ARRAY_SIZE(hash_update_arguments)
            );
        }
    }
    PyErr_SetString(PyExc_RuntimeError, "unknown hash update receiver");
    return NULL;
}

static PyObject *
blake_new_wrapper(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    for (Py_ssize_t index = 0; index < 2; index++) {
        if (type == blake_types[index]) {
            return adapter_buffer_new(
                blake_original_new[index], type, args, kwargs,
                blake_arguments, ARRAY_SIZE(blake_arguments)
            );
        }
    }
    PyErr_SetString(PyExc_RuntimeError, "unknown BLAKE2 constructor");
    return NULL;
}

static int
install_hash_update(PyObject *hash_object)
{
    PyTypeObject *receiver_type = Py_TYPE(hash_object);
    PyTypeObject *type = NULL;
    PyObject *original = NULL;
    PyObject *mro = receiver_type->tp_mro;
    if (mro != NULL) {
        for (Py_ssize_t index = 0; index < PyTuple_GET_SIZE(mro); index++) {
            PyTypeObject *candidate = (PyTypeObject *)PyTuple_GET_ITEM(mro, index);
            PyObject *dict = PyType_GetDict(candidate);
            PyObject *descriptor = dict == NULL
                ? NULL : PyDict_GetItemString(dict, "update");
            if (descriptor != NULL) {
                type = candidate;
                original = descriptor;
                break;
            }
        }
    }
    if (type == NULL || original == NULL ||
        !Py_IS_TYPE(original, &PyMethodDescr_Type)) {
        PyErr_SetString(PyExc_RuntimeError, "hash update is not a method descriptor");
        return -1;
    }
    for (Py_ssize_t index = 0; index < hash_type_count; index++) {
        if (hash_types[index] == type) {
            return 0;
        }
    }
    if (hash_type_count >= ARRAY_SIZE(hash_types)) {
        PyErr_SetString(PyExc_RuntimeError, "too many hash object types");
        return -1;
    }
    PyObject *dict = PyType_GetDict(type);
    Py_ssize_t index = hash_type_count;
    PyObject *owned_original = Py_NewRef(original);
    hash_update_methods[index] = *((PyMethodDescrObject *)original)->d_method;
    hash_update_methods[index].ml_meth = _PyCFunction_CAST(hash_update_wrapper);
    hash_update_methods[index].ml_flags = METH_VARARGS | METH_KEYWORDS;
    PyObject *replacement = PyDescr_NewMethod(type, &hash_update_methods[index]);
    if (replacement == NULL) {
        Py_DECREF(owned_original);
        return -1;
    }
    int status = PyDict_SetItemString(dict, "update", replacement);
    Py_DECREF(replacement);
    if (status < 0) {
        Py_DECREF(owned_original);
        return -1;
    }
    hash_types[index] = type;
    hash_update_originals[index] = owned_original;
    PyType_Modified(type);
    hash_type_count++;
    return 0;
}

static int
capture_hash_types(PyObject *module)
{
    for (Py_ssize_t index = 1; index < ARRAY_SIZE(hash_names); index++) {
        PyObject *constructor = PyObject_GetAttrString(module, hash_names[index]);
        if (constructor == NULL) {
            return -1;
        }
        PyObject *hash_object = PyObject_CallNoArgs(constructor);
        Py_DECREF(constructor);
        if (hash_object == NULL) {
            return -1;
        }
        int status = install_hash_update(hash_object);
        Py_DECREF(hash_object);
        if (status < 0) {
            return -1;
        }
    }
    return 0;
}

static int
install_blake_types(PyObject *module)
{
    const char *names[2] = {"blake2b", "blake2s"};
    for (Py_ssize_t index = 0; index < 2; index++) {
        PyObject *object = PyObject_GetAttrString(module, names[index]);
        if (object == NULL || !PyType_Check(object)) {
            Py_XDECREF(object);
            PyErr_SetString(PyExc_RuntimeError, "BLAKE2 constructor is not a type");
            return -1;
        }
        blake_types[index] = (PyTypeObject *)object;
        PyObject *hash_object = PyObject_CallNoArgs(object);
        if (hash_object == NULL) {
            Py_DECREF(object);
            return -1;
        }
        int update_status = install_hash_update(hash_object);
        Py_DECREF(hash_object);
        if (update_status < 0) {
            Py_DECREF(object);
            return -1;
        }
        blake_original_new[index] = blake_types[index]->tp_new;
        blake_types[index]->tp_new = blake_new_wrapper;
        PyType_Modified(blake_types[index]);
        Py_DECREF(object);
    }
    return 0;
}

int
adapter_hashing_install(PyObject *hashlib, PyObject *hmac)
{
    if (hashing_installed) {
        return 0;
    }
    hashlib_module = Py_NewRef(hashlib);
    hmac_module = Py_NewRef(hmac);
    if (capture_hash_types(hashlib) < 0 || install_blake_types(hashlib) < 0) {
        adapter_hashing_rollback();
        return -1;
    }
    if (adapter_module_functions_install(
            hashlib, "hashlib", hash_names, hash_originals, hash_methods,
            hash_wrappers, ARRAY_SIZE(hash_names),
            ALEFF_MODULE_FUNCTION_ALLOW_CALLABLE
        ) < 0) {
        adapter_hashing_rollback();
        return -1;
    }
    PyCFunction hmac_wrappers[2] = {
        _PyCFunction_CAST(hmac_new_wrapper),
        _PyCFunction_CAST(hmac_digest_wrapper),
    };
    if (adapter_module_functions_install(
            hmac, "hmac", hmac_names, hmac_originals, hmac_methods,
            hmac_wrappers, ARRAY_SIZE(hmac_names),
            ALEFF_MODULE_FUNCTION_ALLOW_CALLABLE
        ) < 0) {
        adapter_hashing_rollback();
        return -1;
    }
    hashing_installed = 1;
    return 0;
}

void
adapter_hashing_rollback(void)
{
    PyObject *raised = PyErr_GetRaisedException();
    if (hashlib_module != NULL) {
        for (Py_ssize_t index = 0; index < ARRAY_SIZE(hash_names); index++) {
            if (hash_originals[index] != NULL) {
                if (PyObject_SetAttrString(
                        hashlib_module, hash_names[index], hash_originals[index]
                    ) < 0) {
                    PyErr_Clear();
                }
                Py_CLEAR(hash_originals[index]);
            }
        }
    }
    if (hmac_module != NULL) {
        for (Py_ssize_t index = 0; index < ARRAY_SIZE(hmac_names); index++) {
            if (hmac_originals[index] != NULL) {
                if (PyObject_SetAttrString(
                        hmac_module, hmac_names[index], hmac_originals[index]
                    ) < 0) {
                    PyErr_Clear();
                }
                Py_CLEAR(hmac_originals[index]);
            }
        }
    }
    for (Py_ssize_t index = 0; index < hash_type_count; index++) {
        PyObject *dict = PyType_GetDict(hash_types[index]);
        if (dict != NULL && hash_update_originals[index] != NULL &&
            PyDict_SetItemString(dict, "update", hash_update_originals[index]) < 0) {
            PyErr_Clear();
        }
        Py_CLEAR(hash_update_originals[index]);
        PyType_Modified(hash_types[index]);
        hash_types[index] = NULL;
    }
    hash_type_count = 0;
    for (Py_ssize_t index = 0; index < 2; index++) {
        if (blake_types[index] != NULL && blake_original_new[index] != NULL) {
            blake_types[index]->tp_new = blake_original_new[index];
            PyType_Modified(blake_types[index]);
        }
        blake_types[index] = NULL;
        blake_original_new[index] = NULL;
    }
    Py_CLEAR(hashlib_module);
    Py_CLEAR(hmac_module);
    hashing_installed = 0;
    PyErr_SetRaisedException(raised);
}
