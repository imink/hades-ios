#define _DARWIN_C_SOURCE
#include "../runtime/ipc.h"
#include "../runtime/memory.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/mman.h>

int main(void) {
    const size_t mib=1024*1024, page=(size_t)sysconf(_SC_PAGESIZE);
    hades_memory_config config={4*mib,4*mib,8*mib,0,0x1234,1ull<<39,1ull<<44,1,0,0};
    assert(!hades_memory_initialize(&config) && !hades_ipc_initialize(123));
    uint64_t registers[4]={0,4*mib,0,0};
    assert(hades_memory_svc(1,registers)==1 && !registers[0]);
    uintptr_t heap=registers[1];
    for (unsigned permission=0;permission<4;permission++) {
        if (permission==2)
            continue;
        registers[0]=0;registers[1]=heap;registers[2]=page;registers[3]=permission;
        assert(hades_ipc_svc(0x15,registers,NULL,0)==1 && !registers[0]);
        uint32_t handle=(uint32_t)registers[1];
        unsigned char *receiver=hades_memory_transfer_buffer(handle,page);
        assert(receiver);
        receiver[0]=(unsigned char)(40+permission);
        hades_memory_info information;
        assert(!hades_memory_info_query(heap,&information));
        assert(information.permissions==permission && information.attributes==1 && information.ipc_references==1);
        assert(!hades_memory_info_query(heap+page,&information) && information.base==heap+page);
        if (permission)
            assert(*(unsigned char *)heap==40+permission);
        if (permission!=3) {
            pid_t child=fork();assert(child>=0);
            if (!child) { *(volatile unsigned char *)heap=99;_exit(0); }
            int status;assert(waitpid(child,&status,0)==child);
            assert(WIFSIGNALED(status) && (WTERMSIG(status)==SIGSEGV || WTERMSIG(status)==SIGBUS));
        } else {
            *(unsigned char *)heap=91;
            assert(receiver[0]==91);
        }
        assert(hades_ipc_shutdown() && hades_memory_shutdown());
        registers[0]=0;registers[1]=0;
        assert(hades_memory_svc(1,registers)==-1);
        registers[0]=handle;
        assert(hades_ipc_svc(0x16,registers,NULL,0)==1 && !registers[0]);
        assert(hades_memory_writable(heap,page));
        assert(*(unsigned char *)heap==(permission==3 ? 91 : 40+permission));
        assert(!hades_memory_transfer_buffer(handle,1));
        assert(hades_memory_transfer_release(handle));
    }
    registers[0]=0;registers[1]=heap+4096;registers[2]=4096;registers[3]=0;
    assert(hades_ipc_svc(0x15,registers,NULL,0)==-1);
    registers[1]=heap;registers[2]=page;registers[3]=2;
    assert(hades_ipc_svc(0x15,registers,NULL,0)==-1);
    unsigned char *data=mmap(NULL,3*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(data!=MAP_FAILED && !hades_memory_register((uintptr_t)data,3*page,4,3,98));
    data[0]=12;data[2*page]=34;
    registers[0]=0;registers[1]=(uintptr_t)data+page;registers[2]=page;registers[3]=0;
    assert(hades_ipc_svc(0x15,registers,NULL,0)==1 && !registers[0]);
    uint32_t handle=(uint32_t)registers[1];
    unsigned char *received=hades_memory_transfer_buffer(handle,page);
    assert(received);received[0]=56;
    assert(data[0]==12 && data[2*page]==34);
    assert(hades_memory_unregister(98));
    registers[0]=handle;
    assert(hades_ipc_svc(0x16,registers,NULL,0)==1 && !registers[0]);
    assert(data[page]==56 && !hades_memory_unregister(98) && !munmap(data,3*page));
    assert(!hades_ipc_shutdown() && !hades_memory_shutdown());
    puts("TRANSFER_MEMORY_OK: real shared alias, physical donor permissions, independent receiver access, shrink/shutdown/duplicate lifetime rejection");
    return 0;
}
