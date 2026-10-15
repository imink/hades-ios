#include "../runtime/display.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    uint32_t width=0,height=0;
    assert(!hades_display_extent(NULL,&width,&height) && width==1280 && height==720);
    assert(!hades_display_extent("1080p",&width,&height) && width==1920 && height==1080);
    assert(!hades_display_extent("wide-720p",&width,&height) && width==1560 && height==720);
    assert(!hades_display_extent("720p",&width,&height) && width==1280 && height==720);
    const char *bad[]={"","1080","1920x1080","1080P"," 1080p","1080p ","4k"};
    for (unsigned i=0;i<sizeof bad/sizeof bad[0];i++) {
        assert(hades_display_extent(bad[i],&width,&height)==-1);
        assert(width==1280 && height==720);
    }
    hades_display_rect rect;
    assert(!hades_display_layout(932,430,1560,720,&rect));
    assert(rect.x==0 && rect.y==0 && rect.width==932 && rect.height==430);
    assert(!hades_display_layout(932,430,1280,720,&rect));
    assert(fabs(rect.width-430*16.0/9)<.0001 && rect.x>83 && rect.y==0 && rect.height==430);
    assert(!hades_display_layout(932,430,1920,1080,&rect));
    assert(fabs(rect.width-430*16.0/9)<.0001 && rect.x>83);
    assert(!hades_display_layout(1024,768,1560,720,&rect));
    assert(rect.x==0 && rect.y>147 && fabs(rect.width/rect.height-1560.0/720)<.0001);
    assert(hades_display_layout(0,430,1560,720,&rect)==-1);
    assert(hades_display_layout(NAN,430,1560,720,&rect)==-1);
    assert(hades_display_layout(932,INFINITY,1560,720,&rect)==-1);
    assert(hades_display_layout(932,430,0,720,&rect)==-1);
    puts("DISPLAY_EXTENT_PRESETS_OK");
    return 0;
}
