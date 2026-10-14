#define _DARWIN_C_SOURCE
#include "../runtime/callout.h"
#include "../runtime/memory.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>

extern unsigned char hades_host_stubs[];
static uint64_t guest(uint64_t depth,uint64_t tag,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    volatile unsigned char scratch[8192];
    assert(a==3 && b==4 && c==5 && d==6);
    hades_memory_info info;
    assert(!hades_memory_info_query((uintptr_t)scratch,&info) && info.state==0xB && info.permissions==3);
    scratch[0]=31;scratch[8191]=73;
    uint64_t (*host)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)=(void *)hades_host_stubs;
    uint64_t result=host(depth,tag,a,b,c,d);
    assert(scratch[0]==31 && scratch[8191]==73);
    return result+2;
}

void hades_host_dispatch(uint64_t index,uint64_t *registers) {
    assert(!index && registers[2]==3 && registers[3]==4 && registers[4]==5 && registers[5]==6);
    volatile unsigned char scratch[32768];
    for (unsigned i=0;i<sizeof scratch;i++) scratch[i]=(unsigned char)i;
    if (registers[0]) {
        uint64_t args[6]={registers[0]-1,registers[1],3,4,5,6};
        registers[0]=hades_callout_run((uintptr_t)guest,args)+1;
    } else
        registers[0]=registers[1];
    for (unsigned i=0;i<sizeof scratch;i++) assert(scratch[i]==(unsigned char)i);
}

static void *worker(void *argument) {
    uint64_t tag=(uintptr_t)argument;
    for (unsigned i=0;i<50;i++) {
        uint64_t args[6]={3,tag,3,4,5,6};
        assert(hades_callout_run((uintptr_t)guest,args)==tag+11);
    }
    return NULL;
}

int main(void) {
    const size_t mib=1024*1024;
    hades_memory_config config={4*mib,4*mib,8*mib,0,0x1234,1ull<<39,1ull<<44,1,0,0};
    assert(!hades_memory_initialize(&config));
    pthread_t threads[8];
    for (unsigned i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)(10+i)));
    for (unsigned i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
    assert(!hades_memory_shutdown());
    puts("CALLOUT_OK: guarded reentrant guest/host stacks, six scalar arguments, nested live frames and 8-thread teardown; synthetic not SDK-thread proof");
    return 0;
}
