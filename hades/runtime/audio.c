#include "audio.h"
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BUFFER_LIMIT=64 };
typedef struct {
    AudioQueueBufferRef buffer;
    uint64_t tag;
    unsigned frames;
    int released;
    uint64_t sequence;
} audio_buffer;
struct hades_audio_output {
    AudioQueueRef queue;
    pthread_mutex_t lock;
    audio_buffer buffers[BUFFER_LIMIT];
    unsigned channels;
    int started, suspended;
    unsigned append_diagnostics, release_diagnostics;
    uint64_t next_sequence;
    _Atomic uint64_t generation, samples;
};

static int failure(const char *operation, OSStatus status) {
    fprintf(stderr,"[hades-audio] %s failed: %d\n",operation,(int)status);
    return -1;
}

static void completed(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    hades_audio_output *output=context;
    (void)queue;
    pthread_mutex_lock(&output->lock);
    audio_buffer *entry=buffer->mUserData;
    if (!entry || entry->buffer!=buffer || entry->released) {
        fprintf(stderr,"[hades-audio] Invalid/duplicate native completion\n");
        abort();
    }
    entry->released=1;
    atomic_fetch_add_explicit(&output->samples,entry->frames,memory_order_relaxed);
    atomic_fetch_add_explicit(&output->generation,1,memory_order_release);
    pthread_mutex_unlock(&output->lock);
}

int hades_audio_open(unsigned rate, unsigned channels, hades_audio_output **result) {
    if (!result || rate!=48000 || !channels || channels>6)
        return failure("Unsupported PCM16 output format",-1);
    hades_audio_output *output=calloc(1,sizeof *output);
    if (!output)
        return failure("Allocate output",-1);
    int status=pthread_mutex_init(&output->lock,NULL);
    if (status) {
        free(output);
        return failure("Initialize output lock",status);
    }
    output->channels=channels;
    atomic_init(&output->generation,0);
    atomic_init(&output->samples,0);
    AudioStreamBasicDescription format={.mSampleRate=rate,.mFormatID=kAudioFormatLinearPCM,
        .mFormatFlags=kLinearPCMFormatFlagIsSignedInteger|kLinearPCMFormatFlagIsPacked,
        .mBytesPerPacket=2*channels,.mFramesPerPacket=1,.mBytesPerFrame=2*channels,
        .mChannelsPerFrame=channels,.mBitsPerChannel=16};
    status=AudioQueueNewOutput(&format,completed,output,NULL,NULL,0,&output->queue);
    if (status) {
        pthread_mutex_destroy(&output->lock);
        free(output);
        return failure("Create real AudioQueue output",status);
    }
    *result=output;
    fprintf(stderr,"[hades-audio] REAL_OUTPUT_OPEN rate=%u channels=%u PCM16; not proof of audible game sound\n",rate,channels);
    return 0;
}

int hades_audio_close(hades_audio_output *output) {
    if (!output)
        return failure("Close missing output",-1);
    OSStatus status=AudioQueueDispose(output->queue,true);
    if (status)
        return failure("Dispose output and drain callbacks",status);
    pthread_mutex_destroy(&output->lock);
    free(output);
    return 0;
}

int hades_audio_start(hades_audio_output *output, int started) {
    if (!output || (started!=0 && started!=1))
        return failure("Invalid playback transition",-1);
    if (started!=output->started && !output->suspended) {
        OSStatus status=started ? AudioQueueStart(output->queue,NULL) : AudioQueuePause(output->queue);
        if (status)
            return failure("Playback transition",status);
    }
    output->started=started;
    return 0;
}

int hades_audio_suspend(hades_audio_output *output, int suspended) {
    if (!output || (suspended!=0 && suspended!=1))
        return failure("Invalid suspension transition",-1);
    if (suspended!=output->suspended && output->started) {
        OSStatus status=suspended ? AudioQueuePause(output->queue) : AudioQueueStart(output->queue,NULL);
        if (status)
            return failure("Audio lifecycle transition",status);
    }
    output->suspended=suspended;
    return 0;
}

int hades_audio_append(hades_audio_output *output, uint64_t tag, const void *pcm, size_t size) {
    if (!output || !tag || !pcm || !size || size>4*1024*1024 || size%(2*output->channels))
        return failure("Invalid PCM extent, tag or interleaved frames",-1);
    pthread_mutex_lock(&output->lock);
    audio_buffer *entry=NULL;
    for (unsigned i=0;i<BUFFER_LIMIT;i++) {
        if (output->buffers[i].tag==tag) {
            pthread_mutex_unlock(&output->lock);
            return failure("Duplicate in-flight/released buffer tag",-1);
        }
        if (!output->buffers[i].buffer && !entry)
            entry=&output->buffers[i];
    }
    if (!entry || output->next_sequence==UINT64_MAX) {
        pthread_mutex_unlock(&output->lock);
        return failure("Owned audio buffer capacity exhausted",-1);
    }
    if (output->append_diagnostics++<16)
        fprintf(stderr,"[hades-audio] append tag=%#llx size=%zu completions=%llu\n",
                (unsigned long long)tag,size,(unsigned long long)hades_audio_generation(output));
    AudioQueueBufferRef buffer=NULL;
    OSStatus status=AudioQueueAllocateBuffer(output->queue,(UInt32)size,&buffer);
    if (status) {
        pthread_mutex_unlock(&output->lock);
        return failure("Allocate native PCM snapshot",status);
    }
    memcpy(buffer->mAudioData,pcm,size);
    buffer->mAudioDataByteSize=(UInt32)size;
    *entry=(audio_buffer){.buffer=buffer,.tag=tag,.frames=(unsigned)(size/(2*output->channels)),
        .sequence=++output->next_sequence};
    buffer->mUserData=entry;
    pthread_mutex_unlock(&output->lock);
    status=AudioQueueEnqueueBuffer(output->queue,buffer,0,NULL);
    if (status) {
        OSStatus cleanup=AudioQueueFreeBuffer(output->queue,buffer);
        pthread_mutex_lock(&output->lock);
        memset(entry,0,sizeof *entry);
        pthread_mutex_unlock(&output->lock);
        if (cleanup)
            return failure("Rollback unqueued PCM buffer",cleanup);
        return failure("Enqueue real PCM buffer",status);
    }
    return 0;
}

unsigned hades_audio_release(hades_audio_output *output, uint64_t *tags, unsigned capacity) {
    unsigned count=0;
    pthread_mutex_lock(&output->lock);
    while (count<capacity) {
        audio_buffer *entry=NULL;
        for (unsigned i=0;i<BUFFER_LIMIT;i++)
            if (output->buffers[i].released==1
                && (!entry || output->buffers[i].sequence<entry->sequence))
                entry=&output->buffers[i];
        if (!entry)
            break;
        {
            entry->released=2;
            pthread_mutex_unlock(&output->lock);
            OSStatus status=AudioQueueFreeBuffer(output->queue,entry->buffer);
            pthread_mutex_lock(&output->lock);
            if (status) {
                failure("Free completed PCM snapshot",status);
                abort();
            }
            tags[count++]=entry->tag;
            if (output->release_diagnostics++<16)
                fprintf(stderr,"[hades-audio] release tag=%#llx\n",(unsigned long long)entry->tag);
            memset(entry,0,sizeof *entry);
        }
    }
    pthread_mutex_unlock(&output->lock);
    return count;
}

unsigned hades_audio_count(hades_audio_output *output) {
    unsigned count=0;
    pthread_mutex_lock(&output->lock);
    for (unsigned i=0;i<BUFFER_LIMIT;i++)
        count+=output->buffers[i].buffer!=NULL && !output->buffers[i].released;
    pthread_mutex_unlock(&output->lock);
    return count;
}

int hades_audio_contains(hades_audio_output *output, uint64_t tag) {
    int found=0;
    pthread_mutex_lock(&output->lock);
    for (unsigned i=0;i<BUFFER_LIMIT;i++)
        found|=output->buffers[i].tag==tag && !output->buffers[i].released;
    pthread_mutex_unlock(&output->lock);
    return found;
}

unsigned hades_audio_state(hades_audio_output *output) { return !output->started; }
uint64_t hades_audio_generation(hades_audio_output *output) {
    return atomic_load_explicit(&output->generation,memory_order_acquire);
}
uint64_t hades_audio_samples(hades_audio_output *output) {
    return atomic_load_explicit(&output->samples,memory_order_relaxed);
}
int hades_audio_volume(hades_audio_output *output, float *volume, int set) {
    if (!volume || (set && (!isfinite(*volume) || *volume<0 || *volume>1)))
        return failure("Invalid audio volume",-1);
    OSStatus status=set ? AudioQueueSetParameter(output->queue,kAudioQueueParam_Volume,*volume)
        : AudioQueueGetParameter(output->queue,kAudioQueueParam_Volume,volume);
    return status ? failure("AudioQueue volume",status) : 0;
}
