#ifndef ALEFF_CONTINUATION_ADAPTERS_JSON_H
#define ALEFF_CONTINUATION_ADAPTERS_JSON_H

#include <Python.h>

int adapter_json_install(PyObject *module);
void adapter_json_rollback(void);

#endif
