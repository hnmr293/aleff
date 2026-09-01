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
PyObject *aleff_adapter_suspend(void);
PyObject *aleff_adapter_restore(PyObject *token);

#endif
