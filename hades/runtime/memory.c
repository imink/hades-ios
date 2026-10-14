#define _DARWIN_C_SOURCE
#include "memory.h"
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

_Static_assert(sizeof(hades_memory_info) == 40, "Horizon MemoryInfo layout");
_Static_assert(sizeof(vm_address_t) == sizeof(uintptr_t), "Public Mach VM API must preserve ARM64 addresses");
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static hades_memory_config config;
static uint8_t *arena, *alias, *stack, *heap;
static size_t arena_size, heap_size, host_page;
static struct { uintptr_t base; size_t size; uint32_t state, permissions; unsigned owner; int external; } regions[4096];
static size_t region_count;
static struct { uintptr_t source, destination; size_t size; uint32_t state; int copied, thread_owned; } mappings[64];
static size_t mapping_count;
static struct { uintptr_t source, receiver; size_t size; uint32_t handle, state, permission, references; } transfers[32];
static struct { uintptr_t source, receiver; size_t size; uint32_t handle; } shared[32];

static int error(const char *message) {
    fprintf(stderr, "[hades-memory] %s\n", message);
    return -1;
}

static int contains(uintptr_t base, size_t size, uintptr_t address, size_t length) {
    return address >= base && length <= size && address - base <= size - length;
}

int hades_memory_initialize(const hades_memory_config *configuration) {
    if (!configuration)
        return error("Missing memory policy");
    pthread_mutex_lock(&lock);
    long page = sysconf(_SC_PAGESIZE);
    if (arena || page <= 0 || !configuration->alias_size || !configuration->stack_size
        || !configuration->heap_capacity || configuration->alias_size % (size_t)page
        || configuration->stack_size % (size_t)page || configuration->heap_capacity % 0x200000
        || configuration->alias_size > SIZE_MAX - configuration->stack_size
        || configuration->alias_size + configuration->stack_size > SIZE_MAX - configuration->heap_capacity
        || configuration->image_charge > SIZE_MAX - configuration->heap_capacity) {
        pthread_mutex_unlock(&lock);
        return error("Invalid, overflowing or already initialized memory policy");
    }
    size_t size = configuration->alias_size + configuration->stack_size + configuration->heap_capacity;
    const size_t alignment = 0x200000;
    if (!configuration->address_space_limit || !configuration->priority_mask || !configuration->core_mask
        || configuration->system_resource_budget
        || (configuration->subpage_copy_compatibility != 0 && configuration->subpage_copy_compatibility != 1)
        || size > configuration->address_space_limit
        || size > SIZE_MAX - alignment || configuration->alias_size % alignment
        || configuration->stack_size % alignment) {
        pthread_mutex_unlock(&lock);
        return error("Invalid guest address space limit");
    }
    void *reservation = mmap(NULL, size + alignment, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (reservation == MAP_FAILED) {
        pthread_mutex_unlock(&lock);
        return error("Cannot reserve guest virtual address space");
    }
    uintptr_t aligned = ((uintptr_t)reservation + alignment - 1) & ~(uintptr_t)(alignment - 1);
    size_t prefix = aligned - (uintptr_t)reservation, suffix = alignment - prefix;
    if (prefix && munmap(reservation, prefix)) {
        if (munmap(reservation, size + alignment))
            error("Cannot release failed aligned reservation");
        pthread_mutex_unlock(&lock);
        return error("Cannot trim reservation prefix");
    }
    void *base = (void *)aligned;
    if (munmap((void *)(aligned + size), suffix)) {
        if (munmap(base, size + suffix))
            error("Cannot release failed aligned reservation");
        pthread_mutex_unlock(&lock);
        return error("Cannot trim reservation suffix");
    }
    if ((uintptr_t)base > configuration->address_space_limit - size) {
        if (munmap(base, size))
            error("Cannot release reservation outside guest address space");
        pthread_mutex_unlock(&lock);
        return error("Host reservation lies outside NPDM address space");
    }
    config = *configuration;
    arena = alias = base;
    stack = alias + config.alias_size;
    heap = stack + config.stack_size;
    arena_size = size;
    host_page = (size_t)page;
    heap_size = 0;
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "[hades-memory] reserved=%zu alias=%zu stack=%zu heap-capacity=%zu; host probe policy, not measured Switch budget\n",
            size, config.alias_size, config.stack_size, config.heap_capacity);
    return 0;
}

int hades_memory_register(uintptr_t base, size_t size, uint32_t state, uint32_t permissions, unsigned owner) {
    pthread_mutex_lock(&lock);
    if (!arena || !size || base >= config.address_space_limit
        || size > config.address_space_limit - base
        || region_count == sizeof regions / sizeof regions[0]) {
        pthread_mutex_unlock(&lock);
        return error("Invalid registered range or registry capacity exceeded");
    }
    for (size_t i = 0; i < region_count; i++)
        if (base < regions[i].base + regions[i].size && regions[i].base < base + size) {
            pthread_mutex_unlock(&lock);
            return error("Overlapping registered memory ranges");
        }
    regions[region_count].base = base;
    regions[region_count].size = size;
    regions[region_count].state = state;
    regions[region_count].permissions = permissions;
    regions[region_count].external = 0;
    regions[region_count++].owner = owner;
    pthread_mutex_unlock(&lock);
    return 0;
}

static int register_external(uintptr_t base, size_t size, unsigned owner, uint32_t state) {
    pthread_mutex_lock(&lock);
    if (!arena || !base || !size || size > UINTPTR_MAX - base || owner < 0x4000 || region_count == 4096) {
        pthread_mutex_unlock(&lock);
        return error("Invalid owned external mapped-buffer range");
    }
    for (size_t i = 0; i < region_count; i++)
        if (base < regions[i].base + regions[i].size && regions[i].base < base + size) {
            pthread_mutex_unlock(&lock);
            return error("External mapping overlaps another registered resource");
        }
    regions[region_count].base = base;
    regions[region_count].size = size;
    regions[region_count].state = state;
    regions[region_count].permissions = 3;
    regions[region_count].owner = owner;
    regions[region_count++].external = 1;
    pthread_mutex_unlock(&lock);
    return 0;
}

int hades_memory_register_external(uintptr_t base, size_t size, unsigned owner) {
    return register_external(base,size,owner,4);
}

int hades_memory_register_callback_stack(uintptr_t base, size_t size, unsigned owner) {
    if (owner<0x50000000 || owner>=0x60000000)
        return error("Invalid callback stack owner namespace");
    return register_external(base,size,owner,0xB);
}

int hades_memory_unregister_external(unsigned owner) {
    pthread_mutex_lock(&lock);
    size_t index;
    for (index = 0; index < region_count && (!regions[index].external || regions[index].owner != owner); index++);
    if (index == region_count) {
        pthread_mutex_unlock(&lock);
        return error("Missing owned external mapping during unmap");
    }
    for (size_t i = 0; i < mapping_count; i++)
        if (regions[index].base < mappings[i].source + mappings[i].size
            && mappings[i].source < regions[index].base + regions[index].size) {
            pthread_mutex_unlock(&lock);
            return error("Cannot unmap borrowed external GPU buffer");
        }
    regions[index] = regions[--region_count];
    pthread_mutex_unlock(&lock);
    return 0;
}

int hades_memory_unregister(unsigned owner) {
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < mapping_count; i++)
        if (mappings[i].thread_owned) {
            pthread_mutex_unlock(&lock);
            return error("Cannot unregister guest regions while a pthread retains a guest stack");
        }
    for (size_t i = 0; i < region_count; i++)
        if (regions[i].owner == owner) {
            for (unsigned j=0;j<32;j++)
                if (shared[j].handle && shared[j].receiver==regions[i].base) {
                    pthread_mutex_unlock(&lock);
                    return error("Cannot unregister a live owned shared-memory receiver");
                }
            for (unsigned j=0;j<32;j++)
                if (transfers[j].handle && contains(regions[i].base,regions[i].size,transfers[j].source,transfers[j].size)) {
                    pthread_mutex_unlock(&lock);
                    return error("Cannot unregister module retained by transfer memory");
                }
            for (size_t j = 0; j < mapping_count; j++)
                if (contains(regions[i].base, regions[i].size, mappings[j].source, mappings[j].size)) {
                    pthread_mutex_unlock(&lock);
                    return error("Cannot unregister module with borrowed memory mappings");
                }
        }
    for (size_t i = 0; i < region_count;)
        if (regions[i].owner == owner)
            regions[i] = regions[--region_count];
        else
            i++;
    pthread_mutex_unlock(&lock);
    return 0;
}

static int stack_ownership(uintptr_t top, int owned) {
    pthread_mutex_lock(&lock);
    for (size_t i = 0; top && i < mapping_count; i++)
        if (contains(mappings[i].destination, mappings[i].size, top - 1, 1)) {
            if (mappings[i].thread_owned == owned) {
                pthread_mutex_unlock(&lock);
                return error("Duplicate guest stack ownership transition");
            }
            mappings[i].thread_owned = owned;
            pthread_mutex_unlock(&lock);
            return 0;
        }
    pthread_mutex_unlock(&lock);
    return error("Guest pthread stack must belong to a live borrowed mapping");
}

int hades_memory_pin_stack(uintptr_t top) { return stack_ownership(top, 1); }
int hades_memory_unpin_stack(uintptr_t top) { return stack_ownership(top, 0); }

int hades_memory_shutdown(void) {
    pthread_mutex_lock(&lock);
    for (unsigned i=0;i<32;i++)
        if (shared[i].handle) {
            pthread_mutex_unlock(&lock);
            return error("Cannot shut down memory retained by shared-memory handles");
        }
    for (unsigned i = 0; i < 32; i++)
        if (transfers[i].handle) {
            pthread_mutex_unlock(&lock);
            return error("Cannot shut down memory retained by transfer handles");
        }
    if (mapping_count) {
        pthread_mutex_unlock(&lock);
        return error("Cannot shut down memory with active borrowed mappings");
    }
    if (arena && munmap(arena, arena_size)) {
        pthread_mutex_unlock(&lock);
        return error("Cannot release guest virtual address space");
    }
    arena = alias = stack = heap = NULL;
    arena_size = heap_size = region_count = 0;
    memset(&config, 0, sizeof config);
    pthread_mutex_unlock(&lock);
    return 0;
}

static void clip_mappings(uintptr_t address, hades_memory_info *information) {
    uintptr_t lo = information->base, hi = lo + information->size;
    for (unsigned i = 0; i < 32; i++)
        if (transfers[i].handle) {
            uintptr_t end = transfers[i].source + transfers[i].size;
            if (end <= address && end > lo) lo = end;
            if (transfers[i].source > address && transfers[i].source < hi) hi = transfers[i].source;
        }
    for (size_t i = 0; i < mapping_count; i++) {
        uintptr_t ranges[2] = {mappings[i].source, mappings[i].destination};
        for (unsigned j = 0; j < 2; j++) {
            uintptr_t end = ranges[j] + mappings[i].size;
            if (end <= address && end > lo) lo = end;
            if (ranges[j] > address && ranges[j] < hi) hi = ranges[j];
        }
    }
    information->base = lo;
    information->size = hi - lo;
}

static int query_locked(uintptr_t address, hades_memory_info *information) {
    *information = (hades_memory_info){0};
    if (!arena)
        return -1;
    for (unsigned i = 0; i < 32; i++)
        if (transfers[i].handle && contains(transfers[i].source, transfers[i].size, address, 1)) {
            *information = (hades_memory_info){.base=transfers[i].source, .size=transfers[i].size,
                .state=transfers[i].state, .attributes=1, .permissions=transfers[i].permission,
                .ipc_references=transfers[i].references};
            return 0;
        }
    for (size_t i = 0; i < mapping_count; i++) {
        if (contains(mappings[i].source, mappings[i].size, address, 1)) {
            *information = (hades_memory_info){.base=mappings[i].source, .size=mappings[i].size,
                .state=mappings[i].state, .attributes=1};
            return 0;
        }
        if (contains(mappings[i].destination, mappings[i].size, address, 1)) {
            *information = (hades_memory_info){.base=mappings[i].destination, .size=mappings[i].size,
                .state=0xB, .permissions=3};
            return 0;
        }
    }
    for (size_t i = 0; i < region_count; i++)
        if (contains(regions[i].base, regions[i].size, address, 1)) {
            information->base = regions[i].base;
            information->size = regions[i].size;
            information->state = regions[i].state;
            information->permissions = regions[i].permissions;
            clip_mappings(address, information);
            return 0;
        }
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    size_t size = pthread_get_stacksize_np(pthread_self());
    if (size <= top && contains(top - size, size, address, 1)) {
        *information = (hades_memory_info){.base=top-size, .size=size, .state=0xB, .permissions=3};
        return 0;
    }
    if (arena && contains((uintptr_t)arena, arena_size, address, 1)) {
        uintptr_t lo = (uintptr_t)arena, hi = lo + arena_size;
        if (heap_size && contains((uintptr_t)heap, heap_size, address, 1)) {
            *information = (hades_memory_info){.base=(uintptr_t)heap, .size=heap_size, .state=5, .permissions=3};
            clip_mappings(address, information);
            return 0;
        }
        if (address >= (uintptr_t)heap + heap_size)
            lo = (uintptr_t)heap + heap_size;
        else
            hi = (uintptr_t)heap;
        for (size_t i = 0; i < region_count; i++) {
            uintptr_t end = regions[i].base + regions[i].size;
            if (end <= address && end > lo) lo = end;
            if (regions[i].base > address && regions[i].base < hi) hi = regions[i].base;
        }
        *information = (hades_memory_info){.base=lo, .size=hi-lo};
        clip_mappings(address, information);
        return 0;
    }
    if (!arena || top > config.address_space_limit || size > top)
        return -1;
    /* Only explicitly reserved guest ranges are allocatable. Other gaps are
       inaccessible, not fabricated code pages or host memory exposed as free. */
    uintptr_t lo = 0, hi = config.address_space_limit;
    if (address >= hi) {
        *information = (hades_memory_info){.base=hi, .size=0-hi, .state=0x10};
        return 0;
    }
    uintptr_t boundaries[][2] = {
        {(uintptr_t)arena, (uintptr_t)arena + arena_size},
        {top - size, top}
    };
    for (size_t i = 0; i < sizeof boundaries / sizeof boundaries[0]; i++) {
        if (boundaries[i][1] <= address && boundaries[i][1] > lo) lo = boundaries[i][1];
        if (boundaries[i][0] > address && boundaries[i][0] < hi) hi = boundaries[i][0];
    }
    for (size_t i = 0; i < region_count; i++) {
        uintptr_t end = regions[i].base + regions[i].size;
        if (end <= address && end > lo) lo = end;
        if (regions[i].base > address && regions[i].base < hi) hi = regions[i].base;
    }
    *information = (hades_memory_info){.base=lo, .size=hi-lo, .state=0x10};
    return 0;
}

int hades_memory_info_query(uintptr_t address, hades_memory_info *information) {
    if (!information)
        return error("Null memory information output");
    pthread_mutex_lock(&lock);
    int result = query_locked(address, information);
    pthread_mutex_unlock(&lock);
    if (result)
        error("QueryMemory address is outside known guest regions and current thread stack");
    return result;
}

int hades_memory_writable(uintptr_t address, size_t size) {
    hades_memory_info information;
    pthread_mutex_lock(&lock);
    int writable = !query_locked(address, &information) && (information.permissions & 2)
        && contains(information.base, information.size, address, size);
    pthread_mutex_unlock(&lock);
    return writable;
}

int hades_memory_shared_create(uint32_t handle, size_t size) {
    pthread_mutex_lock(&lock);
    unsigned slot;
    for (slot=0;slot<32 && shared[slot].handle;slot++);
    int duplicate=0;
    for (unsigned i=0;i<32;i++)
        duplicate|=shared[i].handle==handle;
    if (!arena || !handle || duplicate || slot==32 || !size || size%host_page) {
        pthread_mutex_unlock(&lock);
        return error("Invalid or duplicate owned shared-memory allocation");
    }
    void *source=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if (source==MAP_FAILED) {
        pthread_mutex_unlock(&lock);
        return error("Cannot allocate actual shared-memory publisher pages");
    }
    shared[slot].handle=handle;
    shared[slot].source=(uintptr_t)source;
    shared[slot].size=size;
    pthread_mutex_unlock(&lock);
    return 0;
}

void *hades_memory_shared_buffer(uint32_t handle, size_t size) {
    pthread_mutex_lock(&lock);
    void *buffer=NULL;
    for (unsigned i=0;i<32;i++)
        if (handle && shared[i].handle==handle && size<=shared[i].size)
            buffer=(void *)shared[i].source;
    pthread_mutex_unlock(&lock);
    return buffer;
}

int hades_memory_shared_map(uint32_t handle, uintptr_t destination, size_t size, unsigned permission) {
    pthread_mutex_lock(&lock);
    unsigned slot;
    for (slot=0;slot<32 && shared[slot].handle!=handle;slot++);
    hades_memory_info info;
    fprintf(stderr,"[hades-memory] shared receiver handle=%#x destination=%#llx size=%zu permission=%u host-page=%zu\n",
        handle,(unsigned long long)destination,size,permission,host_page);
    if (!arena || !handle || slot==32 || shared[slot].receiver || size!=shared[slot].size
        || destination%host_page || permission!=1 || region_count==4096
        || !contains((uintptr_t)alias,config.alias_size+config.stack_size,destination,size)
        || query_locked(destination,&info) || info.state || info.permissions
        || !contains(info.base,info.size,destination,size)) {
        pthread_mutex_unlock(&lock);
        return error("Shared memory requires a free reserved range and exact read-only host-page mapping");
    }
    vm_address_t mapped=destination;
    vm_prot_t current,max;
    kern_return_t result=vm_remap(mach_task_self(),&mapped,size,0,VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,
        mach_task_self(),shared[slot].source,FALSE,&current,&max,VM_INHERIT_NONE);
    if (result!=KERN_SUCCESS || mapped!=destination) {
        pthread_mutex_unlock(&lock);
        return error("Cannot establish real shared-memory receiver alias");
    }
    shared[slot].receiver=destination;
    regions[region_count].base=destination;
    regions[region_count].size=size;
    regions[region_count].state=6;
    regions[region_count].permissions=1;
    regions[region_count].external=0;
    regions[region_count++].owner=0x80000000u|handle;
    if (mprotect((void *)destination,size,PROT_READ)) {
        pthread_mutex_unlock(&lock);
        return error("Cannot enforce read-only shared-memory receiver protection");
    }
    pthread_mutex_unlock(&lock);
    fprintf(stderr,"[hades-memory] SHARED_MEMORY_MAPPED handle=%#x size=%zu; live publisher alias, physically read-only receiver\n",handle,size);
    return 0;
}

int hades_memory_shared_unmap(uint32_t handle, uintptr_t destination, size_t size) {
    pthread_mutex_lock(&lock);
    unsigned slot;
    for (slot=0;slot<32 && shared[slot].handle!=handle;slot++);
    if (!arena || !handle || slot==32 || !destination || shared[slot].receiver!=destination
        || shared[slot].size!=size) {
        pthread_mutex_unlock(&lock);
        return error("Shared-memory unmap does not match an owned receiver");
    }
    if (mmap((void *)destination,size,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==MAP_FAILED) {
        pthread_mutex_unlock(&lock);
        return error("Cannot restore shared-memory receiver reservation");
    }
    for (size_t i=0;i<region_count;)
        if (regions[i].owner==(0x80000000u|handle))
            regions[i]=regions[--region_count];
        else
            i++;
    shared[slot].receiver=0;
    pthread_mutex_unlock(&lock);
    return 0;
}

int hades_memory_shared_close(uint32_t handle) {
    pthread_mutex_lock(&lock);
    unsigned slot;
    for (slot=0;slot<32 && shared[slot].handle!=handle;slot++);
    if (!handle || slot==32 || shared[slot].receiver) {
        pthread_mutex_unlock(&lock);
        return error("Unknown shared handle or receiver still mapped; unmap before closing");
    }
    if (munmap((void *)shared[slot].source,shared[slot].size)) {
        pthread_mutex_unlock(&lock);
        return error("Cannot release owned shared-memory publisher pages");
    }
    memset(&shared[slot],0,sizeof shared[slot]);
    pthread_mutex_unlock(&lock);
    return 0;
}

int hades_memory_transfer_acquire(uint32_t handle, uintptr_t address, size_t size, unsigned permission) {
    pthread_mutex_lock(&lock);
    hades_memory_info information;
    unsigned slot;
    for (slot = 0; slot < 32 && transfers[slot].handle; slot++);
    int owned=contains((uintptr_t)heap,heap_size,address,size);
    for (size_t i=0;i<region_count;i++)
        if (!regions[i].external && regions[i].state==4 && regions[i].permissions==3
            && contains(regions[i].base,regions[i].size,address,size))
            owned=1;
    if (arena && !query_locked(address,&information))
        fprintf(stderr,"[hades-memory] transfer donor state=%u permission=%u attributes=%u; heap-owned=%d host-page-offset=%zu host-page=%zu\n",
                information.state,information.permissions,information.attributes,
                contains((uintptr_t)heap,heap_size,address,size),address%host_page,host_page);
    if (!arena || !handle || slot == 32 || !size || address % host_page || size % host_page
        || (permission != 0 && permission != 1 && permission != 3)
        || !owned
        || query_locked(address, &information) || information.permissions != 3 || information.attributes
        || !contains(information.base, information.size, address, size)) {
        pthread_mutex_unlock(&lock);
        return error("Transfer requires unborrowed owned heap/CodeData with exact host-page geometry and valid permission");
    }
    for (unsigned i = 0; i < 32; i++)
        if (transfers[i].handle == handle) {
            pthread_mutex_unlock(&lock);
            return error("Duplicate transfer handle");
        }
    vm_address_t receiver = 0;
    vm_prot_t current = 0, maximum = 0;
    kern_return_t result = vm_remap(mach_task_self(), &receiver, size, 0, VM_FLAGS_ANYWHERE,
        mach_task_self(), address, FALSE, &current, &maximum, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS || (current & (VM_PROT_READ | VM_PROT_WRITE)) != (VM_PROT_READ | VM_PROT_WRITE)) {
        if (result == KERN_SUCCESS && vm_deallocate(mach_task_self(), receiver, size) != KERN_SUCCESS)
            error("Cannot release incompatible transfer receiver alias");
        pthread_mutex_unlock(&lock);
        return error("Cannot create actual shared transfer receiver alias");
    }
    int protection = permission == 3 ? PROT_READ | PROT_WRITE : permission == 1 ? PROT_READ : PROT_NONE;
    if (mprotect((void *)address, size, protection)) {
        if (vm_deallocate(mach_task_self(), receiver, size) != KERN_SUCCESS)
            error("Cannot release failed transfer receiver alias");
        pthread_mutex_unlock(&lock);
        return error("Cannot apply donor transfer permissions");
    }
    transfers[slot].source = address;
    transfers[slot].receiver = receiver;
    transfers[slot].size = size;
    transfers[slot].handle = handle;
    transfers[slot].state = information.state;
    transfers[slot].permission = permission;
    transfers[slot].references = 1;
    fprintf(stderr, "[hades-memory] TRANSFER_MEMORY_CREATED handle=%#x size=%zu; real shared alias and donor permission=%u\n",
            handle, size, permission);
    pthread_mutex_unlock(&lock);
    return 0;
}

void *hades_memory_transfer_buffer(uint32_t handle, size_t size) {
    pthread_mutex_lock(&lock);
    void *buffer = NULL;
    for (unsigned i = 0; i < 32; i++)
        if (transfers[i].handle == handle && handle && size <= transfers[i].size)
            buffer = (void *)transfers[i].receiver;
    pthread_mutex_unlock(&lock);
    if (!buffer)
        error("Missing/undersized owned transfer buffer");
    return buffer;
}

int hades_memory_transfer_retain(uint32_t handle) {
    pthread_mutex_lock(&lock);
    for (unsigned i=0;i<32;i++)
        if (handle && transfers[i].handle==handle && transfers[i].references<UINT32_MAX) {
            transfers[i].references++;
            pthread_mutex_unlock(&lock);
            return 0;
        }
    pthread_mutex_unlock(&lock);
    return error("Cannot retain missing/exhausted owned transfer capability");
}

int hades_memory_transfer_release(uint32_t handle) {
    pthread_mutex_lock(&lock);
    unsigned slot;
    for (slot = 0; slot < 32 && (!handle || transfers[slot].handle != handle); slot++);
    if (slot == 32) {
        pthread_mutex_unlock(&lock);
        return error("Unknown transfer handle");
    }
    if (transfers[slot].references>1) {
        transfers[slot].references--;
        pthread_mutex_unlock(&lock);
        return 0;
    }
    if (mprotect((void *)transfers[slot].source, transfers[slot].size, PROT_READ | PROT_WRITE)
        || vm_deallocate(mach_task_self(), transfers[slot].receiver, transfers[slot].size) != KERN_SUCCESS) {
        pthread_mutex_unlock(&lock);
        return error("Cannot restore donor/release transfer receiver");
    }
    memset(&transfers[slot], 0, sizeof transfers[slot]);
    pthread_mutex_unlock(&lock);
    return 0;
}

int hades_memory_check_empty_key(uintptr_t address, int32_t count) {
    if (address % 4 || count < -1 || !hades_memory_writable(address, 4)
        || __atomic_load_n((uint32_t *)address, __ATOMIC_ACQUIRE))
        return error("Process-wide key is not a writable aligned zero-waiter word");
    return 0;
}

int hades_memory_svc(unsigned svc, uint64_t *registers) {
    if (!registers)
        return error("Missing SVC register frame");
    if (svc != 0x29 && svc != 0x01 && svc != 0x06 && svc != 4 && svc != 5)
        return 0;
    pthread_mutex_lock(&lock);
    if (!arena) {
        pthread_mutex_unlock(&lock);
        return error("Guest address space is not initialized");
    }
    if (svc == 0x29) {
        uint64_t id = registers[1], value;
        if (registers[2] != 0xFFFF8001 || registers[3] != 0) {
            pthread_mutex_unlock(&lock);
            return error("GetInfo requires current-process handle and sub-id zero");
        }
        switch (id) {
            case 0: value = config.core_mask; break;
            case 1: value = config.priority_mask; break;
            case 2: value = (uintptr_t)alias; break;
            case 3: value = config.alias_size; break;
            case 4: value = (uintptr_t)heap; break;
            case 5: value = config.heap_capacity; break;
            case 6: case 21: value = config.heap_capacity + config.image_charge; break;
            case 7: case 22: value = heap_size + config.image_charge; break;
            case 12: value = (uintptr_t)arena; break;
            case 13: value = arena_size; break;
            case 14: value = (uintptr_t)stack; break;
            case 15: value = config.stack_size; break;
            /* This target declares no dynamic kernel system-resource pool.
               Nonzero pools are rejected rather than inventing usage. */
            case 16: case 17: value = config.system_resource_budget; break;
            case 18: value = config.program_id; break;
            case 23: value = 1; break;
            default:
                pthread_mutex_unlock(&lock);
                return 0;
        }
        registers[0] = 0;
        registers[1] = value;
        fprintf(stderr, "[hades-memory] GetInfo id=%llu served from process memory policy\n", (unsigned long long)id);
    } else if (svc == 0x01) {
        size_t size = registers[1];
        if (size % 0x200000 || size > config.heap_capacity) {
            fprintf(stderr, "[hades-memory] SetHeapSize rejected size=%zu capacity=%zu\n", size, config.heap_capacity);
            registers[0] = size % 0x200000 ? 0xD401 : 0x10801;
            pthread_mutex_unlock(&lock);
            return 1;
        }
        if (size != heap_size) {
            for (unsigned i = 0; size < heap_size && i < 32; i++)
                if (transfers[i].handle
                    && contains((uintptr_t)heap,heap_size,transfers[i].source,transfers[i].size)
                    && transfers[i].source + transfers[i].size > (uintptr_t)heap + size) {
                    pthread_mutex_unlock(&lock);
                    return error("Cannot shrink heap containing retained transfer memory");
                }
            for (size_t i = 0; size < heap_size && i < mapping_count; i++)
                if (mappings[i].source >= (uintptr_t)heap
                    && mappings[i].source < (uintptr_t)heap + heap_size
                    && mappings[i].source + mappings[i].size > (uintptr_t)heap + size) {
                    pthread_mutex_unlock(&lock);
                    return error("Cannot shrink a heap containing borrowed source pages");
                }
            size_t start = size < heap_size ? size : heap_size;
            size_t length = size > heap_size ? size - heap_size : heap_size - size;
            int result = -1;
            if (!(start % host_page) && !(length % host_page)) {
                if (size > heap_size)
                    result = mprotect(heap + start, length, PROT_READ | PROT_WRITE);
                else {
                    /* Replace only our owned tail so regrowth is zero-filled,
                       rather than exposing pages retained behind PROT_NONE. */
                    void *replacement = mmap(heap + start, length, PROT_NONE,
                                             MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
                    result = replacement == MAP_FAILED ? -1 : 0;
                }
            }
            if (result) {
                pthread_mutex_unlock(&lock);
                return error("SetHeapSize host mapping/protection failed; cannot continue");
            }
            heap_size = size;
        }
        registers[0] = 0;
        registers[1] = (uintptr_t)heap;
    } else if (svc == 4 || svc == 5) {
        uintptr_t destination = registers[0], source = registers[1];
        size_t size = registers[2];
        int copied = destination % host_page || source % host_page || size % host_page;
        if (!size || destination % 4096 || source % 4096 || size % 4096
            || (copied && !config.subpage_copy_compatibility)) {
            fprintf(stderr, "[hades-memory] native page granularity cannot represent requested alias: host=%zu size=%zu destination-offset=%zu source-offset=%zu\n",
                    host_page, size, destination % host_page, source % host_page);
            pthread_mutex_unlock(&lock);
            return 0;
        }
        if (svc == 4) {
            hades_memory_info from, to;
            if (mapping_count == sizeof mappings / sizeof mappings[0]
                || query_locked(source, &from) || query_locked(destination, &to)
                || (from.state != 4 && from.state != 5) || from.permissions != 3 || from.attributes
                || !contains(from.base, from.size, source, size)
                || !contains((uintptr_t)stack, config.stack_size, destination, size)
                || to.state || to.permissions || !contains(to.base, to.size, destination, size)) {
                pthread_mutex_unlock(&lock);
                return error("MapMemory requires writable unborrowed source and free reserved stack destination");
            }
            uintptr_t lo = destination & ~(uintptr_t)(host_page - 1);
            uintptr_t hi = (destination + size + host_page - 1) & ~(uintptr_t)(host_page - 1);
            for (size_t i = 0; i < mapping_count; i++) {
                uintptr_t other_lo = mappings[i].destination & ~(uintptr_t)(host_page - 1);
                uintptr_t other_hi = (mappings[i].destination + mappings[i].size + host_page - 1)
                    & ~(uintptr_t)(host_page - 1);
                if (lo < other_hi && other_lo < hi && (!copied || !mappings[i].copied)) {
                    pthread_mutex_unlock(&lock);
                    return error("Cannot mix exact aliases and copied mappings within a host page");
                }
            }
            if (copied) {
                if (!contains((uintptr_t)stack, config.stack_size, lo, hi - lo)
                    || mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) {
                    pthread_mutex_unlock(&lock);
                    return error("Copy-compatible mapping cannot open owned destination pages");
                }
                memcpy((void *)destination, (const void *)source, size);
                fprintf(stderr, "[hades-memory] D2R-style copy borrow: size=%zu; source physically accessible, logical permissions revoked; no live alias\n", size);
            } else {
                vm_address_t mapped = destination;
                vm_prot_t current, maximum;
                kern_return_t status = vm_remap(mach_task_self(), &mapped, size, 0,
                    VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), source, FALSE,
                    &current, &maximum, VM_INHERIT_NONE);
                if (status != KERN_SUCCESS || mapped != destination
                    || (current & VM_PROT_EXECUTE)
                    || (current & (VM_PROT_READ | VM_PROT_WRITE)) != (VM_PROT_READ | VM_PROT_WRITE)
                    || mprotect((void *)source, size, PROT_NONE)) {
                    pthread_mutex_unlock(&lock);
                    return error("MapMemory host alias/protection failed; cannot continue");
                }
            }
            mappings[mapping_count].source = source;
            mappings[mapping_count].destination = destination;
            mappings[mapping_count].size = size;
            mappings[mapping_count].copied = copied;
            mappings[mapping_count].thread_owned = 0;
            mappings[mapping_count++].state = from.state;
        } else {
            size_t index;
            for (index = 0; index < mapping_count; index++)
                if (mappings[index].source == source && mappings[index].destination == destination
                    && mappings[index].size == size)
                    break;
            if (index == mapping_count) {
                pthread_mutex_unlock(&lock);
                return error("UnmapMemory does not match a live borrowed mapping");
            }
            if (mappings[index].thread_owned) {
                pthread_mutex_unlock(&lock);
                return error("Cannot unmap a guest stack before its pthread is joined");
            }
            if (mappings[index].copied) {
                memcpy((void *)source, (const void *)destination, size);
                memset((void *)destination, 0, size);
            } else {
                if (mmap((void *)destination, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0)
                    == MAP_FAILED || mprotect((void *)source, size, PROT_READ | PROT_WRITE)) {
                    pthread_mutex_unlock(&lock);
                    return error("UnmapMemory host restoration failed; cannot continue");
                }
            }
            mappings[index] = mappings[--mapping_count];
        }
        registers[0] = 0;
    } else {
        hades_memory_info information, destination;
        if (query_locked(registers[0], &destination) || !(destination.permissions & 2)
            || !contains(destination.base, destination.size, registers[0], sizeof information)
            || query_locked(registers[2], &information)) {
            fprintf(stderr, "[hades-memory] QueryMemory out=%#llx address=%#llx\n",
                    (unsigned long long)registers[0], (unsigned long long)registers[2]);
            pthread_mutex_unlock(&lock);
            return error("QueryMemory has unknown address or non-writable output");
        }
        memcpy((void *)(uintptr_t)registers[0], &information, sizeof information);
        registers[0] = registers[1] = 0;
    }
    pthread_mutex_unlock(&lock);
    return 1;
}
