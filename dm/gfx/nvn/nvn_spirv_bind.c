#include "nvn_spirv_bind.h"
#include <stdio.h>

static int bad(char *error, size_t size, const char *message) {
    if (error && size)
        snprintf(error, size, "%s", message);
    return -1;
}

int nvn_spirv_record(nvn_spirv_state *state, unsigned op, const void *payload, size_t size,
                     char *error, size_t error_size) {
    if (!state || !payload)
        return bad(error, error_size, "Null NVN binding command");
    if (op == OP_BIND_UBO) {
        p_bind_buf buffer;
        if (size != sizeof buffer)
            return bad(error, error_size, "Truncated NVN uniform command");
        memcpy(&buffer, payload, sizeof buffer);
        if (buffer.stage < 0 || buffer.stage > 1 || buffer.index < 0 || buffer.index >= 16
            || !buffer.addr || !buffer.size || buffer.addr > UINT64_MAX - buffer.size)
            return bad(error, error_size, "Invalid NVN uniform range or stage");
        state->uniform[buffer.stage][buffer.index] = buffer;
        state->has_uniform[buffer.stage][buffer.index] = 1;
        return 0;
    }
    if (op == OP_BIND_TEX || op == OP_BIND_SAMP) {
        p_bind_handles handles;
        if (size != sizeof handles)
            return bad(error, error_size, "Truncated NVN descriptor command");
        memcpy(&handles, payload, sizeof handles);
        if (handles.stage < 0 || handles.stage > 1 || handles.first < 0 || handles.n <= 0
            || handles.n > 32 || handles.first > 128 - handles.n)
            return bad(error, error_size, "Invalid NVN descriptor range or stage");
        for (int i = 0; i < handles.n; i++) {
            unsigned slot = (unsigned)(handles.first + i);
            if (op == OP_BIND_TEX) {
                state->textures[handles.stage][slot] = handles.h[i];
                state->has_texture[handles.stage][slot] = 1;
            } else {
                state->samplers[handles.stage][slot] = handles.h[i];
                state->has_sampler[handles.stage][slot] = 1;
            }
        }
        return 0;
    }
    return bad(error, error_size, "Unsupported command in direct SPIR-V binding route");
}

int nvn_spirv_apply(const nvn_spirv_state *state, const nvn_spirv_binding *bindings, size_t count,
                    nvn_spirv_sink sink, void *context, char *error, size_t error_size) {
    if (!state || !sink || (!bindings && count) || count > 64)
        return bad(error, error_size, "Invalid direct MSL binding plan");
    for (unsigned pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < count; i++) {
            const nvn_spirv_binding *binding = &bindings[i];
            if (binding->stage > 1 || binding->kind > NVN_SPIRV_SAMPLER || binding->msl_index >= 31)
                return bad(error, error_size, "Invalid MSL stage, resource kind or slot");
            for (size_t j = 0; j < i; j++)
                if (binding->stage == bindings[j].stage && binding->kind == bindings[j].kind
                    && binding->msl_index == bindings[j].msl_index)
                    return bad(error, error_size, "Colliding MSL resource slots");
            unsigned stage = binding->stage, slot = binding->nvn_slot;
            uint64_t value, size = 0;
            if (binding->kind == NVN_SPIRV_BUFFER) {
                if (slot >= 16 || !state->has_uniform[stage][slot]
                    || state->uniform[stage][slot].size < binding->minimum_size)
                    return bad(error, error_size, "Required NVN uniform is absent or too small");
                value = state->uniform[stage][slot].addr;
                size = state->uniform[stage][slot].size;
            } else if (binding->kind == NVN_SPIRV_TEXTURE) {
                if (slot >= 128 || !state->has_texture[stage][slot])
                    return bad(error, error_size, "Required NVN texture descriptor is absent");
                value = state->textures[stage][slot] & 0xFFFFF;
            } else {
                if (slot >= 128 || !state->has_sampler[stage][slot])
                    return bad(error, error_size, "Required NVN sampler descriptor is absent");
                value = state->samplers[stage][slot] >> 20;
            }
            if (pass && sink(context, binding, value, size))
                return bad(error, error_size, "Metal resource resolution or encoding failed");
        }
    }
    return 0;
}
