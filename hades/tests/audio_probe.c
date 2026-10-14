#define _DARWIN_C_SOURCE
#include "../runtime/audio.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

int main(void) {
    hades_audio_output *output=NULL;
    assert(hades_audio_open(44100,2,&output)==-1 && !output);
    assert(hades_audio_open(48000,2,&output)==0);
    assert(hades_audio_state(output)==1 && !hades_audio_generation(output));
    int16_t silence[1920]={0};
    assert(hades_audio_append(output,123,silence,sizeof silence)==0);
    assert(hades_audio_contains(output,123) && hades_audio_count(output)==1);
    assert(hades_audio_append(output,123,silence,sizeof silence)==-1);
    assert(hades_audio_append(output,124,silence,3)==-1);
    float volume=0.25f;
    assert(hades_audio_volume(output,&volume,1)==0);
    volume=0;
    assert(hades_audio_volume(output,&volume,0)==0 && volume==0.25f);
    volume=2;
    assert(hades_audio_volume(output,&volume,1)==-1);
    assert(hades_audio_suspend(output,1)==0);
    assert(hades_audio_start(output,1)==0 && hades_audio_state(output)==0);
    struct timespec delay={.tv_nsec=10000000};
    nanosleep(&delay,NULL);
    assert(!hades_audio_generation(output));
    assert(hades_audio_suspend(output,0)==0);
    for (unsigned i=0;i<500 && !hades_audio_generation(output);i++)
        nanosleep(&delay,NULL);
    assert(hades_audio_generation(output)==1 && hades_audio_samples(output)==960);
    assert(!hades_audio_contains(output,123) && !hades_audio_count(output));
    uint64_t tags[2]={0};
    assert(hades_audio_release(output,tags,2)==1 && tags[0]==123);
    assert(!hades_audio_release(output,tags,2));
    assert(hades_audio_start(output,0)==0 && hades_audio_state(output)==1);
    assert(hades_audio_append(output,123,silence,sizeof silence)==0);
    assert(hades_audio_close(output)==0);
    puts("AUDIO_QUEUE_OK: real silent PCM completion, tags, lifecycle and ownership; no inferred audible game sound");
    return 0;
}
