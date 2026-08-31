#ifndef ALEFF_CONTINUATION_ADAPTERS_JSON_ENCODER_H
#define ALEFF_CONTINUATION_ADAPTERS_JSON_ENCODER_H

#include "internal.h"

/* Install the continuation-aware call slot on _json.Encoder.  The argument
 * may be either the json module or the _json accelerator module. */
int adapter_json_encoder_install(PyObject *module);
void adapter_json_encoder_rollback(void);

#endif
