#define _DARWIN_C_SOURCE
#include "../runtime/callout.h"
#include "../runtime/memory.h"
#include "../runtime/lifecycle.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <unistd.h>

static atomic_uint ready, callbacks;
static atomic_int stop;
static void wake(void) {}
static int transition(int suspended) {
    (void)suspended;
    assert(hades_lifecycle_quiescent());
    return 0;
}

static void *abandoned(void *unused) {
    (void)unused;
    sigjmp_buf point;
    hades_lifecycle_thread_enter();
    if (!sigsetjmp(point,1)) {
        (void)hades_gate_stack_top();
        hades_lifecycle_host_enter();
        hades_gate_stack_abandon();
        siglongjmp(point,1);
    }
    hades_lifecycle_thread_leave();
    return NULL;
}

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
    hades_lifecycle_host_enter();
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
    hades_lifecycle_thread_enter();
    atomic_fetch_add(&ready,1);
    for (unsigned i=0;i<50 || !atomic_load(&stop);i++) {
        uint64_t args[6]={3,tag,3,4,5,6};
        hades_lifecycle_host_enter();
        assert(hades_callout_run((uintptr_t)guest,args)==tag+11);
        atomic_fetch_add(&callbacks,1);
        hades_lifecycle_host_leave();
    }
    hades_lifecycle_thread_leave();
    return NULL;
}

int main(void) {
    const size_t mib=1024*1024;
    hades_memory_config config={4*mib,4*mib,8*mib,0,0x1234,1ull<<39,1ull<<44,1,0,0};
    assert(!hades_memory_initialize(&config));
    assert(!hades_lifecycle_configure(transition,wake));
    pthread_t exiting;
    assert(!pthread_create(&exiting,NULL,abandoned,NULL));
    assert(!pthread_join(exiting,NULL));
    pthread_t threads[8];
    for (unsigned i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)(10+i)));
    while (atomic_load(&ready)!=8) usleep(100);
    for (unsigned i=0;i<4;i++) {
        assert(!hades_lifecycle_pause(1000000000));
        unsigned before=atomic_load(&callbacks);
        usleep(1000);
        assert(before==atomic_load(&callbacks));
        assert(!hades_lifecycle_resume(1000000000));
    }
    atomic_store(&stop,1);
    for (unsigned i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
    assert(!hades_memory_shutdown());
    puts("CALLOUT_OK: guarded reentrant guest/host stacks, six scalar arguments, nested live frames and 8-thread teardown; synthetic not SDK-thread proof");
    puts("CALLOUT_LIFECYCLE_OK: live guarded callbacks quiesce; ExitThread-style longjmp abandons gate ownership");
    return 0;
}
