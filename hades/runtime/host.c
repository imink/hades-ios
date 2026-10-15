#define _DARWIN_C_SOURCE
#include "host.h"
#include "memory.h"
#include "save.h"
#include "trace.h"
#include "display.h"
#include <CommonCrypto/CommonDigest.h>
#ifdef HADES_VULKAN_HOST
#include "vulkan.h"
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *const names[] = {
#define HADES_BINDING(operation, name) name,
#include "host_bindings.def"
#ifdef HADES_VULKAN_HOST
#include "vulkan_bindings.def"
#include "vulkan_optional.def"
#endif
#undef HADES_BINDING
};
extern const unsigned char hades_host_stubs[];
_Static_assert(HADES_HOST_COUNT <= 256, "Static host trampoline capacity");
static _Atomic unsigned char traced_calls[HADES_HOST_COUNT];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int root_fd = -1, user_fd = -1, fs_ready, pctl_ready, time_ready, account_ready;
static int directory_order_probe;
static const hades_ui_overlay *display_overlays;
static unsigned display_overlay_count;
static unsigned char local_user[16];
static uint64_t user_handles[8], next_user = 0xAACC0001;
static char mounted[32];
static char save_mount[32];
static hades_save save_session = {-1,-1,-1,0};
static struct { uint64_t id; int fd, mode, save; const hades_ui_overlay *overlay; } files[64];
typedef struct { char name[0x301]; uint8_t padding[3]; int8_t type; uint8_t padding2[3]; int64_t size; } directory_entry;
_Static_assert(sizeof(directory_entry) == 0x310, "Horizon directory entry ABI");
static struct { uint64_t id; directory_entry *entries; size_t count, cursor; int save; } directories[32];
static uint64_t next_directory = 0x300000;
static uint64_t next_file = 0x200000;
typedef struct {
    uintptr_t buffer;
    size_t capacity, length;
    uint64_t file;
    int64_t offset;
} read_cache;
static read_cache cache, rom_cache;
static unsigned network_references;
static enum { NETWORK_IDLE, NETWORK_DENIED } network_request;

static int error(const char *message) {
    fprintf(stderr, "[hades-host] %s (errno=%d)\n", message, errno);
    return -1;
}

static const char *guest_string(uintptr_t address) {
    hades_memory_info information;
    if (hades_memory_info_query(address, &information) || !(information.permissions & 1))
        return NULL;
    size_t size = information.size - (address - information.base);
    if (size > 769)
        size = 769;
    const char *text = (const void *)address;
    return strnlen(text, size) < size ? text : NULL;
}

static int guest_readable(uintptr_t address, size_t size) {
    hades_memory_info information;
    return address && !hades_memory_info_query(address, &information) && (information.permissions & 1)
           && size <= information.size - (address - information.base);
}

static int exact_component(int directory, const char *name) {
    int duplicate = dup(directory);
    if (duplicate < 0)
        return -1;
    DIR *entries = fdopendir(duplicate);
    if (!entries) {
        close(duplicate);
        return -1;
    }
    rewinddir(entries);
    int found = 0;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(entries)))
        if (!strcmp(entry->d_name, name)) {
            found = 1;
            break;
        }
    int saved = errno;
    if (closedir(entries))
        return -1;
    if (!found)
        errno = saved ? saved : ENOENT;
    return found ? 0 : -1;
}

static int mount_root(const char *path, const char **rest, int *save) {
    const char *separator = strchr(path, ':');
    size_t prefix = separator ? (size_t)(separator - path) : 0;
    if (!prefix || !separator || separator[1] != '/') {
        errno = EINVAL;
        return -1;
    }
    *rest = separator + 2;
    if (strlen(mounted) == prefix && !strncmp(path, mounted, prefix)) {
        *save = 0;
        return root_fd;
    }
    if (strlen(save_mount) == prefix && !strncmp(path, save_mount, prefix) && save_session.work >= 0) {
        *save = 1;
        return save_session.work;
    }
    errno = EINVAL;
    return -1;
}

static int parent_path(const char *path, char leaf[256], int *save) {
    const char *rest;
    int root = mount_root(path, &rest, save);
    if (root < 0)
        return -1;
    char components[769];
    if (strlcpy(components, rest, sizeof components) >= sizeof components) {
        errno = ENAMETOOLONG;
        return -1;
    }
    leaf[0] = 0;
    int parent = dup(root);
    if (parent < 0)
        return -1;
    char *tokens = NULL, *component = strtok_r(components, "/", &tokens);
    while (component) {
        if (!strcmp(component, ".") || !strcmp(component, "..") || strchr(component, '\\')) {
            close(parent);
            errno = EINVAL;
            return -1;
        }
        char *next = strtok_r(NULL, "/", &tokens);
        if (!next) {
            if (strlcpy(leaf, component, 256) >= 256) {
                close(parent);
                errno = ENAMETOOLONG;
                return -1;
            }
            return parent;
        }
        if (exact_component(parent, component)) {
            int saved = errno;
            close(parent);
            errno = saved;
            return -1;
        }
        int fd = openat(parent, component, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (next ? O_DIRECTORY : O_NONBLOCK));
        int saved = errno;
        if (close(parent)) {
            if (fd >= 0)
                close(fd);
            return -1;
        }
        if (fd < 0) {
            errno = saved;
            return -1;
        }
        parent = fd;
        component = next;
    }
    return parent;
}

static int open_guest_path(const char *path, int flags, int *save) {
    char leaf[256];
    int parent = parent_path(path, leaf, save);
    if (parent < 0)
        return -1;
    if (!leaf[0])
        return parent;
    int fd = exact_component(parent, leaf) ? -1
        : openat(parent, leaf, flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    int saved = errno;
    if (close(parent)) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    errno = saved;
    return fd;
}

static int valid_mount(const char *name) {
    if (!name || !name[0] || strlen(name) >= sizeof mounted)
        return 0;
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
              || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return 0;
    return 1;
}

static int path_result(uint64_t *registers) {
    if (errno == ENOENT || errno == ENOTDIR) {
        registers[0] = 0x202;
        return 0;
    }
    return error("Filesystem operation failed; no fabricated Horizon error/success");
}

static int compare_directory_names(const void *left, const void *right) {
    const directory_entry *a = left, *b = right;
    return strcmp(a->name, b->name);
}

static const hades_ui_overlay *display_ui_for_path(const char *path, int save) {
    const char *relative = strchr(path, ':');
    if (!save && relative && relative[1] == '/')
        for (unsigned i = 0; i < display_overlay_count; i++)
            if (!strcmp(relative + 2, display_overlays[i].path))
                return &display_overlays[i];
    return NULL;
}

static int list_directory(const char *path, unsigned mode, int *save, directory_entry **output, size_t *count) {
    int fd = open_guest_path(path, O_RDONLY | O_DIRECTORY, save);
    if (fd < 0)
        return -1;
    DIR *stream = fdopendir(fd);
    if (!stream) {
        close(fd);
        return -1;
    }
    directory_entry *entries = NULL;
    size_t length = 0, capacity = 0;
    int status = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) {
            if (errno)
                status = -1;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        struct stat info;
        if (fstatat(fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW)
            || (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode))) {
            status = -1;
            break;
        }
        if (!(mode & (S_ISDIR(info.st_mode) ? 1u : 2u)))
            continue;
        if (length == capacity) {
            if (capacity >= 65536) {
                errno = ENOMEM;
                status = -1;
                break;
            }
            size_t next = capacity ? capacity * 2 : 16;
            directory_entry *grown = realloc(entries, next * sizeof *entries);
            if (!grown) {
                status = -1;
                break;
            }
            entries = grown;
            capacity = next;
        }
        entries[length] = (directory_entry){.type=S_ISDIR(info.st_mode) ? 0 : 1,
                                           .size=S_ISDIR(info.st_mode) ? 0 : info.st_size};
        if (!*save && S_ISREG(info.st_mode)) {
            char child[1100];
            int size = snprintf(child, sizeof child, "%s%s%s", path,
                                path[strlen(path) - 1] == '/' ? "" : "/", entry->d_name);
            if (size < 0 || (size_t)size >= sizeof child) {
                status = -1;
                break;
            }
            const hades_ui_overlay *overlay = display_ui_for_path(child, 0);
            if (overlay)
                entries[length].size = (int64_t)overlay->size;
        }
        if (strlcpy(entries[length].name, entry->d_name, sizeof entries[length].name) >= sizeof entries[length].name) {
            status = -1;
            break;
        }
        length++;
    }
    if (closedir(stream))
        status = -1;
    if (status) {
        free(entries);
        return error("Directory enumeration failed or contains unsupported entry");
    }
    if (directory_order_probe && !*save && length > 1)
        qsort(entries, length, sizeof *entries, compare_directory_names);
    *output = entries;
    *count = length;
    return 0;
}

static int read_actual(int fd, unsigned char *destination, int64_t offset, size_t length, size_t *got) {
    *got = 0;
    while (*got < length) {
        size_t amount = length - *got;
        if (amount > 1024 * 1024)
            amount = 1024 * 1024;
        ssize_t read = pread(fd, destination + *got, amount, offset + *got);
        if (read < 0) {
            if (errno == EINTR)
                continue;
            return error("Host pread failed");
        }
        if (!read)
            break;
        *got += (size_t)read;
    }
    return 0;
}

static int verify_display_ui(int fd, off_t size, const hades_ui_overlay *overlay) {
    if (size<0 || (uint64_t)size!=overlay->source_size)
        return error("Wide UI source size differs from the embedded overlay manifest");
    unsigned char *source=malloc(overlay->source_size);
    if (!source)
        return error("Cannot allocate wide UI verification buffer");
    size_t got;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    int status=read_actual(fd,source,0,overlay->source_size,&got);
    if (!status && got==overlay->source_size)
        CC_SHA256(source,(CC_LONG)overlay->source_size,digest);
    else
        status=-1;
    if (!status && memcmp(digest,overlay->source_digest,sizeof digest))
        status=-1;
    free(source);
    if (status)
        return error("Wide UI source verification failed; no overlay applied");
    fprintf(stderr,"[hades-host] WIDE_UI_OVERLAY verified %s source=%zu presented=%zu bytes; source/archive unchanged\n",
            overlay->path,overlay->source_size,overlay->size);
    return 0;
}

static int local_save_directory(void) {
    if (mkdirat(user_fd, "saves", 0700) && errno != EEXIST)
        return error("Cannot create application-local save container");
    int parent = openat(user_fd, "saves", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0)
        return error("Cannot open owned save container");
    char name[33];
    for (unsigned i = 0; i < sizeof local_user; i++)
        snprintf(name + 2*i, 3, "%02x", local_user[i]);
    int status = mkdirat(parent, name, 0700) && errno != EEXIST;
    int directory = status ? -1 : openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0 || fsync(parent) || fsync(user_fd))
        status = 1;
    if (close(parent))
        status = 1;
    if (status) {
        if (directory >= 0)
            close(directory);
        return error("Cannot durably ensure real local save namespace");
    }
    return directory;
}

static int ensure_local_save(void) {
    int directory = local_save_directory();
    if (directory < 0 || close(directory))
        return error("Cannot ensure/close actual local save namespace");
    fprintf(stderr, "[hades-host] actual persistent local save namespace ensured\n");
    return 0;
}

int hades_host_initialize(const char *romfs, const char *render_resolution,
                          const hades_ui_overlay *overlays, unsigned overlay_count) {
    pthread_mutex_lock(&lock);
    uint32_t width,height;
    if (!romfs || root_fd >= 0 || hades_display_extent(render_resolution,&width,&height)) {
        pthread_mutex_unlock(&lock);
        return error("Missing or duplicate host RomFS configuration");
    }
    int wide=render_resolution && !strcmp(render_resolution,"wide-720p");
    if ((!wide && overlay_count) || (wide && !overlay_count)
        || (overlay_count && !overlays) || overlay_count>35) {
        pthread_mutex_unlock(&lock);
        return error("Invalid or non-wide UI overlay configuration");
    }
    for (unsigned i=0;i<overlay_count;i++)
        if (!overlays[i].path || strncmp(overlays[i].path,"Content/",8) || !overlays[i].data
            || !overlays[i].size || overlays[i].size>1024*1024
            || !overlays[i].source_size || overlays[i].source_size>1024*1024) {
            pthread_mutex_unlock(&lock);
            return error("Invalid embedded UI overlay descriptor");
        }
    const char *order = getenv("HADES_FS_DIRECTORY_ORDER_PROBE");
    if (order && strcmp(order, "0") && strcmp(order, "1")) {
        pthread_mutex_unlock(&lock);
        return error("Invalid HADES_FS_DIRECTORY_ORDER_PROBE; expected 0 or 1");
    }
    directory_order_probe = order && !strcmp(order, "1");
    display_overlays=overlays;
    display_overlay_count=overlay_count;
    root_fd = open(romfs, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0) {
        pthread_mutex_unlock(&lock);
        return error("Cannot open explicitly selected local RomFS");
    }
    fprintf(stderr, "[hades-host] configured real read-only RomFS; virtual offline pctl policy has no configured Switch restrictions\n");
    if (directory_order_probe)
        fprintf(stderr, "[hades-host] DIRECTORY_ORDER_PROBE read-only snapshots sorted by bytewise name; save order unchanged, not proof of graphics repair\n");
    pthread_mutex_unlock(&lock);
    return 0;
}

int hades_host_configure_user(const char *storage) {
    pthread_mutex_lock(&lock);
    if (!storage || user_fd >= 0) {
        pthread_mutex_unlock(&lock);
        return error("Missing or duplicate explicitly selected local user storage");
    }
    int directory = open(storage, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int fd = directory < 0 ? -1 : openat(directory, "local-user.bin", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    int created = 0;
    if (directory >= 0 && fd < 0 && errno == ENOENT) {
        fd = openat(directory, "local-user.bin", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            created = 1;
            arc4random_buf(local_user, sizeof local_user);
            if (write(fd, local_user, sizeof local_user) != sizeof local_user || fsync(fd)) {
                close(fd);
                if (unlinkat(directory, "local-user.bin", 0))
                    error("Cannot clean up failed new local identity file");
                fd = -1;
            }
        }
    }
    struct stat information;
    int status = fd < 0 || fstat(fd, &information) || !S_ISREG(information.st_mode)
        || information.st_size != sizeof local_user
        || (!created && pread(fd, local_user, sizeof local_user, 0) != sizeof local_user);
    unsigned char bits = 0;
    for (unsigned i = 0; i < sizeof local_user; i++)
        bits |= local_user[i];
    if (!bits)
        status = 1;
    if (fd >= 0 && close(fd))
        status = 1;
    if (created && !status && fsync(directory))
        status = 1;
    if (status) {
        if (directory >= 0)
            close(directory);
        pthread_mutex_unlock(&lock);
        return error("Cannot create/read durable nonzero application-local identity");
    }
    user_fd = directory;
    fprintf(stderr, "[hades-host] durable application-local offline user %s; not a Switch/Nintendo network account\n",
            created ? "created" : "reloaded");
    pthread_mutex_unlock(&lock);
    return 0;
}

int hades_host_shutdown(void) {
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < sizeof directories / sizeof directories[0]; i++)
        if (directories[i].id) {
            pthread_mutex_unlock(&lock);
            return error("Cannot shut down with open directory handles");
        }
    for (unsigned i = 0; i < sizeof user_handles / sizeof user_handles[0]; i++)
        if (user_handles[i]) {
            pthread_mutex_unlock(&lock);
            return error("Cannot shut down with open local user handles");
        }
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++)
        if (files[i].id) {
            pthread_mutex_unlock(&lock);
            return error("Cannot shut down host filesystem with open guest handles");
        }
    if (hades_save_close(&save_session)) {
        pthread_mutex_unlock(&lock);
        return -1;
    }
    save_mount[0] = 0;
    if (root_fd >= 0 && close(root_fd)) {
        pthread_mutex_unlock(&lock);
        return error("Cannot close host RomFS");
    }
    root_fd = -1;
    if (user_fd >= 0 && close(user_fd)) {
        pthread_mutex_unlock(&lock);
        return error("Cannot close local user storage");
    }
    user_fd = -1;
    display_overlays = NULL;
    display_overlay_count = 0;
    memset(local_user, 0, sizeof local_user);
    fs_ready = pctl_ready = time_ready = account_ready = 0;
    mounted[0] = 0;
    memset(&cache, 0, sizeof cache);
    memset(&rom_cache, 0, sizeof rom_cache);
    network_references = 0;
    network_request = NETWORK_IDLE;
    pthread_mutex_unlock(&lock);
    return 0;
}

const void *hades_host_entrypoint(unsigned index) {
    return index < HADES_HOST_COUNT ? hades_host_stubs + 8 * index : NULL;
}

static int call_locked(unsigned index, uint64_t *registers) {
    if (index==HADES_HOST_HID_VIBRATION_HANDLES) {
        int count=(int32_t)registers[1];
        hades_memory_info info;
        if (count<0 || count>64 || hades_memory_info_query(registers[2],&info)
            || !(info.permissions&1) || info.size-(registers[2]-info.base)<4
            || (count && !hades_memory_writable(registers[0],(size_t)count*4)))
            return error("Invalid vibration-handle output extent or readable Npad ID");
        uint32_t id;
        memcpy(&id,(const void *)(uintptr_t)registers[2],4);
        if ((id>7 && id!=32) || registers[3]&~0x7full)
            return error("Invalid supported Npad ID/style for vibration capability query");
        /* Keyboard/touch providers have no physical rumble actuator. Unlike
           the SDK's Switch-style inference, do not synthesize device handles. */
        registers[0]=0;
        return 0;
    }
    if (root_fd < 0)
        return error("Host service call before explicit configuration");
    if (index == HADES_HOST_PCTL_INITIALIZE) {
        pctl_ready = 1;
        fprintf(stderr, "[hades-host] pctl initialized: offline virtual policy only; no change to iOS restrictions\n");
        registers[0] = 0;
        return 0;
    }
    if (index == HADES_HOST_FS_INITIALIZE) {
        fs_ready = 1;
        fprintf(stderr, "[hades-host] filesystem initialized: immutable ROM and transactional local saves\n");
        registers[0] = 0;
        return 0;
    }
    if (index >= HADES_HOST_ACCOUNT_INITIALIZE && index <= HADES_HOST_ACCOUNT_CLOSE) {
        if (user_fd < 0)
            return error("Local account requires explicit durable application storage");
        if (index == HADES_HOST_ACCOUNT_INITIALIZE) {
            account_ready = 1;
            registers[0] = 0;
            fprintf(stderr, "[hades-host] initialized one offline application-local user; online account services remain unsupported\n");
            return 0;
        }
        if (!account_ready)
            return error("Account call before initialization");
        if (index == HADES_HOST_ACCOUNT_LAST) {
            if (!hades_memory_writable(registers[0], 16))
                return error("Invalid last-user output");
            memcpy((void *)(uintptr_t)registers[0], local_user, 16);
        } else if (index == HADES_HOST_ACCOUNT_LIST) {
            int32_t capacity = (int32_t)registers[2];
            if (capacity < 0 || !hades_memory_writable(registers[0], 4)
                || (capacity && !hades_memory_writable(registers[1], (size_t)capacity * 16)))
                return error("Invalid user list capacity or output");
            if (capacity)
                memcpy((void *)(uintptr_t)registers[1], local_user, 16);
            *(int32_t *)(uintptr_t)registers[0] = capacity ? 1 : 0;
        } else if (index == HADES_HOST_ACCOUNT_CLOSE) {
            if (!guest_readable(registers[0], 24)
                || memcmp((const void *)(uintptr_t)registers[0], local_user, 16))
                return error("Invalid local user handle");
            uint64_t token;
            memcpy(&token, (const unsigned char *)(uintptr_t)registers[0] + 16, 8);
            unsigned slot;
            for (slot = 0; slot < 8 && (!token || user_handles[slot] != token); slot++);
            if (slot == 8)
                return error("Closed or foreign local user handle");
            user_handles[slot] = 0;
        } else {
            if (!hades_memory_writable(registers[0], 24)
                || (index == HADES_HOST_ACCOUNT_OPEN
                    && (!guest_readable(registers[1], 16)
                        || memcmp((const void *)(uintptr_t)registers[1], local_user, 16))))
                return error("Invalid user handle output or foreign user identifier");
            unsigned slot;
            for (slot = 0; slot < 8 && user_handles[slot]; slot++);
            if (slot == 8 || next_user == UINT64_MAX)
                return error("Local user handle capacity exhausted");
            user_handles[slot] = next_user++;
            unsigned char handle[24];
            memcpy(handle, local_user, 16);
            memcpy(handle + 16, &user_handles[slot], 8);
            memcpy((void *)(uintptr_t)registers[0], handle, sizeof handle);
            registers[0] = index == HADES_HOST_ACCOUNT_PRESELECTED ? 1 : 0;
            return 0;
        }
        registers[0] = 0;
        return 0;
    }
    if (index >= HADES_HOST_NETWORK_INITIALIZE && index <= HADES_HOST_NETWORK_ON_HOLD) {
        if (index == HADES_HOST_NETWORK_INITIALIZE) {
            if (network_references == UINT32_MAX)
                return error("Offline network initialization reference count exhausted");
            network_references++;
            fprintf(stderr,"[hades-host] NETWORK_LOCAL_ONLY: registered offline frontend; no Internet capability, no Switch network transfer pool claimed\n");
        } else {
            if (!network_references)
                return error("Network operation before local-only frontend initialization");
            if (index == HADES_HOST_NETWORK_SUBMIT) {
                if (network_request!=NETWORK_DENIED)
                    fprintf(stderr,"[hades-host] network request rejected by local-only policy; no socket opened\n");
                network_request = NETWORK_DENIED;
            } else if (index == HADES_HOST_NETWORK_CANCEL)
                network_request = NETWORK_IDLE;
            else if (index == HADES_HOST_NETWORK_PROXY) {
                registers[0] = 0x4201;
                fprintf(stderr,"[hades-host] proxy settings unavailable in local-only policy; explicit NotImplemented result, output untouched\n");
                return 0;
            } else {
                registers[0] = 0;
                return 0;
            }
        }
        registers[0] = 0;
        return 0;
    }
    if (index >= HADES_HOST_TIME_INITIALIZE && index <= HADES_HOST_TIME_CALENDAR) {
        struct timespec now;
        if (index == HADES_HOST_TIME_INITIALIZE) {
            if (clock_gettime(CLOCK_REALTIME, &now))
                return error("Host wall clock is unavailable");
            time_ready = 1;
            fprintf(stderr, "[hades-host] time backend uses actual host wall clock/local timezone, not a network-calibrated Switch clock\n");
            registers[0] = 0;
        } else {
            if (!time_ready)
                return error("Time interface before initialization");
            if (index == HADES_HOST_TIME_CALENDAR) {
                typedef struct { int16_t year; int8_t month, day, hour, minute, second, padding; } calendar;
                typedef struct { uint32_t week_day, year_day; char zone[8]; uint32_t dst; int32_t offset; } additional;
                _Static_assert(sizeof(calendar) == 8 && sizeof(additional) == 24, "Horizon calendar ABI");
                hades_memory_info source;
                if (!registers[2] || hades_memory_info_query(registers[2], &source) || !(source.permissions & 1)
                    || source.size - (registers[2] - source.base) < 8
                    || (registers[0] && !hades_memory_writable(registers[0], sizeof(calendar)))
                    || (registers[1] && !hades_memory_writable(registers[1], sizeof(additional))))
                    return error("Invalid calendar input or output buffers");
                time_t seconds;
                _Static_assert(sizeof(time_t) == 8, "POSIX time ABI");
                memcpy(&seconds, (const void *)(uintptr_t)registers[2], sizeof seconds);
                struct tm converted;
                if (!localtime_r(&seconds, &converted) || (int64_t)converted.tm_year + 1900 > INT16_MAX
                    || (int64_t)converted.tm_year + 1900 < INT16_MIN || converted.tm_gmtoff > INT32_MAX
                    || converted.tm_gmtoff < INT32_MIN)
                    return error("Host timezone conversion or calendar ABI range failed");
                calendar value = {(int16_t)(converted.tm_year + 1900), (int8_t)(converted.tm_mon + 1),
                                  (int8_t)converted.tm_mday, (int8_t)converted.tm_hour,
                                  (int8_t)converted.tm_min, (int8_t)converted.tm_sec, 0};
                additional details = {.week_day=(uint32_t)converted.tm_wday, .year_day=(uint32_t)converted.tm_yday,
                                      .dst=converted.tm_isdst > 0, .offset=(int32_t)converted.tm_gmtoff};
                if (strlcpy(details.zone, converted.tm_zone ? converted.tm_zone : "UTC", sizeof details.zone)
                    >= sizeof details.zone)
                    return error("Timezone abbreviation exceeds Horizon calendar ABI");
                if (registers[0])
                    memcpy((void *)(uintptr_t)registers[0], &value, sizeof value);
                if (registers[1])
                    memcpy((void *)(uintptr_t)registers[1], &details, sizeof details);
                registers[0] = 0;
            } else {
                if (clock_gettime(CLOCK_REALTIME, &now) || now.tv_sec < 0)
                    return error("Host wall clock query failed");
                if (index == HADES_HOST_TIME_CURRENT) {
                    if (!hades_memory_writable(registers[0], 8))
                        return error("Invalid POSIX time output");
                    *(int64_t *)(uintptr_t)registers[0] = now.tv_sec;
                    registers[0] = 0;
                } else
                    registers[0] = (uint64_t)now.tv_sec;
            }
        }
        return 0;
    }
    if (!fs_ready)
        return error("Filesystem interface before backend initialization");
    if (index == HADES_HOST_FS_SAVE_MOUNT) {
        const char *name = guest_string(registers[0]);
        if (!valid_mount(name) || !strcmp(name, mounted) || save_mount[0]
            || !account_ready || !guest_readable(registers[1], 16)
            || memcmp((const void *)(uintptr_t)registers[1], local_user, 16))
            return error("Invalid/duplicate save mount or foreign local user");
        int directory = local_save_directory();
        if (directory < 0)
            return -1;
        int status = hades_save_open(&save_session, directory);
        if (close(directory))
            status = -1;
        if (status)
            return error("Cannot mount actual transactional local save");
        strlcpy(save_mount, name, sizeof save_mount);
    } else if (index == HADES_HOST_FS_COMMIT) {
        const char *name = guest_string(registers[0]);
        if (!name || !save_mount[0] || strcmp(name, save_mount))
            return error("Commit requires owned transactional save mount");
        if (hades_save_commit(&save_session))
            return -1;
    } else if (index == HADES_HOST_FS_DISABLE_CACHE) {
        memset(&cache, 0, sizeof cache);
    } else if (index == HADES_HOST_FS_ENSURE_SAVE) {
        if (!account_ready || !guest_readable(registers[0], 16)
            || memcmp((const void *)(uintptr_t)registers[0], local_user, 16) || ensure_local_save())
            return error("Save namespace requires an initialized owned local user");
    } else if (index == HADES_HOST_FS_ENABLE_CACHE) {
        if (cache.buffer || !registers[1] || !hades_memory_writable(registers[0], registers[1]))
            return error("Duplicate cache enable or invalid owned cache buffer");
        cache.buffer = registers[0];
        cache.capacity = registers[1];
        fprintf(stderr, "[hades-host] enabled actual immutable RomFS read-window cache with %zu guest-owned bytes\n", cache.capacity);
    } else if (index == HADES_HOST_FS_ROM_CACHE) {
        if (!hades_memory_writable(registers[0], 8))
            return error("Invalid ROM cache-size output");
        *(uint64_t *)(uintptr_t)registers[0] = 4096;
    } else if (index == HADES_HOST_FS_ROM_MOUNT) {
        const char *name = guest_string(registers[0]);
        if (!valid_mount(name) || !strcmp(name, save_mount))
            return error("Invalid RomFS mount name");
        if (mounted[0] && strcmp(mounted, name))
            return error("Multiple RomFS mount aliases not implemented");
        if (mounted[0] || registers[2] != 4096 || !hades_memory_writable(registers[1], 4096))
            return error("RomFS mount requires its actual 4KiB owned metadata/read cache");
        uint64_t header[4] = {0x4841444553434143ull,0,0,0};
        memset((void *)(uintptr_t)registers[1],0,4096);
        memcpy((void *)(uintptr_t)registers[1],header,sizeof header);
        rom_cache=(read_cache){.buffer=registers[1]+32,.capacity=4096-32};
        strlcpy(mounted, name, sizeof mounted);
        fprintf(stderr, "[hades-host] actual read-only directory mounted as %s:/; owned 4KiB metadata/read cache\n", mounted);
    } else if (index == HADES_HOST_FS_UNMOUNT) {
        const char *name = guest_string(registers[0]);
        int save = name && save_mount[0] && !strcmp(name, save_mount);
        if (!name || (!save && (!mounted[0] || strcmp(name, mounted))))
            return error("Unknown mount during unmount");
        for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++)
            if (files[i].id && files[i].save == save)
                return error("Cannot unmount with open guest files");
        for (unsigned i = 0; i < sizeof directories / sizeof directories[0]; i++)
            if (directories[i].id && directories[i].save == save)
                return error("Cannot unmount with open guest directories");
        if (save) {
            if (hades_save_close(&save_session))
                return -1;
            save_mount[0] = 0;
        } else {
            mounted[0] = 0;
            cache.file = 0;
            memset(&rom_cache,0,sizeof rom_cache);
        }
    } else if (index == HADES_HOST_FS_CREATE || index == HADES_HOST_FS_DELETE
               || index == HADES_HOST_FS_CREATE_DIRECTORY) {
        const char *path = guest_string(registers[0]);
        char leaf[256];
        int save = 0;
        if (!path || (index == HADES_HOST_FS_CREATE && (int64_t)registers[1] < 0))
            return error("Invalid mutating filesystem path/size");
        int parent = parent_path(path, leaf, &save);
        if (parent < 0)
            return path_result(registers);
        if (!save || !leaf[0]) {
            close(parent);
            return error("Mutation of readonly ROM or mounted root refused");
        }
        int found = !exact_component(parent, leaf);
        if (!found && errno != ENOENT) {
            close(parent);
            return error("Cannot resolve exact mutation target");
        }
        if (index != HADES_HOST_FS_DELETE && found) {
            if (close(parent))
                return error("Cannot close existing path descriptor");
            registers[0] = 0x402;
            return 0;
        }
        if (index == HADES_HOST_FS_DELETE && !found) {
            if (close(parent))
                return error("Cannot close missing path descriptor");
            registers[0] = 0x202;
            return 0;
        }
        int status = 0;
        if (index == HADES_HOST_FS_CREATE) {
            int fd = openat(parent, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0)
                status = -1;
            else {
                status = ftruncate(fd, (int64_t)registers[1]);
                if (close(fd))
                    status = -1;
            }
        } else if (index == HADES_HOST_FS_CREATE_DIRECTORY)
            status = mkdirat(parent, leaf, 0700);
        else {
            struct stat info;
            status = fstatat(parent, leaf, &info, AT_SYMLINK_NOFOLLOW);
            if (!status)
                status = !S_ISREG(info.st_mode) || info.st_nlink != 1 || unlinkat(parent, leaf, 0);
        }
        if (close(parent))
            status = -1;
        if (status) {
            save_session.poisoned = 1;
            return error("Save mutation failed; transaction poisoned, commit refused");
        }
    } else if (index == HADES_HOST_FS_OPEN_DIRECTORY) {
        const char *path = guest_string(registers[1]);
        if (!path || !hades_memory_writable(registers[0], 8) || !registers[2] || registers[2] > 3)
            return error("Invalid directory path, mode or output");
        unsigned slot;
        for (slot = 0; slot < 32 && directories[slot].id; slot++);
        if (slot == 32 || next_directory == UINT64_MAX)
            return error("Directory handle capacity exhausted");
        if (list_directory(path, (unsigned)registers[2], &directories[slot].save,
                           &directories[slot].entries, &directories[slot].count))
            return path_result(registers);
        directories[slot].id = next_directory++;
        directories[slot].cursor = 0;
        memcpy((void *)(uintptr_t)registers[0], &directories[slot].id, 8);
    } else if (index == HADES_HOST_FS_READ_DIRECTORY || index == HADES_HOST_FS_CLOSE_DIRECTORY) {
        uint64_t handle = index == HADES_HOST_FS_CLOSE_DIRECTORY ? registers[0] : registers[2];
        unsigned slot;
        for (slot = 0; slot < 32 && (!handle || directories[slot].id != handle); slot++);
        if (slot == 32)
            return error("Unknown or closed directory handle");
        if (index == HADES_HOST_FS_CLOSE_DIRECTORY) {
            free(directories[slot].entries);
            memset(&directories[slot], 0, sizeof directories[slot]);
        } else {
            int64_t maximum = (int64_t)registers[3];
            if (maximum < 0 || (uint64_t)maximum > SIZE_MAX / sizeof(directory_entry)
                || !hades_memory_writable(registers[0], 8)
                || (maximum && !hades_memory_writable(registers[1], (size_t)maximum * sizeof(directory_entry))))
                return error("Invalid directory entry capacity or output");
            size_t count = directories[slot].count - directories[slot].cursor;
            if (count > (uint64_t)maximum)
                count = (size_t)maximum;
            if (count)
                memcpy((void *)(uintptr_t)registers[1], directories[slot].entries + directories[slot].cursor,
                       count * sizeof(directory_entry));
            int64_t written = (int64_t)count;
            memcpy((void *)(uintptr_t)registers[0], &written, 8);
            directories[slot].cursor += count;
        }
    } else if (index == HADES_HOST_FS_OPEN || index == HADES_HOST_FS_ENTRY_TYPE) {
        const char *path = guest_string(registers[1]);
        size_t out_size = index == HADES_HOST_FS_OPEN ? 8 : 4;
        if (!path || !hades_memory_writable(registers[0], out_size)
            || (index == HADES_HOST_FS_OPEN && (!(registers[2] & 3) || (registers[2] & ~7ull))))
            return error("Invalid path/output or OpenFile mode");
        int save = 0;
        int mode = index == HADES_HOST_FS_OPEN ? (int)registers[2] : 1;
        int flags = (mode & 3) == 3 ? O_RDWR : (mode & 2) ? O_WRONLY : O_RDONLY;
        int fd = open_guest_path(path, flags, &save);
        if (fd < 0) {
            if (errno == ENOENT || errno == ENOTDIR) {
                fprintf(stderr, "[hades-host] filesystem path not found: %s\n", path);
                registers[0] = 0x202;
                return 0;
            }
            fprintf(stderr,"[hades-host] failed path=%s mode=%d\n",path,mode);
            return error("RomFS path/open failure; traversal and symlinks are not allowed");
        }
        if ((mode & 2) && !save) {
            close(fd);
            return error("Writable ROM file handle refused");
        }
        struct stat information;
        if (fstat(fd, &information) || (!S_ISREG(information.st_mode) && !S_ISDIR(information.st_mode))) {
            close(fd);
            return error("Invalid RomFS file type or failed metadata query");
        }
        if (index == HADES_HOST_FS_ENTRY_TYPE) {
            *(uint32_t *)(uintptr_t)registers[0] = S_ISDIR(information.st_mode) ? 0 : 1;
            if (close(fd))
                return error("Cannot close entry metadata descriptor");
        } else {
            if (!S_ISREG(information.st_mode)) {
                close(fd);
                registers[0] = 0x202;
                return 0;
            }
            unsigned slot;
            for (slot = 0; slot < sizeof files / sizeof files[0] && files[slot].id; slot++);
            if (slot == sizeof files / sizeof files[0] || next_file == UINT64_MAX) {
                close(fd);
                return error("Guest filesystem handle capacity exhausted");
            }
            const hades_ui_overlay *overlay = display_ui_for_path(path, save);
            if (overlay && verify_display_ui(fd,information.st_size,overlay)) {
                close(fd);
                return -1;
            }
            files[slot].fd = fd;
            files[slot].mode = mode;
            files[slot].save = save;
            files[slot].overlay = overlay;
            files[slot].id = next_file++;
            *(uint64_t *)(uintptr_t)registers[0] = files[slot].id;
            fprintf(stderr, "[hades-host] opened actual guest file %s\n", path);
        }
    } else {
        uint64_t handle = index == HADES_HOST_FS_CLOSE || index == HADES_HOST_FS_WRITE || index == HADES_HOST_FS_FLUSH
            ? registers[0] : registers[1];
        unsigned slot;
        for (slot = 0; slot < sizeof files / sizeof files[0] && files[slot].id != handle; slot++);
        if (!handle || slot == sizeof files / sizeof files[0])
            return error("Unknown or closed guest file handle");
        int fd = files[slot].fd;
        if (index == HADES_HOST_FS_CLOSE) {
            if (close(fd))
                return error("Host file close failed");
            files[slot].id = 0;
            if (cache.file == handle)
                cache.file = 0;
            if (rom_cache.file == handle)
                rom_cache.file = 0;
        } else if (index == HADES_HOST_FS_WRITE) {
            int64_t offset = (int64_t)registers[1];
            size_t length = registers[3], written = 0;
            uint32_t option;
            struct stat info;
            if (!files[slot].save || !(files[slot].mode & 2) || offset < 0
                || length > INT64_MAX - (uint64_t)offset
                || (length && !guest_readable(registers[2], length))
                || !guest_readable(registers[4], 4) || fstat(fd, &info)) {
                if (files[slot].save)
                    save_session.poisoned = 1;
                return error("Invalid WriteFile mode, input, option or offset");
            }
            memcpy(&option, (const void *)(uintptr_t)registers[4], 4);
            if (option > 1 || (!(files[slot].mode & 4) && (uint64_t)offset + length > (uint64_t)info.st_size)) {
                save_session.poisoned = 1;
                return error("Unsupported write option or extension without append permission");
            }
            while (written < length) {
                size_t amount = length - written;
                if (amount > 1024*1024)
                    amount = 1024*1024;
                ssize_t count = pwrite(fd, (const unsigned char *)(uintptr_t)registers[2] + written, amount, offset + written);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0) {
                    save_session.poisoned = 1;
                    return error("Actual save write failed; commit will be refused");
                }
                written += (size_t)count;
            }
            if (option && fsync(fd)) {
                save_session.poisoned = 1;
                return error("WriteOption flush failed; transaction poisoned");
            }
        } else if (index == HADES_HOST_FS_FLUSH) {
            if (!files[slot].save || !(files[slot].mode & 2) || fsync(fd)) {
                if (files[slot].save)
                    save_session.poisoned = 1;
                return error("Actual writable save file flush failed");
            }
        } else if (index == HADES_HOST_FS_SIZE) {
            struct stat information;
            if (!hades_memory_writable(registers[0], 8) || fstat(fd, &information))
                return error("Invalid file-size output or fstat failure");
            *(int64_t *)(uintptr_t)registers[0] = files[slot].overlay
                ? (int64_t)files[slot].overlay->size : information.st_size;
        } else if (index == HADES_HOST_FS_READ) {
            int64_t offset = (int64_t)registers[2];
            size_t length = registers[4], got = 0;
            read_cache *selected=cache.buffer ? &cache : &rom_cache;
            if (!(files[slot].mode & 1) || !hades_memory_writable(registers[0], 8) || offset < 0
                || length > INT64_MAX - (uint64_t)offset
                || (length && !hades_memory_writable(registers[3], length)))
                return error("Invalid ReadFile output, offset or writable buffer range");
            if (!files[slot].save) {
                read_cache *owned[] = {&cache,&rom_cache};
                for (unsigned i=0;i<2;i++) {
                    if (!owned[i]->buffer)
                        continue;
                    uintptr_t base=owned[i]->buffer-(i ? 32 : 0);
                    size_t size=owned[i]->capacity+(i ? 32 : 0);
                    if (!hades_memory_writable(base,size)
                        || (length && registers[3]<base+size && base<registers[3]+length)
                        || (registers[0]<base+size && base<registers[0]+8))
                        return error("Cache lost ownership/permission or overlaps read output/count");
                }
            }
            if (files[slot].overlay) {
                const hades_ui_overlay *overlay=files[slot].overlay;
                if ((uint64_t)offset<overlay->size) {
                    got=overlay->size-(size_t)offset;
                    if (got>length)
                        got=length;
                    if (got)
                        memcpy((void *)(uintptr_t)registers[3],overlay->data+(size_t)offset,got);
                }
            } else if (length && !files[slot].save && selected->buffer && length <= selected->capacity) {
                if (selected==&rom_cache) {
                    uint64_t magic;
                    memcpy(&magic,(const void *)(rom_cache.buffer-32),8);
                    if (magic!=0x4841444553434143ull)
                        return error("Opaque ROM cache metadata changed while owned by filesystem");
                }
                if (selected->file == handle && offset >= selected->offset
                    && (uint64_t)(offset - selected->offset) <= selected->length
                    && length <= selected->length - (size_t)(offset - selected->offset)) {
                    memcpy((void *)(uintptr_t)registers[3],
                           (const unsigned char *)selected->buffer + (offset - selected->offset), length);
                    got = length;
                } else {
                    selected->file = 0;
                    if (read_actual(fd, (unsigned char *)selected->buffer, offset, length, &got))
                        return -1;
                    selected->file = handle;
                    selected->offset = offset;
                    selected->length = got;
                    if (selected==&rom_cache) {
                        uint64_t header[4]={0x4841444553434143ull,handle,(uint64_t)offset,got};
                        memcpy((void *)(rom_cache.buffer-32),header,sizeof header);
                    }
                    memcpy((void *)(uintptr_t)registers[3], (const void *)selected->buffer, got);
                }
            } else if (length && read_actual(fd, (unsigned char *)(uintptr_t)registers[3], offset, length, &got))
                return -1;
            *(uint64_t *)(uintptr_t)registers[0] = got;
        } else
            return error("Host file operation has no backend");
    }
    registers[0] = 0;
    return 0;
}

int hades_host_call(unsigned index, uint64_t *registers) {
    if (index >= HADES_HOST_COUNT || !registers)
        return error("Unknown host symbol or missing register frame");
#ifdef HADES_VULKAN_HOST
    if (index >= HADES_HOST_GRAPHICS_INITIALIZE) {
        if (hades_trace_verbose() || !atomic_exchange_explicit(&traced_calls[index],1,memory_order_relaxed))
            fprintf(stderr, "[hades-vulkan] dispatch %s\n", names[index]);
        return hades_vulkan_call(index, names[index], registers);
    }
#endif
    pthread_mutex_lock(&lock);
    if (hades_trace_verbose() || !atomic_exchange_explicit(&traced_calls[index],1,memory_order_relaxed))
        fprintf(stderr, "[hades-host] dispatch %s\n", names[index]);
    int status = call_locked(index, registers);
    pthread_mutex_unlock(&lock);
    return status;
}

void hades_host_dispatch(uint64_t index, uint64_t *registers) {
    if (index >= HADES_HOST_COUNT || hades_host_call((unsigned)index, registers))
        exit(3);
}
