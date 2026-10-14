#pragma once
#include <stdint.h>

void *hades_gate_stack_top(void);
void hades_gate_stack_leave(void);
uint64_t hades_callout_run(uintptr_t function, const uint64_t arguments[6]);
