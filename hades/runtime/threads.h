#pragma once
void hades_threads_wake_lifecycle(void);
#include <stdint.h>

int hades_threads_initialize(unsigned priority, unsigned core, uint64_t priority_mask, uint64_t core_mask);
int hades_threads_shutdown(void);
int hades_thread_svc(unsigned svc, uint64_t *registers);
