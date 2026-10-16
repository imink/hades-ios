#pragma once
#include <pthread.h>
#include <stdint.h>
#include <time.h>

enum hades_lifecycle_state {
    HADES_LIFECYCLE_RUNNING, HADES_LIFECYCLE_REQUESTED, HADES_LIFECYCLE_DRAINING,
    HADES_LIFECYCLE_PAUSED, HADES_LIFECYCLE_RESUMING, HADES_LIFECYCLE_FAILED
};
int hades_lifecycle_configure(int (*transition)(int), void (*wake)(void));
void hades_lifecycle_thread_enter(void);
void hades_lifecycle_thread_leave(void);
void hades_lifecycle_host_enter(void);
void hades_lifecycle_host_leave(void);
int hades_lifecycle_wait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int hades_lifecycle_sleep(uint64_t ns);
uint64_t hades_lifecycle_clock_ns(void);
uint64_t hades_lifecycle_paused_ns(void);
int hades_lifecycle_pause(uint64_t timeout_ns);
int hades_lifecycle_resume(uint64_t timeout_ns);
int hades_lifecycle_quiescent(void);
enum hades_lifecycle_state hades_lifecycle_status(void);
