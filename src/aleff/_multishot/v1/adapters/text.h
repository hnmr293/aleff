#ifndef ALEFF_CONTINUATION_ADAPTERS_TEXT_H
#define ALEFF_CONTINUATION_ADAPTERS_TEXT_H

#include "internal.h"

PyObject *adapter_bytes_new(PyTypeObject *, PyObject *, PyObject *);
int adapter_bytearray_init(PyObject *, PyObject *, PyObject *);

extern PyMethodDef containers_str_encode_method;
extern PyMethodDef containers_bytes_decode_method;
extern PyMethodDef containers_bytearray_decode_method;
extern PyMethodDef containers_str_join_method;
extern PyMethodDef containers_bytes_join_method;
extern PyMethodDef containers_bytearray_join_method;

#endif
