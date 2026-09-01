#ifndef ALEFF_ZONEINFO_ADAPTER_H
#define ALEFF_ZONEINFO_ADAPTER_H

#include <Python.h>

int adapter_zoneinfo_install(PyObject *zoneinfo_module);
void adapter_zoneinfo_rollback(void);

#endif
