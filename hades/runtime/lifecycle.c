#define _DARWIN_C_SOURCE
#include "lifecycle.h"
#include <errno.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER, sleep_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER, sleepers=PTHREAD_COND_INITIALIZER;
static atomic_int enabled;
static enum hades_lifecycle_state phase;
static unsigned guest_running, host_running, registered;
static uint64_t pause_start, paused_total;
static int (*transition_hook)(int);
static void (*wake_hook)(void);
typedef struct { unsigned registered, depth, waiting, transition; } lifecycle_thread;
static _Thread_local lifecycle_thread self;

static void fatal(const char *message) {
    fprintf(stderr,"[hades-lifecycle] %s\n",message);
    abort();
}

static uint64_t raw_clock(void) {
    mach_timebase_info_data_t timebase;
    if (mach_timebase_info(&timebase)!=KERN_SUCCESS || !timebase.denom)
        fatal("Mach monotonic clock unavailable");
    return (uint64_t)((__uint128_t)mach_absolute_time()*timebase.numer/timebase.denom);
}

static uint64_t paused_locked(void) {
    return paused_total+(pause_start ? raw_clock()-pause_start : 0);
}

uint64_t hades_lifecycle_paused_ns(void) {
    if (!atomic_load_explicit(&enabled,memory_order_acquire)) return 0;
    pthread_mutex_lock(&lock);
    uint64_t value=paused_locked();
    pthread_mutex_unlock(&lock);
    return value;
}

uint64_t hades_lifecycle_clock_ns(void) {
    if (!atomic_load_explicit(&enabled,memory_order_acquire)) return raw_clock();
    pthread_mutex_lock(&lock);
    uint64_t now=raw_clock();
    uint64_t value=now-paused_total-(pause_start ? now-pause_start : 0);
    pthread_mutex_unlock(&lock);
    return value;
}

int hades_lifecycle_configure(int (*transition)(int),void (*wake)(void)) {
    pthread_mutex_lock(&lock);
    if (!transition || !wake || atomic_load(&enabled)) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr,"[hades-lifecycle] Invalid or duplicate coordinator configuration\n");
        return -1;
    }
    transition_hook=transition;
    wake_hook=wake;
    atomic_store_explicit(&enabled,1,memory_order_release);
    pthread_mutex_unlock(&lock);
    return 0;
}

/* Only a guest at a lock-free outer gate may drain or restart its own drivers.
   Nested allocator callbacks must finish, never park while a driver owns locks. */
static void await_running(int may_lead) {
    while (phase!=HADES_LIFECYCLE_RUNNING) {
        if (may_lead && phase==HADES_LIFECYCLE_REQUESTED && !guest_running && !host_running) {
            phase=HADES_LIFECYCLE_DRAINING;
            self.transition=1;
            pthread_mutex_unlock(&lock);
            int status=transition_hook(1);
            pthread_mutex_lock(&lock);
            self.transition=0;
            if (phase==HADES_LIFECYCLE_FAILED || status) {
                phase=HADES_LIFECYCLE_FAILED;
                pthread_cond_broadcast(&changed);
            } else {
                phase=HADES_LIFECYCLE_PAUSED;
                fprintf(stderr,"[hades-lifecycle] PAUSED guest-running=0 host-running=0\n");
                pthread_cond_broadcast(&changed);
                while (phase==HADES_LIFECYCLE_PAUSED)
                    if (pthread_cond_wait(&changed,&lock)) fatal("Paused owner wait failed");
                if (phase==HADES_LIFECYCLE_RESUMING) {
                    self.transition=1;
                    pthread_mutex_unlock(&lock);
                    status=transition_hook(0);
                    pthread_mutex_lock(&lock);
                    self.transition=0;
                    if (phase==HADES_LIFECYCLE_FAILED || status) phase=HADES_LIFECYCLE_FAILED;
                    else {
                        paused_total+=raw_clock()-pause_start;
                        pause_start=0;
                        phase=HADES_LIFECYCLE_RUNNING;
                        fprintf(stderr,"[hades-lifecycle] RESUMED paused-ns=%llu\n",
                                (unsigned long long)paused_total);
                    }
                    pthread_cond_broadcast(&changed);
                }
            }
        } else if (pthread_cond_wait(&changed,&lock)) fatal("Guest park wait failed");
    }
}

void hades_lifecycle_thread_enter(void) {
    if (!atomic_load_explicit(&enabled,memory_order_acquire)) return;
    pthread_mutex_lock(&lock);
    if (self.registered) fatal("Duplicate guest execution registration");
    self.registered=1;
    await_running(!registered++);
    guest_running++;
    pthread_mutex_unlock(&lock);
}

void hades_lifecycle_thread_leave(void) {
    if (!self.registered) return;
    pthread_mutex_lock(&lock);
    if (!self.depth) guest_running--;
    else if (self.depth==1 && !self.waiting) host_running--;
    else fatal("Thread exited inside nested callback or native wait");
    registered--;
    self=(lifecycle_thread){0};
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

void hades_lifecycle_host_enter(void) {
    if (!self.registered || self.transition) return;
    if (self.depth++) return;
    pthread_mutex_lock(&lock);
    guest_running--;
    pthread_cond_broadcast(&changed);
    await_running(1);
    host_running++;
    pthread_mutex_unlock(&lock);
}

void hades_lifecycle_host_leave(void) {
    if (!self.registered || self.transition) return;
    if (!self.depth) fatal("Host leave without entry");
    if (self.depth>1) { self.depth--; return; }
    pthread_mutex_lock(&lock);
    host_running--;
    pthread_cond_broadcast(&changed);
    await_running(1);
    self.depth=0;
    guest_running++;
    pthread_mutex_unlock(&lock);
}

int hades_lifecycle_wait(pthread_cond_t *condition,pthread_mutex_t *mutex,const struct timespec *relative) {
    int coordinated=self.registered && self.depth==1 && !self.transition;
    uint64_t before=0;
    struct timespec poll={.tv_nsec=100000000};
    int bounded=coordinated && (!relative || relative->tv_sec || relative->tv_nsec>poll.tv_nsec);
    if (coordinated) {
        pthread_mutex_lock(&lock);
        before=paused_locked();
        host_running--;
        self.waiting=1;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
    }
    int status=bounded ? pthread_cond_timedwait_relative_np(condition,mutex,&poll)
               : relative ? pthread_cond_timedwait_relative_np(condition,mutex,relative)
                        : pthread_cond_wait(condition,mutex);
    if (coordinated) {
        /* pthread_cond_wait reacquires the HLE lock; never park while holding it. */
        pthread_mutex_unlock(mutex);
        pthread_mutex_lock(&lock);
        self.waiting=0;
        await_running(1);
        host_running++;
        if (status==ETIMEDOUT && (bounded || before!=paused_locked())) status=0;
        pthread_mutex_unlock(&lock);
        pthread_mutex_lock(mutex);
    }
    return status;
}

int hades_lifecycle_sleep(uint64_t ns) {
    uint64_t start=hades_lifecycle_clock_ns();
    if (ns>UINT64_MAX-start) return EINVAL;
    uint64_t deadline=start+ns;
    pthread_mutex_lock(&sleep_lock);
    for (;;) {
        uint64_t now=hades_lifecycle_clock_ns();
        if (now>=deadline) break;
        uint64_t remaining=deadline-now;
        struct timespec relative={(time_t)(remaining/1000000000),(long)(remaining%1000000000)};
        int status=hades_lifecycle_wait(&sleepers,&sleep_lock,&relative);
        if (status && status!=ETIMEDOUT) {
            pthread_mutex_unlock(&sleep_lock);
            return status;
        }
    }
    pthread_mutex_unlock(&sleep_lock);
    return 0;
}

static int wait_for(enum hades_lifecycle_state target,uint64_t timeout) {
    uint64_t start=raw_clock();
    while (phase!=target && phase!=HADES_LIFECYCLE_FAILED) {
        uint64_t elapsed=raw_clock()-start;
        if (elapsed>=timeout) {
            phase=HADES_LIFECYCLE_FAILED;
            fprintf(stderr,"[hades-lifecycle] Transition timed out guest-running=%u host-running=%u\n",
                    guest_running,host_running);
            pthread_cond_broadcast(&changed);
            break;
        }
        uint64_t left=timeout-elapsed;
        struct timespec relative={(time_t)(left/1000000000),(long)(left%1000000000)};
        int status=pthread_cond_timedwait_relative_np(&changed,&lock,&relative);
        if (status && status!=ETIMEDOUT) fatal("Lifecycle controller wait failed");
    }
    return phase==target ? 0 : -1;
}

int hades_lifecycle_pause(uint64_t timeout) {
    if (!atomic_load_explicit(&enabled,memory_order_acquire) || !timeout || self.registered) {
        fprintf(stderr,"[hades-lifecycle] Pause requires configuration, a timeout and a non-guest controller\n");
        return -1;
    }
    pthread_mutex_lock(&lock);
    if (phase==HADES_LIFECYCLE_PAUSED) { pthread_mutex_unlock(&lock); return 0; }
    if (phase!=HADES_LIFECYCLE_RUNNING) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr,"[hades-lifecycle] Pause requested during another transition or after failure\n");
        return -1;
    }
    pause_start=raw_clock();
    phase=HADES_LIFECYCLE_REQUESTED;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    wake_hook();
    pthread_mutex_lock(&sleep_lock);
    pthread_cond_broadcast(&sleepers);
    pthread_mutex_unlock(&sleep_lock);
    pthread_mutex_lock(&lock);
    int status=wait_for(HADES_LIFECYCLE_PAUSED,timeout);
    pthread_mutex_unlock(&lock);
    return status;
}

int hades_lifecycle_resume(uint64_t timeout) {
    pthread_mutex_lock(&lock);
    if (phase!=HADES_LIFECYCLE_PAUSED || !timeout || self.registered) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr,"[hades-lifecycle] Resume requires a paused runtime, timeout and non-guest controller\n");
        return -1;
    }
    phase=HADES_LIFECYCLE_RESUMING;
    pthread_cond_broadcast(&changed);
    int status=wait_for(HADES_LIFECYCLE_RUNNING,timeout);
    pthread_mutex_unlock(&lock);
    return status;
}

int hades_lifecycle_quiescent(void) {
    pthread_mutex_lock(&lock);
    int value=atomic_load(&enabled) && !guest_running && !host_running
        && (phase==HADES_LIFECYCLE_DRAINING || phase==HADES_LIFECYCLE_PAUSED
            || phase==HADES_LIFECYCLE_RESUMING);
    pthread_mutex_unlock(&lock);
    return value;
}

enum hades_lifecycle_state hades_lifecycle_status(void) {
    pthread_mutex_lock(&lock);
    enum hades_lifecycle_state value=phase;
    pthread_mutex_unlock(&lock);
    return value;
}
