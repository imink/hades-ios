#include "../runtime/trace.h"
#include <assert.h>
#include <signal.h>

static int expected;
static void rejected(int signal) {
    (void)signal;
    _Exit(77);
}
static void *read_policy(void *argument) {
    (void)argument;
    for (unsigned i=0;i<10000;i++)
        assert(hades_trace_verbose()==expected);
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);
    expected=atoi(argv[1]);
    assert(signal(SIGABRT,rejected)!=SIG_ERR);
    pthread_t readers[16];
    for (unsigned i=0;i<16;i++)
        assert(!pthread_create(&readers[i],NULL,read_policy,NULL));
    for (unsigned i=0;i<16;i++)
        assert(!pthread_join(readers[i],NULL));
    puts("TRACE_POLICY_OK");
    return 0;
}
