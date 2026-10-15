#pragma once
#include <math.h>
#include <stdint.h>

typedef struct {
    int32_t x, y;
    double thumb_x, thumb_y;
} hades_joystick_state;

static inline hades_joystick_state hades_joystick_position(double x, double y) {
    const double dead_zone=0.12;
    double length=hypot(x,y);
    if (length==0)
        return (hades_joystick_state){0};
    double bounded=fmin(length,1.0);
    hades_joystick_state state={.thumb_x=x/length*bounded,.thumb_y=y/length*bounded};
    if (length>dead_zone) {
        double magnitude=(bounded-dead_zone)/(1.0-dead_zone);
        state.x=(int32_t)lround(x/length*magnitude*32767.0);
        state.y=(int32_t)lround(y/length*magnitude*32767.0);
    }
    return state;
}
