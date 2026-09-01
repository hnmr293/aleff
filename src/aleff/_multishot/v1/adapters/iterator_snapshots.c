#include "internal.h"
#include "iterators.h"
#include <string.h>

typedef struct {
    PyObject_HEAD
    Py_ssize_t index;
    PyObject *sequence;
} AleffSequenceIterator;

typedef struct {
    PyObject_HEAD
    PyObject *dict;
    Py_ssize_t used;
    Py_ssize_t position;
    PyObject *result;
    Py_ssize_t remaining;
} AleffDictIterator;

typedef struct {
    PyObject_HEAD
    PyObject *set;
    Py_ssize_t used;
    Py_ssize_t position;
    Py_ssize_t remaining;
} AleffSetIterator;

PyTypeObject *tuple_iterator_type = NULL;
PyTypeObject *list_iterator_type = NULL;

static int
is_dict_iterator(PyObject *iterator)
{
    PyTypeObject *type = Py_TYPE(iterator);
    return type == &PyDictIterKey_Type ||
        type == &PyDictIterValue_Type ||
        type == &PyDictIterItem_Type ||
        type == &PyDictRevIterKey_Type ||
        type == &PyDictRevIterValue_Type ||
        type == &PyDictRevIterItem_Type;
}

static PyObject *
clone_dict_iterator(PyObject *iterator)
{
    PyTypeObject *type = Py_TYPE(iterator);
    if (type->tp_basicsize != (Py_ssize_t)sizeof(AleffDictIterator)) {
        PyErr_SetString(PyExc_RuntimeError, "unsupported dict iterator layout");
        return NULL;
    }
    AleffDictIterator *source = (AleffDictIterator *)iterator;
    AleffDictIterator *copy = PyObject_GC_New(AleffDictIterator, type);
    if (copy == NULL) {
        return NULL;
    }
    copy->dict = Py_XNewRef(source->dict);
    copy->used = source->used;
    copy->position = source->position;
    copy->result = Py_XNewRef(source->result);
    copy->remaining = source->remaining;
    PyObject_GC_Track(copy);
    return (PyObject *)copy;
}

static PyObject *
clone_set_iterator(PyObject *iterator)
{
    if (Py_TYPE(iterator)->tp_basicsize !=
        (Py_ssize_t)sizeof(AleffSetIterator)) {
        PyErr_SetString(PyExc_RuntimeError, "unsupported set iterator layout");
        return NULL;
    }
    AleffSetIterator *source = (AleffSetIterator *)iterator;
    AleffSetIterator *copy = PyObject_GC_New(
        AleffSetIterator,
        &PySetIter_Type
    );
    if (copy == NULL) {
        return NULL;
    }
    copy->set = Py_XNewRef(source->set);
    copy->used = source->used;
    copy->position = source->position;
    copy->remaining = source->remaining;
    PyObject_GC_Track(copy);
    return (PyObject *)copy;
}

static int
iterator_supports_reduce_snapshot(PyObject *iterator)
{
    if (!PyType_HasFeature(Py_TYPE(iterator), Py_TPFLAGS_IMMUTABLETYPE)) {
        return 0;
    }
    static const char *const names[] = {
        "array.arrayiterator",
        "bytearray_iterator",
        "bytes_iterator",
        "callable_iterator",
        "enumerate",
        "filter",
        "list_reverseiterator",
        "longrange_iterator",
        "map",
        "range_iterator",
        "reversed",
        "str_ascii_iterator",
        "str_iterator",
        "zip",
    };
    const char *name = Py_TYPE(iterator)->tp_name;
    for (size_t index = 0; index < sizeof(names) / sizeof(*names); index++) {
        if (strcmp(name, names[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

static PyObject *
clone_reduce_iterator(PyObject *iterator)
{
    PyObject *reduction = PyObject_CallMethod(iterator, "__reduce__", NULL);
    if (reduction == NULL) {
        return NULL;
    }
    if (!PyTuple_Check(reduction) ||
        PyTuple_GET_SIZE(reduction) < 2 ||
        PyTuple_GET_SIZE(reduction) > 3 ||
        !PyTuple_Check(PyTuple_GET_ITEM(reduction, 1))) {
        Py_DECREF(reduction);
        PyErr_SetString(PyExc_RuntimeError, "unsupported iterator reduction");
        return NULL;
    }
    PyObject *arguments = PyTuple_GET_ITEM(reduction, 1);
    Py_ssize_t count = PyTuple_GET_SIZE(arguments);
    PyObject *arguments_copy = PyTuple_New(count);
    if (arguments_copy == NULL) {
        Py_DECREF(reduction);
        return NULL;
    }
    for (Py_ssize_t index = 0; index < count; index++) {
        PyObject *argument = PyTuple_GET_ITEM(arguments, index);
        PyObject *copy = PyIter_Check(argument)
            ? clone_iterator_for_snapshot(argument)
            : Py_NewRef(argument);
        if (copy == NULL) {
            Py_DECREF(arguments_copy);
            Py_DECREF(reduction);
            return NULL;
        }
        PyTuple_SET_ITEM(arguments_copy, index, copy);
    }
    PyObject *copy = PyObject_Call(
        PyTuple_GET_ITEM(reduction, 0),
        arguments_copy,
        NULL
    );
    Py_DECREF(arguments_copy);
    if (copy != NULL && PyTuple_GET_SIZE(reduction) == 3) {
        PyObject *state = PyTuple_GET_ITEM(reduction, 2);
        if (state != Py_None) {
            PyObject *set = PyObject_CallMethod(copy, "__setstate__", "O", state);
            if (set == NULL) {
                Py_CLEAR(copy);
            }
            else {
                Py_DECREF(set);
            }
        }
    }
    Py_DECREF(reduction);
    return copy;
}

PyObject *
clone_iterator_for_snapshot(PyObject *iterator)
{
    if (Py_IS_TYPE(iterator, tuple_iterator_type) ||
        Py_IS_TYPE(iterator, list_iterator_type)) {
        AleffSequenceIterator *source = (AleffSequenceIterator *)iterator;
        if (source->sequence == NULL) {
            return Py_NewRef(iterator);
        }
        PyObject *copy = PyObject_GetIter(source->sequence);
        if (copy == NULL) {
            return NULL;
        }
        ((AleffSequenceIterator *)copy)->index = source->index;
        return copy;
    }
    if (is_dict_iterator(iterator)) {
        return clone_dict_iterator(iterator);
    }
    if (Py_IS_TYPE(iterator, &PySetIter_Type)) {
        return clone_set_iterator(iterator);
    }
    if (iterator_supports_reduce_snapshot(iterator)) {
        return clone_reduce_iterator(iterator);
    }
    return Py_NewRef(iterator);
}
