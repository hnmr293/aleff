#ifndef ALEFF_CONTINUATION_ADAPTERS_CSV_H
#define ALEFF_CONTINUATION_ADAPTERS_CSV_H

#include <Python.h>

int adapter_csv_install(PyObject *csv_module);
void adapter_csv_rollback(void);

#endif
