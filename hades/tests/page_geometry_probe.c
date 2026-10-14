#define _DARWIN_C_SOURCE
#include <assert.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    assert(page == 4096 || page == 16384);
    unsigned char *source = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    unsigned char *target = mmap(NULL, 4 * page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(source != MAP_FAILED && target != MAP_FAILED);
    for (size_t i = 0; i < 4 * page; i += 4096)
        source[i] = (unsigned char)(i / 4096 + 1);
    vm_address_t requested = (vm_address_t)target + 4096, mapped = requested;
    vm_prot_t current, maximum;
    kern_return_t result = vm_remap(mach_task_self(), &mapped, 4096, 0,
                                    VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                                    (vm_address_t)source, FALSE, &current, &maximum, VM_INHERIT_NONE);
    int exact = result == KERN_SUCCESS && mapped == requested && *(unsigned char *)requested == source[0];
    assert(page == 4096 ? exact : !exact);
    assert(!mprotect(source, 4096, PROT_NONE));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        signal(SIGBUS, SIG_DFL);
        signal(SIGSEGV, SIG_DFL);
        source[8192] = 9;
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    int neighbor_denied = WIFSIGNALED(status)
        && (WTERMSIG(status) == SIGBUS || WTERMSIG(status) == SIGSEGV);
    assert(page == 4096 ? WIFEXITED(status) && WEXITSTATUS(status) == 0 : neighbor_denied);
    printf("PAGE_GEOMETRY host=%zu guest=4096 remap_result=%d exact_unaligned_alias=%d adjacent_guest_page_denied=%d\n",
           page, result, exact, neighbor_denied);
    assert(!munmap(source, 4 * page) && !munmap(target, 4 * page));
    return 0;
}
