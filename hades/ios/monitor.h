#pragma once
#include <math.h>
#include <stdio.h>
#include <sys/resource.h>

typedef struct {
    double cpu_seconds, time;
    int initialized;
} hades_cpu_counter;

static inline int hades_cpu_seconds(double *seconds) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF,&usage)) {
        perror("[Hades-iOS] getrusage system monitor");
        return -1;
    }
    *seconds=(double)usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1000000.0
        +(double)usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1000000.0;
    return 0;
}

static inline int hades_cpu_sample(hades_cpu_counter *counter, double cpu_seconds,
                                    double time, double *percent) {
    if (!isfinite(cpu_seconds) || cpu_seconds<0 || !isfinite(time) || time<0
        || (counter->initialized && (time<counter->time || cpu_seconds<counter->cpu_seconds))) {
        *counter=(hades_cpu_counter){0};
        return -1;
    }
    if (!counter->initialized) {
        *counter=(hades_cpu_counter){.cpu_seconds=cpu_seconds,.time=time,.initialized=1};
        return 0;
    }
    double elapsed=time-counter->time;
    if (elapsed<1.0)
        return 0;
    *percent=100.0*(cpu_seconds-counter->cpu_seconds)/elapsed;
    counter->cpu_seconds=cpu_seconds;
    counter->time=time;
    return 1;
}
