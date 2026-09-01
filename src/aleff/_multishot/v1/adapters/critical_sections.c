#define Py_BUILD_CORE_MODULE
#include <Python.h>

#include "critical_sections.h"

#if defined(Py_GIL_DISABLED) && PY_VERSION_HEX >= 0x030d0000
#  include <internal/pycore_critical_section.h>
#endif

#undef Py_BUILD_CORE
#undef Py_BUILD_CORE_MODULE

void
aleff_critical_sections_suspend(AleffCriticalSectionState *state)
{
#if defined(Py_GIL_DISABLED) && PY_VERSION_HEX >= 0x030d0000
    PyThreadState *thread = PyThreadState_Get();
    if (thread->critical_section != 0) {
        _PyCriticalSection_SuspendAll(thread);
        state->head = thread->critical_section;
        thread->critical_section = 0;
    }
#else
    (void)state;
#endif
}

void
aleff_critical_sections_restore(AleffCriticalSectionState *state)
{
    if (state->restored) {
        return;
    }
    state->restored = 1;
#if defined(Py_GIL_DISABLED) && PY_VERSION_HEX >= 0x030d0000
    if (state->head != 0) {
        PyThreadState *thread = PyThreadState_Get();
        thread->critical_section = state->head;
        state->head = 0;
        _PyCriticalSection_Resume(thread);
    }
#endif
}
