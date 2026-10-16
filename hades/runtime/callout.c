#define _DARWIN_C_SOURCE
#include "callout.h"
#include "memory.h"
#include "lifecycle.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEPTH_LIMIT 8
#ifdef HADES_VULKAN_HOST
#define HOST_STACK_SIZE (1024*1024)
#else
#define HOST_STACK_SIZE (64*1024)
#endif
#define CALLBACK_STACK_SIZE (256*1024)
typedef struct {
    unsigned host_depth, callback_depth;
    void *host[DEPTH_LIMIT], *callback[DEPTH_LIMIT];
    unsigned owner[DEPTH_LIMIT];
    size_t page;
} stacks;
static pthread_key_t key;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static int key_error;
static atomic_uint next_owner=0x50000000;
extern uint64_t hades_callout_enter(uintptr_t, const uint64_t *, void *);

static void fatal(const char *message) {
    fprintf(stderr,"[hades-callout] %s\n",message);
    abort();
}

static void destroy(void *argument) {
    stacks *state=argument;
    if (state->host_depth || state->callback_depth)
        fatal("Thread exited inside an active guest/host callout");
    for (unsigned i=0;i<DEPTH_LIMIT;i++) {
        if (state->host[i] && munmap(state->host[i],HOST_STACK_SIZE+2*state->page))
            fatal("Cannot release owned host gate stack");
        if (state->callback[i] && munmap(state->callback[i],CALLBACK_STACK_SIZE+2*state->page))
            fatal("Cannot release owned guest callback stack");
    }
    free(state);
}

static void create_key(void) { key_error=pthread_key_create(&key,destroy); }

static stacks *state(void) {
    if (pthread_once(&once,create_key) || key_error)
        fatal("Cannot create callout thread storage");
    stacks *value=pthread_getspecific(key);
    if (!value) {
        value=calloc(1,sizeof *value);
        long page=sysconf(_SC_PAGESIZE);
        if (!value || page<=0 || HOST_STACK_SIZE%(size_t)page || CALLBACK_STACK_SIZE%(size_t)page)
            fatal("Invalid callout allocation or host page geometry");
        value->page=(size_t)page;
        if (pthread_setspecific(key,value))
            fatal("Cannot publish owned callout thread storage");
    }
    return value;
}

static void *prepare(void **reservation, size_t size, size_t page) {
    if (!*reservation) {
        void *base=mmap(NULL,size+2*page,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
        if (base==MAP_FAILED)
            fatal("Cannot reserve guarded callout stack");
        *reservation=base;
        if (mprotect((char *)base+page,size,PROT_READ|PROT_WRITE))
            fatal("Cannot commit guarded callout stack");
    }
    return (char *)*reservation+page;
}

void *hades_gate_stack_top(void) {
    stacks *value=state();
    if (value->host_depth==DEPTH_LIMIT)
        fatal("Guest/host gate nesting limit exceeded");
    void *base=prepare(&value->host[value->host_depth++],HOST_STACK_SIZE,value->page);
    return (char *)base+HOST_STACK_SIZE;
}

void hades_gate_stack_leave(void) {
    stacks *value=state();
    if (!value->host_depth)
        fatal("Host gate stack leave without active entry");
    hades_lifecycle_host_leave();
    value->host_depth--;
}

void hades_gate_stack_abandon(void) {
    stacks *value=state();
    if (value->host_depth!=1 || value->callback_depth)
        fatal("ExitThread outside an outermost host gate");
    value->host_depth=0;
}

uint64_t hades_callout_run(uintptr_t function, const uint64_t arguments[6]) {
    stacks *value=state();
    if (!function || !arguments || value->callback_depth==DEPTH_LIMIT)
        fatal("Invalid callback or guest callback nesting limit exceeded");
    unsigned slot=value->callback_depth++;
    void *base=prepare(&value->callback[slot],CALLBACK_STACK_SIZE,value->page);
    if (!value->owner[slot]) {
        unsigned owner=atomic_fetch_add(&next_owner,1);
        if (owner<0x50000000 || owner>=0x60000000)
            fatal("Callback stack owner namespace exhausted");
        value->owner[slot]=owner;
    }
    if (hades_memory_register_callback_stack((uintptr_t)base,CALLBACK_STACK_SIZE,value->owner[slot]))
        fatal("Cannot register actual guest callback stack");
    uint64_t result=hades_callout_enter(function,arguments,(char *)base+CALLBACK_STACK_SIZE);
    if (hades_memory_unregister_external(value->owner[slot]))
        fatal("Cannot revoke guest callback stack registration");
    value->callback_depth--;
    return result;
}
