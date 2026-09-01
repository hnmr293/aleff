#ifndef ALEFF_ADAPTERS_CRITICAL_SECTIONS_H
#define ALEFF_ADAPTERS_CRITICAL_SECTIONS_H

#include <stdint.h>

typedef struct {
    uintptr_t head;
    int restored;
} AleffCriticalSectionState;

void aleff_critical_sections_suspend(AleffCriticalSectionState *state);
void aleff_critical_sections_restore(AleffCriticalSectionState *state);

#endif
