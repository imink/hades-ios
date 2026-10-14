#define _DARWIN_C_SOURCE
#include "ipc.h"
#include "memory.h"
#include "audio.h"
#include "trace.h"
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdatomic.h>

typedef struct {
    uint32_t id;
    unsigned kind;
} domain_object;
typedef struct {
    uint32_t handle;
    int registered;
    unsigned kind;
    int domain;
    uint32_t next_object_id;
    domain_object objects[16];
    uint8_t pointer_buffer[256];
    uint32_t controller_id;
    int signalled;
    int endpoint_closed;
    hades_audio_output *audio;
    uint64_t audio_acknowledged;
} kernel_object;
enum { SM = 1, LM, LOGGER, APM, APM_SESSION, APPLICATION_SERVICE, APPLICATION_PROXY,
       COMMON_STATE, SELF_CONTROLLER, WINDOW_CONTROLLER, AUDIO_CONTROLLER, DISPLAY_CONTROLLER,
       APPLICATION_FUNCTIONS, LIBRARY_APPLET_CREATOR, DEBUG_FUNCTIONS, SUSPEND_EVENT, GPU_ERROR_EVENT,
       APPLET_MESSAGE_EVENT, AOC, AOC_EVENT, TRANSFER_MEMORY, NIFM, NIFM_GENERAL, BSD,
       HID, HID_RESOURCE, SHARED_MEMORY, HID_STYLE_EVENT, AUDOUT, AUDOUT_STREAM, AUDOUT_EVENT };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static kernel_object sessions[64];
static unsigned char traced_headers[32], traced_commands[32][256];
static unsigned traced_waits;
static uint64_t process_id;
static uint32_t next_handle = 0x10000;
static pthread_cond_t event_changed = PTHREAD_COND_INITIALIZER;
static unsigned event_waiters;
static int suspended, tick_changed, gpu_error, focus_message;
static unsigned focus_state=1;
static int foreground_rights;
static uint8_t focus_handling[3];
static uint8_t out_of_focus_suspending;
static uint32_t performance_configuration[2];
static uint32_t display_size[2];
static uint64_t suspension_start, suspended_ns;
static uint32_t termination_result;
static char display_version[16];
static struct { uint32_t transfer, config[8]; uint64_t size; int events[2], monitoring; } bsd_client={.events={-1,-1}};
static struct { uint32_t shared, resource, styles, ids[10]; uint64_t user, joy_hold;
                unsigned revision, id_count; int active; } hid_client;
static int input_frontend;
static struct { uint64_t buttons, sampling; int32_t lx,ly; } input_state;
typedef struct {
    uint64_t sampling, buttons;
    int32_t lx,ly,rx,ry;
    uint32_t attributes,reserved;
} npad_state;
typedef struct { uint64_t sampling; npad_state state; } npad_storage;
typedef struct { uint64_t unused,buffer_count,tail,count; npad_storage storage[17]; } npad_lifo;
_Static_assert(sizeof(npad_state)==0x28,"Public Horizon Npad state size");
_Static_assert(sizeof(npad_lifo)==0x350,"Public Horizon Npad LIFO size");

static int publish_npad(void) {
    if (!hid_client.shared || !input_frontend)
        return 0;
    uint8_t *shared=hades_memory_shared_buffer(hid_client.shared,0x40000);
    if (!shared)
        return -1;
    int supported=0;
    for (unsigned i=0;i<hid_client.id_count;i++)
        supported|=hid_client.ids[i]==0;
    int connected=input_frontend && hid_client.active && (hid_client.styles&1) && supported;
    uint8_t *entry=shared+0x9a00;
    uint32_t style=0;
    memcpy(&style,entry,4);
    if (input_state.sampling==UINT64_MAX)
        return -1;
    npad_state state={.sampling=++input_state.sampling,.buttons=connected ? input_state.buttons : 0,
        .lx=connected ? input_state.lx : 0,.ly=connected ? input_state.ly : 0,.attributes=connected ? 1u : 0u};
    /* Public HID layout: Npad entry 0 at 0x9a00, FullKey LIFO at 0x28,
       SystemExt LIFO after six other 0x350-byte common-state LIFOs. */
    for (unsigned lifo_index=0;lifo_index<2;lifo_index++) {
        npad_lifo *lifo=(npad_lifo *)(entry+0x28+(lifo_index ? 6*0x350 : 0));
        uint64_t tail=__atomic_load_n(&lifo->tail,__ATOMIC_ACQUIRE);
        uint64_t next=(tail+1)%17;
        npad_storage *storage=&lifo->storage[next];
        __atomic_store_n(&storage->sampling,0,__ATOMIC_RELEASE);
        memcpy(&storage->state,&state,sizeof state);
        __atomic_store_n(&storage->sampling,state.sampling,__ATOMIC_RELEASE);
        __atomic_store_n(&lifo->buffer_count,17,__ATOMIC_RELEASE);
        __atomic_store_n(&lifo->tail,next,__ATOMIC_RELEASE);
        uint64_t count=__atomic_load_n(&lifo->count,__ATOMIC_RELAXED);
        __atomic_store_n(&lifo->count,count<16 ? count+1 : 16,__ATOMIC_RELEASE);
    }
    uint32_t new_style=connected ? 1u : 0u;
    __atomic_store_n((uint32_t *)entry,new_style,__ATOMIC_RELEASE);
    if (style!=new_style) {
        for (unsigned i=0;i<64;i++)
            if (sessions[i].handle && sessions[i].kind==HID_STYLE_EVENT && !sessions[i].controller_id)
                sessions[i].signalled=1;
        pthread_cond_broadcast(&event_changed);
        fprintf(stderr,"[hades-input] VIRTUAL_FULLKEY_STYLE=%u; actual configured frontend, not physical Switch hardware\n",new_style);
    }
    return 0;
}

int hades_ipc_configure_input_frontend(void) {
    pthread_mutex_lock(&lock);
    if (input_frontend || process_id) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr,"[hades-input] Frontend registration must precede IPC startup and be unique\n");
        return -1;
    }
    input_frontend=1;
    pthread_mutex_unlock(&lock);
    fprintf(stderr,"[hades-input] Frontend controls registered before guest startup\n");
    return 0;
}

int hades_ipc_publish_input(uint64_t buttons, int32_t lx, int32_t ly) {
    if (buttons&~0xffffull || lx<-32767 || lx>32767 || ly<-32767 || ly>32767) {
        fprintf(stderr,"[hades-input] Invalid supported button mask/analog range\n");
        return -1;
    }
    pthread_mutex_lock(&lock);
    if (!input_frontend) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr,"[hades-input] Input without an owned frontend\n");
        return -1;
    }
    input_state.buttons=buttons;
    input_state.lx=lx;input_state.ly=ly;
    int result=publish_npad();
    pthread_mutex_unlock(&lock);
    return result;
}

static int error(const char *message) {
    fprintf(stderr, "[hades-ipc] %s\n", message);
    return -1;
}

static int close_bsd(void) {
    if (bsd_client.transfer && hades_memory_transfer_release(bsd_client.transfer))
        return -1;
    int status=0;
    for (unsigned i=0;i<2;i++)
        if (bsd_client.events[i]>=0 && close(bsd_client.events[i]))
            status=-1;
    memset(&bsd_client,0,sizeof bsd_client);
    bsd_client.events[0]=bsd_client.events[1]=-1;
    return status ? error("Cannot close owned local BSD monitoring resources") : 0;
}

int hades_ipc_initialize(uint64_t pid) {
    pthread_mutex_lock(&lock);
    if (!pid || process_id) {
        pthread_mutex_unlock(&lock);
        return error("Invalid or already initialized kernel IPC process");
    }
    process_id = pid;
    /* The headless probe starts as a foreground virtual application. Publish
       that initial state once; subsequent invalidations come from the host. */
    focus_message = 1;
    fprintf(stderr, "[hades-ipc] virtual application initial foreground state published\n");
    pthread_mutex_unlock(&lock);
    return 0;
}

int hades_ipc_shutdown(void) {
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < sizeof sessions / sizeof sessions[0]; i++)
        if (sessions[i].handle && (sessions[i].kind == TRANSFER_MEMORY || sessions[i].kind==SHARED_MEMORY)) {
            pthread_mutex_unlock(&lock);
            return error("Cannot discard active transfer/shared-memory handles during IPC shutdown");
        }
    if (event_waiters) {
        pthread_mutex_unlock(&lock);
        return error("Cannot shut down IPC with active kernel event waits");
    }
    for (unsigned i=0;i<64;i++)
        if (sessions[i].handle && (sessions[i].kind==AUDOUT_STREAM || sessions[i].kind==AUDOUT_EVENT)) {
            pthread_mutex_unlock(&lock);
            return error("Audio outputs/events must be closed before IPC shutdown");
        }
    if (close_bsd()) {
        pthread_mutex_unlock(&lock);
        return -1;
    }
    process_id = 0;
    memset(sessions, 0, sizeof sessions);
    suspended = tick_changed = gpu_error = focus_message = 0;
    focus_state=1;
    foreground_rights = 0;
    memset(focus_handling, 0, sizeof focus_handling);
    out_of_focus_suspending = 0;
    memset(performance_configuration, 0, sizeof performance_configuration);
    memset(display_size, 0, sizeof display_size);
    suspension_start = suspended_ns = 0;
    termination_result = 0;
    memset(display_version,0,sizeof display_version);
    memset(&hid_client,0,sizeof hid_client);
    input_frontend=0;
    memset(&input_state,0,sizeof input_state);
    pthread_mutex_unlock(&lock);
    return 0;
}

int hades_ipc_set_focus_state(unsigned state) {
    if (state!=1 && state!=2)
        return error("Background focus requires actual guest-thread suspension, not a focus-only notification");
    pthread_mutex_lock(&lock);
    if (!process_id || suspended || (state==2 && (focus_handling[0] || focus_handling[1]))) {
        pthread_mutex_unlock(&lock);
        return error("Foreground focus transition unavailable or requires unimplemented thread suspension");
    }
    if (focus_state!=state) {
        focus_state=state;
        focus_message=1;
        pthread_cond_broadcast(&event_changed);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

int hades_ipc_set_suspended(int value) {
    if (value != 0 && value != 1)
        return error("Invalid lifecycle transition");
    pthread_mutex_lock(&lock);
    if (!process_id) {
        pthread_mutex_unlock(&lock);
        return error("Lifecycle transition without kernel IPC process");
    }
    if (value && (focus_handling[2] || out_of_focus_suspending)) {
        pthread_mutex_unlock(&lock);
        return error("Selected focus policy requires guest-thread suspension, which is not yet integrated");
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        pthread_mutex_unlock(&lock);
        return error("Unavailable monotonic lifecycle clock");
    }
    uint64_t ns = (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec;
    if (value != suspended) {
        for (unsigned i=0;i<64;i++)
            if (sessions[i].handle && sessions[i].kind==AUDOUT_STREAM
                && hades_audio_suspend(sessions[i].audio,value)) {
                pthread_mutex_unlock(&lock);
                return error("Cannot apply actual audio lifecycle transition");
            }
        if (value)
            suspension_start = ns;
        else {
            if (ns < suspension_start || ns - suspension_start > UINT64_MAX - suspended_ns) {
                pthread_mutex_unlock(&lock);
                return error("Invalid or overflowing suspension clock interval");
            }
            suspended_ns += ns - suspension_start;
            tick_changed = 1;
            pthread_cond_broadcast(&event_changed);
        }
        suspended = value;
        focus_state=value ? 3 : 1;
        focus_message = 1;
        pthread_cond_broadcast(&event_changed);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

int hades_ipc_report_gpu_error(void) {
    pthread_mutex_lock(&lock);
    if (!process_id) {
        pthread_mutex_unlock(&lock);
        return error("GPU failure notification without kernel IPC process");
    }
    gpu_error = 1;
    pthread_cond_broadcast(&event_changed);
    pthread_mutex_unlock(&lock);
    return 0;
}

int hades_ipc_set_display_size(uint32_t width, uint32_t height) {
    if (!width || !height || width>16384 || height>16384)
        return error("Invalid configured host display extent");
    pthread_mutex_lock(&lock);
    display_size[0]=width;
    display_size[1]=height;
    pthread_mutex_unlock(&lock);
    return 0;
}

int hades_ipc_set_display_version(const char *version) {
    if (!version || !version[0] || strnlen(version,sizeof display_version)>=sizeof display_version)
        return error("Invalid extracted application display version");
    pthread_mutex_lock(&lock);
    if (!process_id || display_version[0]) {
        pthread_mutex_unlock(&lock);
        return error("Display version requires a live, not yet configured application");
    }
    memcpy(display_version,version,strlen(version)+1);
    pthread_mutex_unlock(&lock);
    return 0;
}

static kernel_object *find(uint64_t handle) {
    for (size_t i = 0; i < sizeof sessions / sizeof sessions[0]; i++)
        if (sessions[i].handle && sessions[i].handle == handle)
            return &sessions[i];
    return NULL;
}

static kernel_object *allocate(unsigned kind) {
    if (next_handle == UINT32_MAX)
        return NULL;
    for (size_t i = 0; i < sizeof sessions / sizeof sessions[0]; i++)
        if (!sessions[i].handle) {
            sessions[i] = (kernel_object){.handle=next_handle++, .kind=kind};
            return &sessions[i];
        }
    return NULL;
}

static uint32_t word(const uint8_t *data) {
    uint32_t value;
    memcpy(&value, data, sizeof value);
    return value;
}

static int accessible(uintptr_t address, size_t size, unsigned permission) {
    hades_memory_info info;
    return size && !hades_memory_info_query(address,&info) && (info.permissions&permission)==permission
        && address>=info.base && size<=info.size-(address-info.base);
}

static int audio_buffer_descriptor(const uint8_t *descriptor, uintptr_t *address, size_t *size,
                                   unsigned permission) {
    uint32_t high=word(descriptor+8);
    *address=word(descriptor+4) | ((uint64_t)(high>>28)<<32)
        | ((uint64_t)((high>>2)&0x3fffff)<<36);
    *size=word(descriptor) | ((uint64_t)((high>>24)&15)<<32);
    if (!*address && !*size)
        return 0;
    if ((high&3)==2 || *size>4*1024*1024 || !accessible(*address,*size,permission))
        return error("Invalid audio MapAlias mode/address/permissions/extent");
    return 0;
}

static int reply(uint8_t *message, size_t capacity, uint32_t token, int domain,
                 uint32_t transferred_handle, uint32_t object_id, const void *data, size_t size, int copy_handle) {
    size_t raw_size = 32 + (domain ? 16 : 0) + size + (object_id ? 4 : 0);
    size_t raw_words = (raw_size + 3) / 4, end = (transferred_handle ? 16 : 8) + raw_words * 4;
    if (end > capacity)
        return error("IPC response would exceed current-thread buffer");
    memset(message, 0, capacity);
    uint32_t header[4] = {0, (uint32_t)raw_words, 0, 0};
    if (transferred_handle) {
        header[1] |= 0x80000000;
        header[2] = copy_handle ? 1u << 1 : 1u << 5;
        header[3] = transferred_handle;
    }
    memcpy(message, header, sizeof header);
    size_t cmif = domain ? 32 : 16;
    if (domain) {
        uint32_t count = object_id ? 1 : 0;
        memcpy(message + 16, &count, 4);
    }
    uint32_t output[4] = {0x4F434653, 0, 0, token};
    memcpy(message + cmif, output, sizeof output);
    if (size)
        memcpy(message + cmif + 16, data, size);
    if (object_id)
        memcpy(message + cmif + 16 + size, &object_id, 4);
    return 1;
}

static int dispatch(kernel_object *session, uint8_t *message, size_t capacity) {
    if (capacity != 256 || (uintptr_t)message % 16)
        return error("Invalid current-thread IPC buffer extent");
    uint32_t first = word(message), second = word(message + 4);
    unsigned type = first & 0xFFFF, raw_words = second & 0x3FF;
    unsigned input_pointers=(first>>16)&15;
    unsigned sends=(first>>20)&15, receives=(first>>24)&15, exchanges=first>>28;
    unsigned receive_mode=(second>>10)&15;
    int audio=session->kind==AUDOUT || session->kind==AUDOUT_STREAM;
    if (session->endpoint_closed)
        return error("Request on a closed service endpoint");
    if (first==2 && !second && audio) {
        if (session->kind==AUDOUT_STREAM) {
            for (unsigned i=0;i<64;i++)
                if (sessions[i].handle && sessions[i].kind==AUDOUT_EVENT && sessions[i].audio==session->audio)
                    return error("Close audio release event before its endpoint");
            if (hades_audio_close(session->audio))
                return -1;
            session->audio=NULL;
        }
        session->endpoint_closed=1;
        memset(message,0,capacity);
        return 2;
    }
    if (hades_trace_verbose() || session->kind>=32 || !traced_headers[session->kind]) {
        fprintf(stderr, "[hades-ipc] request kind=%u header=%#x/%#x\n", session->kind, first, second);
        if (session->kind<32)
            traced_headers[session->kind]=1;
    }
    if ((type != 4 && type != 5 && type != 6 && type != 7)
        || exchanges || sends>1 || receives>1 || input_pointers>1
        || ((sends || receives || receive_mode) && !audio)
        || (input_pointers && session->kind!=HID && !audio)
        || (receive_mode && receive_mode!=2 && receive_mode!=3)
        || (second & 0x7FFFC000))
        return error("Unsupported HIPC type or buffer descriptors");
    size_t offset = 8;
    int send_pid = 0;
    int copied_process = 0;
    uint32_t copied_transfer = 0;
    if (second >> 31) {
        if (capacity < 12)
            return error("Truncated HIPC special header");
        uint32_t special = word(message + offset);
        unsigned copies = (special >> 1) & 15;
        if ((special & ~31u) || copies > 1)
            return error("Unsupported HIPC handle descriptors");
        send_pid = special & 1;
        offset += 4;
        if (send_pid)
            offset += 8;
        if (copies) {
            if (offset > capacity - 4)
                return error("Truncated HIPC copy capability");
            uint32_t handle=word(message+offset);
            kernel_object *capability=find(handle);
            if (handle==0xFFFF8001)
                copied_process=1;
            else if (session->kind==BSD && capability && capability->kind==TRANSFER_MEMORY)
                copied_transfer=handle;
            else
                return error("HIPC copy handle is not an authorized kernel process/transfer capability");
            offset += 4;
        }
    }
    size_t pointer_size=0;
    if (input_pointers) {
        if (offset>capacity-8)
            return error("Truncated HID input pointer descriptor");
        uint32_t descriptor=word(message+offset);
        uintptr_t address=word(message+offset+4) | ((uint64_t)((descriptor>>12)&15)<<32)
            | ((uint64_t)((descriptor>>6)&7)<<36);
        pointer_size=descriptor>>16;
        hades_memory_info info;
        if (!audio || address || pointer_size) {
        if ((descriptor&0xe3fu) || !pointer_size || pointer_size>sizeof session->pointer_buffer
            || hades_memory_info_query(address,&info) || !(info.permissions&1)
            || address<info.base || pointer_size>info.size-(address-info.base))
            return error("Invalid HID input pointer index, address or owned buffer extent");
        memcpy(session->pointer_buffer,(const void *)address,pointer_size);
        }
        offset+=8;
    }
    uintptr_t input_address=0, output_address=0;
    size_t input_size=0, output_size=0;
    if (offset>capacity || (sends+receives)>(capacity-offset)/12)
        return error("Truncated audio MapAlias descriptors");
    if (sends) {
        if (audio_buffer_descriptor(message+offset,&input_address,&input_size,1))
            return -1;
        offset+=12;
    }
    if (receives) {
        if (audio_buffer_descriptor(message+offset,&output_address,&output_size,2))
            return -1;
        offset+=12;
    }
    if (offset > capacity || raw_words > (capacity - offset) / 4)
        return error("HIPC raw data is outside current-thread buffer");
    size_t end = offset + raw_words * 4, cmif = (offset + 15) & ~(size_t)15;
    if (receive_mode) {
        if (end>capacity-8)
            return error("Truncated audio receive-static descriptor");
        uintptr_t address=word(message+end) | ((uint64_t)(word(message+end+4)&0xffff)<<32);
        size_t size=word(message+end+4)>>16;
        if (address || size) {
            if (output_address || !accessible(address,size,2))
                return error("Ambiguous/invalid audio receive-static output buffer");
            output_address=address;
            output_size=size;
        }
    }
    int domain = session->domain && (type == 4 || type == 6);
    unsigned kind = session->kind;
    if (domain) {
        if (cmif > end || end - cmif < 16)
            return error("Truncated domain request header");
        unsigned domain_type = message[cmif], input_objects = message[cmif + 1];
        uint16_t data_size;
        memcpy(&data_size, message + cmif + 2, 2);
        uint32_t id = word(message + cmif + 4);
        domain_object *object = NULL;
        for (size_t i = 0; i < sizeof session->objects / sizeof session->objects[0]; i++)
            if (session->objects[i].id == id && id) {
                object = &session->objects[i];
                break;
            }
        if (!object || input_objects || word(message + cmif + 8)
            || data_size > end - cmif - 16)
            return error("Invalid domain object, inputs or data extent");
        if (domain_type == 2 && !data_size) {
            memset(object, 0, sizeof *object);
            return reply(message, capacity, 0, 1, 0, 0, NULL, 0, 0);
        }
        if (domain_type != 1 || data_size < 16)
            return error("Unsupported domain request type");
        kind = object->kind;
        cmif += 16;
        end = cmif + data_size;
    }
    if (hades_trace_verbose() && cmif <= end && end - cmif >= 16)
        fprintf(stderr, "[hades-ipc] CMIF offset=%zu end=%zu magic=%#x version=%u command=%u token=%u\n",
                cmif, end, word(message + cmif), word(message + cmif + 4),
                word(message + cmif + 8), word(message + cmif + 12));
    if (cmif > end || end - cmif < 16 || word(message + cmif) != 0x49434653
        || word(message + cmif + 4) > (type >= 6 ? 1u : 0u)) {
        fprintf(stderr, "[hades-ipc] special header=%#x words12=%#x words16=%#x words20=%#x\n",
                word(message + 8), word(message + 12), word(message + 16), word(message + 20));
        return error("Invalid or unsupported CMIF request header");
    }
    unsigned command = word(message + cmif + 8);
    if (hades_trace_verbose() || kind>=32 || command>=256 || !traced_commands[kind][command]) {
        fprintf(stderr,"[hades-ipc] service kind=%u command=%u\n",kind,command);
        if (kind<32 && command<256)
            traced_commands[kind][command]=1;
    }
    if (input_pointers && !audio && (kind!=HID || command!=102 || (type!=4 && type!=6)))
        return error("HID input pointers are only implemented for supported Npad IDs");
    uint32_t token = word(message + cmif + 12);
    const uint8_t *parameters = message + cmif + 16;
    size_t parameter_size = end - cmif - 16;
    if (audio) {
        if (domain || (input_address && pointer_size)
            || (output_address && output_address<(uintptr_t)message+capacity
                && (uintptr_t)message<output_address+output_size))
            return error("Unsupported audio domain, ambiguous input or IPC-overlapping output");
        if (pointer_size) {
            input_address=(uintptr_t)session->pointer_buffer;
            input_size=pointer_size;
        }
        if (kind==AUDOUT && (type==4 || type==6) && (command==1 || command==3)) {
            uint64_t user=0;
            if (parameter_size>=16)
                memcpy(&user,parameters+8,8);
            unsigned rate=parameter_size>=4 ? word(parameters) : 0;
            unsigned channels=parameter_size>=8 ? word(parameters+4)&0xffff : 0;
            if (!rate) rate=48000;
            if (!channels) channels=2;
            fprintf(stderr,"[hades-ipc] OpenAudioOut rate=%u channels=%u packed=%#x\n",
                    rate,channels,parameter_size>=8 ? word(parameters+4) : 0);
            if (!send_pid || !copied_process || parameter_size<16 || user!=process_id
                || !input_address || input_size>256 || output_size<256
                || !memchr((void *)input_address,0,input_size)
                || (((char *)input_address)[0] && strcmp((char *)input_address,"DeviceOut")))
                return error("Invalid owned AudioOut caller, device name or parameters");
            kernel_object *stream=allocate(AUDOUT_STREAM);
            if (!stream)
                return error("Audio output handle capacity exhausted");
            if (hades_audio_open(rate,channels,&stream->audio)
                || (suspended && hades_audio_suspend(stream->audio,1))) {
                if (stream->audio && hades_audio_close(stream->audio))
                    return error("Cannot roll back output initialization");
                memset(stream,0,sizeof *stream);
                return -1;
            }
            memset((void *)output_address,0,output_size);
            memcpy((void *)output_address,"DeviceOut",10);
            uint32_t result[4]={rate,channels,2,1};
            return reply(message,capacity,token,0,stream->handle,0,result,sizeof result,0);
        }
        if (kind==AUDOUT && (type==4 || type==6) && (command==0 || command==2) && output_size>=256 && !input_address) {
            memset((void *)output_address,0,output_size);
            memcpy((void *)output_address,"DeviceOut",10);
            uint32_t count=1;
            return reply(message,capacity,token,0,0,0,&count,4,0);
        }
        if (kind==AUDOUT_STREAM && (type==4 || type==6)) {
            if (command==3 || command==7) {
                uint64_t tag=0, buffer[5]={0};
                if (parameter_size<8 || input_size!=sizeof buffer || output_address)
                    return error("Invalid AudioOut append request shape");
                memcpy(&tag,parameters,8);
                memcpy(buffer,(void *)input_address,sizeof buffer);
                if (buffer[4]>buffer[2] || buffer[3]>buffer[2]-buffer[4]
                    || buffer[1]>UINTPTR_MAX-buffer[4] || !accessible(buffer[1]+buffer[4],buffer[3],1)
                    || hades_audio_append(session->audio,tag,(void *)(uintptr_t)(buffer[1]+buffer[4]),buffer[3]))
                    return error("Cannot append actual readable PCM snapshot");
            } else if (command==5 || command==8) {
                if (input_address || !output_size || output_size%8 || output_size>64*8)
                    return error("Invalid released audio tag output extent");
                uint64_t tags[64];
                uint32_t count=hades_audio_release(session->audio,tags,(unsigned)(output_size/8));
                memset((void *)output_address,0,output_size);
                memcpy((void *)output_address,tags,count*8);
                return reply(message,capacity,token,0,0,0,&count,4,0);
            } else {
                if (input_address || output_address)
                    return error("Unexpected descriptors on scalar AudioOut request");
                if (command==1 || command==2) {
                    if (hades_audio_start(session->audio,command==1))
                        return -1;
                } else if (command==4) {
                    for (unsigned i=0;i<64;i++)
                        if (sessions[i].handle && sessions[i].kind==AUDOUT_EVENT && sessions[i].audio==session->audio)
                            return error("Duplicate audio release event acquisition");
                    kernel_object *event=allocate(AUDOUT_EVENT);
                    if (!event) return error("Audio release event capacity exhausted");
                    event->audio=session->audio;
                    event->audio_acknowledged=hades_audio_generation(session->audio);
                    return reply(message,capacity,token,0,event->handle,0,NULL,0,1);
                } else if (command==0 || command==6 || command==9) {
                    uint64_t tag=0;
                    if (command==6) {
                        if (parameter_size<8) return error("Truncated audio ContainsBuffer tag");
                        memcpy(&tag,parameters,8);
                    }
                    uint32_t value=command==0 ? hades_audio_state(session->audio)
                        : command==9 ? hades_audio_count(session->audio) : hades_audio_contains(session->audio,tag);
                    return reply(message,capacity,token,0,0,0,&value,command==6 ? 1 : 4,0);
                } else if (command==10) {
                    uint64_t samples=hades_audio_samples(session->audio);
                    return reply(message,capacity,token,0,0,0,&samples,8,0);
                } else if (command==12 || command==13) {
                    float volume=0;
                    if (command==12) {
                        if (parameter_size<4) return error("Truncated AudioOut volume");
                        memcpy(&volume,parameters,4);
                    }
                    if (hades_audio_volume(session->audio,&volume,command==12)) return -1;
                    if (command==13) return reply(message,capacity,token,0,0,0,&volume,4,0);
                } else {
                    fprintf(stderr,"[hades-ipc] AudioOut command=%u is not implemented\n",command);
                    return 0;
                }
            }
            return reply(message,capacity,token,0,0,0,NULL,0,0);
        }
        if (type!=5 && type!=7) {
            fprintf(stderr,"[hades-ipc] AudioOut manager command=%u is not implemented\n",command);
            return 0;
        }
    }
    uint32_t moved_handle = 0, object_id = 0;
    if (type == 5 || type == 7) {
        if (command == 0 && !session->domain && session->kind != SM && !audio) {
            uint32_t id = 1;
            int result = reply(message, capacity, token, 0, 0, 0, &id, sizeof id, 0);
            if (result > 0) {
                session->domain = 1;
                session->next_object_id = 2;
                session->objects[0] = (domain_object){.id=id, .kind=session->kind};
                fprintf(stderr, "[hades-ipc] session converted to domain root=1\n");
            }
            return result;
        } else if ((command==2 || command==4) && session->kind==BSD) {
            if (command==4 && parameter_size<4)
                return error("Truncated BSD session clone tag");
            kernel_object *clone=allocate(BSD);
            if (!clone)
                return error("BSD session clone capacity exhausted");
            uint32_t cloned_handle=clone->handle;
            *clone=*session;
            clone->handle=cloned_handle;
            return reply(message,capacity,token,0,clone->handle,0,NULL,0,0);
        } else if (command == 3) {
            uint16_t size = session->kind==HID ? sizeof session->pointer_buffer : 0;
            fprintf(stderr, "[hades-ipc] QueryPointerBufferSize: %u owned receive bytes\n",size);
            return reply(message, capacity, token, 0, 0, 0, &size, sizeof size, 0);
        } else {
            fprintf(stderr, "[hades-ipc] unsupported session control command %u\n", command);
            return 0;
        }
    } else if (kind == SM && command == 0) {
        if (!send_pid || parameter_size < 8 || session->registered)
            return error("Invalid SM process registration");
        /* SendPid identifies the kernel caller, not client-provided PID bytes. */
        session->registered = 1;
        fprintf(stderr, "[hades-ipc] sm registered kernel process %llu\n", (unsigned long long)process_id);
    } else if (kind == SM && command == 1) {
        if (!session->registered || parameter_size < 8)
            return error("SM service lookup requires a registered client");
        char service[9];
        memcpy(service, parameters, 8);
        service[8] = 0;
        if (!service[0])
            return error("Empty SM service name");
        for (size_t i = 0; i < 8 && service[i]; i++)
            if (!((service[i] >= 'a' && service[i] <= 'z') || (service[i] >= 'A' && service[i] <= 'Z')
                || (service[i] >= '0' && service[i] <= '9') || service[i] == ':' || service[i] == '-'))
                return error("Invalid SM service name");
        if (!strcmp(service, "lm") || !strcmp(service, "apm")
            || !strcmp(service, "appletOE") || !strcmp(service, "aoc:u") || !strcmp(service, "nifm:u")
            || !strcmp(service,"bsd:u") || !strcmp(service,"hid") || !strcmp(service,"audout:u")) {
            unsigned service_kind = !strcmp(service, "lm") ? LM : !strcmp(service, "apm") ? APM
                : !strcmp(service, "aoc:u") ? AOC : !strcmp(service, "nifm:u") ? NIFM
                : !strcmp(service,"bsd:u") ? BSD : !strcmp(service,"hid") ? HID
                : !strcmp(service,"audout:u") ? AUDOUT : APPLICATION_SERVICE;
            kernel_object *object = allocate(service_kind);
            if (!object)
                return error("Service session handle capacity exhausted");
            moved_handle = object->handle;
            fprintf(stderr, "[hades-ipc] service %s bound to host endpoint kind=%u\n", service, service_kind);
        } else {
            fprintf(stderr, "[hades-ipc] required service: %s; backend is unimplemented\n", service);
            return 0;
        }
    } else if (kind==HID && command==0) {
        if (!send_pid || parameter_size<8)
            return error("HID applet resource requires kernel caller identity");
        if (domain)
            return error("HID domain applet resources are not yet implemented");
        uint64_t user;
        memcpy(&user,parameters,8);
        if (hid_client.resource || user!=process_id)
            return error("HID frontend currently owns only one applet resource publisher");
        kernel_object *resource=allocate(HID_RESOURCE);
        if (!resource)
            return error("HID applet resource capacity exhausted");
        moved_handle=resource->handle;
        hid_client.resource=resource->handle;
        hid_client.user=user;
    } else if (kind==HID_RESOURCE && command==0) {
        if (session->registered)
            return error("Duplicate HID shared-memory capability acquisition is not yet supported");
        kernel_object *memory=allocate(SHARED_MEMORY);
        if (!memory || hades_memory_shared_create(memory->handle,0x40000)) {
            if (memory)
                memset(memory,0,sizeof *memory);
            return error("Cannot allocate actual HID read-only shared-memory backend");
        }
        session->registered=1;
        hid_client.shared=memory->handle;
        if (publish_npad())
            return error("Cannot initialize actual Npad publisher");
        fprintf(stderr,"[hades-ipc] HID_SHARED_MEMORY: owned publisher; virtual connection requires registered frontend and supported configuration\n");
        return reply(message,capacity,token,domain,memory->handle,0,NULL,0,1);
    } else if (kind==HID && command==109) {
        uint64_t user=0;
        unsigned revision=word(parameters);
        if (parameter_size>=16)
            memcpy(&user,parameters+8,8);
        fprintf(stderr,"[hades-ipc] HID ActivateNpad revision=%u user=%llu\n",revision,(unsigned long long)user);
        if (!send_pid || parameter_size<16 || !hid_client.shared || user!=hid_client.user
            || revision>3 || hid_client.active)
            return error("Unsupported/invalid HID Npad activation or resource identity");
        hid_client.revision=revision;
        hid_client.active=1;
        if (publish_npad())
            return error("Cannot publish Npad activation");
    } else if (kind==HID && command==100) {
        uint64_t user=0;
        uint32_t styles=word(parameters);
        if (parameter_size>=16)
            memcpy(&user,parameters+8,8);
        if (!send_pid || parameter_size<16 || !hid_client.active || user!=hid_client.user
            || (styles&~0x7fu))
            return error("Unsupported/invalid owned HID supported-style configuration");
        hid_client.styles=styles;
        if (publish_npad())
            return error("Cannot publish supported Npad styles");
        fprintf(stderr,"[hades-ipc] HID_SUPPORTED_STYLES=%#x\n",styles);
    } else if (kind==HID && (command==101 || command==121)) {
        uint64_t user=0;
        if (parameter_size>=8)
            memcpy(&user,parameters,8);
        if (!send_pid || parameter_size<8 || user!=hid_client.user || !hid_client.active)
            return error("Invalid owned Npad configuration query identity");
        return command==101 ? reply(message,capacity,token,domain,0,0,&hid_client.styles,4,0)
            : reply(message,capacity,token,domain,0,0,&hid_client.joy_hold,8,0);
    } else if (kind==HID && command==120) {
        uint64_t values[2]={0,0};
        if (parameter_size>=sizeof values)
            memcpy(values,parameters,sizeof values);
        if (!send_pid || parameter_size<sizeof values || !hid_client.active
            || values[0]!=hid_client.user || values[1]>1)
            return error("Invalid owned HID Joy-Con hold configuration");
        hid_client.joy_hold=values[1];
        fprintf(stderr,"[hades-ipc] HID_JOY_HOLD=%llu; no Joy-Con hardware claimed\n",(unsigned long long)values[1]);
    } else if (kind==HID && command==102) {
        uint64_t user=0;
        if (parameter_size>=8)
            memcpy(&user,parameters,8);
        if (!send_pid || parameter_size<8 || user!=hid_client.user || !hid_client.active
            || !pointer_size || pointer_size%4 || pointer_size>sizeof hid_client.ids)
            return error("Invalid owned HID supported-controller ID array");
        uint32_t ids[10];
        memcpy(ids,session->pointer_buffer,pointer_size);
        unsigned count=(unsigned)(pointer_size/4);
        for (unsigned i=0;i<count;i++) {
            if (ids[i]>7 && ids[i]!=0x20)
                return error("Unsupported HID controller ID");
            for (unsigned j=0;j<i;j++)
                if (ids[i]==ids[j])
                    return error("Duplicate HID controller ID");
        }
        memcpy(hid_client.ids,ids,pointer_size);
        hid_client.id_count=count;
        if (publish_npad())
            return error("Cannot publish supported Npad IDs");
        fprintf(stderr,"[hades-ipc] HID_SUPPORTED_IDS=%u; owned receive buffer\n",count);
    } else if (kind==HID && command==106) {
        uint64_t user=0;
        uint32_t id=word(parameters);
        if (parameter_size>=24)
            memcpy(&user,parameters+8,8);
        int supported=0;
        for (unsigned i=0;i<hid_client.id_count;i++)
            supported|=hid_client.ids[i]==id;
        if (!send_pid || parameter_size<24 || !hid_client.active || user!=hid_client.user || !supported)
            return error("Invalid owned HID controller style-event subscription");
        kernel_object *event=allocate(HID_STYLE_EVENT);
        if (!event)
            return error("HID style-event handle capacity exhausted");
        event->controller_id=id;
        event->signalled=1;
        fprintf(stderr,"[hades-ipc] HID_STYLE_EVENT id=%u; initial configured frontend state published\n",id);
        return reply(message,capacity,token,domain,event->handle,0,NULL,0,1);
    } else if (kind==HID && command==205) {
        uint8_t permitted=0;
        fprintf(stderr,"[hades-ipc] Vibration permitted=0: keyboard/touch virtual controller has no rumble actuator\n");
        return reply(message,capacity,token,domain,0,0,&permitted,1,0);
    } else if (kind==BSD && command==0) {
        if (!send_pid || !copied_transfer || parameter_size<48 || bsd_client.transfer)
            return error("BSD registration requires one real transfer pool and kernel caller PID");
        uint64_t size;
        memcpy(&size,parameters+40,8);
        uint32_t config[8];
        memcpy(config,parameters,sizeof config);
        uint64_t sum=(config[3] ? config[3] : config[1])+(uint64_t)(config[4] ? config[4] : config[2])
            +config[5]+(uint64_t)config[6];
        uint64_t minimum=((sum+4095)&~4095ull)*config[7];
        void *pool=hades_memory_transfer_buffer(copied_transfer,size);
        fprintf(stderr,"[hades-ipc] BSD pool version=%u efficiency=%u size=%llu minimum=%llu\n",
                config[0],config[7],(unsigned long long)size,(unsigned long long)minimum);
        if (!pool || (config[0]!=1 && config[0]!=7) || !config[7] || config[7]>32 || size<minimum || size<64)
            return error("Invalid/undersized owned BSD pool configuration");
        int events[2];
        if (pipe(events))
            return error("Cannot create actual local BSD monitor pipe");
        int failed=0;
        for (unsigned i=0;i<2;i++)
            if (fcntl(events[i],F_SETFL,O_NONBLOCK) || fcntl(events[i],F_SETFD,FD_CLOEXEC))
                failed=1;
        if (failed || hades_memory_transfer_retain(copied_transfer)) {
            close(events[0]);close(events[1]);
            return error("Cannot retain BSD monitoring/pool resources");
        }
        bsd_client.transfer=copied_transfer;
        bsd_client.size=size;
        memcpy(bsd_client.config,config,sizeof config);
        memcpy(bsd_client.events,events,sizeof events);
        uint64_t header[8]={0x4841444553425344ull,process_id,size,minimum,0,0,0,0};
        memcpy(pool,header,sizeof header);
        session->registered=1;
        fprintf(stderr,"[hades-ipc] BSD_LOCAL_REGISTERED: live protected pool and real local monitor resources; Internet operations not enabled\n");
        if (config[0]==7) {
            /* This SDK consumes a signed registration status, not the v1 PID. */
            int32_t registered=0;
            return reply(message,capacity,token,domain,0,0,&registered,sizeof registered,0);
        }
        return reply(message,capacity,token,domain,0,0,&process_id,sizeof process_id,0);
    } else if (kind==BSD && command==1) {
        uint64_t client=0;
        if (parameter_size>=8)
            memcpy(&client,parameters,8);
        if (!send_pid || parameter_size<8 || !bsd_client.transfer
            || client!=(bsd_client.config[0]==7 ? 0 : process_id) || bsd_client.monitoring)
            return error("Invalid/duplicate BSD process monitoring subscription");
        unsigned char alive=1;
        if (write(bsd_client.events[1],&alive,1)!=1)
            return error("Cannot publish local BSD process monitoring state");
        bsd_client.monitoring=1;
        fprintf(stderr,"[hades-ipc] BSD_LOCAL_MONITORING: subscribed to current owned process\n");
    } else if (kind==BSD && (command==2 || command==3)) {
        if (!bsd_client.transfer || parameter_size<12)
            return error("Socket request before BSD registration or truncated arguments");
        int32_t offline[2]={-1,50};
        fprintf(stderr,"[hades-ipc] BSD Internet socket creation denied by explicit local-only policy: ENETDOWN\n");
        return reply(message,capacity,token,domain,0,0,offline,sizeof offline,0);
    } else if (((kind == LM || kind == APM || kind == APPLICATION_SERVICE) && command == 0)
               || (kind == NIFM && command == 5)
               || (kind == APPLICATION_PROXY && (command <= 4 || command == 11
                                                  || command == 20 || command == 1000))) {
        if (kind == LM && (!send_pid || parameter_size < 8))
            return error("LM OpenLogger requires kernel caller PID");
        if (kind == APPLICATION_SERVICE && (!send_pid || !copied_process || parameter_size < 8))
            return error("OpenApplicationProxy requires kernel PID and copied process capability");
        if (kind == NIFM && (!send_pid || parameter_size < 8))
            return error("CreateGeneralService requires the kernel caller PID");
        unsigned child_kind = kind == LM ? LOGGER : kind == APM ? APM_SESSION
            : kind == APPLICATION_SERVICE ? APPLICATION_PROXY : kind == NIFM ? NIFM_GENERAL
            : command == 1000 ? DEBUG_FUNCTIONS
            : command == 20 ? APPLICATION_FUNCTIONS : command == 11 ? LIBRARY_APPLET_CREATOR
            : COMMON_STATE + command;
        if (domain) {
            domain_object *object = NULL;
            for (size_t i = 0; i < sizeof session->objects / sizeof session->objects[0]; i++)
                if (!session->objects[i].id) {
                    object = &session->objects[i];
                    break;
                }
            if (!object || session->next_object_id == UINT32_MAX)
                return error("Domain child object capacity exhausted");
            object_id = session->next_object_id++;
            *object = (domain_object){.id=object_id, .kind=child_kind};
        } else {
            kernel_object *object = allocate(child_kind);
            if (!object)
                return error("Child session handle capacity exhausted");
            moved_handle = object->handle;
        }
        fprintf(stderr, "[hades-ipc] child object kind=%u created for kernel process %llu\n",
                child_kind, (unsigned long long)process_id);
    } else if (kind==APPLICATION_FUNCTIONS && command==22) {
        if (parameter_size<4)
            return error("Truncated application termination result");
        termination_result=word(parameters);
        fprintf(stderr,"[hades-ipc] application termination result recorded=%#x\n",termination_result);
    } else if (kind == APM && command == 1) {
        /* Explicit virtual handheld/normal mode. No Switch clock control or
           unimplemented performance-configuration setter is claimed. */
        uint32_t normal_mode = 0;
        fprintf(stderr, "[hades-ipc] APM virtual policy: normal mode, no guest-controlled clock changes\n");
        return reply(message, capacity, token, domain, 0, 0, &normal_mode, sizeof normal_mode, 0);
    } else if (kind == WINDOW_CONTROLLER && command == 1) {
        fprintf(stderr, "[hades-ipc] virtual application resource ID is bound to kernel process\n");
        return reply(message, capacity, token, domain, 0, 0, &process_id, sizeof process_id, 0);
    } else if (kind == WINDOW_CONTROLLER && command == 10) {
        if (suspended)
            return error("Cannot acquire virtual foreground rights while suspended");
        foreground_rights = 1;
        fprintf(stderr, "[hades-ipc] foreground rights assigned to the single virtual application; no display integration claimed\n");
    } else if (kind == SELF_CONTROLLER && command == 13) {
        if (parameter_size < 3 || parameters[0] > 1 || parameters[1] > 1 || parameters[2] > 1)
            return error("Invalid focus-handling flags");
        fprintf(stderr, "[hades-ipc] focus policy request=%u/%u/%u\n",
                parameters[0], parameters[1], parameters[2]);
        memcpy(focus_handling, parameters, 3);
        fprintf(stderr, "[hades-ipc] focus policy stored; foreground-only bootstrap, required thread suspension transitions explicitly rejected\n");
    } else if (kind == SELF_CONTROLLER && command == 16) {
        if (parameter_size < 1 || parameters[0] > 1)
            return error("Invalid out-of-focus suspension flag");
        out_of_focus_suspending = parameters[0];
        fprintf(stderr, "[hades-ipc] out-of-focus suspension policy stored=%u\n", out_of_focus_suspending);
    } else if ((kind == SELF_CONTROLLER && command == 91)
               || (kind == APPLICATION_FUNCTIONS && command == 130)
               || (kind == COMMON_STATE && command == 0)) {
        unsigned event_kind = kind == SELF_CONTROLLER ? SUSPEND_EVENT
            : kind == COMMON_STATE ? APPLET_MESSAGE_EVENT : GPU_ERROR_EVENT;
        kernel_object *event = allocate(event_kind);
        if (!event)
            return error("Suspension event handle capacity exhausted");
        fprintf(stderr, "[hades-ipc] event reader kind=%u created; host-source signaling, no fabricated notification\n",
                event_kind);
        return reply(message, capacity, token, domain, event->handle, 0, NULL, 0, 1);
    } else if (kind == SELF_CONTROLLER && command == 90) {
        uint64_t ticks = (uint64_t)((__uint128_t)suspended_ns * 19200000 / 1000000000);
        return reply(message, capacity, token, domain, 0, 0, &ticks, sizeof ticks, 0);
    } else if (kind == COMMON_STATE && command == 1) {
        uint32_t notification = 15;
        int result = reply(message, capacity, token, domain, 0, 0, &notification, sizeof notification, 0);
        if (!focus_message) {
            uint32_t no_message = 0x680;
            memcpy(message + (domain ? 40 : 24), &no_message, 4);
            fprintf(stderr, "[hades-ipc] ReceiveMessage: no pending host lifecycle notification\n");
        } else
            focus_message = 0;
        return result;
    } else if (kind==APPLICATION_FUNCTIONS && command==21) {
        if (!display_version[0])
            return error("Display version requested before extracted NACP metadata configuration");
        return reply(message,capacity,token,domain,0,0,display_version,sizeof display_version,0);
    } else if (kind == COMMON_STATE && command == 66) {
        if (!display_size[0] || !display_size[1])
            return error("Default display resolution requested without real host surface configuration");
        return reply(message, capacity, token, domain, 0, 0, display_size, sizeof display_size, 0);
    } else if (kind == APM_SESSION && (command == 0 || command == 1)) {
        if (parameter_size < (command ? 4u : 8u))
            return error("Truncated performance configuration request");
        uint32_t mode;
        memcpy(&mode, parameters, 4);
        if (mode > 1)
            return error("Invalid virtual performance mode");
        if (command == 0) {
            memcpy(&performance_configuration[mode], parameters + 4, 4);
            fprintf(stderr, "[hades-ipc] virtual performance configuration mode=%u id=%#x stored; host frequencies are OS-managed\n",
                    mode, performance_configuration[mode]);
        } else
            return reply(message, capacity, token, domain, 0, 0, &performance_configuration[mode], 4, 0);
    } else if (kind == COMMON_STATE && (command == 5 || command == 6 || command == 9)) {
        uint32_t value = command == 9 ? focus_state : 0;
        return reply(message, capacity, token, domain, 0, 0, &value,
                     command == 6 ? 4 : 1, 0);
    } else if (kind == AOC && command == 8) {
        kernel_object *event = allocate(AOC_EVENT);
        if (!event)
            return error("AOC event handle capacity exhausted");
        fprintf(stderr, "[hades-ipc] AOC base-package-only policy: no dynamic content changes; real unsignaled event reader\n");
        return reply(message, capacity, token, domain, event->handle, 0, NULL, 0, 1);
    } else if (kind == AOC && command == 2) {
        if (!send_pid || parameter_size < 8)
            return error("AOC count requires authenticated kernel caller PID");
        uint32_t count = 0;
        fprintf(stderr, "[hades-ipc] AOC count=0 for mounted base package; no D2R DLC identity inherited\n");
        return reply(message, capacity, token, domain, 0, 0, &count, sizeof count, 0);
    } else {
        fprintf(stderr, "[hades-ipc] unsupported service kind=%u command=%u\n", kind, command);
        return 0;
    }
    return reply(message, capacity, token, domain, moved_handle, object_id, NULL, 0, 0);
}

int hades_ipc_svc(unsigned svc, uint64_t *registers, void *message, size_t capacity) {
    if (svc != 0x1F && svc != 0x21 && svc != 0x16 && svc != 0x17 && svc != 0x18 && svc != 0x15
        && svc!=0x13 && svc!=0x14)
        return 0;
    if (!registers)
        return error("Missing IPC SVC register frame");
    pthread_mutex_lock(&lock);
    if (!process_id) {
        pthread_mutex_unlock(&lock);
        return error("Kernel IPC process is not initialized");
    }
    int result = 1;
    if (svc==0x13 || svc==0x14) {
        kernel_object *shared=find(registers[0]);
        if (!shared || shared->kind!=SHARED_MEMORY || registers[3]>UINT32_MAX
            || (svc==0x13 ? hades_memory_shared_map(shared->handle,registers[1],registers[2],(unsigned)registers[3])
                         : hades_memory_shared_unmap(shared->handle,registers[1],registers[2]))) {
            pthread_mutex_unlock(&lock);
            return error("Map/UnmapSharedMemory requires a live owned capability and exact extent");
        }
        registers[0]=0;
    } else if (svc == 0x15) {
        fprintf(stderr, "[hades-ipc] CreateTransferMemory source=%#llx size=%llu permission=%llu\n",
                (unsigned long long)registers[1],(unsigned long long)registers[2],
                (unsigned long long)registers[3]);
        kernel_object *transfer = allocate(TRANSFER_MEMORY);
        if (!transfer || registers[3] > UINT32_MAX
            || hades_memory_transfer_acquire(transfer->handle, registers[1], registers[2], (unsigned)registers[3])) {
            if (transfer)
                memset(transfer, 0, sizeof *transfer);
            pthread_mutex_unlock(&lock);
            return error("CreateTransferMemory requires a real owned shared-memory backend");
        }
        registers[0] = 0;
        registers[1] = transfer->handle;
    } else if (svc == 0x1F) {
        uintptr_t address = registers[1];
        hades_memory_info information;
        if (hades_memory_info_query(address, &information) || !(information.permissions & 1)
            || information.size - (address - information.base) < 4) {
            pthread_mutex_unlock(&lock);
            return error("ConnectToNamedPort name is not readable guest memory");
        }
        if (memcmp((const void *)address, "sm:", 4)) {
            pthread_mutex_unlock(&lock);
            return 0;
        }
        kernel_object *session = allocate(SM);
        if (!session) {
            pthread_mutex_unlock(&lock);
            return error("IPC session handle capacity exhausted");
        }
        registers[0] = 0;
        registers[1] = session->handle;
        fprintf(stderr, "[hades-ipc] connected sm: session=%#x\n", session->handle);
    } else if (svc == 0x18) {
        int trace_wait=hades_trace_verbose() || traced_waits<4;
        if (traced_waits<4)
            traced_waits++;
        if (trace_wait)
            fprintf(stderr, "[hades-ipc] event wait count=%llu timeout=%lld\n",
                    (unsigned long long)registers[2], (long long)registers[3]);
        uintptr_t address = registers[1];
        unsigned count = (uint32_t)registers[2];
        int64_t timeout = (int64_t)registers[3];
        hades_memory_info information;
        if (!count || count > 64 || timeout < -1 || hades_memory_info_query(address, &information)
            || !(information.permissions & 1) || information.size - (address - information.base) < count * 4) {
            pthread_mutex_unlock(&lock);
            return error("Invalid event wait array, extent or timeout");
        }
        uint32_t handles[64];
        memcpy(handles, (const void *)address, count * 4);
        for (unsigned i = 0; i < count; i++) {
            kernel_object *event = find(handles[i]);
            if (trace_wait)
                fprintf(stderr, "[hades-ipc] wait handle[%u]=%#x kind=%u\n", i, handles[i], event ? event->kind : 0);
        }
        uint64_t deadline = 0;
        if (timeout > 0) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now)) {
                pthread_mutex_unlock(&lock);
                return error("Cannot construct event wait deadline");
            }
            uint64_t ns = (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec;
            if ((uint64_t)timeout > UINT64_MAX - ns) {
                pthread_mutex_unlock(&lock);
                return error("Event wait deadline overflows monotonic clock");
            }
            deadline = ns + (uint64_t)timeout;
        }
        for (;;) {
            for (unsigned i = 0; i < count; i++) {
                kernel_object *event = find(handles[i]);
                if (!event || (event->kind != SUSPEND_EVENT && event->kind != GPU_ERROR_EVENT
                               && event->kind != APPLET_MESSAGE_EVENT && event->kind != AOC_EVENT
                               && event->kind!=HID_STYLE_EVENT && event->kind!=AUDOUT_EVENT)) {
                    pthread_mutex_unlock(&lock);
                    return error("WaitSynchronization handle is not a live supported kernel event");
                }
            }
            for (unsigned i = 0; i < count; i++) {
                kernel_object *event = find(handles[i]);
                if ((event->kind == SUSPEND_EVENT && tick_changed)
                    || (event->kind == GPU_ERROR_EVENT && gpu_error)
                    || (event->kind == APPLET_MESSAGE_EVENT && focus_message)
                    || (event->kind==HID_STYLE_EVENT && event->signalled)
                    || (event->kind==AUDOUT_EVENT && hades_audio_generation(event->audio)>event->audio_acknowledged)) {
                    registers[0] = 0;
                    registers[1] = i;
                    pthread_mutex_unlock(&lock);
                    return 1;
                }
            }
            if (!timeout) {
                registers[0] = 0xEA01;
                pthread_mutex_unlock(&lock);
                return 1;
            }
            struct timespec relative = {0};
            int audio_wait=0;
            for (unsigned i=0;i<count;i++)
                audio_wait|=find(handles[i])->kind==AUDOUT_EVENT;
            if (timeout > 0) {
                struct timespec now;
                if (clock_gettime(CLOCK_MONOTONIC, &now)) {
                    pthread_mutex_unlock(&lock);
                    return error("Cannot read event wait clock");
                }
                uint64_t ns = (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec;
                if (ns >= deadline) {
                    timeout = 0;
                    continue;
                }
                relative.tv_sec = (time_t)((deadline - ns) / 1000000000);
                relative.tv_nsec = (long)((deadline - ns) % 1000000000);
            }
            /* AudioQueue callbacks never acquire the IPC lock. Bounded polling
               observes their release generation without a lost-wakeup race. */
            if (audio_wait && (timeout<0 || relative.tv_sec || relative.tv_nsec>10000000))
                relative=(struct timespec){.tv_nsec=10000000};
            event_waiters++;
            int status = timeout < 0 && !audio_wait ? pthread_cond_wait(&event_changed, &lock)
                : pthread_cond_timedwait_relative_np(&event_changed, &lock, &relative);
            event_waiters--;
            if (status == ETIMEDOUT && !audio_wait)
                timeout = 0;
            else if (status==ETIMEDOUT)
                continue;
            else if (status) {
                pthread_mutex_unlock(&lock);
                return error("Host condition wait failed");
            }
        }
    } else {
        kernel_object *session = find(registers[0]);
        if (!session) {
            pthread_mutex_unlock(&lock);
            return error("Unknown or closed IPC session handle");
        }
        if (svc == 0x16) {
            if (session->kind==AUDOUT_STREAM && session->audio) {
                for (unsigned i=0;i<64;i++)
                    if (sessions[i].handle && sessions[i].kind==AUDOUT_EVENT && sessions[i].audio==session->audio) {
                        pthread_mutex_unlock(&lock);
                        return error("Close audio release events before disposing their output");
                    }
                if (hades_audio_close(session->audio)) {
                    pthread_mutex_unlock(&lock);
                    return -1;
                }
            }
            if (session->kind==SHARED_MEMORY && hades_memory_shared_close(session->handle)) {
                pthread_mutex_unlock(&lock);
                return -1;
            }
            if (session->kind==SHARED_MEMORY && hid_client.shared==session->handle)
                memset(&hid_client,0,sizeof hid_client);
            if (session->kind == TRANSFER_MEMORY && hades_memory_transfer_release(session->handle)) {
                pthread_mutex_unlock(&lock);
                return -1;
            }
            memset(session, 0, sizeof *session);
            int bsd_open=0;
            for (unsigned i=0;i<64;i++)
                if (sessions[i].handle && sessions[i].kind==BSD)
                    bsd_open=1;
            if (!bsd_open && close_bsd()) {
                pthread_mutex_unlock(&lock);
                return -1;
            }
            pthread_cond_broadcast(&event_changed);
        } else if (svc == 0x17 && (session->kind == SUSPEND_EVENT || session->kind == GPU_ERROR_EVENT
                                 || session->kind == AOC_EVENT || session->kind==HID_STYLE_EVENT
                                 || session->kind==AUDOUT_EVENT)) {
            if (session->kind == SUSPEND_EVENT)
                tick_changed = 0;
            else if (session->kind == GPU_ERROR_EVENT)
                gpu_error = 0;
            else if (session->kind==HID_STYLE_EVENT)
                session->signalled=0;
            else if (session->kind==AUDOUT_EVENT)
                session->audio_acknowledged=hades_audio_generation(session->audio);
        } else if (svc == 0x17 || session->kind == SUSPEND_EVENT || session->kind == GPU_ERROR_EVENT
                   || session->kind == APPLET_MESSAGE_EVENT || session->kind == AOC_EVENT
                   || session->kind==HID_STYLE_EVENT || session->kind==AUDOUT_EVENT) {
            pthread_mutex_unlock(&lock);
            return error("Kernel handle has the wrong type for IPC/reset operation");
        }
        else if (!message) {
            pthread_mutex_unlock(&lock);
            return error("Missing current-thread IPC buffer");
        } else
            result = dispatch(session, message, capacity);
        if (result==2) {
            registers[0]=0xF601;
            result=1;
        } else if (result > 0)
            registers[0] = 0;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
