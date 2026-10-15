#include "../ios/monitor.h"
#include <assert.h>
#include <time.h>

static double monotonic(void) {
    struct timespec time;
    assert(!clock_gettime(CLOCK_MONOTONIC,&time));
    return time.tv_sec+time.tv_nsec/1000000000.0;
}

int main(void) {
    hades_cpu_counter counter={0};
    double percent=-1;
    assert(!hades_cpu_sample(&counter,10,1,&percent) && percent==-1);
    assert(!hades_cpu_sample(&counter,10.5,1.5,&percent));
    assert(hades_cpu_sample(&counter,12.5,2,&percent)==1 && percent==250);
    assert(hades_cpu_sample(&counter,12.5,3,&percent)==1 && percent==0);
    assert(hades_cpu_sample(&counter,12,4,&percent)==-1 && !counter.initialized);
    assert(!hades_cpu_sample(&counter,20,5,&percent));
    assert(hades_cpu_sample(&counter,21,4,&percent)==-1 && !counter.initialized);
    assert(hades_cpu_sample(&counter,NAN,6,&percent)==-1);
    assert(hades_cpu_sample(&counter,1,INFINITY,&percent)==-1);
    assert(hades_cpu_sample(&counter,-1,6,&percent)==-1);
    double before,after;
    assert(!hades_cpu_seconds(&before) && before>=0);
    double deadline=monotonic()+.05;
    volatile unsigned long work=0;
    while (monotonic()<deadline)
        work++;
    assert(work && !hades_cpu_seconds(&after) && after>before);
    puts("PROCESS_CPU_MONITOR_OK: actual self accounting, elapsed-time utilization, multicore >100%, reset and invalid sample rejection");
    return 0;
}
