#pragma once
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_once_t hades_trace_once=PTHREAD_ONCE_INIT;
static int hades_trace_full;
static void hades_trace_configure(void) {
    const char *value=getenv("HADES_TRACE");
    if (!value || !strcmp(value,"0"))
        return;
    if (strcmp(value,"1")) {
        fprintf(stderr,"[hades-trace] HADES_TRACE must be 0 or 1\n");
        abort();
    }
    hades_trace_full=1;
}
static inline int hades_trace_verbose(void) {
    if (pthread_once(&hades_trace_once,hades_trace_configure)) {
        fprintf(stderr,"[hades-trace] Cannot initialize trace policy\n");
        abort();
    }
    return hades_trace_full;
}
