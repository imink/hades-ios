#define _DARWIN_C_SOURCE
#include "../runtime/lifecycle.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t native_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event=PTHREAD_COND_INITIALIZER;
static atomic_uint ready, steps, native_steps, nested_steps, parked, resumes;
static atomic_int stop, held, late_started, fail_hook;

static void wake(void) { pthread_cond_broadcast(&event); }
static int transition(int suspended) {
    assert(hades_lifecycle_quiescent());
    assert(!atomic_load(&held));
    assert(!pthread_mutex_lock(&native_lock));
    pthread_mutex_unlock(&native_lock);
    int failure=atomic_load(&fail_hook);
    if (failure==1 || (failure==2 && !suspended)) return -1;
    atomic_fetch_add(suspended ? &parked : &resumes,1);
    return 0;
}

static void *worker(void *unused) {
    (void)unused;
    hades_lifecycle_thread_enter();
    atomic_fetch_add(&ready,1);
    while (!atomic_load(&stop)) {
        atomic_fetch_add(&steps,1);
        hades_lifecycle_host_enter();
        atomic_store(&held,1);
        hades_lifecycle_host_enter();
        atomic_fetch_add(&nested_steps,1);
        usleep(1000);
        hades_lifecycle_host_leave();
        atomic_store(&held,0);
        atomic_fetch_add(&native_steps,1);
        hades_lifecycle_host_leave();
    }
    hades_lifecycle_thread_leave();
    return NULL;
}

static void *waiter(void *unused) {
    (void)unused;
    hades_lifecycle_thread_enter();
    hades_lifecycle_host_enter();
    pthread_mutex_lock(&native_lock);
    atomic_fetch_add(&ready,1);
    while (!atomic_load(&stop))
        assert(!hades_lifecycle_wait(&event,&native_lock,NULL));
    pthread_mutex_unlock(&native_lock);
    hades_lifecycle_host_leave();
    hades_lifecycle_thread_leave();
    return NULL;
}

static void *sleeper(void *unused) {
    (void)unused;
    hades_lifecycle_thread_enter();
    hades_lifecycle_host_enter();
    atomic_fetch_add(&ready,1);
    uint64_t start=hades_lifecycle_clock_ns();
    assert(!hades_lifecycle_sleep(50000000));
    assert(hades_lifecycle_clock_ns()-start>=50000000);
    hades_lifecycle_host_leave();
    hades_lifecycle_thread_leave();
    return NULL;
}

static void *late(void *unused) {
    (void)unused;
    hades_lifecycle_thread_enter();
    atomic_store(&late_started,1);
    hades_lifecycle_thread_leave();
    return NULL;
}

static void *uncooperative(void *unused) {
    (void)unused;
    hades_lifecycle_thread_enter();
    atomic_store(&ready,1);
    for (;;) usleep(1000);
}

int main(int argc,char **argv) {
    assert(!hades_lifecycle_configure(transition,wake));
    pthread_t thread,wait,sleep,created;
    if (argc==2 && !strcmp(argv[1],"timeout")) {
        assert(!pthread_create(&thread,NULL,uncooperative,NULL));
        while (!atomic_load(&ready)) usleep(100);
        assert(hades_lifecycle_pause(20000000)==-1);
        assert(hades_lifecycle_status()==HADES_LIFECYCLE_FAILED);
        assert(!hades_lifecycle_quiescent());
        assert(!atomic_load(&parked));
        puts("LIFECYCLE_TIMEOUT_OK");
        return 0;
    }
    assert(!pthread_create(&thread,NULL,worker,NULL));
    assert(!pthread_create(&wait,NULL,waiter,NULL));
    assert(!pthread_create(&sleep,NULL,sleeper,NULL));
    while (atomic_load(&ready)!=3) usleep(100);
    if (argc==2 && !strcmp(argv[1],"failure")) {
        atomic_store(&fail_hook,1);
        assert(hades_lifecycle_pause(1000000000)==-1);
        assert(hades_lifecycle_status()==HADES_LIFECYCLE_FAILED);
        assert(!atomic_load(&parked));
        puts("LIFECYCLE_HOOK_FAILURE_OK");
        return 0;
    }
    if (argc==2 && !strcmp(argv[1],"resume-failure")) {
        assert(!hades_lifecycle_pause(1000000000));
        unsigned before=atomic_load(&steps);
        atomic_store(&fail_hook,2);
        assert(hades_lifecycle_resume(1000000000)==-1);
        assert(hades_lifecycle_status()==HADES_LIFECYCLE_FAILED);
        usleep(10000);
        assert(atomic_load(&steps)==before);
        assert(!atomic_load(&resumes));
        puts("LIFECYCLE_RESUME_FAILURE_PARKED_OK");
        return 0;
    }
    for (unsigned i=0;i<16;i++) {
        assert(!hades_lifecycle_pause(1000000000));
        assert(hades_lifecycle_status()==HADES_LIFECYCLE_PAUSED);
        assert(!hades_lifecycle_pause(1000000000));
        unsigned a=atomic_load(&steps),b=atomic_load(&native_steps),c=atomic_load(&nested_steps);
        uint64_t before=hades_lifecycle_clock_ns();
        if (!i) assert(!pthread_create(&created,NULL,late,NULL));
        usleep(10000);
        assert(atomic_load(&steps)==a && atomic_load(&native_steps)==b && atomic_load(&nested_steps)==c);
        assert(hades_lifecycle_clock_ns()==before);
        if (!i) assert(!atomic_load(&late_started));
        assert(!hades_lifecycle_resume(1000000000));
        assert(!hades_lifecycle_quiescent());
        if (!i) assert(!pthread_join(created,NULL) && atomic_load(&late_started));
        usleep(2000);
        assert(atomic_load(&steps)>a);
    }
    atomic_store(&stop,1);
    wake();
    assert(!pthread_join(thread,NULL));
    assert(!pthread_join(wait,NULL));
    assert(!pthread_join(sleep,NULL));
    assert(atomic_load(&parked)==16 && atomic_load(&resumes)==16);
    assert(hades_lifecycle_paused_ns()>=160000000);
    puts("LIFECYCLE_REPEATED_PAUSE_WAIT_CALLBACK_TIMER_OK");
    return 0;
}
