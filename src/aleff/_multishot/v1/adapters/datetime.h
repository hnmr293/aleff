#ifndef ALEFF_DATETIME_ADAPTER_H
#define ALEFF_DATETIME_ADAPTER_H

#include <Python.h>

int adapter_datetime_install(PyObject *datetime_module);
void adapter_datetime_rollback(void);

#endif
