#pragma once
#include <math.h>
#include <stdint.h>

typedef struct {
    uint64_t frames;
    double time;
    int initialized;
} hades_fps_counter;

static inline int hades_fps_sample(hades_fps_counter *counter, uint64_t frames,
                                   double time, double *fps) {
    if (!isfinite(time) || time<0 || (counter->initialized && time<counter->time))
        return -1;
    if (!counter->initialized || frames<counter->frames) {
        int reset=counter->initialized;
        *counter=(hades_fps_counter){.frames=frames,.time=time,.initialized=1};
        return reset ? 2 : 0;
    }
    double elapsed=time-counter->time;
    if (elapsed<1.0)
        return 0;
    *fps=(double)(frames-counter->frames)/elapsed;
    counter->frames=frames;
    counter->time=time;
    return 1;
}
