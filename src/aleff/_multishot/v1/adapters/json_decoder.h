#ifndef ALEFF_CONTINUATION_ADAPTERS_JSON_DECODER_H
#define ALEFF_CONTINUATION_ADAPTERS_JSON_DECODER_H

#include <Python.h>

/* Install the continuation-aware scanner used by json.JSONDecoder.  The
 * public json functions and classes remain owned by json.py; this small API
 * is for the json adapter, which also owns the matching rollback. */
int adapter_json_decoder_install(PyObject *json_module);
void adapter_json_decoder_rollback(void);

#endif
