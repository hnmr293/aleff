#ifndef ALEFF_CONTINUATION_ADAPTERS_MARSHAL_STREAM_H
#define ALEFF_CONTINUATION_ADAPTERS_MARSHAL_STREAM_H

#include <Python.h>

/* The implementation owns all continuation state.  In particular, callers
 * must not retain or inspect an AleffMarshalStream after handing it to
 * aleff_marshal_stream_run(). */
typedef struct AleffMarshalStream AleffMarshalStream;

AleffMarshalStream *aleff_marshal_stream_new(
    PyObject *load,
    PyObject *loads,
    PyObject *reader,
    int allow_code
);
PyObject *aleff_marshal_stream_run(AleffMarshalStream *stream);
void aleff_marshal_stream_free(AleffMarshalStream *stream);

#endif
