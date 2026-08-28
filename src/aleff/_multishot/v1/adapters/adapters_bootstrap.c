#include "builtins.h"
#include "containers.h"
#include "functools.h"
#include "iterators.h"
#include "itertools.h"
#include "mappings.h"
#include "operator.h"
#include "protocols.h"
#include "sets.h"
#include "text.h"

static PyMethodDef sum_method = {
    .ml_name = "sum",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_sum,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Return the sum of a 'start' value plus an iterable of numbers.",
};

static PyMethodDef reduce_method = {
    .ml_name = "reduce",
#if PY_VERSION_HEX >= 0x030e0000
    .ml_meth = (PyCFunction)(void(*)(void))adapter_reduce,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc =
        "reduce($module, function, iterable, /, initial=<unrepresentable>)\n"
        "--\n\n"
        "Apply a function of two arguments cumulatively to the items of an "
        "iterable, from left to right.\n\n"
        "This effectively reduces the iterable to a single value.  If initial "
        "is present,\n"
        "it is placed before the items of the iterable in the calculation, "
        "and serves as\n"
        "a default when the iterable is empty.\n\n"
        "For example, reduce(lambda x, y: x+y, [1, 2, 3, 4, 5])\n"
        "calculates ((((1 + 2) + 3) + 4) + 5).",
#else
    .ml_meth = adapter_reduce,
    .ml_flags = METH_VARARGS,
    .ml_doc =
        "reduce(function, iterable[, initial], /) -> value\n\n"
        "Apply a function of two arguments cumulatively to the items of an "
        "iterable, from left to right.\n\n"
        "This effectively reduces the iterable to a single value.  If initial "
        "is present,\n"
        "it is placed before the items of the iterable in the calculation, "
        "and serves as\n"
        "a default when the iterable is empty.\n\n"
        "For example, reduce(lambda x, y: x+y, [1, 2, 3, 4, 5])\n"
        "calculates ((((1 + 2) + 3) + 4) + 5).",
#endif
};

static PyMethodDef bin_method = {
    .ml_name = "bin",
    .ml_meth = adapter_bin,
    .ml_flags = METH_O,
    .ml_doc = "Return the binary representation of an integer.",
};

static PyMethodDef oct_method = {
    .ml_name = "oct",
    .ml_meth = adapter_oct,
    .ml_flags = METH_O,
    .ml_doc = "Return the octal representation of an integer.",
};

static PyMethodDef hex_method = {
    .ml_name = "hex",
    .ml_meth = adapter_hex,
    .ml_flags = METH_O,
    .ml_doc = "Return the hexadecimal representation of an integer.",
};

static PyMethodDef sorted_method = {
    .ml_name = "sorted",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_sorted,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Return a new list containing all items from the iterable in ascending order.",
};

static PyMethodDef dir_method = {
    .ml_name = "dir",
    .ml_meth = adapter_dir,
    .ml_flags = METH_VARARGS,
    .ml_doc = "If called without an argument, return the names in the current scope. Otherwise, return an alphabetized list of names.",
};

static PyMethodDef isinstance_method = {
    .ml_name = "isinstance",
    .ml_meth = adapter_isinstance,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return whether an object is an instance of a class or tuple of classes.",
};

static PyMethodDef issubclass_method = {
    .ml_name = "issubclass",
    .ml_meth = adapter_issubclass,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return whether a class is a subclass of a class or tuple of classes.",
};

static PyMethodDef setattr_method = {
    .ml_name = "setattr",
    .ml_meth = adapter_setattr,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Set an attribute on an object.",
};

static PyMethodDef delattr_method = {
    .ml_name = "delattr",
    .ml_meth = adapter_delattr,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Delete an attribute from an object.",
};

static PyMethodDef list_extend_method = {
    .ml_name = "extend",
    .ml_meth = adapter_list_extend,
    .ml_flags = METH_O,
    .ml_doc = "Extend list by appending elements from the iterable.",
};

static PyMethodDef list_count_method = {
    .ml_name = "count",
    .ml_meth = adapter_list_count,
    .ml_flags = METH_O,
    .ml_doc = "Return number of occurrences of value.",
};

static PyMethodDef list_sort_method = {
    .ml_name = "sort",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_list_sort,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Sort the list in ascending order and return None.",
};

static PyMethodDef list_index_method = {
    .ml_name = "index",
    .ml_meth = adapter_sequence_index,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return first index of value.",
};

static PyMethodDef list_remove_method = {
    .ml_name = "remove",
    .ml_meth = adapter_list_remove,
    .ml_flags = METH_O,
    .ml_doc = "Remove first occurrence of value.",
};

static PyMethodDef tuple_count_method = {
    .ml_name = "count",
    .ml_meth = adapter_sequence_count,
    .ml_flags = METH_O,
    .ml_doc = "Return number of occurrences of value.",
};

static PyMethodDef tuple_index_method = {
    .ml_name = "index",
    .ml_meth = adapter_sequence_index,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return first index of value.",
};

static PyMethodDef dict_get_method = {
    .ml_name = "get",
    .ml_meth = adapter_dict_get,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the value for key if key is in the dictionary.",
};

static PyMethodDef dict_pop_method = {
    .ml_name = "pop",
    .ml_meth = adapter_dict_pop,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Remove specified key and return the corresponding value.",
};

static PyMethodDef chain_from_iterable_method = {
    .ml_name = "from_iterable",
    .ml_meth = adapter_chain_from_iterable,
    .ml_flags = METH_O | METH_CLASS,
    .ml_doc =
        "from_iterable($type, iterable, /)\n"
        "--\n\n"
        "Alternative chain() constructor taking a single iterable argument "
        "that evaluates lazily.",
};

static PyMethodDef all_method = {
    .ml_name = "all",
    .ml_meth = adapter_all,
    .ml_flags = METH_O,
    .ml_doc = "Return True if bool(x) is True for all values x in the iterable.",
};

static PyMethodDef any_method = {
    .ml_name = "any",
    .ml_meth = adapter_any,
    .ml_flags = METH_O,
    .ml_doc = "Return True if bool(x) is True for any value x in the iterable.",
};

static PyMethodDef next_method = {
    .ml_name = "next",
    .ml_meth = adapter_next,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the next item from the iterator.",
};

static PyMethodDef len_method = {
    .ml_name = "len",
    .ml_meth = adapter_len,
    .ml_flags = METH_O,
    .ml_doc = "Return the number of items in a container.",
};

static PyMethodDef repr_method = {
    .ml_name = "repr",
    .ml_meth = adapter_repr,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the canonical string representation of the object.",
};

static PyMethodDef format_method = {
    .ml_name = "format",
    .ml_meth = adapter_format,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return type(value).__format__(value, format_spec).",
};

static PyMethodDef hash_method = {
    .ml_name = "hash",
    .ml_meth = adapter_hash,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the hash value for the given object.",
};

static PyMethodDef ascii_method = {
    .ml_name = "ascii",
    .ml_meth = adapter_ascii,
    .ml_flags = METH_O,
    .ml_doc = "Return an ASCII-only representation of an object.",
};

static PyMethodDef hasattr_method = {
    .ml_name = "hasattr",
    .ml_meth = adapter_hasattr,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return whether the object has an attribute with the given name.",
};

static PyMethodDef getattr_method = {
    .ml_name = "getattr",
    .ml_meth = adapter_getattr,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Get a named attribute from an object, with an optional default.",
};

static PyMethodDef input_method = {
    .ml_name = "input",
    .ml_meth = adapter_input,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Read a string from standard input.",
};

static PyMethodDef anext_method = {
    .ml_name = "anext",
    .ml_meth = adapter_anext,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Return the next item from an async iterator.",
};

static PyMethodDef open_method = {
    .ml_name = "open",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_open,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Open a file and return a stream.",
};

static PyMethodDef import_method = {
    .ml_name = "__import__",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_import,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Import a module.",
};

static PyMethodDef build_class_method = {
    .ml_name = "__build_class__",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_build_class,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Create a class from a class body function.",
};

static PyMethodDef print_method = {
    .ml_name = "print",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_print,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Print values to a stream.",
};

static PyMethodDef min_method = {
    .ml_name = "min",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_min,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Return the smallest item in an iterable or of two or more arguments.",
};

static PyMethodDef max_method = {
    .ml_name = "max",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_max,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Return the largest item in an iterable or of two or more arguments.",
};

static int adapters_installed = 0;

/* Installation changes type dictionaries and module dictionaries in place.
 * Keep a borrowed container alive and an owned copy of every old value so a
 * failed installation can be rolled back without depending on the partially
 * initialized adapter globals. */
typedef struct {
    PyObject *dict;
    PyObject *key;
    const char *name;
    PyObject *value;
    int present;
} AleffInstallAttribute;

#define ALEFF_INSTALL_ATTRIBUTE_MAX 256

typedef struct {
    AleffInstallAttribute attributes[ALEFF_INSTALL_ATTRIBUTE_MAX];
    Py_ssize_t attribute_count;
} AleffInstallBackup;

typedef struct {
    newfunc map_new;
    vectorcallfunc map_vectorcall;
    iternextfunc map_next;
    newfunc zip_new;
    iternextfunc zip_next;
    newfunc enumerate_new;
    vectorcallfunc enumerate_vectorcall;
    iternextfunc enumerate_next;
    newfunc reversed_new;
    vectorcallfunc reversed_vectorcall;
    iternextfunc reversed_next;
    iternextfunc filter_next;
    newfunc filter_new;
    vectorcallfunc filter_vectorcall;
    initproc list_init;
    vectorcallfunc list_vectorcall;
    newfunc tuple_new;
    vectorcallfunc tuple_vectorcall;
    initproc dict_init;
    vectorcallfunc dict_vectorcall;
    initproc set_init;
    vectorcallfunc set_vectorcall;
    newfunc frozenset_new;
    vectorcallfunc frozenset_vectorcall;
    newfunc bytes_new;
    initproc bytearray_init;
    hashfunc slice_hash;
    reprfunc list_repr;
    hashfunc tuple_hash;
    richcmpfunc list_richcompare;
    richcmpfunc tuple_richcompare;
    richcmpfunc dict_richcompare;
    binaryfunc dict_subscript;
    vectorcallfunc type_vectorcall;
    vectorcallfunc bool_vectorcall;
    vectorcallfunc int_vectorcall;
    vectorcallfunc float_vectorcall;
    vectorcallfunc complex_vectorcall;
    vectorcallfunc str_vectorcall;
    PyTypeObject *operator_accessor_types[3];
    ternaryfunc operator_accessor_calls[3];
    unsigned long operator_accessor_flags[3];
    PySequenceMethods *list_sequence;
    PySequenceMethods *tuple_sequence;
    objobjproc list_contains;
    objobjproc tuple_contains;
} AleffInstallSlots;

static int
backup_attribute(
    AleffInstallBackup *backup,
    PyObject *dict,
    const char *name
)
{
    if (backup->attribute_count >= ALEFF_INSTALL_ATTRIBUTE_MAX) {
        PyErr_SetString(PyExc_RuntimeError, "too many adapter installation attributes");
        return -1;
    }
    AleffInstallAttribute *attribute =
        &backup->attributes[backup->attribute_count++];
    attribute->dict = Py_NewRef(dict);
    attribute->key = NULL;
    attribute->name = name;
    attribute->value = NULL;
    Py_ssize_t position = 0;
    PyObject *key;
    PyObject *value;
    while (PyDict_Next(dict, &position, &key, &value)) {
        if (PyUnicode_Check(key) && PyUnicode_CompareWithASCIIString(key, name) == 0) {
            attribute->key = Py_NewRef(key);
            attribute->value = value;
            break;
        }
    }
    attribute->present = attribute->value != NULL;
    if (attribute->key == NULL) {
        attribute->key = PyUnicode_FromString(name);
        if (attribute->key == NULL) {
            Py_DECREF(attribute->dict);
            backup->attribute_count--;
            return -1;
        }
    }
    Py_XINCREF(attribute->value);
    return 0;
}

static int
backup_type_attribute(
    AleffInstallBackup *backup,
    PyTypeObject *type,
    const char *name
)
{
    PyObject *dict = PyType_GetDict(type);
    if (dict == NULL) {
        return -1;
    }
    return backup_attribute(backup, dict, name);
}

static int
backup_module_attribute(
    AleffInstallBackup *backup,
    PyObject *module,
    const char *name
)
{
    PyObject *dict = PyModule_GetDict(module);
    if (dict == NULL) {
        return -1;
    }
    return backup_attribute(backup, dict, name);
}

static void
free_install_backup(AleffInstallBackup *backup)
{
    for (Py_ssize_t index = 0; index < backup->attribute_count; index++) {
        AleffInstallAttribute *attribute = &backup->attributes[index];
        Py_XDECREF(attribute->value);
        Py_DECREF(attribute->key);
        Py_DECREF(attribute->dict);
    }
    backup->attribute_count = 0;
}

static void
restore_install_backup(AleffInstallBackup *backup)
{
    for (Py_ssize_t index = backup->attribute_count - 1; index >= 0; index--) {
        AleffInstallAttribute *attribute = &backup->attributes[index];
        int status;
        if (attribute->present) {
            status = PyDict_SetItem(
                attribute->dict,
                attribute->key,
                attribute->value
            );
        }
        else {
            status = PyDict_DelItem(attribute->dict, attribute->key);
            if (status < 0 && PyErr_ExceptionMatches(PyExc_KeyError)) {
                PyErr_Clear();
                status = 0;
            }
        }
        if (status < 0) {
            PyErr_Clear();
        }
    }
}

static int
replace_builtin(PyObject *builtins, const char *name, PyMethodDef *method)
{
    PyObject *original = PyDict_GetItemString(builtins, name);
    PyObject *self = NULL;
    PyObject *module = NULL;
    if (original != NULL && PyCFunction_Check(original)) {
        PyMethodDef *original_method = ((PyCFunctionObject *)original)->m_ml;
        method->ml_doc = original_method->ml_doc;
        self = PyCFunction_GET_SELF(original);
        module = PyObject_GetAttrString(original, "__module__");
        if (module == NULL) {
            return -1;
        }
    }
    PyObject *function = PyCFunction_NewEx(method, self, module);
    Py_XDECREF(module);
    if (function == NULL) {
        return -1;
    }
    int result = PyDict_SetItemString(builtins, name, function);
    Py_DECREF(function);
    return result;
}

static int
replace_type_method(PyTypeObject *type, const char *name, PyMethodDef *method)
{
    PyObject *type_dict = PyType_GetDict(type);
    if (type_dict == NULL) {
        return -1;
    }
    PyObject *original = PyDict_GetItemString(type_dict, name);
    if (original != NULL && Py_IS_TYPE(original, &PyMethodDescr_Type)) {
        method->ml_doc = ((PyMethodDescrObject *)original)->d_method->ml_doc;
    }
    PyObject *descriptor = (method->ml_flags & METH_CLASS) != 0
        ? PyDescr_NewClassMethod(type, method)
        : PyDescr_NewMethod(type, method);
    if (descriptor == NULL) {
        return -1;
    }
    int result = PyDict_SetItemString(type_dict, name, descriptor);
    Py_DECREF(descriptor);
    if (result == 0) {
        PyType_Modified(type);
    }
    return result;
}

int
aleff_adapter_install(void)
{
    AleffInstallBackup backup = {0};
    AleffInstallSlots slots = {0};
    int slots_captured = 0;
    PyObject *pre_itertools = NULL;
    PyObject *pre_operator = NULL;
    PyObject *pre_functools = NULL;
    if (adapters_installed) {
        return 0;
    }
    PyObject *builtins = PyEval_GetBuiltins();
    if (!PyDict_Check(builtins)) {
        PyErr_SetString(PyExc_RuntimeError, "cannot access the builtins dictionary");
        goto rollback;
    }

    /* Capture every dictionary entry changed below before the first mutation. */
#define BACKUP_BUILTIN(name) \
    if (backup_attribute(&backup, builtins, name) < 0) goto rollback
#define BACKUP_TYPE(type, name) \
    if (backup_type_attribute(&backup, type, name) < 0) goto rollback
#define BACKUP_MODULE(module, name) \
    if (backup_module_attribute(&backup, module, name) < 0) goto rollback

    static const char *builtin_names[] = {
        "dir", "isinstance", "issubclass", "setattr", "delattr", "sum",
        "all", "any", "next", "len", "repr", "format", "hash", "ascii",
        "hasattr", "getattr", "anext", "input", "open", "print", "__import__",
        "__build_class__", "min", "max", "bin", "oct", "hex", "sorted",
    };
    for (Py_ssize_t index = 0; index < (Py_ssize_t)(sizeof(builtin_names) / sizeof(*builtin_names)); index++) {
        BACKUP_BUILTIN(builtin_names[index]);
    }
    BACKUP_TYPE(&PyList_Type, "extend");
    BACKUP_TYPE(&PyList_Type, "count");
    BACKUP_TYPE(&PyList_Type, "sort");
    BACKUP_TYPE(&PyList_Type, "index");
    BACKUP_TYPE(&PyList_Type, "remove");
    BACKUP_TYPE(&PyTuple_Type, "count");
    BACKUP_TYPE(&PyTuple_Type, "index");
    BACKUP_TYPE(&PyDict_Type, "get");
    BACKUP_TYPE(&PyDict_Type, "pop");
    BACKUP_TYPE(&PyDict_Type, "fromkeys");
    BACKUP_TYPE(&PyDict_Type, "update");
    BACKUP_TYPE(&PyDict_Type, "__getitem__");
    BACKUP_TYPE(&PyDict_Type, "__setitem__");
    BACKUP_TYPE(&PyDict_Type, "__delitem__");
    BACKUP_TYPE(&PyDict_Type, "__contains__");
    BACKUP_TYPE(&PyDict_Type, "__eq__");
    BACKUP_TYPE(&PyDict_Type, "__ne__");
    BACKUP_TYPE(&PyUnicode_Type, "join");
    BACKUP_TYPE(&PyUnicode_Type, "encode");
    BACKUP_TYPE(&PyBytes_Type, "join");
    BACKUP_TYPE(&PyBytes_Type, "decode");
    BACKUP_TYPE(&PyByteArray_Type, "join");
    BACKUP_TYPE(&PyByteArray_Type, "decode");
    BACKUP_TYPE(&PySet_Type, "update");
    BACKUP_TYPE(&PySet_Type, "intersection_update");
    BACKUP_TYPE(&PySet_Type, "difference_update");
    BACKUP_TYPE(&PySet_Type, "symmetric_difference_update");
    BACKUP_TYPE(&PySet_Type, "union");
    BACKUP_TYPE(&PySet_Type, "intersection");
    BACKUP_TYPE(&PySet_Type, "difference");
    BACKUP_TYPE(&PySet_Type, "symmetric_difference");
    BACKUP_TYPE(&PySet_Type, "isdisjoint");
    BACKUP_TYPE(&PySet_Type, "issubset");
    BACKUP_TYPE(&PySet_Type, "issuperset");
    BACKUP_TYPE(&PyFrozenSet_Type, "union");
    BACKUP_TYPE(&PyFrozenSet_Type, "intersection");
    BACKUP_TYPE(&PyFrozenSet_Type, "difference");
    BACKUP_TYPE(&PyFrozenSet_Type, "symmetric_difference");
    BACKUP_TYPE(&PyFrozenSet_Type, "isdisjoint");
    BACKUP_TYPE(&PyFrozenSet_Type, "issubset");
    BACKUP_TYPE(&PyFrozenSet_Type, "issuperset");
    BACKUP_TYPE(&PyRange_Type, "count");
    BACKUP_TYPE(&PyRange_Type, "index");
    BACKUP_TYPE(&PySlice_Type, "indices");

    pre_itertools = PyImport_ImportModule("itertools");
    pre_operator = PyImport_ImportModule("operator");
    pre_functools = PyImport_ImportModule("functools");
    if (pre_itertools == NULL || pre_operator == NULL || pre_functools == NULL) {
        goto rollback;
    }
    BACKUP_MODULE(pre_itertools, "tee");
    BACKUP_MODULE(pre_operator, "indexOf");
    BACKUP_MODULE(pre_operator, "countOf");
    BACKUP_MODULE(pre_functools, "cmp_to_key");
    BACKUP_MODULE(pre_functools, "lru_cache");
    BACKUP_MODULE(pre_functools, "_lru_cache_wrapper");
    BACKUP_MODULE(pre_functools, "reduce");
    PyObject *pre_chain = PyObject_GetAttrString(pre_itertools, "chain");
    if (pre_chain == NULL || !PyType_Check(pre_chain)) {
        Py_XDECREF(pre_chain);
        PyErr_SetString(PyExc_RuntimeError, "cannot access itertools.chain type");
        goto rollback;
    }
    BACKUP_TYPE((PyTypeObject *)pre_chain, "from_iterable");
    Py_DECREF(pre_chain);
    static const char *operator_names[] = {
        "abs", "add", "and_", "call", "concat", "delitem", "eq", "floordiv",
        "ge", "getitem", "gt", "iadd", "iand", "iconcat", "ifloordiv",
        "ilshift", "imatmul", "imod", "imul", "index", "inv", "invert", "ior",
        "ipow", "irshift", "isub", "itruediv", "ixor", "le", "lshift", "lt",
        "matmul", "mod", "mul", "ne", "neg", "not_", "or_", "pos", "pow",
        "rshift", "setitem", "sub", "truediv", "truth", "xor", "length_hint",
        "contains", "attrgetter", "itemgetter", "methodcaller",
        "__abs__", "__add__", "__and__", "__call__", "__concat__",
        "__contains__", "__delitem__", "__eq__", "__floordiv__", "__ge__",
        "__getitem__", "__gt__", "__iadd__", "__iand__", "__iconcat__",
        "__ifloordiv__", "__ilshift__", "__imatmul__", "__imod__", "__imul__",
        "__index__", "__inv__", "__invert__", "__ior__", "__ipow__",
        "__irshift__", "__isub__", "__itruediv__", "__ixor__", "__le__",
        "__lshift__", "__lt__", "__matmul__", "__mod__", "__mul__",
        "__ne__", "__neg__", "__not__", "__or__", "__pos__", "__pow__",
        "__rshift__", "__setitem__", "__sub__", "__truediv__", "__xor__",
    };
    for (Py_ssize_t index = 0; index < (Py_ssize_t)(sizeof(operator_names) / sizeof(*operator_names)); index++) {
        if (backup_module_attribute(&backup, pre_operator, operator_names[index]) < 0) goto rollback;
    }
    static const char *operator_accessor_names[] = {
        "attrgetter", "itemgetter", "methodcaller",
    };
    for (Py_ssize_t index = 0; index < 3; index++) {
        PyObject *object = PyObject_GetAttrString(
            pre_operator,
            operator_accessor_names[index]
        );
        if (object == NULL || !PyType_Check(object)) {
            Py_XDECREF(object);
            PyErr_SetString(PyExc_RuntimeError, "cannot access operator accessor type");
            goto rollback;
        }
        PyTypeObject *type = (PyTypeObject *)object;
        slots.operator_accessor_types[index] = type;
        slots.operator_accessor_calls[index] = type->tp_call;
        slots.operator_accessor_flags[index] = type->tp_flags;
        Py_DECREF(object);
    }
    PyObject *slot_object = PyDict_GetItemString(builtins, "map");
    if (slot_object != NULL && PyType_Check(slot_object)) {
        PyTypeObject *type = (PyTypeObject *)slot_object;
        slots.map_new = type->tp_new;
        slots.map_vectorcall = type->tp_vectorcall;
        slots.map_next = type->tp_iternext;
    }
    slot_object = PyDict_GetItemString(builtins, "zip");
    if (slot_object != NULL && PyType_Check(slot_object)) {
        PyTypeObject *type = (PyTypeObject *)slot_object;
        slots.zip_new = type->tp_new;
        slots.zip_next = type->tp_iternext;
    }
    slot_object = PyDict_GetItemString(builtins, "enumerate");
    if (slot_object != NULL && PyType_Check(slot_object)) {
        PyTypeObject *type = (PyTypeObject *)slot_object;
        slots.enumerate_new = type->tp_new;
        slots.enumerate_vectorcall = type->tp_vectorcall;
        slots.enumerate_next = type->tp_iternext;
    }
    slot_object = PyDict_GetItemString(builtins, "reversed");
    if (slot_object != NULL && PyType_Check(slot_object)) {
        PyTypeObject *type = (PyTypeObject *)slot_object;
        slots.reversed_new = type->tp_new;
        slots.reversed_vectorcall = type->tp_vectorcall;
        slots.reversed_next = type->tp_iternext;
    }
    slot_object = PyDict_GetItemString(builtins, "filter");
    if (slot_object != NULL && PyType_Check(slot_object)) {
        PyTypeObject *type = (PyTypeObject *)slot_object;
        slots.filter_new = type->tp_new;
        slots.filter_vectorcall = type->tp_vectorcall;
        slots.filter_next = type->tp_iternext;
    }
    slots.list_init = PyList_Type.tp_init;
    slots.list_vectorcall = PyList_Type.tp_vectorcall;
    slots.tuple_new = PyTuple_Type.tp_new;
    slots.tuple_vectorcall = PyTuple_Type.tp_vectorcall;
    slots.dict_init = PyDict_Type.tp_init;
    slots.dict_vectorcall = PyDict_Type.tp_vectorcall;
    slots.set_init = PySet_Type.tp_init;
    slots.set_vectorcall = PySet_Type.tp_vectorcall;
    slots.frozenset_new = PyFrozenSet_Type.tp_new;
    slots.frozenset_vectorcall = PyFrozenSet_Type.tp_vectorcall;
    slots.bytes_new = PyBytes_Type.tp_new;
    slots.bytearray_init = PyByteArray_Type.tp_init;
    slots.slice_hash = PySlice_Type.tp_hash;
    slots.list_repr = PyList_Type.tp_repr;
    slots.tuple_hash = PyTuple_Type.tp_hash;
    slots.list_richcompare = PyList_Type.tp_richcompare;
    slots.tuple_richcompare = PyTuple_Type.tp_richcompare;
    slots.dict_richcompare = PyDict_Type.tp_richcompare;
    slots.dict_subscript = PyDict_Type.tp_as_mapping == NULL
        ? NULL : PyDict_Type.tp_as_mapping->mp_subscript;
    slots.type_vectorcall = PyType_Type.tp_vectorcall;
    slots.bool_vectorcall = PyBool_Type.tp_vectorcall;
    slots.int_vectorcall = PyLong_Type.tp_vectorcall;
    slots.float_vectorcall = PyFloat_Type.tp_vectorcall;
    slots.complex_vectorcall = PyComplex_Type.tp_vectorcall;
    slots.str_vectorcall = PyUnicode_Type.tp_vectorcall;
    slots.list_sequence = PyList_Type.tp_as_sequence;
    slots.tuple_sequence = PyTuple_Type.tp_as_sequence;
    slots.list_contains = slots.list_sequence == NULL ? NULL : slots.list_sequence->sq_contains;
    slots.tuple_contains = slots.tuple_sequence == NULL ? NULL : slots.tuple_sequence->sq_contains;
    slots_captured = 1;
    PyObject *input_function = PyDict_GetItemString(builtins, "input");
    PyObject *dir_function = PyDict_GetItemString(builtins, "dir");
    PyObject *open_function = PyDict_GetItemString(builtins, "open");
    PyObject *anext_function = PyDict_GetItemString(builtins, "anext");
    PyObject *import_function = PyDict_GetItemString(builtins, "__import__");
    PyObject *build_class_function = PyDict_GetItemString(
        builtins,
        "__build_class__"
    );
    PyObject *repr_function = PyDict_GetItemString(builtins, "repr");
    PyObject *format_function = PyDict_GetItemString(builtins, "format");
    PyObject *hash_function = PyDict_GetItemString(builtins, "hash");
    if (
        input_function == NULL || dir_function == NULL || open_function == NULL ||
        anext_function == NULL || import_function == NULL ||
        build_class_function == NULL || repr_function == NULL ||
        format_function == NULL ||
        hash_function == NULL
    ) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "cannot access input, open, anext, or __import__ built-in"
        );
        goto rollback;
    }
    original_input = Py_NewRef(input_function);
    original_dir = Py_NewRef(dir_function);
    original_open = Py_NewRef(open_function);
    original_anext = Py_NewRef(anext_function);
    original_import = Py_NewRef(import_function);
    original_build_class = Py_NewRef(build_class_function);
    original_repr = Py_NewRef(repr_function);
    original_format = Py_NewRef(format_function);
    original_hash = Py_NewRef(hash_function);
    original_bool_vectorcall = PyBool_Type.tp_vectorcall;
    original_int_vectorcall = PyLong_Type.tp_vectorcall;
    original_float_vectorcall = PyFloat_Type.tp_vectorcall;
    original_complex_vectorcall = PyComplex_Type.tp_vectorcall;
    original_str_vectorcall = PyUnicode_Type.tp_vectorcall;
    if (PyType_Ready(&AleffAnextAwaitable_Type) < 0) {
        goto rollback;
    }
    PyObject *bootstrap = PyImport_ImportModule("importlib._bootstrap");
    if (bootstrap == NULL) {
        goto rollback;
    }
    import_get_module_lock = PyObject_GetAttrString(
        bootstrap,
        "_get_module_lock"
    );
    Py_DECREF(bootstrap);
    if (import_get_module_lock == NULL) {
        goto rollback;
    }
    PyObject *imp_module = PyImport_ImportModule("_imp");
    if (imp_module == NULL) {
        goto rollback;
    }
    import_global_lock_held = PyObject_GetAttrString(imp_module, "lock_held");
    import_global_lock_acquire = PyObject_GetAttrString(imp_module, "acquire_lock");
    Py_DECREF(imp_module);
    if (import_global_lock_held == NULL || import_global_lock_acquire == NULL) {
        goto rollback;
    }

    PyObject *map_type_object = PyDict_GetItemString(builtins, "map");
    if (map_type_object == NULL || !PyType_Check(map_type_object)) {
        PyErr_SetString(PyExc_RuntimeError, "cannot access the built-in map type");
        goto rollback;
    }
    PyTypeObject *map_type = (PyTypeObject *)map_type_object;
    original_map_new = map_type->tp_new;
    map_type->tp_new = adapter_map_new;
    original_map_vectorcall = map_type->tp_vectorcall;
    if (original_map_vectorcall != NULL) {
        map_type->tp_vectorcall = adapter_map_vectorcall;
    }
    original_map_next = map_type->tp_iternext;
    map_type->tp_iternext = adapter_map_next;
    PyType_Modified(map_type);

    PyObject *empty_tuple = PyTuple_New(0);
    PyObject *tuple_iterator = empty_tuple == NULL ? NULL : PyObject_GetIter(empty_tuple);
    Py_XDECREF(empty_tuple);
    if (tuple_iterator == NULL) {
        goto rollback;
    }
    tuple_iterator_type = Py_TYPE(tuple_iterator);
    Py_DECREF(tuple_iterator);

    PyObject *zip_type_object = PyDict_GetItemString(builtins, "zip");
    if (
        zip_type_object == NULL || !PyType_Check(zip_type_object) ||
        ((PyTypeObject *)zip_type_object)->tp_basicsize <
            adapter_zip_basicsize()
    ) {
        PyErr_SetString(PyExc_RuntimeError, "unsupported built-in zip layout");
        goto rollback;
    }
    PyTypeObject *zip_type = (PyTypeObject *)zip_type_object;
    original_zip_new = zip_type->tp_new;
    zip_type->tp_new = adapter_zip_new;
    zip_type->tp_iternext = adapter_zip_next;
    PyType_Modified(zip_type);

    PyObject *enumerate_type_object = PyDict_GetItemString(builtins, "enumerate");
    if (
        enumerate_type_object == NULL || !PyType_Check(enumerate_type_object) ||
        ((PyTypeObject *)enumerate_type_object)->tp_basicsize <
            adapter_enumerate_basicsize() ||
        ((PyTypeObject *)enumerate_type_object)->tp_vectorcall == NULL
    ) {
        PyErr_SetString(PyExc_RuntimeError, "unsupported built-in enumerate layout");
        goto rollback;
    }
    PyTypeObject *enumerate_type = (PyTypeObject *)enumerate_type_object;
    original_enumerate_new = enumerate_type->tp_new;
    enumerate_type->tp_new = adapter_enumerate_new;
    original_enumerate_vectorcall = enumerate_type->tp_vectorcall;
    enumerate_type->tp_vectorcall = adapter_enumerate_vectorcall;
    enumerate_type->tp_iternext = adapter_enumerate_next;
    PyType_Modified(enumerate_type);

    PyObject *reversed_type_object = PyDict_GetItemString(builtins, "reversed");
    if (
        reversed_type_object == NULL || !PyType_Check(reversed_type_object) ||
        ((PyTypeObject *)reversed_type_object)->tp_basicsize <
            adapter_reversed_basicsize() ||
        ((PyTypeObject *)reversed_type_object)->tp_vectorcall == NULL
    ) {
        PyErr_SetString(PyExc_RuntimeError, "unsupported built-in reversed layout");
        goto rollback;
    }
    PyTypeObject *reversed_type = (PyTypeObject *)reversed_type_object;
    original_reversed_new = reversed_type->tp_new;
    reversed_type->tp_new = adapter_reversed_new;
    original_reversed_vectorcall = reversed_type->tp_vectorcall;
    reversed_type->tp_vectorcall = adapter_reversed_vectorcall;
    original_reversed_next = reversed_type->tp_iternext;
    reversed_type->tp_iternext = adapter_reversed_next;
    PyType_Modified(reversed_type);

    PyObject *filter_type_object = PyDict_GetItemString(builtins, "filter");
    if (
        filter_type_object == NULL || !PyType_Check(filter_type_object) ||
        ((PyTypeObject *)filter_type_object)->tp_basicsize <
            adapter_filter_basicsize()
    ) {
        PyErr_SetString(PyExc_RuntimeError, "unsupported built-in filter layout");
        goto rollback;
    }
    PyTypeObject *filter_type = (PyTypeObject *)filter_type_object;
    original_filter_new = filter_type->tp_new;
    filter_type->tp_new = adapter_filter_new;
    original_filter_vectorcall = filter_type->tp_vectorcall;
    if (original_filter_vectorcall != NULL) {
        filter_type->tp_vectorcall = adapter_filter_vectorcall;
    }
    filter_type->tp_iternext = adapter_filter_next;
    PyType_Modified(filter_type);

    PyObject *itertools = PyImport_ImportModule("itertools");
    if (itertools == NULL) {
        goto rollback;
    }
    PyObject *accumulate_type_object = PyObject_GetAttrString(itertools, "accumulate");
    PyObject *batched_type_object = PyObject_GetAttrString(itertools, "batched");
    PyObject *chain_type_object = PyObject_GetAttrString(itertools, "chain");
    Py_DECREF(itertools);
    if (
        accumulate_type_object == NULL || !PyType_Check(accumulate_type_object) ||
        ((PyTypeObject *)accumulate_type_object)->tp_basicsize <
            adapter_accumulate_basicsize()
    ) {
        Py_XDECREF(accumulate_type_object);
        PyErr_SetString(PyExc_RuntimeError, "unsupported itertools.accumulate layout");
        goto rollback;
    }
    PyTypeObject *accumulate_type = (PyTypeObject *)accumulate_type_object;
    original_accumulate_type = accumulate_type;
    original_accumulate_next = accumulate_type->tp_iternext;
    accumulate_type->tp_iternext = adapter_accumulate_next;
    PyType_Modified(accumulate_type);
    Py_DECREF(accumulate_type_object);
    if (batched_type_object == NULL || !PyType_Check(batched_type_object)) {
        Py_XDECREF(batched_type_object);
        PyErr_SetString(PyExc_RuntimeError, "cannot access itertools.batched type");
        goto rollback;
    }
    PyTypeObject *batched_type = (PyTypeObject *)batched_type_object;
    original_batched_type = batched_type;
    original_batched_new = batched_type->tp_new;
    batched_type->tp_new = adapter_batched_new;
    PyType_Modified(batched_type);
    Py_DECREF(batched_type_object);

    if (
        chain_type_object == NULL || !PyType_Check(chain_type_object) ||
        ((PyTypeObject *)chain_type_object)->tp_basicsize <
            adapter_chain_basicsize()
    ) {
        Py_XDECREF(chain_type_object);
        PyErr_SetString(PyExc_RuntimeError, "unsupported itertools.chain layout");
        goto rollback;
    }
    PyTypeObject *chain_type = (PyTypeObject *)chain_type_object;
    chain_type->tp_iternext = adapter_chain_next;
    PyType_Modified(chain_type);
    if (replace_type_method(chain_type, "from_iterable", &chain_from_iterable_method) < 0) {
        Py_DECREF(chain_type_object);
        goto rollback;
    }
    Py_DECREF(chain_type_object);

    original_list_init = PyList_Type.tp_init;
    PyList_Type.tp_init = adapter_list_init;
    original_list_vectorcall = PyList_Type.tp_vectorcall;
    PyList_Type.tp_vectorcall = adapter_list_vectorcall;
    original_tuple_new = PyTuple_Type.tp_new;
    PyTuple_Type.tp_new = adapter_tuple_new;
    original_tuple_vectorcall = PyTuple_Type.tp_vectorcall;
    PyTuple_Type.tp_vectorcall = adapter_tuple_vectorcall;
    original_dict_init = PyDict_Type.tp_init;
    PyDict_Type.tp_init = adapter_dict_init;
    original_dict_vectorcall = PyDict_Type.tp_vectorcall;
    PyDict_Type.tp_vectorcall = adapter_dict_vectorcall;
    original_set_init = PySet_Type.tp_init;
    PySet_Type.tp_init = adapter_set_init;
    original_set_vectorcall = PySet_Type.tp_vectorcall;
    PySet_Type.tp_vectorcall = adapter_set_vectorcall;
    original_frozenset_new = PyFrozenSet_Type.tp_new;
    PyFrozenSet_Type.tp_new = adapter_frozenset_new;
    original_frozenset_vectorcall = PyFrozenSet_Type.tp_vectorcall;
    PyFrozenSet_Type.tp_vectorcall = adapter_frozenset_vectorcall;
    original_bytes_new = PyBytes_Type.tp_new;
    PyBytes_Type.tp_new = adapter_bytes_new;
    original_bytearray_init = PyByteArray_Type.tp_init;
    PyByteArray_Type.tp_init = adapter_bytearray_init;
    original_slice_hash = PySlice_Type.tp_hash;
    if (original_slice_hash == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "slice is not hashable on this CPython");
        goto rollback;
    }
    PySlice_Type.tp_hash = adapter_slice_hash;
    original_tuple_hash = PyTuple_Type.tp_hash;
    if (original_tuple_hash == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "tuple is not hashable on this CPython");
        goto rollback;
    }
    PyList_Type.tp_repr = adapter_list_repr;
    PyTuple_Type.tp_hash = adapter_tuple_hash;
    original_list_richcompare = PyList_Type.tp_richcompare;
    original_tuple_richcompare = PyTuple_Type.tp_richcompare;
    PyList_Type.tp_richcompare = adapter_sequence_richcompare;
    PyTuple_Type.tp_richcompare = adapter_sequence_richcompare;
    PyList_Type.tp_as_sequence->sq_contains = adapter_sequence_contains;
    PyTuple_Type.tp_as_sequence->sq_contains = adapter_sequence_contains;
    original_type_vectorcall = PyType_Type.tp_vectorcall;
    if (original_type_vectorcall == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "built-in type has no vectorcall");
        goto rollback;
    }
    PyType_Type.tp_vectorcall = adapter_type_vectorcall;
    PyType_Modified(&PyList_Type);
    PyType_Modified(&PyTuple_Type);
    PyType_Modified(&PyDict_Type);
    PyType_Modified(&PySet_Type);
    PyType_Modified(&PyFrozenSet_Type);
    PyType_Modified(&PyBytes_Type);
    PyType_Modified(&PyByteArray_Type);
    PyType_Modified(&PySlice_Type);
    PyType_Modified(&PyType_Type);
    PyBool_Type.tp_vectorcall = adapter_core_type_vectorcall;
    PyLong_Type.tp_vectorcall = adapter_core_type_vectorcall;
    PyFloat_Type.tp_vectorcall = adapter_core_type_vectorcall;
    PyComplex_Type.tp_vectorcall = adapter_core_type_vectorcall;
    PyUnicode_Type.tp_vectorcall = adapter_core_type_vectorcall;
    PyType_Modified(&PyBool_Type);
    PyType_Modified(&PyLong_Type);
    PyType_Modified(&PyFloat_Type);
    PyType_Modified(&PyComplex_Type);
    PyType_Modified(&PyUnicode_Type);

    if (replace_builtin(builtins, "dir", &dir_method) < 0 ||
        replace_builtin(builtins, "isinstance", &isinstance_method) < 0 ||
        replace_builtin(builtins, "issubclass", &issubclass_method) < 0 ||
        replace_builtin(builtins, "setattr", &setattr_method) < 0 ||
        replace_builtin(builtins, "delattr", &delattr_method) < 0 ||
        replace_builtin(builtins, "sum", &sum_method) < 0 ||
        replace_builtin(builtins, "all", &all_method) < 0 ||
        replace_builtin(builtins, "any", &any_method) < 0 ||
        replace_builtin(builtins, "next", &next_method) < 0 ||
        replace_builtin(builtins, "len", &len_method) < 0 ||
        replace_builtin(builtins, "repr", &repr_method) < 0 ||
        replace_builtin(builtins, "format", &format_method) < 0 ||
        replace_builtin(builtins, "hash", &hash_method) < 0 ||
        replace_builtin(builtins, "ascii", &ascii_method) < 0 ||
        replace_builtin(builtins, "hasattr", &hasattr_method) < 0 ||
        replace_builtin(builtins, "getattr", &getattr_method) < 0 ||
        replace_builtin(builtins, "anext", &anext_method) < 0 ||
        replace_builtin(builtins, "input", &input_method) < 0 ||
        replace_builtin(builtins, "open", &open_method) < 0 ||
        replace_builtin(builtins, "print", &print_method) < 0 ||
        replace_builtin(builtins, "__import__", &import_method) < 0 ||
        replace_builtin(builtins, "__build_class__", &build_class_method) < 0 ||
        replace_builtin(builtins, "min", &min_method) < 0 ||
        replace_builtin(builtins, "max", &max_method) < 0 ||
        replace_builtin(builtins, "bin", &bin_method) < 0 ||
        replace_builtin(builtins, "oct", &oct_method) < 0 ||
        replace_builtin(builtins, "hex", &hex_method) < 0 ||
        replace_builtin(builtins, "sorted", &sorted_method) < 0 ||
        replace_type_method(&PyList_Type, "extend", &list_extend_method) < 0 ||
        replace_type_method(&PyList_Type, "count", &list_count_method) < 0 ||
        replace_type_method(&PyList_Type, "sort", &list_sort_method) < 0 ||
        replace_type_method(&PyList_Type, "index", &list_index_method) < 0 ||
        replace_type_method(&PyList_Type, "remove", &list_remove_method) < 0 ||
        replace_type_method(&PyTuple_Type, "count", &tuple_count_method) < 0 ||
        replace_type_method(&PyTuple_Type, "index", &tuple_index_method) < 0 ||
        replace_type_method(&PyDict_Type, "get", &dict_get_method) < 0 ||
        replace_type_method(&PyDict_Type, "pop", &dict_pop_method) < 0) {
        goto rollback;
    }
    PyObject *functools = PyImport_ImportModule("functools");
    if (functools == NULL) {
        goto rollback;
    }
    PyObject *original_reduce = PyObject_GetAttrString(functools, "reduce");
    if (original_reduce == NULL || !PyCFunction_Check(original_reduce)) {
        Py_XDECREF(original_reduce);
        Py_DECREF(functools);
        goto rollback;
    }
    reduce_method.ml_doc =
        ((PyCFunctionObject *)original_reduce)->m_ml->ml_doc;
    Py_DECREF(original_reduce);
    PyObject *functools_name = PyUnicode_FromString("_functools");
    if (functools_name == NULL) {
        Py_DECREF(functools);
        goto rollback;
    }
    PyObject *reduce_function = PyCFunction_NewEx(
        &reduce_method,
        NULL,
        functools_name
    );
    Py_DECREF(functools_name);
    if (reduce_function == NULL) {
        Py_DECREF(functools);
        goto rollback;
    }
    int reduce_status = PyObject_SetAttrString(
        functools,
        "reduce",
        reduce_function
    );
    Py_DECREF(reduce_function);
    Py_DECREF(functools);
    if (reduce_status < 0) {
        goto rollback;
    }
    if (adapter_containers_install() < 0) {
        goto rollback;
    }
    PyObject *itertools_module = PyImport_ImportModule("itertools");
    if (itertools_module == NULL) {
        goto rollback;
    }
    int itertools_status = adapter_itertools_install(itertools_module);
    Py_DECREF(itertools_module);
    if (itertools_status < 0) {
        goto rollback;
    }
    PyObject *operator_module = PyImport_ImportModule("operator");
    if (operator_module == NULL) {
        goto rollback;
    }
    int operator_status = adapter_operator_install(operator_module);
    Py_DECREF(operator_module);
    if (operator_status < 0) {
        goto rollback;
    }
    PyObject *functools_module = PyImport_ImportModule("functools");
    if (functools_module == NULL) {
        goto rollback;
    }
    int functools_status = adapter_functools_install(functools_module);
    Py_DECREF(functools_module);
    if (functools_status < 0) {
        goto rollback;
    }
    adapters_installed = 1;
    Py_XDECREF(pre_itertools);
    Py_XDECREF(pre_operator);
    Py_XDECREF(pre_functools);
    free_install_backup(&backup);
    return 0;

rollback:
    {
        PyObject *error_type = NULL;
        PyObject *error_value = NULL;
        PyObject *error_traceback = NULL;
        PyErr_Fetch(&error_type, &error_value, &error_traceback);

        if (slots_captured) {
            PyObject *slot_object = PyDict_GetItemString(builtins, "map");
            if (slot_object != NULL && PyType_Check(slot_object)) {
            PyTypeObject *type = (PyTypeObject *)slot_object;
            type->tp_new = slots.map_new;
            type->tp_vectorcall = slots.map_vectorcall;
            type->tp_iternext = slots.map_next;
            PyType_Modified(type);
            }
            slot_object = PyDict_GetItemString(builtins, "zip");
            if (slot_object != NULL && PyType_Check(slot_object)) {
            PyTypeObject *type = (PyTypeObject *)slot_object;
            type->tp_new = slots.zip_new;
            type->tp_iternext = slots.zip_next;
            PyType_Modified(type);
            }
            slot_object = PyDict_GetItemString(builtins, "enumerate");
            if (slot_object != NULL && PyType_Check(slot_object)) {
            PyTypeObject *type = (PyTypeObject *)slot_object;
            type->tp_new = slots.enumerate_new;
            type->tp_vectorcall = slots.enumerate_vectorcall;
            type->tp_iternext = slots.enumerate_next;
            PyType_Modified(type);
            }
            slot_object = PyDict_GetItemString(builtins, "reversed");
            if (slot_object != NULL && PyType_Check(slot_object)) {
            PyTypeObject *type = (PyTypeObject *)slot_object;
            type->tp_new = slots.reversed_new;
            type->tp_vectorcall = slots.reversed_vectorcall;
            type->tp_iternext = slots.reversed_next;
            PyType_Modified(type);
            }
            slot_object = PyDict_GetItemString(builtins, "filter");
            if (slot_object != NULL && PyType_Check(slot_object)) {
            PyTypeObject *type = (PyTypeObject *)slot_object;
            type->tp_new = slots.filter_new;
            type->tp_vectorcall = slots.filter_vectorcall;
            type->tp_iternext = slots.filter_next;
            PyType_Modified(type);
            }
            PyList_Type.tp_init = slots.list_init;
        PyList_Type.tp_vectorcall = slots.list_vectorcall;
        PyTuple_Type.tp_new = slots.tuple_new;
        PyTuple_Type.tp_vectorcall = slots.tuple_vectorcall;
        PyDict_Type.tp_init = slots.dict_init;
        PyDict_Type.tp_vectorcall = slots.dict_vectorcall;
        PySet_Type.tp_init = slots.set_init;
        PySet_Type.tp_vectorcall = slots.set_vectorcall;
        PyFrozenSet_Type.tp_new = slots.frozenset_new;
        PyFrozenSet_Type.tp_vectorcall = slots.frozenset_vectorcall;
        PyBytes_Type.tp_new = slots.bytes_new;
        PyByteArray_Type.tp_init = slots.bytearray_init;
        PySlice_Type.tp_hash = slots.slice_hash;
        PyList_Type.tp_repr = slots.list_repr;
        PyTuple_Type.tp_hash = slots.tuple_hash;
        PyList_Type.tp_richcompare = slots.list_richcompare;
        PyTuple_Type.tp_richcompare = slots.tuple_richcompare;
        PyDict_Type.tp_richcompare = slots.dict_richcompare;
        if (PyDict_Type.tp_as_mapping != NULL) {
            PyDict_Type.tp_as_mapping->mp_subscript = slots.dict_subscript;
        }
        PyType_Type.tp_vectorcall = slots.type_vectorcall;
        PyBool_Type.tp_vectorcall = slots.bool_vectorcall;
        PyLong_Type.tp_vectorcall = slots.int_vectorcall;
        PyFloat_Type.tp_vectorcall = slots.float_vectorcall;
        PyComplex_Type.tp_vectorcall = slots.complex_vectorcall;
        PyUnicode_Type.tp_vectorcall = slots.str_vectorcall;
        for (Py_ssize_t index = 0; index < 3; index++) {
            PyTypeObject *type = slots.operator_accessor_types[index];
            if (type != NULL) {
                type->tp_call = slots.operator_accessor_calls[index];
                type->tp_flags = slots.operator_accessor_flags[index];
                PyType_Modified(type);
            }
        }
        if (slots.list_sequence != NULL) {
            slots.list_sequence->sq_contains = slots.list_contains;
        }
        if (slots.tuple_sequence != NULL) {
            slots.tuple_sequence->sq_contains = slots.tuple_contains;
        }
        PyType_Modified(&PyList_Type);
        PyType_Modified(&PyTuple_Type);
        PyType_Modified(&PyDict_Type);
        PyType_Modified(&PySet_Type);
        PyType_Modified(&PyFrozenSet_Type);
        PyType_Modified(&PyBytes_Type);
        PyType_Modified(&PyByteArray_Type);
        PyType_Modified(&PySlice_Type);
        PyType_Modified(&PyType_Type);
        PyType_Modified(&PyBool_Type);
        PyType_Modified(&PyLong_Type);
        PyType_Modified(&PyFloat_Type);
        PyType_Modified(&PyComplex_Type);
        PyType_Modified(&PyUnicode_Type);
        }
        adapter_itertools_rollback();
        adapter_functools_rollback();
        restore_install_backup(&backup);

        Py_CLEAR(original_input);
        Py_CLEAR(original_dir);
        Py_CLEAR(original_open);
        Py_CLEAR(original_anext);
        Py_CLEAR(original_import);
        Py_CLEAR(original_build_class);
        Py_CLEAR(original_repr);
        Py_CLEAR(original_format);
        Py_CLEAR(original_hash);
        Py_CLEAR(import_get_module_lock);
        Py_CLEAR(import_global_lock_held);
        Py_CLEAR(import_global_lock_acquire);
        Py_CLEAR(original_slice_indices);
        adapters_installed = 0;
        Py_XDECREF(pre_itertools);
        Py_XDECREF(pre_operator);
        Py_XDECREF(pre_functools);
        free_install_backup(&backup);
        PyErr_Restore(error_type, error_value, error_traceback);
    }
    return -1;
}

#undef BACKUP_BUILTIN
#undef BACKUP_TYPE
#undef BACKUP_MODULE
