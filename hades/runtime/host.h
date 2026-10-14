#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
#define HADES_BINDING(operation, name) HADES_HOST_##operation,
#include "host_bindings.def"
#ifdef HADES_VULKAN_HOST
#include "vulkan_bindings.def"
#include "vulkan_optional.def"
#endif
#undef HADES_BINDING
    HADES_HOST_COUNT
};

int hades_host_initialize(const char *romfs);
int hades_host_configure_user(const char *storage);
int hades_host_shutdown(void);
const void *hades_host_entrypoint(unsigned index);
int hades_host_call(unsigned index, uint64_t *registers);
void hades_host_dispatch(uint64_t index, uint64_t *registers);
