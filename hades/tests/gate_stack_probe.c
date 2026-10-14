#define _DARWIN_C_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static _Alignas(16) _Thread_local unsigned char host_stack[65536];
static uintptr_t guest_base;
static int active;
extern void gate_stack_test(uintptr_t top, uint64_t *output);
extern void gate_stack_return(void);

void *hades_gate_stack_top(void) {
    assert(!active);
    active = 1;
    return host_stack + sizeof host_stack;
}

void hades_gate_stack_leave(void) { assert(active); active = 0; }
uint64_t unimpl_c(unsigned index, const uint64_t *registers) {
    (void)index; (void)registers;
    abort();
}
void dm_thread_returned(void) { abort(); }

uint64_t gate_dispatch(uint64_t ret, uint64_t *registers) {
    volatile unsigned char scratch[24576];
    assert(active && (uintptr_t)scratch >= (uintptr_t)host_stack);
    assert((uintptr_t)scratch + sizeof scratch <= (uintptr_t)host_stack + sizeof host_stack);
    assert((uintptr_t)registers >= guest_base && (uintptr_t)registers + 800 <= guest_base + 4096);
    assert(ret == (uintptr_t)gate_stack_return);
    for (unsigned i = 0; i < 30; i++)
        if (i != 18)
#ifdef HADES_SYMBOL_GATE_TEST
            assert(registers[i] == (i == 9 ? 3 : 0x100 + i));
#else
            assert(registers[i] == 0x100 + i);
#endif
    for (unsigned i = 0; i < 32; i++)
        for (unsigned byte = 0; byte < 16; byte++)
            assert(((uint8_t *)registers)[256 + i*16 + byte] == i + 1);
    assert(registers[96] == 0xA0000000 && registers[97] == 0x08000001 && registers[98] == 0x00400000);
    for (unsigned i = 0; i < sizeof scratch; i++)
        scratch[i] = (unsigned char)i;
    assert(scratch[1000] == (unsigned char)1000);
    registers[0] = 0xFEED;
    registers[19] = 0xBEEF;
    for (unsigned byte = 0; byte < 16; byte++)
        ((uint8_t *)registers)[256 + 31*16 + byte] = 0xA5;
    return 0;
}

#ifdef HADES_SYMBOL_GATE_TEST
void hades_host_dispatch(uint64_t index, uint64_t *registers) {
    assert(index == 3);
    assert(!gate_dispatch(registers[30], registers));
}
#endif

int main(void) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *reservation = mmap(NULL, 3*page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(reservation != MAP_FAILED);
    uintptr_t middle = (uintptr_t)reservation + page;
    assert(!mprotect((void *)middle, page, PROT_READ | PROT_WRITE));
    guest_base = middle;
    _Alignas(16) uint64_t output[100] = {0};
    gate_stack_test(guest_base + 4096, output);
    assert(!active);
    for (unsigned i = 0; i < 30; i++)
        if (i != 18)
            assert(output[i] == (i == 0 ? 0xFEED : i == 19 ? 0xBEEF :
#ifdef HADES_SYMBOL_GATE_TEST
                                 i == 9 ? 3 :
#endif
                                 0x100 + i));
    assert(output[30] == (uintptr_t)gate_stack_return);
    for (unsigned i = 0; i < 32; i++)
        for (unsigned byte = 0; byte < 16; byte++)
            assert(((uint8_t *)output)[256 + i*16 + byte] == (i == 31 ? 0xA5 : i + 1));
    assert(output[96] == 0xA0000000 && output[97] == 0x08000001 && output[98] == 0x00400000);
    assert(!munmap(reservation, 3*page));
    puts("GATE_STACK_OK: 4KiB guest stack, 24KiB host C frame, GP/SIMD/NZCV/FPSR/FPCR preservation and modified outputs");
    return 0;
}
