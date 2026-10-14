#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct hades_audio_output hades_audio_output;
int hades_audio_open(unsigned rate, unsigned channels, hades_audio_output **output);
int hades_audio_close(hades_audio_output *output);
int hades_audio_start(hades_audio_output *output, int started);
int hades_audio_suspend(hades_audio_output *output, int suspended);
int hades_audio_append(hades_audio_output *output, uint64_t tag, const void *pcm, size_t size);
unsigned hades_audio_release(hades_audio_output *output, uint64_t *tags, unsigned capacity);
unsigned hades_audio_count(hades_audio_output *output);
int hades_audio_contains(hades_audio_output *output, uint64_t tag);
unsigned hades_audio_state(hades_audio_output *output);
uint64_t hades_audio_generation(hades_audio_output *output);
uint64_t hades_audio_samples(hades_audio_output *output);
int hades_audio_volume(hades_audio_output *output, float *volume, int set);
