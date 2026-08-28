static PyMethodDef sum_method = {
    .ml_name = "sum",
    .ml_meth = (PyCFunction)(void(*)(void))adapter_sum,
    .ml_flags = METH_VARARGS | METH_KEYWORDS,
    .ml_doc = "Return the sum of a 'start' value plus an iterable of numbers.",
};

static PyMethodDef reduce_method = {
    .ml_name = "reduce",
    .ml_meth = adapter_reduce,
    .ml_flags = METH_VARARGS,
    .ml_doc = "Apply a function of two arguments cumulatively to an iterable.",
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

static PyMethodDef chain_from_iterable_method = {
    .ml_name = "from_iterable",
    .ml_meth = adapter_chain_from_iterable,
    .ml_flags = METH_O | METH_CLASS,
    .ml_doc = "Alternative chain() constructor taking a single iterable argument.",
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

static int
replace_builtin(PyObject *builtins, const char *name, PyMethodDef *method)
{
    PyObject *function = PyCFunction_NewEx(method, NULL, NULL);
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
    PyObject *descriptor = (method->ml_flags & METH_CLASS) != 0
        ? PyDescr_NewClassMethod(type, method)
        : PyDescr_NewMethod(type, method);
    if (descriptor == NULL) {
        return -1;
    }
    PyObject *type_dict = PyType_GetDict(type);
    if (type_dict == NULL) {
        Py_DECREF(descriptor);
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
    if (adapters_installed) {
        return 0;
    }
    PyObject *builtins = PyEval_GetBuiltins();
    if (!PyDict_Check(builtins)) {
        PyErr_SetString(PyExc_RuntimeError, "cannot access the builtins dictionary");
        return -1;
    }

    PyObject *input_function = PyDict_GetItemString(builtins, "input");
    PyObject *open_function = PyDict_GetItemString(builtins, "open");
    PyObject *anext_function = PyDict_GetItemString(builtins, "anext");
    PyObject *import_function = PyDict_GetItemString(builtins, "__import__");
    PyObject *build_class_function = PyDict_GetItemString(
        builtins,
        "__build_class__"
    );
    if (
        input_function == NULL || open_function == NULL ||
        anext_function == NULL || import_function == NULL ||
        build_class_function == NULL
    ) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "cannot access input, open, anext, or __import__ built-in"
        );
        return -1;
    }
    original_input = Py_NewRef(input_function);
    original_open = Py_NewRef(open_function);
    original_anext = Py_NewRef(anext_function);
    original_import = Py_NewRef(import_function);
    original_build_class = Py_NewRef(build_class_function);
    if (PyType_Ready(&AleffAnextAwaitable_Type) < 0) {
        return -1;
    }
    PyObject *bootstrap = PyImport_ImportModule("importlib._bootstrap");
    if (bootstrap == NULL) {
        return -1;
    }
    import_get_module_lock = PyObject_GetAttrString(
        bootstrap,
        "_get_module_lock"
    );
    Py_DECREF(bootstrap);
    if (import_get_module_lock == NULL) {
        return -1;
    }
    PyObject *imp_module = PyImport_ImportModule("_imp");
    if (imp_module == NULL) {
        return -1;
    }
    import_global_lock_held = PyObject_GetAttrString(imp_module, "lock_held");
    import_global_lock_acquire = PyObject_GetAttrString(imp_module, "acquire_lock");
    Py_DECREF(imp_module);
    if (import_global_lock_held == NULL || import_global_lock_acquire == NULL) {
        return -1;
    }

    PyObject *map_type_object = PyDict_GetItemString(builtins, "map");
    if (map_type_object == NULL || !PyType_Check(map_type_object)) {
        PyErr_SetString(PyExc_RuntimeError, "cannot access the built-in map type");
        return -1;
    }
    PyTypeObject *map_type = (PyTypeObject *)map_type_object;
    original_map_new = map_type->tp_new;
    map_type->tp_new = adapter_map_new;
    original_map_next = map_type->tp_iternext;
    map_type->tp_iternext = adapter_map_next;
    PyType_Modified(map_type);

    PyObject *empty_tuple = PyTuple_New(0);
    PyObject *tuple_iterator = empty_tuple == NULL ? NULL : PyObject_GetIter(empty_tuple);
    Py_XDECREF(empty_tuple);
    if (tuple_iterator == NULL) {
        return -1;
    }
    tuple_iterator_type = Py_TYPE(tuple_iterator);
    Py_DECREF(tuple_iterator);

    PyObject *zip_type_object = PyDict_GetItemString(builtins, "zip");
    if (
        zip_type_object == NULL || !PyType_Check(zip_type_object) ||
        ((PyTypeObject *)zip_type_object)->tp_basicsize <
            (Py_ssize_t)sizeof(AleffZipObject)
    ) {
        PyErr_SetString(PyExc_RuntimeError, "unsupported built-in zip layout");
        return -1;
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
            (Py_ssize_t)sizeof(AleffEnumerateObject) ||
        ((PyTypeObject *)enumerate_type_object)->tp_vectorcall == NULL
    ) {
        PyErr_SetString(PyExc_RuntimeError, "unsupported built-in enumerate layout");
        return -1;
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
            (Py_ssize_t)sizeof(AleffReversedObject) ||
        ((PyTypeObject *)reversed_type_object)->tp_vectorcall == NULL
    ) {
        PyErr_SetString(PyExc_RuntimeError, "unsupported built-in reversed layout");
        return -1;
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
            (Py_ssize_t)sizeof(AleffFilterObject)
    ) {
        PyErr_SetString(PyExc_RuntimeError, "unsupported built-in filter layout");
        return -1;
    }
    PyTypeObject *filter_type = (PyTypeObject *)filter_type_object;
    filter_type->tp_iternext = adapter_filter_next;
    PyType_Modified(filter_type);

    PyObject *itertools = PyImport_ImportModule("itertools");
    if (itertools == NULL) {
        return -1;
    }
    PyObject *accumulate_type_object = PyObject_GetAttrString(itertools, "accumulate");
    PyObject *batched_type_object = PyObject_GetAttrString(itertools, "batched");
    PyObject *chain_type_object = PyObject_GetAttrString(itertools, "chain");
    Py_DECREF(itertools);
    if (
        accumulate_type_object == NULL || !PyType_Check(accumulate_type_object) ||
        ((PyTypeObject *)accumulate_type_object)->tp_basicsize <
            (Py_ssize_t)sizeof(AleffAccumulateObject)
    ) {
        Py_XDECREF(accumulate_type_object);
        PyErr_SetString(PyExc_RuntimeError, "unsupported itertools.accumulate layout");
        return -1;
    }
    PyTypeObject *accumulate_type = (PyTypeObject *)accumulate_type_object;
    accumulate_type->tp_iternext = adapter_accumulate_next;
    PyType_Modified(accumulate_type);
    Py_DECREF(accumulate_type_object);
    if (batched_type_object == NULL || !PyType_Check(batched_type_object)) {
        Py_XDECREF(batched_type_object);
        PyErr_SetString(PyExc_RuntimeError, "cannot access itertools.batched type");
        return -1;
    }
    PyTypeObject *batched_type = (PyTypeObject *)batched_type_object;
    original_batched_new = batched_type->tp_new;
    batched_type->tp_new = adapter_batched_new;
    PyType_Modified(batched_type);
    Py_DECREF(batched_type_object);

    if (
        chain_type_object == NULL || !PyType_Check(chain_type_object) ||
        ((PyTypeObject *)chain_type_object)->tp_basicsize <
            (Py_ssize_t)sizeof(AleffChainObject)
    ) {
        Py_XDECREF(chain_type_object);
        PyErr_SetString(PyExc_RuntimeError, "unsupported itertools.chain layout");
        return -1;
    }
    PyTypeObject *chain_type = (PyTypeObject *)chain_type_object;
    chain_type->tp_iternext = adapter_chain_next;
    PyType_Modified(chain_type);
    if (replace_type_method(chain_type, "from_iterable", &chain_from_iterable_method) < 0) {
        Py_DECREF(chain_type_object);
        return -1;
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
        return -1;
    }
    PySlice_Type.tp_hash = adapter_slice_hash;
    original_list_richcompare = PyList_Type.tp_richcompare;
    original_tuple_richcompare = PyTuple_Type.tp_richcompare;
    PyList_Type.tp_richcompare = adapter_sequence_richcompare;
    PyTuple_Type.tp_richcompare = adapter_sequence_richcompare;
    PyList_Type.tp_as_sequence->sq_contains = adapter_sequence_contains;
    PyTuple_Type.tp_as_sequence->sq_contains = adapter_sequence_contains;
    original_type_vectorcall = PyType_Type.tp_vectorcall;
    if (original_type_vectorcall == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "built-in type has no vectorcall");
        return -1;
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

    if (replace_builtin(builtins, "sum", &sum_method) < 0 ||
        replace_builtin(builtins, "all", &all_method) < 0 ||
        replace_builtin(builtins, "any", &any_method) < 0 ||
        replace_builtin(builtins, "next", &next_method) < 0 ||
        replace_builtin(builtins, "len", &len_method) < 0 ||
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
        replace_type_method(&PyDict_Type, "get", &dict_get_method) < 0) {
        return -1;
    }
    PyObject *functools = PyImport_ImportModule("functools");
    if (functools == NULL) {
        return -1;
    }
    PyObject *functools_name = PyUnicode_FromString("functools");
    if (functools_name == NULL) {
        Py_DECREF(functools);
        return -1;
    }
    PyObject *reduce_function = PyCFunction_NewEx(
        &reduce_method,
        NULL,
        functools_name
    );
    Py_DECREF(functools_name);
    if (reduce_function == NULL) {
        Py_DECREF(functools);
        return -1;
    }
    int reduce_status = PyObject_SetAttrString(
        functools,
        "reduce",
        reduce_function
    );
    Py_DECREF(reduce_function);
    Py_DECREF(functools);
    if (reduce_status < 0) {
        return -1;
    }
    adapters_installed = 1;
    return 0;
}
