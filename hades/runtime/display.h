#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct { double x,y,width,height; } hades_display_rect;

static inline int hades_display_extent(const char *preset, uint32_t *width, uint32_t *height) {
    if (!preset || !strcmp(preset,"720p")) {
        *width=1280;
        *height=720;
        return 0;
    }
    if (!strcmp(preset,"1080p")) {
        *width=1920;
        *height=1080;
        return 0;
    }
    if (!strcmp(preset,"wide-720p")) {
        *width=1560;
        *height=720;
        return 0;
    }
    return -1;
}

static inline int hades_display_layout(double width,double height,uint32_t render_width,
                                      uint32_t render_height,hades_display_rect *rect) {
    if (!isfinite(width) || !isfinite(height) || width<=0 || height<=0 || !render_width || !render_height)
        return -1;
    double aspect=(double)render_width/render_height;
    double w=width,h=height;
    /* Integer render-width alignment may differ from the phone aspect by less than one pixel. */
    if (fabs((width/height)/aspect-1)>.001) {
        if (width/height>aspect) w=height*aspect;
        else h=width/aspect;
    }
    *rect=(hades_display_rect){(width-w)/2,(height-h)/2,w,h};
    return 0;
}
