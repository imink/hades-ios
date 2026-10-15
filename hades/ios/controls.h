#pragma once
#include "../runtime/display.h"

static inline int hades_action_button_contains(double width, double height, double x, double y) {
    if (!isfinite(width) || !isfinite(height) || !isfinite(x) || !isfinite(y)
        || width<=0 || height<=0)
        return 0;
    double radius=fmin(width,height)/2;
    double dx=x-width/2, dy=y-height/2;
    return hypot(dx,dy)<=radius;
}

static inline int hades_action_button_layout(double width, double height,
                                              double left, double top, double right, double bottom,
                                              unsigned index, hades_display_rect *frame) {
    if (!frame || index>=4 || !isfinite(width) || !isfinite(height)
        || !isfinite(left) || !isfinite(top) || !isfinite(right) || !isfinite(bottom)
        || left<0 || top<0 || right<0 || bottom<0)
        return -1;
    double available_width=width-left-right, available_height=height-top-bottom;
    const double diameter=72, step=56, margin=12, bottom_margin=6;
    if (available_width<diameter+2*step+2*margin
        || available_height<diameter+2*step+2*bottom_margin)
        return -1;
    const double offsets[4][2]={{-1,0},{0,1},{0,-1},{1,0}};
    double center_x=width-right-margin-diameter/2-step;
    double center_y=height-bottom-bottom_margin-diameter/2-step;
    *frame=(hades_display_rect){center_x+offsets[index][0]*step-diameter/2,
                               center_y+offsets[index][1]*step-diameter/2,
                               diameter,diameter};
    return 0;
}
