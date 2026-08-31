#ifndef ALEFF_CONTINUATION_ADAPTERS_REGEX_H
#define ALEFF_CONTINUATION_ADAPTERS_REGEX_H

#include "internal.h"

int adapter_regex_install(PyObject *re_module);
void adapter_regex_rollback(void);

#endif
