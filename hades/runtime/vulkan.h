#pragma once
#include <stdint.h>
int hades_vulkan_prepare(void *metal_layer);
int hades_vulkan_configure_surface(void *metal_layer);
int hades_vulkan_configure_capture(const char *directory, unsigned step);
uint64_t hades_vulkan_present_count(void);
int hades_vulkan_call(unsigned index, const char *name, uint64_t *registers);
int hades_vulkan_set_callback_runner(uint64_t (*runner)(uintptr_t, const uint64_t[6]));
int hades_vulkan_drain_lifecycle(void);
