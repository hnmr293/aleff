#ifndef ALEFF_CONTINUATION_ADAPTERS_H
#define ALEFF_CONTINUATION_ADAPTERS_H

#include <Python.h>

typedef struct AleffAdapterSnapshot AleffAdapterSnapshot;

AleffAdapterSnapshot *aleff_adapter_snapshot_capture(PyFrameObject *start_frame, int depth);
AleffAdapterSnapshot *aleff_adapter_snapshot_from_token(PyObject *token);
void aleff_adapter_snapshot_free(AleffAdapterSnapshot *snapshot);
int aleff_adapter_snapshot_prepare(AleffAdapterSnapshot *snapshot);

PyObject *aleff_adapter_resume_before_frame(
    AleffAdapterSnapshot *snapshot,
    int outer_frame_index,
    PyObject *value
);

int aleff_adapter_install(void);
int aleff_adapter_register_callable(PyObject *callable);
int aleff_adapter_register_c_function(PyCFunction function);
int aleff_adapter_callable_is_registered(PyObject *callable);
int aleff_adapter_c_function_is_registered(PyCFunction function);
void aleff_adapter_clear_registered_callables(void);
int aleff_adapter_has_callable(PyObject *callable);
PyObject *aleff_adapter_suspend(void);
PyObject *aleff_adapter_restore(PyObject *token);

#endif
