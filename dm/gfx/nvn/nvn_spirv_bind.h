#pragma once
#include "nvn_obj.h"

enum { NVN_SPIRV_BUFFER, NVN_SPIRV_TEXTURE, NVN_SPIRV_SAMPLER };
typedef struct { unsigned kind, stage, msl_index, nvn_slot; uint64_t minimum_size; } nvn_spirv_binding;
typedef struct {
    p_bind_buf uniform[2][16];
    uint64_t textures[2][128], samplers[2][128];
    uint8_t has_uniform[2][16], has_texture[2][128], has_sampler[2][128];
} nvn_spirv_state;
typedef int (*nvn_spirv_sink)(void *context, const nvn_spirv_binding *binding, uint64_t value, uint64_t size);

int nvn_spirv_record(nvn_spirv_state *state, unsigned op, const void *payload, size_t size,
                     char *error, size_t error_size);
int nvn_spirv_apply(const nvn_spirv_state *state, const nvn_spirv_binding *bindings, size_t count,
                    nvn_spirv_sink sink, void *context, char *error, size_t error_size);
