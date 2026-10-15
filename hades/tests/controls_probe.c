#include "../ios/controls.h"
#include <assert.h>
#include <stdio.h>

static void check(double width,double height,double left,double top,double right,double bottom) {
    hades_display_rect frames[4];
    for (unsigned i=0;i<4;i++) {
        assert(!hades_action_button_layout(width,height,left,top,right,bottom,i,&frames[i]));
        assert(frames[i].width==72 && frames[i].height==72);
        assert(frames[i].x>=left && frames[i].x+72<=width-right);
        assert(frames[i].y>=top && frames[i].y+72<=height-bottom);
        for (unsigned j=0;j<i;j++)
            assert(hypot(frames[i].x-frames[j].x,frames[i].y-frames[j].y)>=72);
    }
    assert(frames[0].x<frames[1].x && frames[1].x<frames[3].x);
    assert(frames[2].y<frames[0].y && frames[0].y<frames[1].y);
    assert(frames[1].y-frames[0].y==56);
    double old_center_y=height-bottom-12-36-80;
    assert(frames[0].y+36==old_center_y+30);
    assert(frames[1].y+72==height-bottom-6);
}

int main(void) {
    check(932,430,59,0,59,21);
    check(932,430,0,0,59,21);
    check(667,375,0,0,0,0);
    check(568,320,0,0,0,0);
    check(1194,834,0,0,0,20);
    hades_display_rect frame;
    assert(hades_action_button_layout(932,430,59,0,59,21,4,&frame));
    assert(hades_action_button_layout(932,430,59,0,59,21,0,NULL));
    assert(hades_action_button_layout(200,200,0,0,0,0,0,&frame));
    assert(hades_action_button_layout(NAN,430,0,0,0,0,0,&frame));
    assert(hades_action_button_layout(932,430,-1,0,0,0,0,&frame));
    assert(hades_action_button_contains(72,72,36,36));
    assert(hades_action_button_contains(72,72,36,0));
    assert(hades_action_button_contains(72,72,72,36));
    assert(hades_action_button_contains(72,72,57,64));
    assert(!hades_action_button_contains(72,72,0,0));
    assert(!hades_action_button_contains(72,72,71,71));
    assert(!hades_action_button_contains(72,72,60,63));
    assert(!hades_action_button_contains(72,72,73,36));
    assert(!hades_action_button_contains(72,72,NAN,36));
    assert(!hades_action_button_contains(INFINITY,72,36,36));
    assert(!hades_action_button_contains(0,72,0,36));
    assert(!hades_action_button_contains(-72,72,0,36));
    puts("ACTION_BUTTON_LAYOUT_OK: 72pt circles, 56pt diamond steps, center 30pt lower, disjoint circular targets and safe areas");
    return 0;
}
