#ifndef ALEFF_CONTINUATION_ADAPTERS_PROTOCOLS_H
#define ALEFF_CONTINUATION_ADAPTERS_PROTOCOLS_H

#include "internal.h"

typedef enum {
    PROTOCOL_BOOL,
    PROTOCOL_LEN,
    PROTOCOL_INT,
    PROTOCOL_INDEX,
    PROTOCOL_TRUNC,
    PROTOCOL_FLOAT,
    PROTOCOL_FLOAT_INDEX,
    PROTOCOL_COMPLEX,
    PROTOCOL_COMPLEX_FLOAT,
    PROTOCOL_COMPLEX_INDEX,
    PROTOCOL_STR,
    PROTOCOL_REPR,
    PROTOCOL_FORMAT,
    PROTOCOL_HASH,
} ProtocolResumeKind;

ProtocolResumeKind protocol_vectorcall_kind(
    PyObject *const *args,
    size_t nargsf,
    const char *first,
    const char *second,
    const char *third,
    ProtocolResumeKind first_kind,
    ProtocolResumeKind second_kind,
    ProtocolResumeKind third_kind
);

PyObject *protocol_type_vectorcall(
    PyObject *callable,
    PyObject *const *args,
    size_t nargsf,
    PyObject *kwnames,
    ProtocolResumeKind kind
);

extern vectorcallfunc original_type_vectorcall;
extern PyObject *original_repr;
extern PyObject *original_format;
extern PyObject *original_hash;
extern vectorcallfunc original_bool_vectorcall;
extern vectorcallfunc original_int_vectorcall;
extern vectorcallfunc original_float_vectorcall;
extern vectorcallfunc original_complex_vectorcall;
extern vectorcallfunc original_str_vectorcall;

PyObject *adapter_repr(PyObject *, PyObject *);
PyObject *adapter_format(PyObject *, PyObject *);
PyObject *adapter_hash(PyObject *, PyObject *);
PyObject *adapter_core_type_vectorcall(
    PyObject *, PyObject *const *, size_t, PyObject *
);

#endif
