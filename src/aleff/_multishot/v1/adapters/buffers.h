#ifndef ALEFF_CONTINUATION_ADAPTERS_BUFFERS_H
#define ALEFF_CONTINUATION_ADAPTERS_BUFFERS_H

#include "internal.h"

typedef struct {
    Py_ssize_t position;
    const char *keyword;
    const char *exclusive_keyword;
    int flags;
    int make_bytearray;
} AleffBufferArgument;

PyObject *adapter_buffer_call(
    PyObject *callable,
    PyObject *receiver,
    PyObject *args,
    PyObject *kwargs,
    const AleffBufferArgument *arguments,
    Py_ssize_t argument_count
);

PyObject *adapter_buffer_new(
    newfunc constructor,
    PyTypeObject *type,
    PyObject *args,
    PyObject *kwargs,
    const AleffBufferArgument *arguments,
    Py_ssize_t argument_count
);

#endif
