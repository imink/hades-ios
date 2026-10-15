#include "../ios/fps.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    hades_fps_counter counter={0};
    double fps=-1;
    assert(hades_fps_sample(&counter,100,10,&fps)==0 && fps==-1);
    assert(hades_fps_sample(&counter,130,10.5,&fps)==0 && fps==-1);
    assert(hades_fps_sample(&counter,160,11,&fps)==1 && fps==60);
    assert(hades_fps_sample(&counter,205,12.5,&fps)==1 && fps==30);
    assert(hades_fps_sample(&counter,205,13.5,&fps)==1 && fps==0);
    assert(hades_fps_sample(&counter,205,13.5,&fps)==0);
    assert(hades_fps_sample(&counter,295,14.5,&fps)==1 && fps==90);
    assert(hades_fps_sample(&counter,1,15,&fps)==2 && counter.frames==1);
    assert(hades_fps_sample(&counter,61,16,&fps)==1 && fps==60);
    assert(hades_fps_sample(&counter,62,15.9,&fps)==-1 && counter.frames==61);
    assert(hades_fps_sample(&counter,62,NAN,&fps)==-1 && counter.time==16);
    assert(hades_fps_sample(&counter,62,INFINITY,&fps)==-1);
    counter=(hades_fps_counter){0};
    assert(hades_fps_sample(&counter,10000,50,&fps)==0);
    assert(hades_fps_sample(&counter,10060,51,&fps)==1 && fps==60);
    counter=(hades_fps_counter){0};
    uint64_t start=UINT64_MAX-100;
    assert(hades_fps_sample(&counter,start,100,&fps)==0);
    assert(hades_fps_sample(&counter,start+60,101,&fps)==1 && fps==60);
    assert(hades_fps_sample(&counter,2,102,&fps)==2);
    puts("IOS_PRESENT_FPS_OK");
    return 0;
}
