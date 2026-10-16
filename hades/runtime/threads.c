#define _DARWIN_C_SOURCE
#include "threads.h"
#include "core.h"
#include "memory.h"
#include "lifecycle.h"
#include "callout.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    pthread_t pthread;
    uintptr_t entry, argument, stack_top;
    uint64_t id, mask;
    unsigned priority, core;
    uint32_t handle;
    int ready, started, exited, closed, cancel;
    unsigned waiting;
    uintptr_t wait_address, wait_key;
    uint32_t wait_tag;
    uint64_t wait_sequence;
} guest_thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static guest_thread threads[64];
static unsigned count;
static uint64_t priorities, cores;
static uint64_t sequence;
static _Thread_local guest_thread *current;
static _Thread_local sigjmp_buf exit_point;
void hades_threads_wake_lifecycle(void) { pthread_cond_broadcast(&changed); }
extern void dm_thread_enter(uint64_t entry, uint64_t argument, uint64_t stack_top);

static int error(const char *message) {
    fprintf(stderr, "[hades-thread] %s\n", message);
    return -1;
}

static guest_thread *find(uint64_t handle) {
    for (unsigned i = 0; i < count; i++)
        if (threads[i].handle == handle && !threads[i].closed)
            return &threads[i];
    return NULL;
}

static guest_thread *pick(unsigned kind, uintptr_t address) {
    guest_thread *best = NULL;
    for (unsigned i = 0; i < count; i++) {
        guest_thread *thread = &threads[i];
        if (thread->waiting == kind && (kind == 2 ? thread->wait_key : thread->wait_address) == address
            && (!best || thread->priority < best->priority
                || (thread->priority == best->priority && thread->wait_sequence < best->wait_sequence)))
            best = thread;
    }
    return best;
}

static int word_writable(uintptr_t address) {
    return !(address % 4) && hades_memory_writable(address, 4);
}

static void handoff(uintptr_t address) {
    guest_thread *thread = pick(1, address);
    uint32_t value = 0;
    if (thread) {
        thread->waiting = 0;
        value = thread->wait_tag | (pick(1, address) ? 0x40000000u : 0);
    }
    __atomic_store_n((uint32_t *)address, value, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&changed);
}

static void acquire(guest_thread *thread) {
    uint32_t *word = (uint32_t *)thread->wait_address;
    uint32_t value = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    for (;;) {
        if (!value) {
            if (__atomic_compare_exchange_n(word, &value, thread->wait_tag, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                thread->waiting = 0;
                pthread_cond_broadcast(&changed);
                return;
            }
        } else if (__atomic_compare_exchange_n(word, &value, value | 0x40000000u, 0,
                                               __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            thread->waiting = 1;
            thread->wait_sequence = ++sequence;
            return;
        }
    }
}

static int block(int64_t timeout, uint64_t *result) {
    uint64_t deadline = 0;
    if (timeout < -1)
        return error("Invalid key wait timeout");
    if (timeout > 0) {
        uint64_t ns = hades_lifecycle_clock_ns();
        if ((uint64_t)timeout > UINT64_MAX - ns)
            return error("Key wait deadline overflow");
        deadline = ns + (uint64_t)timeout;
    }
    while (current->waiting) {
        if (current->waiting == 2 && !timeout) {
            uintptr_t key = current->wait_key;
            current->waiting = 0;
            if (!pick(2, key))
                __atomic_store_n((uint32_t *)key, 0, __ATOMIC_RELEASE);
            *result = 0xEA01;
            return 0;
        }
        struct timespec relative = {0};
        if (timeout > 0 && current->waiting == 2) {
            uint64_t ns = hades_lifecycle_clock_ns();
            if (ns >= deadline) {
                timeout = 0;
                continue;
            }
            relative.tv_sec = (time_t)((deadline - ns) / 1000000000);
            relative.tv_nsec = (long)((deadline - ns) % 1000000000);
        }
        int status = hades_lifecycle_wait(&changed,&lock,
            timeout > 0 && current->waiting == 2 ? &relative : NULL);
        if (status == ETIMEDOUT)
            timeout = 0;
        else if (status)
            return error("Guest lock/key condition wait failed");
    }
    *result = 0;
    return 0;
}

int hades_threads_initialize(unsigned priority, unsigned core, uint64_t priority_mask, uint64_t core_mask) {
    pthread_mutex_lock(&lock);
    if (count || priority >= 64 || core >= 64 || !(priority_mask & (1ull << priority))
        || !(core_mask & (1ull << core)) || current) {
        pthread_mutex_unlock(&lock);
        return error("Invalid or duplicate main guest thread registration");
    }
    threads[0] = (guest_thread){.pthread=pthread_self(), .handle=0x100, .priority=priority,
        .core=core, .mask=core_mask, .ready=1, .started=1};
    if (pthread_threadid_np(pthread_self(), &threads[0].id)) {
        pthread_mutex_unlock(&lock);
        return error("Cannot obtain main host kernel thread identity");
    }
    priorities = priority_mask;
    cores = core_mask;
    count = 1;
    current = &threads[0];
    pthread_mutex_unlock(&lock);
    return 0;
}

static void *run(void *object) {
    guest_thread *thread = object;
    current = thread;
    if (hades_initialize_thread() || pthread_threadid_np(pthread_self(), &thread->id)) {
        error("Guest pthread ABI initialization failed");
        exit(3);
    }
    pthread_mutex_lock(&lock);
    thread->ready = 1;
    pthread_cond_broadcast(&changed);
    while (!thread->started && !thread->cancel)
        if (pthread_cond_wait(&changed, &lock)) {
            error("Guest start condition wait failed");
            exit(3);
        }
    int cancel = thread->cancel;
    pthread_mutex_unlock(&lock);
    if (!cancel) {
        hades_lifecycle_thread_enter();
        if (!sigsetjmp(exit_point, 1)) {
            dm_thread_enter(thread->entry, thread->argument, thread->stack_top);
            error("Guest thread returned instead of ExitThread");
            exit(3);
        }
    }
    pthread_mutex_lock(&lock);
    thread->exited = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    if (!cancel) hades_lifecycle_thread_leave();
    current = NULL;
    return NULL;
}

int hades_threads_shutdown(void) {
    pthread_mutex_lock(&lock);
    if (current != &threads[0] || !count) {
        pthread_mutex_unlock(&lock);
        return error("Thread shutdown must be performed by registered main thread");
    }
    for (unsigned i = 1; i < count; i++)
        if (threads[i].started && !threads[i].exited) {
            pthread_mutex_unlock(&lock);
            return error("Cannot unload modules with a live guest thread");
        }
    for (unsigned i = 1; i < count; i++)
        threads[i].cancel = 1;
    pthread_cond_broadcast(&changed);
    unsigned length = count;
    pthread_mutex_unlock(&lock);
    for (unsigned i = 1; i < length; i++) {
        if (pthread_join(threads[i].pthread, NULL))
            return error("Guest pthread join failed");
        if (hades_memory_unpin_stack(threads[i].stack_top))
            return error("Joined pthread stack ownership release failed");
    }
    pthread_mutex_lock(&lock);
    count = 0;
    current = NULL;
    pthread_mutex_unlock(&lock);
    return 0;
}

int hades_thread_svc(unsigned svc, uint64_t *registers) {
    if (!((svc >= 8 && svc <= 0x10) || (svc >= 0x1A && svc <= 0x1D)
          || svc == 0x25 || svc == 0x16 || svc == 0x18))
        return 0;
    if (!registers)
        return error("Missing thread register frame");
    pthread_mutex_lock(&lock);
    if (!current || !count) {
        pthread_mutex_unlock(&lock);
        return error("Kernel thread SVC from unregistered guest thread");
    }
    if (svc >= 0x1A && svc <= 0x1D) {
        uintptr_t address = svc == 0x1A ? registers[1] : registers[0];
        uintptr_t key = svc == 0x1C ? registers[1] : 0;
        if (!word_writable(address) || (key && !word_writable(key)) || (key && key == address)
            || ((svc == 0x1A || svc == 0x1C) && registers[2] != current->handle)
            || (svc == 0x1C && ((int64_t)registers[3] < -1))) {
            pthread_mutex_unlock(&lock);
            return error("Invalid kernel lock/key address, tag or timeout");
        }
        uint32_t *word = (uint32_t *)address;
        if (svc == 0x1A) {
            uint32_t owner = (uint32_t)registers[0];
            if (!find(owner)) {
                pthread_mutex_unlock(&lock);
                return error("ArbitrateLock names an unknown owner");
            }
            if (__atomic_load_n(word, __ATOMIC_ACQUIRE) == (owner | 0x40000000u)) {
                current->waiting = 1;
                current->wait_address = address;
                current->wait_tag = current->handle;
                current->wait_sequence = ++sequence;
                if (block(-1, &registers[0])) {
                    pthread_mutex_unlock(&lock);
                    return -1;
                }
            } else
                registers[0] = 0;
        } else if (svc == 0x1B) {
            if ((__atomic_load_n(word, __ATOMIC_ACQUIRE) & ~0x40000000u) != current->handle) {
                pthread_mutex_unlock(&lock);
                return error("ArbitrateUnlock caller does not own the mutex");
            }
            handoff(address);
            registers[0] = 0;
        } else if (svc == 0x1C) {
            if (!key || (__atomic_load_n(word, __ATOMIC_ACQUIRE) & ~0x40000000u) != current->handle) {
                pthread_mutex_unlock(&lock);
                return error("WaitProcessWideKey caller does not own the mutex");
            }
            /* Match D2R's corrected ordering: publish the waiter key before
               releasing the lock, otherwise a userspace signal can be lost. */
            __atomic_store_n((uint32_t *)key, 1, __ATOMIC_RELEASE);
            current->waiting = 2;
            current->wait_key = key;
            current->wait_address = address;
            current->wait_tag = current->handle;
            current->wait_sequence = ++sequence;
            handoff(address);
            if (block((int64_t)registers[3], &registers[0])) {
                pthread_mutex_unlock(&lock);
                return -1;
            }
        } else {
            int amount = (int32_t)registers[1], released = 0;
            guest_thread *thread;
            while ((amount <= 0 || released < amount) && (thread = pick(2, address))) {
                acquire(thread);
                released++;
            }
            if (!pick(2, address))
                __atomic_store_n(word, 0, __ATOMIC_RELEASE);
            registers[0] = 0;
        }
    } else if (svc == 8) {
        unsigned priority = (uint32_t)registers[4];
        int core = (int32_t)registers[5];
        if (core == -2)
            core = (int)threads[0].core;
        hades_memory_info entry = {0}, stack = {0};
        uintptr_t top = registers[3];
        if (count == sizeof threads / sizeof threads[0] || priority >= 64 || core < 0 || core >= 64
            || !(priorities & (1ull << priority)) || !(cores & (1ull << core))
            || hades_memory_info_query(registers[1], &entry) || !(entry.permissions & 4)
            || !top || top % 16 || hades_memory_info_query(top - 1, &stack)
            || stack.state != 0xB || stack.permissions != 3 || top - stack.base < 4096 - 32) {
            fprintf(stderr, "[hades-thread] rejected priority=%u core=%d entry=%#llx stack-top=%#llx\n",
                    priority, core, (unsigned long long)registers[1], (unsigned long long)top);
            pthread_mutex_unlock(&lock);
            return error("Invalid CreateThread entry, stack, NPDM policy or capacity");
        }
        guest_thread *thread = &threads[count];
        if (hades_memory_pin_stack(top)) {
            pthread_mutex_unlock(&lock);
            return error("Cannot retain exclusive guest stack ownership");
        }
        *thread = (guest_thread){.entry=registers[1], .argument=registers[2], .stack_top=top,
            .priority=priority, .core=(unsigned)core, .mask=1ull << core, .handle=0x100 + count};
        int result = pthread_create(&thread->pthread, NULL, run, thread);
        if (result) {
            if (hades_memory_unpin_stack(top))
                error("Failed pthread creation stack ownership rollback");
            pthread_mutex_unlock(&lock);
            return error("Cannot create stopped guest pthread");
        }
        count++;
        while (!thread->ready)
            if (hades_lifecycle_wait(&changed, &lock, NULL)) {
                error("Guest thread creation handshake failed");
                exit(3);
            }
        registers[0] = 0;
        registers[1] = thread->handle;
        fprintf(stderr, "[hades-thread] created stopped pthread handle=%#x priority=%u core=%u\n",
                thread->handle, priority, (unsigned)core);
    } else if (svc == 0x0A) {
        if (current == &threads[0]) {
            pthread_mutex_unlock(&lock);
            return error("Main thread ExitThread requires process lifecycle integration");
        }
        pthread_mutex_unlock(&lock);
#ifdef HADES_HOST_GATE_STACK
        hades_gate_stack_abandon();
#endif
        siglongjmp(exit_point, 1);
    } else if (svc == 0x0B) {
        int64_t ns = (int64_t)registers[0];
        pthread_mutex_unlock(&lock);
        if (ns > 0) {
            if (hades_lifecycle_sleep((uint64_t)ns))
                return error("Guest sleep failed");
        } else if (ns >= -2)
            sched_yield();
        else
            return error("Invalid negative sleep interval");
        registers[0] = 0;
        return 1;
    } else if (svc == 0x18) {
        uintptr_t address = registers[1];
        unsigned length = (uint32_t)registers[2];
        hades_memory_info info;
        if (length != 1 || hades_memory_info_query(address, &info) || !(info.permissions & 1)
            || info.size - (address - info.base) < 4) {
            pthread_mutex_unlock(&lock);
            return 0;
        }
        uint32_t handle;
        __builtin_memcpy(&handle, (const void *)address, 4);
        guest_thread *thread = find(handle);
        if (!thread) {
            pthread_mutex_unlock(&lock);
            return 0;
        }
        if (thread == current) {
            pthread_mutex_unlock(&lock);
            return error("Guest thread cannot wait for itself");
        }
        int64_t timeout = (int64_t)registers[3];
        uint64_t deadline = 0;
        if (timeout < -1) {
            pthread_mutex_unlock(&lock);
            return error("Invalid thread wait timeout or unavailable monotonic clock");
        }
        if (timeout > 0) {
            uint64_t ns = hades_lifecycle_clock_ns();
            if ((uint64_t)timeout > UINT64_MAX - ns) {
                pthread_mutex_unlock(&lock);
                return error("Thread wait deadline overflow");
            }
            deadline = ns + (uint64_t)timeout;
        }
        while (!thread->exited) {
            struct timespec relative = {0};
            if (timeout > 0) {
                uint64_t ns = hades_lifecycle_clock_ns();
                if (ns >= deadline)
                    timeout = 0;
                else {
                    relative.tv_sec = (time_t)((deadline - ns) / 1000000000);
                    relative.tv_nsec = (long)((deadline - ns) % 1000000000);
                }
            }
            if (!timeout) {
                registers[0] = 0xEA01;
                pthread_mutex_unlock(&lock);
                return 1;
            }
            int status = hades_lifecycle_wait(&changed,&lock,timeout > 0 ? &relative : NULL);
            if (status != 0 && status != ETIMEDOUT) {
                error("Guest thread exit wait failed");
                exit(3);
            }
        }
        registers[0] = registers[1] = 0;
    } else if (svc == 0x10) {
        registers[0] = current->core;
    } else {
        unsigned index = svc == 0x0C || svc == 0x25 ? 1 : svc == 0x0E ? 2 : 0;
        guest_thread *thread = find(registers[index]);
        if (!thread) {
            pthread_mutex_unlock(&lock);
            if (svc == 0x16)
                return 0;
            return error("Unknown or closed guest thread handle");
        }
        if (svc == 9) {
            if (thread->started || thread->exited) {
                pthread_mutex_unlock(&lock);
                return error("Guest thread cannot be started twice");
            }
            thread->started = 1;
            pthread_cond_broadcast(&changed);
            fprintf(stderr, "[hades-thread] started handle=%#x on SDK supplied stack\n", thread->handle);
        } else if (svc == 0x16) {
            if (thread == &threads[0] || (thread->started && !thread->exited)) {
                pthread_mutex_unlock(&lock);
                return error("Cannot close a running guest thread in this runtime");
            }
            thread->closed = 1;
            thread->cancel = 1;
            pthread_cond_broadcast(&changed);
        } else if (svc == 0x0C)
            registers[1] = thread->priority;
        else if (svc == 0x0D) {
            unsigned priority = (uint32_t)registers[1];
            if (priority >= 64 || !(priorities & (1ull << priority))) {
                pthread_mutex_unlock(&lock);
                return error("Thread priority is outside NPDM capability");
            }
            thread->priority = priority;
        } else if (svc == 0x0E) {
            registers[1] = thread->core;
            registers[2] = thread->mask;
        } else if (svc == 0x0F) {
            int ideal = (int32_t)registers[1];
            uint64_t mask = registers[2];
            if (ideal == -2) {
                ideal = (int)threads[0].core;
                mask = threads[0].mask;
            }
            if (!mask || (mask & ~cores) || ideal < -3 || ideal >= 64
                || (ideal >= 0 && !(mask & (1ull << ideal)))) {
                pthread_mutex_unlock(&lock);
                return error("Thread core mask is outside NPDM capability");
            }
            if (ideal >= 0)
                thread->core = (unsigned)ideal;
            thread->mask = mask;
            fprintf(stderr, "[hades-thread] logical core mask=%#llx; not host CPU affinity\n",
                    (unsigned long long)mask);
        } else if (svc == 0x25)
            registers[1] = thread->id;
        registers[0] = 0;
    }
    pthread_mutex_unlock(&lock);
    return 1;
}
