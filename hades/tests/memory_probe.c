#define _DARWIN_C_SOURCE
#include "../runtime/memory.h"
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t get_info(unsigned id) {
    uint64_t registers[4] = {0,id,0xFFFF8001,0};
    assert(hades_memory_svc(0x29, registers) == 1);
    assert(registers[0] == 0);
    return registers[1];
}

int main(void) {
    const size_t mib = 1024 * 1024;
    hades_memory_config config = {4*mib,4*mib,8*mib,2*mib,0x1234,1ull << 39,1ull << 44,1,0,0};
    assert(!hades_memory_initialize(&config));
    assert(hades_memory_initialize(&config));
    uintptr_t arena = get_info(12), heap = get_info(4);
    assert(!(arena % 0x200000) && !(heap % 0x200000));
    assert(get_info(13) == 16*mib && heap == arena + 8*mib);
    assert(get_info(6) == 10*mib && get_info(7) == 2*mib && get_info(18) == 0x1234);
    assert(get_info(0) == 1 && get_info(1) == (1ull << 44));
    assert(!get_info(16) && !get_info(17));
    hades_memory_info information;
    uint32_t key = 0;
    assert(!hades_memory_check_empty_key((uintptr_t)&key, -1));
    assert(hades_memory_check_empty_key((uintptr_t)&key, -2));
    assert(hades_memory_check_empty_key((uintptr_t)&key + 1, 1));
    key = 1;
    assert(hades_memory_check_empty_key((uintptr_t)&key, 1));
    assert(!hades_memory_info_query(0, &information));
    assert(information.base == 0 && information.size && information.state == 0x10
           && !information.permissions && !hades_memory_writable(0, 1));
    assert(!hades_memory_info_query(UINTPTR_MAX, &information));
    assert(information.base == config.address_space_limit && information.state == 0x10
           && information.size == 0 - config.address_space_limit);
    assert(!hades_memory_info_query(heap, &information));
    assert(!information.permissions && !information.state);
    uint64_t registers[4] = {0,4*mib,0,0};
    assert(hades_memory_svc(1, registers) == 1 && registers[0] == 0 && registers[1] == heap);
    *(volatile unsigned char *)heap = 23;
    *(volatile unsigned char *)(heap + 3*mib) = 31;
    assert(!hades_memory_info_query(heap, &information));
    assert(information.state == 5 && information.permissions == 3 && information.size == 4*mib);
    assert(get_info(7) == 6*mib);
    assert(hades_memory_writable(heap, 4*mib));
    assert(!hades_memory_writable(heap + 4*mib - 1, 2));
    registers[0] = (uintptr_t)&information;
    registers[2] = heap;
    assert(hades_memory_svc(6, registers) == 1 && information.state == 5);
    registers[1] = 1;
    assert(hades_memory_svc(1, registers) == 1 && registers[0] == 0xD401);
    registers[1] = 10*mib;
    assert(hades_memory_svc(1, registers) == 1 && registers[0] == 0x10801);
    assert(get_info(7) == 6*mib);
    registers[1] = 2*mib;
    assert(hades_memory_svc(1, registers) == 1 && registers[0] == 0);
    assert(get_info(7) == 4*mib);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        signal(SIGBUS, SIG_DFL);
        signal(SIGSEGV, SIG_DFL);
        *(volatile unsigned char *)(heap + 3*mib) = 9;
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGBUS || WTERMSIG(status) == SIGSEGV);
    registers[1] = 4*mib;
    assert(hades_memory_svc(1, registers) == 1 && !registers[0]);
    assert(*(volatile unsigned char *)heap == 23);
    assert(*(volatile unsigned char *)(heap + 3*mib) == 0);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t stack = get_info(14), source = heap + page;
    *(volatile unsigned char *)source = 93;
    registers[0] = stack;
    registers[1] = source;
    registers[2] = page;
    assert(hades_memory_svc(4, registers) == 1 && !registers[0]);
    assert(*(volatile unsigned char *)stack == 93);
    *(volatile unsigned char *)stack = 94;
    assert(!hades_memory_info_query(source, &information));
    assert(!information.permissions && information.attributes == 1);
    assert(!hades_memory_writable(source, 1));
    assert(!hades_memory_info_query(heap, &information) && information.size == page);
    assert(!hades_memory_info_query(stack, &information));
    assert(information.state == 0xB && information.size == page && information.permissions == 3);
    child = fork();
    assert(child >= 0);
    if (!child) {
        signal(SIGBUS, SIG_DFL);
        signal(SIGSEGV, SIG_DFL);
        *(volatile unsigned char *)source = 9;
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGBUS || WTERMSIG(status) == SIGSEGV);
    registers[0] = stack + page;
    assert(hades_memory_svc(4, registers) == -1);
    registers[1] = 0;
    assert(hades_memory_svc(1, registers) == -1);
    assert(hades_memory_shutdown());
    registers[0] = stack;
    registers[1] = source;
    registers[2] = page;
    assert(hades_memory_svc(5, registers) == 1 && !registers[0]);
    assert(*(volatile unsigned char *)source == 94);
    assert(!hades_memory_info_query(stack, &information) && !information.permissions);
    child = fork();
    assert(child >= 0);
    if (!child) {
        signal(SIGBUS, SIG_DFL);
        signal(SIGSEGV, SIG_DFL);
        *(volatile unsigned char *)stack = 9;
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGBUS || WTERMSIG(status) == SIGSEGV);
    registers[0] = stack + 1;
    assert(hades_memory_svc(4, registers) == 0);
    registers[0] = stack;
    assert(hades_memory_svc(5, registers) == -1);
    if (page == 16384) {
        registers[2] = 4096;
        assert(hades_memory_svc(4, registers) == 0);
    }
    void *data = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(data != MAP_FAILED && !hades_memory_register((uintptr_t)data, page, 4, 3, 98));
    registers[0] = stack;
    registers[1] = (uintptr_t)data;
    registers[2] = page;
    assert(hades_memory_svc(4, registers) == 1);
    assert(hades_memory_unregister(98));
    registers[0] = stack;
    assert(hades_memory_svc(5, registers) == 1);
    assert(!hades_memory_unregister(98) && !munmap(data, page));
    void *code = mmap(NULL, page, PROT_READ, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(code != MAP_FAILED && !hades_memory_register((uintptr_t)code, page, 3, 5, 99));
    assert(!hades_memory_info_query((uintptr_t)code, &information) && information.permissions == 5);
    assert(!hades_memory_writable((uintptr_t)code, 1));
    registers[0] = (uintptr_t)code;
    registers[2] = heap;
    assert(hades_memory_svc(6, registers) == -1);
    assert(hades_memory_register((uintptr_t)code, page, 3, 5, 99));
    assert(hades_memory_register(UINTPTR_MAX - 8, page, 3, 5, 99));
    assert(!hades_memory_unregister(99));
    assert(!hades_memory_info_query((uintptr_t)code, &information));
    assert(information.state == 0x10 && !information.permissions);
    assert(!munmap(code, page));
    assert(!hades_memory_info_query((uintptr_t)&information, &information));
    assert(information.state == 0xB && information.size == pthread_get_stacksize_np(pthread_self()));
    registers[1] = 999;
    registers[2] = 0xFFFF8001;
    registers[3] = 0;
    assert(hades_memory_svc(0x29, registers) == 0 && registers[1] == 999);
    assert(!hades_memory_shutdown());
    assert(hades_memory_info_query(0, &information));
    assert(!hades_memory_writable((uintptr_t)&information, sizeof information));
    config.subpage_copy_compatibility = 1;
    assert(!hades_memory_initialize(&config));
    heap = get_info(4);
    stack = get_info(14);
    registers[1] = 2*mib;
    assert(hades_memory_svc(1, registers) == 1 && !registers[0]);
    if (page == 16384) {
        unsigned char *bytes = (unsigned char *)heap;
        bytes[0] = 41;
        bytes[4096] = 42;
        bytes[8192] = 43;
        registers[0] = stack + 4096;
        registers[1] = heap;
        registers[2] = 4096;
        assert(hades_memory_svc(4, registers) == 1);
        assert(*(unsigned char *)(stack + 4096) == 41);
        *(unsigned char *)(stack + 4096) = 51;
        assert(bytes[0] == 41 && bytes[4096] == 42 && bytes[8192] == 43);
        assert(!hades_memory_writable(heap, 1));
        registers[0] = stack + 8192;
        registers[1] = heap + 4096;
        assert(hades_memory_svc(4, registers) == 1);
        *(unsigned char *)(stack + 8192) = 52;
        registers[0] = stack;
        registers[1] = heap + page;
        registers[2] = page;
        assert(hades_memory_svc(4, registers) == -1);
        registers[0] = stack + 4096;
        registers[1] = heap;
        registers[2] = 4096;
        assert(hades_memory_svc(5, registers) == 1);
        assert(bytes[0] == 51 && bytes[4096] == 42 && bytes[8192] == 43);
        assert(*(unsigned char *)(stack + 8192) == 52);
        assert(!hades_memory_writable(stack + 4096, 1));
        assert(*(unsigned char *)(stack + 4096) == 0);
        registers[0] = stack + 8192;
        registers[1] = heap + 4096;
        assert(hades_memory_svc(5, registers) == 1);
        assert(bytes[4096] == 52 && bytes[8192] == 43);
    }
    assert(!hades_memory_shutdown());
    return 0;
}
