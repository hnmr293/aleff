#include "buffers.h"
#include "compression.h"

#define ARRAY_SIZE(array) ((Py_ssize_t)(sizeof(array) / sizeof(*(array))))

static const char *const zlib_function_names[] = {
    "adler32", "crc32", "compress", "decompress",
};
static PyObject *zlib_originals[ARRAY_SIZE(zlib_function_names)];
static PyMethodDef zlib_methods[ARRAY_SIZE(zlib_function_names)];
static PyObject *installed_zlib;

static const char *const stream_function_names[] = {"compress", "decompress"};
static PyObject *bz2_originals[ARRAY_SIZE(stream_function_names)];
static PyMethodDef bz2_methods[ARRAY_SIZE(stream_function_names)];
static PyObject *lzma_originals[ARRAY_SIZE(stream_function_names)];
static PyMethodDef lzma_methods[ARRAY_SIZE(stream_function_names)];

typedef struct {
    PyTypeObject *type;
    PyObject *original;
    PyMethodDef method;
    const char *keyword;
} CompressionMethod;

static CompressionMethod compression_methods[8];
static Py_ssize_t compression_method_count;

static PyObject *installed_bz2;
static PyObject *installed_lzma;
static PyObject *installed_zstd;
static PyObject *zstd_frame_original;
static PyMethodDef zstd_frame_method;
static int installed;

static PyObject *
zlib_call(Py_ssize_t index, PyObject *args, PyObject *kwargs)
{
    AleffBufferArgument argument = {
        .position = 0,
        .keyword = NULL,
        .flags = PyBUF_SIMPLE,
    };
    return adapter_buffer_call(
        zlib_originals[index], NULL, args, kwargs, &argument, 1
    );
}

#define ZLIB_WRAPPER(number) \
    static PyObject *zlib_wrapper_##number( \
        PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs \
    ) { return zlib_call(number, args, kwargs); }

ZLIB_WRAPPER(0)
ZLIB_WRAPPER(1)
ZLIB_WRAPPER(2)
ZLIB_WRAPPER(3)

static PyCFunction zlib_wrappers[] = {
    _PyCFunction_CAST(zlib_wrapper_0),
    _PyCFunction_CAST(zlib_wrapper_1),
    _PyCFunction_CAST(zlib_wrapper_2),
    _PyCFunction_CAST(zlib_wrapper_3),
};

static PyObject *
stream_function_call(
    PyObject *original,
    PyObject *args,
    PyObject *kwargs
)
{
    AleffBufferArgument argument = {
        .position = 0,
        .keyword = "data",
        .flags = PyBUF_SIMPLE,
    };
    return adapter_buffer_call(original, NULL, args, kwargs, &argument, 1);
}

static PyObject *
bz2_compress_wrapper(
    PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs
)
{
    return stream_function_call(bz2_originals[0], args, kwargs);
}

static PyObject *
bz2_decompress_wrapper(
    PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs
)
{
    return stream_function_call(bz2_originals[1], args, kwargs);
}

static PyObject *
lzma_compress_wrapper(
    PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs
)
{
    return stream_function_call(lzma_originals[0], args, kwargs);
}

static PyObject *
lzma_decompress_wrapper(
    PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs
)
{
    return stream_function_call(lzma_originals[1], args, kwargs);
}

static PyCFunction bz2_wrappers[] = {
    _PyCFunction_CAST(bz2_compress_wrapper),
    _PyCFunction_CAST(bz2_decompress_wrapper),
};
static PyCFunction lzma_wrappers[] = {
    _PyCFunction_CAST(lzma_compress_wrapper),
    _PyCFunction_CAST(lzma_decompress_wrapper),
};

static PyObject *
compression_method_wrapper(PyObject *self, PyObject *args, PyObject *kwargs)
{
    for (Py_ssize_t index = 0; index < compression_method_count; index++) {
        CompressionMethod *method = &compression_methods[index];
        if (Py_TYPE(self) == method->type) {
            AleffBufferArgument argument = {
                .position = 0,
                .keyword = method->keyword,
                .flags = PyBUF_SIMPLE,
            };
            return adapter_buffer_call(
                method->original, self, args, kwargs, &argument, 1
            );
        }
    }
    PyErr_SetString(PyExc_RuntimeError, "unknown compression method receiver");
    return NULL;
}

static PyObject *
zstd_frame_wrapper(PyObject *Py_UNUSED(self), PyObject *args, PyObject *kwargs)
{
    AleffBufferArgument argument = {
        .position = 0,
        .keyword = "frame_buffer",
        .flags = PyBUF_SIMPLE,
    };
    return adapter_buffer_call(
        zstd_frame_original, NULL, args, kwargs, &argument, 1
    );
}

static PyObject *
make_function_replacement(
    PyObject *original,
    const char *name,
    PyCFunction wrapper,
    PyMethodDef *method
)
{
    if (PyCFunction_Check(original)) {
        *method = *((PyCFunctionObject *)original)->m_ml;
    }
    else {
        *method = (PyMethodDef){
            .ml_name = name,
            .ml_meth = NULL,
            .ml_flags = 0,
            .ml_doc = NULL,
        };
    }
    method->ml_meth = wrapper;
    method->ml_flags = METH_VARARGS | METH_KEYWORDS;
    PyObject *module_name = PyObject_GetAttrString(original, "__module__");
    if (module_name == NULL) {
        return NULL;
    }
    PyObject *self = PyCFunction_Check(original)
        ? PyCFunction_GET_SELF(original) : NULL;
    PyObject *bridge = PyCFunction_NewEx(method, self, module_name);
    Py_DECREF(module_name);
    if (bridge == NULL || !PyFunction_Check(original)) {
        return bridge;
    }
    PyObject *replacement = adapter_wrap_python_callable(original, bridge);
    Py_DECREF(bridge);
    return replacement;
}

static int
install_module_functions(
    PyObject *module,
    const char *const *names,
    PyObject **originals,
    PyMethodDef *methods,
    PyCFunction *wrappers,
    Py_ssize_t count
)
{
    for (Py_ssize_t index = 0; index < count; index++) {
        originals[index] = PyObject_GetAttrString(module, names[index]);
        if (originals[index] == NULL) {
            return -1;
        }
        PyObject *replacement = make_function_replacement(
            originals[index], names[index], wrappers[index], &methods[index]
        );
        if (replacement == NULL) {
            return -1;
        }
        int status = PyObject_SetAttrString(
            module, names[index], replacement
        );
        Py_DECREF(replacement);
        if (status < 0) {
            return -1;
        }
    }
    return 0;
}

static int
install_compression_method(
    PyObject *type_object,
    const char *name,
    const char *keyword
)
{
    if (!PyType_Check(type_object)) {
        PyErr_SetString(PyExc_RuntimeError, "compression receiver is not a type");
        return -1;
    }
    if (compression_method_count >= ARRAY_SIZE(compression_methods)) {
        PyErr_SetString(PyExc_RuntimeError, "too many compression method types");
        return -1;
    }
    PyTypeObject *type = (PyTypeObject *)type_object;
    PyObject *dict = PyType_GetDict(type);
    PyObject *original = dict == NULL ? NULL : PyDict_GetItemString(dict, name);
    if (original == NULL || !Py_IS_TYPE(original, &PyMethodDescr_Type)) {
        PyErr_SetString(PyExc_RuntimeError, "compression method is not a descriptor");
        return -1;
    }
    CompressionMethod *entry = &compression_methods[compression_method_count];
    entry->type = type;
    entry->original = Py_NewRef(original);
    entry->method = *((PyMethodDescrObject *)original)->d_method;
    entry->method.ml_meth = _PyCFunction_CAST(compression_method_wrapper);
    entry->method.ml_flags = METH_VARARGS | METH_KEYWORDS;
    entry->keyword = keyword;
    PyObject *replacement = PyDescr_NewMethod(type, &entry->method);
    if (replacement == NULL) {
        return -1;
    }
    int status = PyDict_SetItemString(dict, name, replacement);
    Py_DECREF(replacement);
    if (status < 0) {
        return -1;
    }
    compression_method_count++;
    PyType_Modified(type);
    return 0;
}

static int
install_exported_method(
    PyObject *module,
    const char *type_name,
    const char *method_name,
    const char *keyword
)
{
    PyObject *type = PyObject_GetAttrString(module, type_name);
    if (type == NULL) {
        return -1;
    }
    int status = install_compression_method(type, method_name, keyword);
    Py_DECREF(type);
    return status;
}

static int
install_zlib_state_methods(PyObject *module)
{
    PyObject *compressor = PyObject_CallMethod(module, "compressobj", NULL);
    PyObject *decompressor = PyObject_CallMethod(module, "decompressobj", NULL);
    if (compressor == NULL || decompressor == NULL) {
        Py_XDECREF(compressor);
        Py_XDECREF(decompressor);
        return -1;
    }
    int status = install_compression_method(
        (PyObject *)Py_TYPE(compressor), "compress", NULL
    );
    if (status == 0) {
        status = install_compression_method(
            (PyObject *)Py_TYPE(decompressor), "decompress", NULL
        );
    }
    Py_DECREF(compressor);
    Py_DECREF(decompressor);
    return status;
}

static int
install_zstd(PyObject *module)
{
    if (module == NULL) {
        return 0;
    }
    zstd_frame_original = PyObject_GetAttrString(module, "get_frame_size");
    if (zstd_frame_original == NULL) {
        return -1;
    }
    PyObject *replacement = make_function_replacement(
        zstd_frame_original,
        "get_frame_size",
        _PyCFunction_CAST(zstd_frame_wrapper),
        &zstd_frame_method
    );
    if (replacement == NULL) {
        return -1;
    }
    int status = PyObject_SetAttrString(module, "get_frame_size", replacement);
    Py_DECREF(replacement);
    if (status < 0) {
        return -1;
    }
    if (install_exported_method(
            module, "ZstdCompressor", "compress", "data"
        ) < 0 ||
        install_exported_method(
            module, "ZstdDecompressor", "decompress", "data"
        ) < 0) {
        return -1;
    }
    return 0;
}

int
adapter_compression_install(
    PyObject *zlib_module,
    PyObject *bz2_module,
    PyObject *lzma_module,
    PyObject *zstd_module
)
{
    if (installed) {
        return 0;
    }
    installed_zlib = Py_NewRef(zlib_module);
    installed_bz2 = Py_NewRef(bz2_module);
    installed_lzma = Py_NewRef(lzma_module);
    installed_zstd = Py_XNewRef(zstd_module);
    if (install_module_functions(
            zlib_module,
            zlib_function_names,
            zlib_originals,
            zlib_methods,
            zlib_wrappers,
            ARRAY_SIZE(zlib_function_names)
        ) < 0 ||
        install_module_functions(
            bz2_module,
            stream_function_names,
            bz2_originals,
            bz2_methods,
            bz2_wrappers,
            ARRAY_SIZE(stream_function_names)
        ) < 0 ||
        install_module_functions(
            lzma_module,
            stream_function_names,
            lzma_originals,
            lzma_methods,
            lzma_wrappers,
            ARRAY_SIZE(stream_function_names)
        ) < 0 ||
        install_zlib_state_methods(zlib_module) < 0 ||
        install_exported_method(
            bz2_module, "BZ2Compressor", "compress", NULL
        ) < 0 ||
        install_exported_method(
            bz2_module, "BZ2Decompressor", "decompress", "data"
        ) < 0 ||
        install_exported_method(
            lzma_module, "LZMACompressor", "compress", NULL
        ) < 0 ||
        install_exported_method(
            lzma_module, "LZMADecompressor", "decompress", "data"
        ) < 0 ||
        install_zstd(zstd_module) < 0) {
        adapter_compression_rollback();
        return -1;
    }
    installed = 1;
    return 0;
}

void
adapter_compression_rollback(void)
{
    if (installed_zlib != NULL) {
        for (Py_ssize_t index = 0; index < ARRAY_SIZE(zlib_function_names); index++) {
            if (zlib_originals[index] != NULL) {
                if (PyObject_SetAttrString(
                        installed_zlib,
                        zlib_function_names[index],
                        zlib_originals[index]
                    ) < 0) {
                    PyErr_Clear();
                }
                Py_CLEAR(zlib_originals[index]);
            }
        }
    }
    if (installed_bz2 != NULL) {
        for (Py_ssize_t index = 0; index < ARRAY_SIZE(stream_function_names); index++) {
            if (bz2_originals[index] != NULL) {
                if (PyObject_SetAttrString(
                        installed_bz2,
                        stream_function_names[index],
                        bz2_originals[index]
                    ) < 0) {
                    PyErr_Clear();
                }
                Py_CLEAR(bz2_originals[index]);
            }
        }
    }
    if (installed_lzma != NULL) {
        for (Py_ssize_t index = 0; index < ARRAY_SIZE(stream_function_names); index++) {
            if (lzma_originals[index] != NULL) {
                if (PyObject_SetAttrString(
                        installed_lzma,
                        stream_function_names[index],
                        lzma_originals[index]
                    ) < 0) {
                    PyErr_Clear();
                }
                Py_CLEAR(lzma_originals[index]);
            }
        }
    }
    if (installed_zstd != NULL && zstd_frame_original != NULL) {
        if (PyObject_SetAttrString(
                installed_zstd, "get_frame_size", zstd_frame_original
            ) < 0) {
            PyErr_Clear();
        }
    }
    Py_CLEAR(zstd_frame_original);
    for (Py_ssize_t index = 0; index < compression_method_count; index++) {
        CompressionMethod *entry = &compression_methods[index];
        PyObject *dict = PyType_GetDict(entry->type);
        if (dict != NULL && entry->original != NULL &&
            PyDict_SetItemString(
                dict, entry->method.ml_name, entry->original
            ) < 0) {
            PyErr_Clear();
        }
        Py_CLEAR(entry->original);
        PyType_Modified(entry->type);
        entry->type = NULL;
    }
    compression_method_count = 0;
    Py_CLEAR(installed_zlib);
    Py_CLEAR(installed_bz2);
    Py_CLEAR(installed_lzma);
    Py_CLEAR(installed_zstd);
    installed = 0;
}
