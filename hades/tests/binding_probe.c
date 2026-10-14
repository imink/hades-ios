#include "../../dm/gfx/nvn/nvn_spirv_bind.h"
#include <assert.h>

static int calls;
static int sink(void *context, const nvn_spirv_binding *binding, uint64_t value, uint64_t size) {
    (void)context;
    if (binding->kind == NVN_SPIRV_BUFFER)
        assert(value == 0x1000 && size == 32);
    else
        assert(value == (binding->kind == NVN_SPIRV_TEXTURE ? 42 : 5));
    calls++;
    return 0;
}

int main(void) {
    nvn_spirv_state state = {0};
    char error[128];
    nvn_spirv_binding plan[] = {{NVN_SPIRV_BUFFER,0,0,3,32},
                                {NVN_SPIRV_TEXTURE,1,2,7,0},
                                {NVN_SPIRV_SAMPLER,1,1,4,0}};
    assert(nvn_spirv_apply(&state, plan, 3, sink, NULL, error, sizeof error));
    assert(!calls && strstr(error, "absent"));
    p_bind_buf buffer = {0,3,0x1000,32};
    p_bind_handles texture = {.stage=1,.first=7,.n=1,.h={42}};
    p_bind_handles sampler = {.stage=1,.first=4,.n=1,.h={5ull << 20}};
    assert(!nvn_spirv_record(&state, OP_BIND_UBO, &buffer, sizeof buffer, error, sizeof error));
    assert(nvn_spirv_record(&state, OP_BIND_TEX, &texture, sizeof texture - 1, error, sizeof error));
    texture.stage = -1;
    assert(nvn_spirv_record(&state, OP_BIND_TEX, &texture, sizeof texture, error, sizeof error));
    texture.stage = 1;
    assert(!nvn_spirv_record(&state, OP_BIND_TEX, &texture, sizeof texture, error, sizeof error));
    assert(!nvn_spirv_record(&state, OP_BIND_SAMP, &sampler, sizeof sampler, error, sizeof error));
    assert(!nvn_spirv_apply(&state, plan, 3, sink, NULL, error, sizeof error));
    assert(calls == 3);
    plan[0].minimum_size = 33;
    assert(nvn_spirv_apply(&state, plan, 3, sink, NULL, error, sizeof error));
    assert(calls == 3);
    plan[0].minimum_size = 32;
    plan[2] = plan[1];
    assert(nvn_spirv_apply(&state, plan, 3, sink, NULL, error, sizeof error));
    assert(calls == 3 && strstr(error, "Colliding"));
    assert(nvn_spirv_record(&state, OP_DRAW, &buffer, sizeof buffer, error, sizeof error));
    return 0;
}
