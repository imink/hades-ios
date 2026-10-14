#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    size_t original_size, writable_start, patch_offset, key_offset, key_size, size;
    unsigned key_index[4];
    size_t load_bias;
} hades_spec;
typedef struct { unsigned module; uint64_t ret, original; unsigned kind, arg; } hades_gate;
typedef struct { const char *name; unsigned module; size_t offset; } hades_entry;
typedef struct { uint64_t offset, value; int32_t target; uint32_t module; } hades_relocation;
typedef struct { uint8_t *base; int loaded; } hades_image;
typedef struct { uint64_t pid; const void *registration; int initialized, registered; } hades_ro_session;

extern const hades_spec hades_specs[];
extern const unsigned hades_spec_count, hades_relocation_count, hades_gate_count;
extern const unsigned hades_main_priority, hades_main_core;
extern const int hades_host_services_enabled;
extern const uint64_t hades_address_space_limit;
extern const uint64_t hades_priority_mask, hades_core_mask;
extern const uint64_t hades_system_resource_budget;
extern const char hades_display_version[16];
extern const hades_gate hades_gates[];
extern const hades_entry hades_bootstrap[6];
extern const hades_entry hades_initializers[], hades_game_entry, hades_set_argc, hades_set_argv;
extern const unsigned hades_initializer_count;
extern const unsigned char hades_nrr_digest[32], hades_nro_digest[32];
extern const size_t hades_eos_bss, hades_version_offset, hades_tick_offset, hades_frequency_offset;
extern hades_image hades_images[5];

int hades_initialize_thread(void);
int hades_map(unsigned index);
void hades_unmap(unsigned index);
int hades_relocate(void);
/* Decoded ldr:ro command core; HIPC/SVC transport is not implemented here. */
int hades_ro_command(hades_ro_session *session, unsigned command, uint64_t pid,
                     const void *data, size_t size, size_t bss_size, uintptr_t *output);
uint64_t gate_dispatch(uint64_t ret, uint64_t *registers);
uint32_t hades_register_main_thread(void);
int hades_initialize_memory(size_t heap_capacity);
uint64_t hades_guest_callback(uintptr_t function, const uint64_t arguments[6]);
