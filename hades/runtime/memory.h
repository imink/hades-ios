#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t alias_size, stack_size, heap_capacity, image_charge;
    uint64_t program_id;
    uint64_t address_space_limit;
    uint64_t priority_mask, core_mask;
    uint64_t system_resource_budget;
    int subpage_copy_compatibility;
} hades_memory_config;
typedef struct {
    uint64_t base, size;
    uint32_t state, attributes, permissions, ipc_references, device_references, reserved;
} hades_memory_info;

int hades_memory_initialize(const hades_memory_config *configuration);
int hades_memory_register(uintptr_t base, size_t size, uint32_t state, uint32_t permissions, unsigned owner);
int hades_memory_unregister(unsigned owner);
int hades_memory_shutdown(void);
int hades_memory_pin_stack(uintptr_t top);
int hades_memory_unpin_stack(uintptr_t top);
int hades_memory_info_query(uintptr_t address, hades_memory_info *information);
int hades_memory_writable(uintptr_t address, size_t size);
int hades_memory_check_empty_key(uintptr_t address, int32_t count);
int hades_memory_transfer_acquire(uint32_t handle, uintptr_t address, size_t size, unsigned permission);
int hades_memory_transfer_release(uint32_t handle);
int hades_memory_transfer_retain(uint32_t handle);
void *hades_memory_transfer_buffer(uint32_t handle, size_t size);
int hades_memory_shared_create(uint32_t handle, size_t size);
int hades_memory_shared_map(uint32_t handle, uintptr_t destination, size_t size, unsigned permission);
int hades_memory_shared_unmap(uint32_t handle, uintptr_t destination, size_t size);
int hades_memory_shared_close(uint32_t handle);
void *hades_memory_shared_buffer(uint32_t handle, size_t size);
int hades_memory_register_external(uintptr_t base, size_t size, unsigned owner);
int hades_memory_register_callback_stack(uintptr_t base, size_t size, unsigned owner);
int hades_memory_unregister_external(unsigned owner);
/* Returns 1 for a handled SVC, 0 for unsupported semantics, -1 for host failures. */
int hades_memory_svc(unsigned svc, uint64_t *registers);
