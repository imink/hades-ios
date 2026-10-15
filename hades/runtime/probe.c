#define _DARWIN_C_SOURCE
#include "core.h"
#include "memory.h"
#include "ipc.h"
#include "threads.h"
#include "probes.h"
#include "host.h"
#ifdef HADES_VULKAN_HOST
#include "vulkan.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <sys/stat.h>
#include <unistd.h>

static void *read_aligned(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    struct stat st;
    void *data = NULL;
    if (fstat(fileno(file), &st) || st.st_size <= 0 || st.st_size > 32 * 1024 * 1024
        || posix_memalign(&data, 4096, (size_t)st.st_size)) {
        fclose(file);
        return NULL;
    }
    *size = (size_t)st.st_size;
    if (fread(data, 1, *size, file) != *size) {
        free(data);
        data = NULL;
    }
    fclose(file);
    return data;
}

static void *timer_worker(void *context) {
    (void)context;
    if (hades_initialize_thread())
        return (void *)1;
    uint64_t (*tick)(void) = (void *)(hades_images[3].base + hades_tick_offset);
    uint64_t previous = tick();
    for (unsigned i = 0; i < 100; i++) {
        uint64_t current = tick();
        if (current < previous)
            return (void *)1;
        previous = current;
    }
    return NULL;
}

static char *append_text(char *output, const char *text) {
    while (*text)
        *output++ = *text++;
    return output;
}

static char *append_hex(char *output, uintptr_t value) {
    output = append_text(output, "0x");
    for (int shift = (int)(sizeof value * 8) - 4; shift >= 0; shift -= 4)
        *output++ = "0123456789abcdef"[(value >> shift) & 15];
    return output;
}

static void fault_handler(int signal_number, siginfo_t *information, void *context) {
    const ucontext_t *state = context;
    uintptr_t pc = (uintptr_t)state->uc_mcontext->__ss.__pc;
    uintptr_t lr = (uintptr_t)state->uc_mcontext->__ss.__lr;
    char buffer[512];
    unsigned module = 5;
    uintptr_t offset = pc;
    for (unsigned i = 0; i < hades_spec_count; i++) {
        uintptr_t base = (uintptr_t)hades_images[i].base;
        if (hades_images[i].loaded && pc >= base && pc - base < hades_specs[i].size) {
            module = i;
            offset = pc - base;
            break;
        }
    }
    char *output = append_text(buffer, "[hades] fatal signal ");
    output = append_hex(output, (uintptr_t)signal_number);
    output = append_text(output, " address=");
    output = append_hex(output, (uintptr_t)information->si_addr);
    output = append_text(output, " pc=");
    output = append_hex(output, module);
    output = append_text(output, "+");
    output = append_hex(output, offset);
    output = append_text(output, " lr=");
    output = append_hex(output, lr);
    for (unsigned i=0;i<hades_spec_count;i++) {
        uintptr_t base=(uintptr_t)hades_images[i].base;
        if (hades_images[i].loaded && lr>=base && lr-base<hades_specs[i].size) {
            output=append_text(output," lr-module=");
            output=append_hex(output,i);
            output=append_text(output,"+");
            output=append_hex(output,lr-base);
            break;
        }
    }
    *output++ = '\n';
    write(STDERR_FILENO, buffer, (size_t)(output - buffer));
    _exit(128 + signal_number);
}

static void *entrypoint(const hades_entry *entry) {
    if (entry->module >= hades_spec_count || !hades_images[entry->module].loaded
        || entry->offset >= hades_specs[entry->module].writable_start)
        return NULL;
    return hades_images[entry->module].base + entry->offset;
}

static int sdk_init(size_t heap_capacity, int game_mode) {
    if (hades_initialize_memory(heap_capacity))
        return -1;
    if (hades_ipc_initialize((uint64_t)getpid()))
        return -1;
    if (hades_ipc_set_display_version(hades_display_version))
        return -1;
    uint32_t handle = hades_register_main_thread();
    for (unsigned i = 0; i < 6; i++) {
        const hades_entry *entry = &hades_bootstrap[i];
        if (entry->module >= hades_spec_count || !hades_images[entry->module].loaded
            || entry->offset >= hades_specs[entry->module].writable_start)
            return -1;
        void *function = hades_images[entry->module].base + entry->offset;
        fprintf(stderr, "[hades] SDK bootstrap %s at %s+%#llx\n", entry->name,
                hades_specs[entry->module].name, (unsigned long long)entry->offset);
        if (i == 1)
            ((void (*)(uint64_t, uint64_t))function)(handle, 0);
        else
            ((void (*)(void))function)();
        fprintf(stderr, "[hades] SDK bootstrap completed %s\n", entry->name);
    }
    if (game_mode) {
        for (unsigned i = 0; i < hades_initializer_count; i++) {
            const hades_entry *entry = &hades_initializers[i];
            if (!entry->offset)
                continue;
            void *function = entrypoint(entry);
            if (!function)
                return -1;
            fprintf(stderr, "[hades] resident DT_INIT %s\n", entry->name);
            ((void (*)(void))function)();
            fprintf(stderr, "[hades] resident DT_INIT completed %s\n", entry->name);
        }
        void *argc_function = entrypoint(&hades_set_argc);
        void *argv_function = entrypoint(&hades_set_argv);
        void *game = entrypoint(&hades_game_entry);
        if (!argc_function || !argv_function || !game)
            return -1;
        static char name[] = "Hades";
        static char *arguments[] = {name, NULL};
        ((void (*)(int))argc_function)(1);
        ((void (*)(char **))argv_function)(arguments);
        fprintf(stderr, "[hades] entering actual Hades nnMain; no verified frame or gameplay\n");
        ((void (*)(void))game)();
        fprintf(stderr, "[hades] actual Hades nnMain returned\n");
        return 0;
    }
    const unsigned char *base = hades_images[3].base;
    size_t size = hades_specs[3].original_size;
    uint32_t mod0;
    memcpy(&mod0, base + 4, 4);
    if (mod0 > size - 8 || memcmp(base + mod0, "MOD0", 4))
        return -1;
    int32_t relative;
    memcpy(&relative, base + mod0 + 4, 4);
    int64_t dynamic = (int64_t)mod0 + relative;
    for (unsigned i = 0; i < 4096; i++, dynamic += 16) {
        if (dynamic < 0 || (uint64_t)dynamic > size - 16)
            return -1;
        int64_t tag;
        uint64_t value;
        memcpy(&tag, base + dynamic, 8);
        memcpy(&value, base + dynamic + 8, 8);
        if (!tag)
            break;
        if (tag == 12 && value && value < hades_specs[3].writable_start) {
            fprintf(stderr, "[hades] Entering actual SDK DT_INIT +%#llx; unsupported services terminate\n",
                    (unsigned long long)value);
            ((void (*)(void))(base + value))();
            fprintf(stderr, "[hades] SDK DT_INIT returned; other constructors and game entry not executed\n");
            return 0;
        }
    }
    return -1;
}

int hades_run_core_probe(int argc, const char *const *argv) {
    if (argc != 3 && ((argc != 4 && argc != 5 && argc != 7)
        || (strcmp(argv[3], "--sdk-init") && strcmp(argv[3], "--sdk-host") && strcmp(argv[3], "--game-host")
            && strcmp(argv[3], "--game-vulkan")))) {
        fprintf(stderr, "Usage: hades-core <original NRR> <original NRO> [--sdk-init|--sdk-host|--game-host [heap-capacity-MiB [--storage <existing private application directory>]]]\n");
        return 2;
    }
    int vulkan_mode = argc >= 4 && !strcmp(argv[3], "--game-vulkan");
    int game_mode = argc >= 4 && (!strcmp(argv[3], "--game-host") || vulkan_mode);
#ifdef HADES_VULKAN_HOST
    if (game_mode && !vulkan_mode) {
        fprintf(stderr,"[hades] Vulkan-compiled profile requires explicit --game-vulkan invocation\n");
        return 2;
    }
    if (vulkan_mode && (hades_vulkan_set_callback_runner(hades_guest_callback) || hades_vulkan_prepare(NULL)))
        return 2;
#else
    if (vulkan_mode) {
        fprintf(stderr,"[hades] --game-vulkan requires explicitly compiled pinned Vulkan backend\n");
        return 2;
    }
#endif
    int host_mode = argc >= 4 && (!strcmp(argv[3], "--sdk-host") || game_mode);
    if (argc == 7 && (!host_mode || strcmp(argv[5], "--storage")))
        return 2;
    if (argc >= 4 && host_mode != hades_host_services_enabled) {
        fprintf(stderr, "[hades] SDK invocation must match the explicit compiled host-service profile\n");
        return 2;
    }
    if (host_mode) {
        char root[4096];
        if (strlcpy(root, argv[1], sizeof root) >= sizeof root)
            return 2;
        char *end = strrchr(root, '/');
        if (!end)
            return 2;
        *end = 0;
        end = strrchr(root, '/');
        if (!end || strcmp(end + 1, ".nrr"))
            return 2;
        *end = 0;
        if (hades_host_initialize(root,hades_render_resolution,hades_ui_overlays,hades_ui_overlay_count))
            return 2;
        if (argc == 7 && hades_host_configure_user(argv[6]))
            return 2;
    }
    size_t heap_capacity = 2048ull << 20;
    if (argc >= 5) {
        char *end = NULL;
        unsigned long value = strtoul(argv[4], &end, 10);
        if (!argv[4][0] || *end || value < 2 || value > 4096 || value % 2)
            return 2;
        heap_capacity = (size_t)value << 20;
    }
    struct sigaction action = {0};
    action.sa_sigaction = fault_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, NULL) || sigaction(SIGBUS, &action, NULL) || sigaction(SIGILL, &action, NULL))
        return 2;
    size_t nrr_size = 0, nro_size = 0;
    void *nrr = read_aligned(argv[1], &nrr_size);
    void *nro = read_aligned(argv[2], &nro_size);
    if (!nrr || !nro || hades_initialize_thread()) {
        fprintf(stderr, "[hades] Cannot initialize local core probe\n");
        return 2;
    }
    for (unsigned i = 0; i < 4; i++)
        if (hades_map(i))
            return 2;
    hades_ro_session session = {0};
    uintptr_t base = 0;
    uint64_t pid = (uint64_t)getpid();
    if (!hades_ro_command(&session, 0, pid, nro, nro_size, hades_eos_bss, &base)
        || hades_ro_command(&session, 4, pid, NULL, 0, 0, NULL)
        || !hades_ro_command(&session, 2, pid + 1, nrr, nrr_size, 0, NULL))
        return 2;
    ((unsigned char *)nrr)[0] ^= 1;
    int rejected = hades_ro_command(&session, 2, pid, nrr, nrr_size, 0, NULL) != 0;
    ((unsigned char *)nrr)[0] ^= 1;
    if (!rejected || session.registered
        || hades_ro_command(&session, 2, pid, nrr, nrr_size, 0, NULL)
        || !hades_ro_command(&session, 0, pid, nro, nro_size, hades_eos_bss + 4096, &base))
        return 2;
    ((unsigned char *)nro)[0] ^= 1;
    rejected = hades_ro_command(&session, 0, pid, nro, nro_size, hades_eos_bss, &base) != 0;
    ((unsigned char *)nro)[0] ^= 1;
    if (!rejected || hades_images[4].loaded
        || hades_ro_command(&session, 0, pid, nro, nro_size, hades_eos_bss, &base)
        || !hades_ro_command(&session, 3, pid, nrr, 0, 0, NULL)
        || hades_relocate())
        return 2;
    const char *(*version)(void) = (void *)(base + hades_version_offset);
    const char *result = version();
    uintptr_t offset = (uintptr_t)result - base;
    if (offset >= hades_specs[4].original_size
        || !memchr(result, 0, hades_specs[4].original_size - offset))
        return 2;
    printf("EOS_GetVersion native result: %s\n", result);
    uint64_t (*tick)(void) = (void *)(hades_images[3].base + hades_tick_offset);
    uint64_t (*frequency)(void) = (void *)(hades_images[3].base + hades_frequency_offset);
    uint64_t first = tick();
    usleep(1000);
    uint64_t second = tick();
    if (!first || second <= first || frequency() != 19200000) {
        fprintf(stderr, "[hades] Native guest timer mismatch\n");
        return 2;
    }
    printf("Guest timer native gate: monotonic, frequency=%llu\n", (unsigned long long)frequency());
    pthread_t workers[8];
    for (unsigned i = 0; i < 8; i++)
        if (pthread_create(&workers[i], NULL, timer_worker, NULL))
            return 2;
    for (unsigned i = 0; i < 8; i++) {
        void *status = NULL;
        if (pthread_join(workers[i], &status) || status)
            return 2;
    }
    printf("Native TSD/timer probe: 8 pthreads x 100 reads; not a guest atomic/threading proof\n");
    fflush(stdout);
    if (argc >= 4 && sdk_init(heap_capacity, game_mode))
        return 2;
    if (host_mode) {
        fprintf(stderr, "[hades] HOST_BOOTSTRAP_RETURN: guest mappings and live SDK threads retained; no forced unload, no complete concurrency/gameplay claim\n");
        return 0;
    }
    if (argc >= 4 && hades_threads_shutdown())
        return 2;
    if (hades_ro_command(&session, 1, pid, (void *)base, 0, 0, NULL)
        || hades_ro_command(&session, 3, pid, nrr, 0, 0, NULL))
        return 2;
    for (unsigned i = 0; i < 4; i++)
        hades_unmap(i);
    if (hades_memory_shutdown())
        return 2;
    if (hades_ipc_shutdown())
        return 2;
    if (host_mode && hades_host_shutdown())
        return 2;
    free(nrr);
    free(nro);
    printf("CORE_OK: signed AOT mapping, %u relocations, decoded ldr:ro lifecycle; no constructors or game entry\n",
           hades_relocation_count);
    return 0;
}

#ifndef HADES_IOS_APP
int main(int argc, char **argv) {
#ifdef HADES_VULKAN_HOST
    if (argc>1 && !strcmp(argv[argc-1],"--window")) {
        extern int hades_run_window_probe(int,const char *const *);
        return hades_run_window_probe(argc-1,(const char *const *)argv);
    }
#endif
    return hades_run_core_probe(argc, (const char *const *)argv);
}
#endif
