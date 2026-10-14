#include "core.h"
#include "memory.h"
#include "ipc.h"
#include "threads.h"
#include "host.h"
#include "callout.h"
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

hades_image hades_images[5];
static pthread_key_t keys[4];
static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static int key_error;
static _Alignas(16) _Thread_local unsigned char tlr[4096];
static uint32_t main_handle;
static pthread_once_t entropy_once = PTHREAD_ONCE_INIT;
static uint64_t process_entropy[4];
extern void dm_gate(void);
_Static_assert(sizeof(hades_relocation) == 24, "Relocation manifest layout");

static int fail(const char *message) {
    fprintf(stderr, "[hades] error: %s\n", message);
    return -1;
}

static void guest_backtrace(uintptr_t frame) {
    for (unsigned depth = 0; depth < 24 && frame; depth++) {
        hades_memory_info information;
        if (frame % 16 || hades_memory_info_query(frame, &information) || !(information.permissions & 1)
            || information.size - (frame - information.base) < 16)
            break;
        uintptr_t words[2];
        memcpy(words, (const void *)frame, sizeof words);
        for (unsigned module = 0; module < hades_spec_count; module++) {
            uintptr_t base = (uintptr_t)hades_images[module].base;
            if (hades_images[module].loaded && words[1] >= base && words[1] - base < hades_specs[module].size)
                fprintf(stderr, "[hades] guest frame %u %s+%#llx\n", depth, hades_specs[module].name,
                        (unsigned long long)(words[1] - base));
        }
        if (words[0] <= frame)
            break;
        frame = words[0];
    }
}

uint64_t hades_guest_callback(uintptr_t function, const uint64_t arguments[6]) {
    uintptr_t thread;
    memcpy(&thread,tlr+0x1f8,sizeof thread);
    hades_memory_info info;
    if (!thread || !pthread_getspecific(keys[2])
        || hades_memory_info_query(function,&info) || (info.permissions&5)!=5) {
        fail("Guest callback requires an initialized SDK thread and executable guest target");
        abort();
    }
    return hades_callout_run(function,arguments);
}

static void create_entropy(void) {
    arc4random_buf(process_entropy, sizeof process_entropy);
}

uint32_t hades_register_main_thread(void) {
    if (main_handle) {
        fail("Main kernel thread already registered");
        abort();
    }
    if (hades_threads_initialize(hades_main_priority, hades_main_core, hades_priority_mask, hades_core_mask)) {
        fail("Cannot register main kernel thread");
        abort();
    }
    main_handle = 0x100;
    return main_handle;
}

int hades_initialize_memory(size_t heap_capacity) {
    size_t charge = 0;
    for (unsigned i = 0; i < hades_spec_count; i++) {
        if (!hades_images[i].loaded || hades_specs[i].size > SIZE_MAX - charge)
            return fail("Memory model requires all mapped module sizes");
        charge += hades_specs[i].size;
    }
    hades_memory_config configuration = {
        .alias_size=256ull << 20, .stack_size=128ull << 20,
        .heap_capacity=heap_capacity, .image_charge=charge,
        .program_id=0x0100535012974000ull, .address_space_limit=hades_address_space_limit,
        .priority_mask=hades_priority_mask, .core_mask=hades_core_mask,
        .system_resource_budget=hades_system_resource_budget, .subpage_copy_compatibility=1};
    if (hades_memory_initialize(&configuration))
        return -1;
    for (unsigned i = 0; i < hades_spec_count; i++) {
        const hades_spec *spec = &hades_specs[i];
        uintptr_t base = (uintptr_t)hades_images[i].base;
        if (hades_memory_register(base, spec->writable_start, 3, 5, i)
            || hades_memory_register(base + spec->writable_start,
                spec->key_offset - spec->writable_start, 4, 3, i)
            || hades_memory_register(base + spec->key_offset, spec->key_size, 4, 1, i)
            || (spec->patch_offset > spec->key_offset + spec->key_size &&
                hades_memory_register(base + spec->key_offset + spec->key_size,
                    spec->patch_offset - spec->key_offset - spec->key_size, 4, 3, i))
            || hades_memory_register(base + spec->patch_offset, spec->size - spec->patch_offset, 3, 5, i)) {
            hades_memory_shutdown();
            return -1;
        }
    }
    return 0;
}

static void create_keys(void) {
    for (unsigned i = 0; i < 4; i++) {
        if (pthread_key_create(&keys[i], NULL) || !keys[i] || keys[i] >= 512) {
            key_error = 1;
            return;
        }
    }
}

int hades_initialize_thread(void) {
    if (pthread_once(&key_once, create_keys) || key_error)
        return fail("Cannot allocate compatible pthread keys");
    if (pthread_setspecific(keys[0], NULL) || pthread_setspecific(keys[1], NULL)
        || pthread_setspecific(keys[2], tlr) || pthread_setspecific(keys[3], (void *)dm_gate))
        return fail("Cannot initialize guest thread slots");
    uintptr_t tsd;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tsd));
    if (!tsd || tsd % 8)
        return fail("Host TSD pointer is incompatible with portable ABI");
    for (unsigned i = 0; i < 4; i++) {
        const void *actual = ((void *const *)tsd)[keys[i]];
        if (actual != pthread_getspecific(keys[i]))
            return fail("Host TSD slot layout does not match pthread_getspecific");
    }
    return 0;
}

static int remap(vm_address_t to, const void *from, size_t size, boolean_t copy, vm_prot_t permissions) {
    vm_address_t result = to;
    vm_prot_t current = 0, maximum = 0;
    kern_return_t status = vm_remap(mach_task_self(), &result, size, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)from, copy,
        &current, &maximum, VM_INHERIT_NONE);
    if (status != KERN_SUCCESS || result != to || (current & permissions) != permissions)
        return fail("Signed-page remap failed or lacks required permissions");
    return 0;
}

int hades_map(unsigned index) {
    if (index >= hades_spec_count || hades_images[index].loaded)
        return fail("Invalid or already mapped module");
    const hades_spec *spec = &hades_specs[index];
    char first[17], patch[17], writable[17];
    snprintf(first, sizeof first, "__dm%sx0", spec->name);
    snprintf(patch, sizeof patch, "__dm%sx1", spec->name);
    snprintf(writable, sizeof writable, "__DM%sW", spec->name);
    unsigned long a = 0, b = 0, c = 0;
    const void *x0 = getsectiondata(&_mh_execute_header, "__TEXT", first, &a);
    const void *w = getsectiondata(&_mh_execute_header, writable, "__img", &b);
    const void *x1 = getsectiondata(&_mh_execute_header, "__TEXT", patch, &c);
    if (spec->load_bias>=16384 || spec->load_bias%4096 || spec->size>SIZE_MAX-spec->load_bias
        || !x0 || !w || !x1 || a != spec->writable_start+spec->load_bias
        || a + b != spec->patch_offset+spec->load_bias
        || a + b + c != spec->size+spec->load_bias
        || ((uintptr_t)x0 | (uintptr_t)w | (uintptr_t)x1 | a | b | c) % 16384)
        return fail("Embedded signed section layout mismatch");
    vm_address_t reservation = 0;
    size_t reserved_size=spec->size+spec->load_bias;
    if (vm_allocate(mach_task_self(), &reservation, reserved_size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return fail("Cannot reserve module address space");
    vm_address_t base=reservation+spec->load_bias;
    if (remap(reservation, x0, a, FALSE, VM_PROT_READ | VM_PROT_EXECUTE)
        || remap(reservation + a, w, b, TRUE, VM_PROT_READ | VM_PROT_WRITE)
        || remap(reservation + a + b, x1, c, FALSE, VM_PROT_READ | VM_PROT_EXECUTE)) {
        vm_deallocate(mach_task_self(), reservation, reserved_size);
        return -1;
    }
    if (spec->key_offset < spec->writable_start || spec->key_size != 16384
        || spec->key_offset + spec->key_size > spec->patch_offset
        || (base+spec->key_offset)%16384) {
        vm_deallocate(mach_task_self(), reservation, reserved_size);
        return fail("Key page outside writable region");
    }
    uint64_t *page = (void *)(base + spec->key_offset);
    memset(page, 0, spec->key_size);
    for (unsigned i = 0; i < 4; i++) {
        if (spec->key_index[i] >= spec->key_size / 8) {
            vm_deallocate(mach_task_self(), reservation, reserved_size);
            return fail("Key index out of bounds");
        }
        page[spec->key_index[i]] = (uint64_t)keys[i] * 8;
    }
    if (mprotect(page, spec->key_size, PROT_READ)) {
        vm_deallocate(mach_task_self(), reservation, reserved_size);
        return fail("Cannot protect key page");
    }
    hades_images[index] = (hades_image){(void *)base, 1};
#ifdef HADES_VULKAN_HOST
    const char *capture=getenv("HADES_VULKAN_PASS_CAPTURE_FRAME");
    if (capture && strcmp(capture,"0"))
        fprintf(stderr,"[hades-capture] GUEST_IMAGE name=%s base=%#llx size=%#llx\n",
            spec->name,(unsigned long long)base,(unsigned long long)spec->size);
#endif
    return 0;
}

void hades_unmap(unsigned index) {
    if (index >= hades_spec_count || !hades_images[index].loaded) {
        fail("Invalid or already unmapped module");
        abort();
    }
    if (hades_memory_unregister(index)) {
        fail("Module still owns borrowed memory");
        abort();
    }
    if (vm_deallocate(mach_task_self(), (vm_address_t)hades_images[index].base-hades_specs[index].load_bias,
                      hades_specs[index].size+hades_specs[index].load_bias) != KERN_SUCCESS) {
        fail("Cannot unmap module");
        abort();
    }
    hades_images[index] = (hades_image){0};
}

int hades_relocate(void) {
    unsigned long size = 0;
    const hades_relocation *plan = (const void *)getsectiondata(
        &_mh_execute_header, "__DATA", "__hdrel", &size);
    if (!plan || size != hades_relocation_count * sizeof *plan)
        return fail("Relocation manifest size mismatch");
    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < hades_relocation_count; i++) {
            const hades_relocation *record = &plan[i];
            if (record->module >= hades_spec_count || !hades_images[record->module].loaded
                || record->offset < hades_specs[record->module].writable_start
                || record->offset > hades_specs[record->module].original_size - 8
                || record->offset % 8
                || record->target >= (int)hades_spec_count)
                return fail("Invalid relocation destination or target");
            const void *host = NULL;
            if (record->target < -1) {
                uint64_t index = (uint64_t)(-(int64_t)record->target - 2);
                if (!hades_host_services_enabled || index >= HADES_HOST_COUNT || record->value
                    || !(host = hades_host_entrypoint((unsigned)index)))
                    return fail("Invalid or disabled host function binding");
            }
            if (record->target >= 0 && (!hades_images[record->target].loaded
                || record->value > hades_specs[record->target].original_size))
                return fail("Relocation targets an unmapped module or invalid offset");
            if (pass) {
                uint64_t value = record->value;
                if (record->target >= 0)
                    value += (uintptr_t)hades_images[record->target].base;
                else if (host)
                    value = (uintptr_t)host;
                memcpy(hades_images[record->module].base + record->offset, &value, sizeof value);
            }
        }
    }
    return 0;
}

static int digest_matches(const void *data, size_t size, const unsigned char expected[32]) {
    unsigned char digest[32];
    if (!data || size > UINT32_MAX)
        return 0;
    CC_SHA256(data, (CC_LONG)size, digest);
    return !memcmp(digest, expected, sizeof digest);
}

int hades_ro_command(hades_ro_session *session, unsigned command, uint64_t pid,
                     const void *data, size_t size, size_t bss_size, uintptr_t *output) {
    if (!session || !pid)
        return fail("Invalid ldr:ro session or PID");
    if (command == 4) {
        if (session->initialized)
            return fail("ldr:ro process already registered");
        session->pid = pid;
        session->initialized = 1;
        return 0;
    }
    if (!session->initialized || session->pid != pid)
        return fail("ldr:ro caller PID mismatch or uninitialized session");
    switch (command) {
        case 2:
            if (session->registered || size != 4096 || (uintptr_t)data % 4096
                || !digest_matches(data, size, hades_nrr_digest))
                return fail("NRR differs from the signature-checked package manifest");
            session->registration = data;
            session->registered = 1;
            return 0;
        case 0:
            if (!session->registered || !output || (uintptr_t)data % 4096
                || size != hades_specs[4].original_size - hades_eos_bss
                || bss_size != hades_eos_bss || !digest_matches(data, size, hades_nro_digest))
                return fail("NRO is not authorized, aligned or compatible with its AOT image");
            if (hades_map(4))
                return -1;
            *output = (uintptr_t)hades_images[4].base;
            return 0;
        case 1:
            if (!hades_images[4].loaded || data != hades_images[4].base)
                return fail("ldr:ro unload address does not match loaded module");
            hades_unmap(4);
            return 0;
        case 3:
            if (!session->registered || data != session->registration || hades_images[4].loaded)
                return fail("NRR is not registered or still owns a loaded module");
            session->registered = 0;
            session->registration = NULL;
            return 0;
        default:
            return fail("Unsupported ldr:ro command; HIPC transport is not implemented");
    }
}

uint64_t gate_dispatch(uint64_t ret, uint64_t *registers) {
    for (unsigned i = 0; i < hades_gate_count; i++) {
        const hades_gate *gate = &hades_gates[i];
        if (!hades_images[gate->module].loaded
            || ret != (uintptr_t)hades_images[gate->module].base + gate->ret)
            continue;
        if (gate->kind == 1 && gate->arg < 31) {
            mach_timebase_info_data_t timebase;
            if (mach_timebase_info(&timebase) != KERN_SUCCESS || !timebase.denom)
                break;
            __uint128_t ticks = (__uint128_t)mach_absolute_time() * timebase.numer * 19200000;
            registers[gate->arg] = (uint64_t)(ticks / ((__uint128_t)timebase.denom * 1000000000));
            return registers[0];
        }
        if (!gate->kind && gate->arg == 0x29 && registers[1] == 11
            && registers[2] == 0 && registers[3] < 4) {
            if (pthread_once(&entropy_once, create_entropy)) {
                fail("Cannot initialize process entropy");
                exit(3);
            }
            registers[1] = process_entropy[registers[3]];
            registers[0] = 0;
            return 0;
        }
        if (!gate->kind) {
            int threaded = hades_thread_svc(gate->arg, registers);
            if (threaded > 0)
                return registers[0];
            if (threaded < 0)
                exit(3);
            int transport = hades_ipc_svc(gate->arg, registers, tlr, 256);
            if (transport > 0)
                return registers[0];
            if (transport < 0) {
                fprintf(stderr,"[hades] IPC backend failure for SVC %#x at %s+%#llx\n",
                        gate->arg,hades_specs[gate->module].name,(unsigned long long)gate->original);
                guest_backtrace(registers[29]);
                exit(3);
            }
            if (gate->arg == 4)
                for (unsigned module = 0; module < hades_spec_count; module++) {
                    uintptr_t base = (uintptr_t)hades_images[module].base;
                    if (hades_images[module].loaded && registers[1] >= base
                        && registers[1] - base < hades_specs[module].original_size)
                        fprintf(stderr, "[hades] MapMemory source module=%s offset=%#llx\n",
                                hades_specs[module].name, (unsigned long long)(registers[1] - base));
                }
            int handled = hades_memory_svc(gate->arg, registers);
            if (handled > 0)
                return registers[0];
            if (handled < 0)
                exit(3);
        }
        if (!gate->kind && gate->arg == 0x1F) {
            hades_memory_info information;
            uintptr_t address = registers[1];
            if (!hades_memory_info_query(address, &information) && (information.permissions & 1)) {
                size_t available = information.size - (address - information.base);
                size_t length = strnlen((const char *)address, available < 12 ? available : 12);
                if (length && length < 12 && length < available) {
                    const char *name = (const char *)address;
                    size_t i;
                    for (i = 0; i < length; i++)
                        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z')
                            || (name[i] >= '0' && name[i] <= '9') || name[i] == ':' || name[i] == '_'))
                            break;
                    if (i == length)
                        fprintf(stderr, "[hades] required named port: %.*s; transport is unimplemented\n",
                                (int)length, name);
                }
            }
        }
        fprintf(stderr, "[hades] unsupported SVC %#x at %s+%#llx; x0=%#llx x1=%#llx x2=%#llx x3=%#llx\n",
                gate->arg, hades_specs[gate->module].name, (unsigned long long)gate->original,
                (unsigned long long)registers[0], (unsigned long long)registers[1],
                (unsigned long long)registers[2], (unsigned long long)registers[3]);
        if (gate->arg==0x26 && registers[2]==4) {
            hades_memory_info data;
            if (!hades_memory_info_query(registers[1],&data) && (data.permissions&1)
                && registers[1]>=data.base && data.size-(registers[1]-data.base)>=4) {
                uint32_t result;
                memcpy(&result,(const void *)(uintptr_t)registers[1],4);
                fprintf(stderr,"[hades] guest Break result=%#x module=%u description=%u\n",
                        result,result&511,(result>>9)&8191);
            }
        }
        guest_backtrace(registers[29]);
        exit(3);
    }
    fail("Unregistered or unsupported host gate");
    exit(3);
}

uint64_t unimpl_c(unsigned index, const uint64_t *registers) {
    (void)index;
    (void)registers;
    fail("Unexpected unresolved import trampoline");
    exit(3);
}

void dm_thread_returned(void) {
    fail("Guest thread trampoline is not part of the core probe");
    exit(3);
}
