#include "../ios/joystick.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    hades_joystick_state state=hades_joystick_position(0,0);
    assert(!state.x && !state.y && !state.thumb_x && !state.thumb_y);
    state=hades_joystick_position(.12,0);
    assert(!state.x && !state.y && fabs(state.thumb_x-.12)<1e-12);
    state=hades_joystick_position(.06,.06);
    assert(!state.x && !state.y);
    state=hades_joystick_position(.56,0);
    assert(state.x==16384 && !state.y);
    state=hades_joystick_position(1,0);
    assert(state.x==32767 && !state.y && state.thumb_x==1);
    state=hades_joystick_position(-2,0);
    assert(state.x==-32767 && !state.y && state.thumb_x==-1);
    state=hades_joystick_position(0,1);
    assert(!state.x && state.y==32767 && state.thumb_y==1);
    state=hades_joystick_position(0,-1);
    assert(!state.x && state.y==-32767 && state.thumb_y==-1);
    state=hades_joystick_position(1,1);
    assert(state.x==23170 && state.y==23170);
    assert(fabs(hypot(state.thumb_x,state.thumb_y)-1)<1e-12);
    for (int angle=0;angle<360;angle++) {
        double radians=angle*acos(-1.0)/180.0;
        hades_joystick_state edge=hades_joystick_position(cos(radians),sin(radians));
        state=hades_joystick_position(10*cos(radians),10*sin(radians));
        assert(abs(state.x-edge.x)<=1 && abs(state.y-edge.y)<=1);
        assert(state.x>=-32767 && state.x<=32767 && state.y>=-32767 && state.y<=32767);
        assert(hypot(state.x,state.y)<=32768);
        assert(hypot(state.thumb_x,state.thumb_y)<=1.0+1e-12);
    }
    state=hades_joystick_position(0,0);
    assert(!state.x && !state.y && !state.thumb_x && !state.thumb_y);
    puts("IOS_ANALOG_JOYSTICK_OK");
    return 0;
}
