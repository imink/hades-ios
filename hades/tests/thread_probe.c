#define _DARWIN_C_SOURCE
#include "../runtime/threads.h"
#include "../runtime/memory.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static _Atomic uint32_t mutex_word, key;
static atomic_uint phase;
static uint32_t worker_handle;
static uintptr_t stack_top;
enum { ITERATIONS = 64 };

int hades_initialize_thread(void) { return 0; }
uint64_t unimpl_c(unsigned index, const uint64_t *registers) {
    (void)index; (void)registers;
    abort();
}
uint64_t gate_dispatch(uint64_t ret, uint64_t *registers) {
    (void)ret; (void)registers;
    abort();
}
void dm_thread_returned(void) { abort(); }

static void take_mutex(uint32_t tag) {
    uint32_t value = atomic_load(&mutex_word);
    for (;;) {
        if (!value) {
            if (atomic_compare_exchange_weak(&mutex_word, &value, tag))
                return;
        } else {
            uint32_t owner = value & ~0x40000000u;
            if (atomic_compare_exchange_weak(&mutex_word, &value, value | 0x40000000u)) {
                uint64_t registers[6] = {owner, (uintptr_t)&mutex_word, tag, 0, 0, 0};
                assert(hades_thread_svc(0x1A, registers) == 1 && !registers[0]);
                if ((atomic_load(&mutex_word) & ~0x40000000u) == tag)
                    return;
            }
            value = atomic_load(&mutex_word);
        }
    }
}

static void release_mutex(void) {
    uint64_t registers[6] = {(uintptr_t)&mutex_word, 0, 0, 0, 0, 0};
    assert(hades_thread_svc(0x1B, registers) == 1 && !registers[0]);
}

static void worker(void *argument) {
    unsigned char local;
    assert(argument == &phase && (uintptr_t)&local < stack_top && stack_top - (uintptr_t)&local < 1024 * 1024);
    uint64_t registers[6] = {0, worker_handle, 0, 0, 0, 0};
    assert(hades_thread_svc(0x25, registers) == 1 && registers[1]);
    for (unsigned i = 0; i < ITERATIONS; i++) {
        take_mutex(worker_handle);
        atomic_store(&phase, 2*i + 1);
        registers[0] = (uintptr_t)&mutex_word;
        registers[1] = (uintptr_t)&key;
        registers[2] = worker_handle;
        registers[3] = UINT64_MAX;
        assert(hades_thread_svc(0x1C, registers) == 1 && !registers[0]);
        assert((atomic_load(&mutex_word) & ~0x40000000u) == worker_handle);
        atomic_store(&phase, 2*i + 2);
        release_mutex();
        while (atomic_load(&phase) != 2*i + 3)
            sched_yield();
    }
    take_mutex(worker_handle);
    registers[0] = (uintptr_t)&mutex_word;
    registers[1] = (uintptr_t)&key;
    registers[2] = worker_handle;
    registers[3] = 0;
    assert(hades_thread_svc(0x1C, registers) == 1 && registers[0] == 0xEA01);
    assert(!atomic_load(&mutex_word) && !atomic_load(&key));
    registers[0] = 0;
    assert(hades_thread_svc(0x0A, registers) != 1);
    abort();
}

int main(void) {
    const size_t mib = 1024 * 1024, page = (size_t)sysconf(_SC_PAGESIZE);
    hades_memory_config configuration = {4*mib,4*mib,8*mib,0,0x1234,1ull << 39,1ull << 44,7,0,0};
    assert(!hades_memory_initialize(&configuration));
    assert(!hades_threads_initialize(44, 0, 1ull << 44, 7));
    assert(hades_threads_initialize(44, 0, 1ull << 44, 7));
    uintptr_t code = (uintptr_t)worker & ~(uintptr_t)(page - 1);
    assert(!hades_memory_register(code, page, 3, 5, 77));
    uintptr_t data = (uintptr_t)&mutex_word & ~(uintptr_t)(page - 1);
    assert(!hades_memory_register(data, page, 4, 3, 78));
    uint64_t registers[6] = {0,4,0xFFFF8001,0,0,0};
    assert(hades_memory_svc(0x29, registers) == 1);
    uintptr_t heap = registers[1];
    registers[1] = 2*mib;
    assert(hades_memory_svc(1, registers) == 1);
    registers[1] = 14;
    registers[2] = 0xFFFF8001;
    assert(hades_memory_svc(0x29, registers) == 1);
    uintptr_t stack = registers[1];
    registers[0] = stack;
    registers[1] = heap;
    registers[2] = mib;
    assert(hades_memory_svc(4, registers) == 1);
    stack_top = stack + mib;
    registers[1] = (uintptr_t)worker;
    registers[2] = (uintptr_t)&phase;
    registers[3] = stack_top;
    registers[4] = 20;
    registers[5] = 0;
    assert(hades_thread_svc(8, registers) == -1);
    registers[4] = 44;
    assert(hades_thread_svc(8, registers) == 1);
    worker_handle = (uint32_t)registers[1];
    assert(worker_handle == 0x101 && !atomic_load(&phase));
    uint64_t unmap[6] = {stack, heap, mib, 0, 0, 0};
    assert(hades_memory_svc(5, unmap) == -1);
    assert(hades_memory_unregister(77));
    uint32_t handles[1] = {worker_handle};
    registers[1] = (uintptr_t)handles;
    registers[2] = 1;
    registers[3] = 0;
    assert(hades_thread_svc(0x18, registers) == 1 && registers[0] == 0xEA01);
    registers[3] = 1000000;
    assert(hades_thread_svc(0x18, registers) == 1 && registers[0] == 0xEA01);
    registers[0] = worker_handle;
    registers[1] = -2;
    registers[2] = 0;
    assert(hades_thread_svc(0x0F, registers) == 1);
    atomic_store(&mutex_word, 0x100);
    registers[0] = worker_handle;
    assert(hades_thread_svc(9, registers) == 1);
    registers[0] = worker_handle;
    assert(hades_thread_svc(9, registers) == -1);
    release_mutex();
    for (unsigned i = 0; i < ITERATIONS; i++) {
        while (atomic_load(&phase) != 2*i + 1 || !atomic_load(&key))
            sched_yield();
        take_mutex(0x100);
        assert(hades_threads_shutdown());
        registers[0] = (uintptr_t)&key;
        registers[1] = 1;
        assert(hades_thread_svc(0x1D, registers) == 1 && !registers[0]);
        release_mutex();
        while (atomic_load(&phase) != 2*i + 2)
            sched_yield();
        atomic_store(&phase, 2*i + 3);
    }
    registers[1] = (uintptr_t)handles;
    registers[2] = 1;
    registers[3] = UINT64_MAX;
    assert(hades_thread_svc(0x18, registers) == 1 && !registers[0]);
    registers[0] = worker_handle;
    assert(hades_thread_svc(0x16, registers) == 1);
    assert(!hades_threads_shutdown());
    registers[0] = stack;
    registers[1] = heap;
    registers[2] = mib;
    assert(hades_memory_svc(5, registers) == 1);
    assert(!hades_memory_unregister(77) && !hades_memory_unregister(78));
    assert(!hades_memory_shutdown());
    printf("THREAD_OK: stopped pthread/start/SDK stack/exit/join and %u real mutex-condition handoffs; host tests, not full guest-exclusive correctness\n",
           ITERATIONS);
    return 0;
}
