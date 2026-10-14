#define _DARWIN_C_SOURCE
#include "../runtime/ipc.h"
#include "../runtime/memory.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static _Alignas(16) uint8_t message[256];
static int frontend;

static uint32_t read_word(size_t offset) {
    uint32_t value;
    memcpy(&value, message + offset, 4);
    return value;
}

static void put_word(size_t offset, uint32_t value) {
    memcpy(message + offset, &value, 4);
}

static void request(unsigned type, unsigned command, int pid, int process,
                    uint32_t object, const void *data, size_t size) {
    memset(message, 0, sizeof message);
    put_word(0, type);
    size_t offset = 8;
    if (pid || process) {
        put_word(8, (unsigned)pid | ((unsigned)process << 1));
        offset += 4;
        if (pid)
            offset += 8;
        if (process) {
            put_word(offset, 0xFFFF8001);
            offset += 4;
        }
    }
    put_word(4, (uint32_t)((32 + size + (object ? 16 : 0) + 3) / 4)
             | ((pid || process) ? 0x80000000 : 0));
    size_t cmif = (offset + 15) & ~(size_t)15;
    if (object) {
        message[cmif] = 1;
        uint16_t extent = (uint16_t)(16 + size);
        memcpy(message + cmif + 2, &extent, 2);
        put_word(cmif + 4, object);
        cmif += 16;
    }
    put_word(cmif, 0x49434653);
    put_word(cmif + 4, type >= 6 ? 1 : 0);
    put_word(cmif + 8, command);
    put_word(cmif + 12, 123);
    if (size)
        memcpy(message + cmif + 16, data, size);
}

static int send_expected(uint32_t handle, uint32_t expected_result) {
    int closing = message[16] == 2;
    uint64_t registers[4] = {handle, 0, 0, 0};
    int result = hades_ipc_svc(0x21, registers, message, sizeof message);
    if (result == 1) {
        assert(!registers[0]);
        size_t cmif = (read_word(16) == 0x4F434653) ? 16 : 32;
        assert(read_word(cmif) == 0x4F434653 && read_word(cmif + 8) == expected_result);
        assert(read_word(cmif + 12) == (closing ? 0u : 123u));
        size_t raw = (read_word(4) >> 31) ? 16 : 8;
        assert(raw + (read_word(4) & 1023) * 4 <= sizeof message);
    }
    return result;
}

static int send(uint32_t handle) { return send_expected(handle, 0); }

static uint32_t connect(void) {
    char name[] = "sm:";
    uint64_t registers[4] = {0, (uintptr_t)name, 0, 0};
    assert(hades_ipc_svc(0x1F, registers, NULL, 0) == 1 && !registers[0]);
    return (uint32_t)registers[1];
}

static void close_handle(uint32_t handle) {
    uint64_t registers[4] = {handle, 0, 0, 0};
    assert(hades_ipc_svc(0x16, registers, NULL, 0) == 1 && !registers[0]);
}

static uint32_t service(uint32_t sm, const char *name) {
    char input[8] = {0};
    assert(strlen(name) <= sizeof input);
    memcpy(input, name, strlen(name));
    request(6, 1, 0, 0, 0, input, sizeof input);
    assert(send(sm) == 1);
    assert(read_word(8) == (1u << 5) && (read_word(4) >> 31));
    return read_word(12);
}

static void alias_descriptor(size_t offset, uintptr_t address, size_t size) {
    put_word(offset,(uint32_t)size);
    put_word(offset+4,(uint32_t)address);
    put_word(offset+8,(uint32_t)((address>>36)<<2) | (uint32_t)((size>>32)<<24)
                         | (uint32_t)((address>>32)&15)<<28);
}

static void audio_request(unsigned command, uintptr_t input, size_t input_size,
                          uintptr_t output, size_t output_size, int open, uint64_t tag) {
    memset(message,0,sizeof message);
    unsigned sends=!!input, receives=!!output;
    put_word(0,6 | sends<<20 | receives<<24);
    size_t offset=8;
    if (open) {
        put_word(8,3);
        put_word(20,0xFFFF8001);
        offset=24;
    }
    if (sends) { alias_descriptor(offset,input,input_size); offset+=12; }
    if (receives) { alias_descriptor(offset,output,output_size); offset+=12; }
    size_t cmif=(offset+15)&~(size_t)15, size=open ? 16 : command==7 ? 8 : 0;
    put_word(4,(uint32_t)((cmif+16+size-offset+3)/4) | (open ? 0x80000000 : 0));
    put_word(cmif,0x49434653);put_word(cmif+4,1);
    put_word(cmif+8,command);put_word(cmif+12,123);
    if (open) {
        put_word(cmif+16,48000);put_word(cmif+20,2);
        uint64_t user=0x5678;
        memcpy(message+cmif+24,&user,8);
    } else if (size)
        memcpy(message+cmif+16,&tag,8);
}

static void audio_wire(uint32_t sm) {
    uint64_t registers[4]={0,4*1024*1024,0,0};
    assert(hades_memory_svc(1,registers)==1 && !registers[0]);
    uintptr_t heap=registers[1];
    memset((void *)heap,0,16384);
    uint32_t manager=service(sm,"audout:u");
    audio_request(3,heap,256,heap+256,256,1,0);
    put_word(32,read_word(32)|2);
    assert(send(manager)==-1);
    audio_request(3,heap,256,heap+256,256,1,0);
    assert(send(manager)==1 && read_word(32)==48000 && read_word(36)==2);
    uint32_t stream=read_word(12);
    assert(!strcmp((char *)(heap+256),"DeviceOut"));
    request(7,3,0,0,0,NULL,0);
    assert(send(stream)==1 && !read_word(32));
    request(6,4,0,0,0,NULL,0);
    assert(send(stream)==1 && read_word(8)==2);
    uint32_t event=read_word(12);
    uint64_t *released=(uint64_t *)(heap+1024);
    *released=0xBAD;
    audio_request(8,0,0,(uintptr_t)released,8,0,0);
    assert(send(stream)==1 && !read_word(32) && !*released);
    uint64_t descriptor[5]={0,heap+4096,4096,4096,0};
    memcpy((void *)(heap+512),descriptor,sizeof descriptor);
    audio_request(7,heap+512,sizeof descriptor,0,0,0,777);
    assert(send(stream)==1);
    audio_request(7,heap+512,sizeof descriptor,0,0,0,777);
    assert(send(stream)==-1);
    request(6,1,0,0,0,NULL,0);
    assert(send(stream)==1);
    memcpy((void *)(heap+2048),&event,4);
    registers[0]=0;registers[1]=heap+2048;registers[2]=1;registers[3]=2000000000;
    assert(hades_ipc_svc(0x18,registers,NULL,0)==1 && !registers[0]);
    audio_request(8,0,0,(uintptr_t)released,8,0,0);
    assert(send(stream)==1 && read_word(32)==1 && *released==777);
    audio_request(8,0,0,(uintptr_t)released,8,0,0);
    assert(send(stream)==1 && !read_word(32) && !*released);
    registers[0]=event;
    assert(hades_ipc_svc(0x17,registers,NULL,0)==1 && !registers[0]);
    registers[0]=0;registers[1]=heap+2048;registers[2]=1;registers[3]=0;
    assert(hades_ipc_svc(0x18,registers,NULL,0)==1 && registers[0]==0xEA01);
    registers[0]=stream;
    assert(hades_ipc_svc(0x16,registers,NULL,0)==-1);
    close_handle(event);
    memset(message,0,sizeof message);put_word(0,2);
    registers[0]=stream;
    assert(hades_ipc_svc(0x21,registers,message,sizeof message)==1 && registers[0]==0xF601);
    close_handle(stream);
    memset(message,0,sizeof message);put_word(0,2);
    registers[0]=manager;
    assert(hades_ipc_svc(0x21,registers,message,sizeof message)==1 && registers[0]==0xF601);
    close_handle(manager);
    puts("AUDIO_IPC_OK: native completion event, tag reuse, empty output clearing, malformed descriptors and endpoint close");
}

static void bsd_registration(uint32_t sm) {
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    uint64_t registers[4]={0,4*1024*1024,0,0};
    assert(hades_memory_svc(1,registers)==1 && !registers[0]);
    uintptr_t heap=registers[1];
    for (unsigned version=1;version<=7;version+=6) {
        uint32_t bsd=service(sm,"bsd:u");
        request(7,0,0,0,0,NULL,0);
        assert(send(bsd)==1 && read_word(32)==1);
        request(7,2,0,0,0,NULL,0);
        assert(send(bsd)==1 && read_word(8)==32);
        uint32_t clone=read_word(12);
        registers[0]=0;registers[1]=heap;registers[2]=page;registers[3]=0;
        assert(hades_ipc_svc(0x15,registers,NULL,0)==1 && !registers[0]);
        uint32_t transfer=(uint32_t)registers[1];
        struct { uint32_t config[8]; uint64_t pid,size; } input={{version,4096,4096,0,0,0,0,1},0,page};
        input.config[0]=6;
        request(6,0,1,1,1,&input,sizeof input);put_word(20,transfer);
        assert(send(bsd)==-1);
        input.config[0]=version;input.size=64;
        request(6,0,1,1,1,&input,sizeof input);put_word(20,transfer);
        assert(send(bsd)==-1);
        input.size=page;
        request(6,0,1,1,1,&input,sizeof input);put_word(20,transfer);
        assert(send(bsd)==1 && !read_word(16));
        assert((read_word(4)&1023)==(version==7 ? 13u : 14u));
        assert(read_word(48)==(version==7 ? 0u : 0x5678u));
        if (version==1)
            assert(!read_word(52));
        request(6,0,1,1,1,&input,sizeof input);put_word(20,transfer);
        assert(send(bsd)==-1);
        hades_memory_info info;
        assert(!hades_memory_info_query(heap,&info) && !info.permissions && info.ipc_references==2);
        close_handle(transfer);
        assert(!hades_memory_info_query(heap,&info) && !info.permissions && info.ipc_references==1);
        assert(hades_memory_transfer_buffer(transfer,page));
        uint64_t client=0xdeadbeef;
        request(6,1,1,0,1,&client,sizeof client);
        assert(send(clone)==-1);
        client=version==7 ? 0 : 0x5678;
        request(6,1,0,0,1,&client,sizeof client);
        assert(send(clone)==-1);
        request(6,1,1,0,1,&client,sizeof client);
        assert(send(clone)==1);
        request(6,1,1,0,1,&client,sizeof client);
        assert(send(clone)==-1);
        uint32_t socket[3]={2,1,0};
        request(6,2,0,0,1,socket,sizeof socket);
        assert(send(clone)==1 && read_word(48)==UINT32_MAX && read_word(52)==50);
        request(6,99,0,0,1,NULL,0);
        assert(send(clone)==0);
        close_handle(bsd);
        assert(!hades_memory_writable(heap,page));
        close_handle(clone);
        assert(hades_memory_writable(heap,page) && !hades_memory_transfer_buffer(transfer,1));
    }
}

static void hid_shared_memory(uint32_t sm) {
    uint32_t hid=service(sm,"hid");
    uint64_t user=0x5678;
    request(6,0,0,0,0,&user,sizeof user);
    assert(send(hid)==-1);
    request(6,0,1,0,0,&user,sizeof user);
    assert(send(hid)==1 && read_word(8)==32);
    uint32_t resource=read_word(12);
    request(6,0,0,0,0,NULL,0);
    assert(send(resource)==1 && read_word(8)==2);
    uint32_t shared=read_word(12);
    request(6,0,0,0,0,NULL,0);
    assert(send(resource)==-1);
    struct { uint32_t revision,padding; uint64_t user; } activation={4,0,user};
    request(6,109,1,0,0,&activation,sizeof activation);
    assert(send(hid)==-1);
    activation.revision=3;activation.user=0xdeadbeef;
    request(6,109,1,0,0,&activation,sizeof activation);
    assert(send(hid)==-1);
    activation.user=user;
    request(6,109,1,0,0,&activation,sizeof activation);
    assert(send(hid)==1);
    request(6,109,1,0,0,&activation,sizeof activation);
    assert(send(hid)==-1);
    activation.revision=7;
    request(6,100,1,0,0,&activation,sizeof activation);
    assert(send(hid)==1);
    uint64_t hold[2]={user,2};
    request(6,120,1,0,0,hold,sizeof hold);
    assert(send(hid)==-1);
    hold[1]=1;
    request(6,120,1,0,0,hold,sizeof hold);
    assert(send(hid)==1);
    request(7,3,0,0,0,NULL,0);
    assert(send(hid)==1 && read_word(32)==256);
    uint32_t ids[2]={0,0x20};
    uintptr_t address=(uintptr_t)ids;
    uint32_t descriptor=(sizeof ids<<16) | (uint32_t)(((address>>32)&15)<<12)
        | (uint32_t)(((address>>36)&7)<<6);
    request(6,102,1,0,0,&user,sizeof user);
    put_word(0,0x10006);put_word(20,descriptor|1);put_word(24,(uint32_t)address);
    assert(send(hid)==-1);
    request(6,102,1,0,0,&user,sizeof user);
    put_word(0,0x10006);put_word(20,descriptor);put_word(24,(uint32_t)address);
    assert(send(hid)==1);
    struct { uint32_t id,padding; uint64_t user,cookie; } subscription={1,0,user,0};
    request(6,106,1,0,0,&subscription,sizeof subscription);
    assert(send(hid)==-1);
    subscription.id=0;
    request(6,106,1,0,0,&subscription,sizeof subscription);
    assert(send(hid)==1 && read_word(8)==2);
    uint32_t style_event=read_word(12);
    uint64_t wait[4]={0,(uintptr_t)&style_event,1,0};
    assert(hades_ipc_svc(0x18,wait,NULL,0)==1 && !wait[0]);
    uint64_t reset[4]={style_event,0,0,0};
    assert(hades_ipc_svc(0x17,reset,NULL,0)==1 && !reset[0]);
    wait[0]=0;wait[1]=(uintptr_t)&style_event;
    assert(hades_ipc_svc(0x18,wait,NULL,0)==1 && wait[0]==0xea01);
    close_handle(style_event);
    size_t size=0x40000;
    unsigned char *publisher=hades_memory_shared_buffer(shared,size);
    assert(publisher && !publisher[0] && !publisher[size-1]);
    uint64_t registers[4]={0,2,0xFFFF8001,0};
    assert(hades_memory_svc(0x29,registers)==1 && !registers[0]);
    uintptr_t destination=registers[1];
    uint64_t mapping[4]={shared,destination+1,size,1};
    assert(hades_ipc_svc(0x13,mapping,NULL,0)==-1);
    mapping[0]=shared;mapping[1]=destination;mapping[3]=3;
    assert(hades_ipc_svc(0x13,mapping,NULL,0)==-1);
    mapping[0]=shared;mapping[3]=1;
    assert(hades_ipc_svc(0x13,mapping,NULL,0)==1 && !mapping[0]);
    publisher[0]=47;publisher[size-1]=81;
    assert(*(volatile unsigned char *)destination==47);
    assert(*(volatile unsigned char *)(destination+size-1)==81);
    uint32_t style;
    memcpy(&style,(const void *)(destination+0x9a00),4);
    assert(style==(frontend ? 1u : 0u));
    if (frontend) {
        assert(hades_ipc_publish_input(1,40000,0)==-1);
        for (unsigned i=0;i<40;i++)
            assert(!hades_ipc_publish_input(i&1,-32767,32767));
        for (unsigned ring=0;ring<2;ring++) {
            const uint64_t *lifo=(const uint64_t *)(destination+0x9a28+(ring ? 6*0x350 : 0));
            assert(lifo[1]==17 && lifo[2]<17 && lifo[3]==16);
            const uint64_t *storage=lifo+4+lifo[2]*6;
            assert(storage[0]==storage[1] && storage[2]==1);
            const int32_t *axes=(const int32_t *)(storage+3);
            assert(axes[0]==-32767 && axes[1]==32767 && !axes[2] && !axes[3]);
            assert(((const uint32_t *)(storage+5))[0]==1);
        }
        assert(!hades_ipc_publish_input(0,0,0));
        const uint64_t *lifo=(const uint64_t *)(destination+0x9a28);
        const uint64_t *storage=lifo+4+lifo[2]*6;
        assert(!storage[2] && !storage[3]);
    } else
        assert(hades_ipc_publish_input(1,0,0)==-1);
    hades_memory_info info;
    assert(!hades_memory_info_query(destination,&info) && info.state==6 && info.permissions==1 && info.size==size);
    assert(hades_memory_unregister(0x80000000u|shared));
    assert(!hades_memory_info_query(destination+size,&info) && !info.state && info.base==destination+size);
    pid_t child=fork();assert(child>=0);
    if (!child) { *(volatile unsigned char *)destination=99;_exit(0); }
    int status;assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && (WTERMSIG(status)==SIGSEGV || WTERMSIG(status)==SIGBUS));
    mapping[0]=shared;
    assert(hades_ipc_svc(0x13,mapping,NULL,0)==-1);
    uint64_t closing[4]={shared,0,0,0};
    assert(hades_ipc_svc(0x16,closing,NULL,0)==-1);
    assert(hades_ipc_shutdown() && hades_memory_shutdown());
    mapping[0]=shared;mapping[2]=size/2;
    assert(hades_ipc_svc(0x14,mapping,NULL,0)==-1);
    mapping[0]=shared;mapping[2]=size;
    assert(hades_ipc_svc(0x14,mapping,NULL,0)==1 && !mapping[0]);
    assert(!hades_memory_info_query(destination,&info) && !info.state && !info.permissions);
    uint64_t stack[4]={0,14,0xFFFF8001,0};
    assert(hades_memory_svc(0x29,stack)==1 && !stack[0]);
    mapping[0]=shared;mapping[1]=stack[1];mapping[3]=1;
    assert(hades_ipc_svc(0x13,mapping,NULL,0)==1 && !mapping[0]);
    assert(*(volatile unsigned char *)(uintptr_t)stack[1]==47);
    mapping[0]=shared;
    assert(hades_ipc_svc(0x14,mapping,NULL,0)==1 && !mapping[0]);
    close_handle(shared);
    assert(!hades_memory_shared_buffer(shared,1));
    close_handle(resource);close_handle(hid);
}

static uint32_t child(uint32_t handle, uint32_t parent, unsigned command, int process) {
    uint64_t reserved = 0;
    request(6, command, process, process, parent, process ? &reserved : NULL, process ? 8 : 0);
    assert(send(handle) == 1 && read_word(16) == 1);
    return read_word(48);
}

static atomic_uint ready;
static uint32_t gpu_event;
static void *wait_event(void *unused) {
    (void)unused;
    uint32_t handles[1] = {gpu_event};
    uint64_t registers[4] = {0, (uintptr_t)handles, 1, 2000000000};
    atomic_fetch_add(&ready, 1);
    assert(hades_ipc_svc(0x18, registers, NULL, 0) == 1);
    assert(!registers[0] && !registers[1]);
    return NULL;
}

int main(int argc,char **argv) {
    assert(argc==1 || (argc==2 && !strcmp(argv[1],"--frontend")));
    frontend=argc==2;
    const size_t mib = 1024 * 1024;
    hades_memory_config config = {4*mib,4*mib,8*mib,0,0x1234,1ull << 39,1ull << 44,1,0,0};
    assert(!hades_memory_initialize(&config));
    if (frontend) {
        assert(!hades_ipc_configure_input_frontend());
        assert(hades_ipc_configure_input_frontend()==-1);
    }
    assert(hades_ipc_initialize(0));
    assert(!hades_ipc_initialize(0x5678));
    assert(hades_ipc_initialize(0x5678));
    uint32_t sm = connect();
    char lm_name[8] = {'l','m'};
    request(6, 1, 0, 0, 0, lm_name, 8);
    assert(send(sm) == -1);
    uint64_t spoofed_pid = 0xDEADBEEF;
    request(6, 0, 1, 0, 0, &spoofed_pid, 8);
    assert(send(sm) == 1);
    request(6, 0, 1, 0, 0, &spoofed_pid, 8);
    assert(send(sm) == -1);
    bsd_registration(sm);
    audio_wire(sm);
    hid_shared_memory(sm);
    uint32_t lm = service(sm, "lm");
    request(7, 3, 0, 0, 0, NULL, 0);
    assert(send(lm) == 1 && !read_word(32));
    request(7, 0, 0, 0, 0, NULL, 0);
    assert(send(lm) == 1 && read_word(32) == 1);
    request(6, 0, 1, 0, 1, &spoofed_pid, 8);
    assert(send(lm) == 1 && read_word(16) == 1 && read_word(48) == 2);
    request(6, 999, 0, 0, 2, NULL, 0);
    assert(send(lm) == 0);
    request(6, 0, 0, 0, 2, NULL, 0);
    message[16] = 2;
    message[18] = message[19] = 0;
    assert(send(lm) == 1);
    request(6, 0, 0, 0, 2, NULL, 0);
    assert(send(lm) == -1);

    char unavailable[8] = {'f','s','p','-','s','r','v'};
    request(6, 1, 0, 0, 0, unavailable, 8);
    uint8_t original[256];
    memcpy(original, message, sizeof original);
    assert(send(sm) == 0 && !memcmp(original, message, sizeof original));
    request(6, 1, 0, 0, 0, lm_name, 8);
    put_word(4, 1023);
    assert(send(sm) == -1);
    request(6, 1, 0, 0, 0, lm_name, 8);
    put_word(20, 2);
    assert(send(sm) == -1);
    request(6, 1, 0, 0, 0, lm_name, 8);
    put_word(0, 0x10006);
    assert(send(sm) == -1);
    request(6, 1, 0, 0, 0, lm_name, 8);
    uint64_t unaligned[4] = {sm, 0, 0, 0};
    assert(hades_ipc_svc(0x21, unaligned, message + 1, sizeof message - 1) == -1);

    uint32_t aoc = service(sm, "aoc:u");
    request(6, 8, 0, 0, 0, NULL, 0);
    assert(send(aoc) == 1 && read_word(8) == 2);
    uint32_t aoc_event = read_word(12);
    uint64_t aoc_wait[4] = {0, (uintptr_t)&aoc_event, 1, 0};
    assert(hades_ipc_svc(0x18, aoc_wait, NULL, 0) == 1 && aoc_wait[0] == 0xEA01);
    uint64_t aoc_reserved = 0;
    request(6, 2, 0, 0, 0, &aoc_reserved, 8);
    assert(send(aoc) == -1);
    request(6, 2, 1, 0, 0, &aoc_reserved, 8);
    assert(send(aoc) == 1 && !read_word(32));
    close_handle(aoc_event);
    close_handle(aoc);

    uint32_t apm = service(sm, "apm");
    request(6, 1, 0, 0, 0, NULL, 0);
    assert(send(apm) == 1 && !read_word(32));
    request(7, 0, 0, 0, 0, NULL, 0);
    assert(send(apm) == 1 && read_word(32) == 1);
    uint32_t apm_session = child(apm, 1, 0, 0);
    uint32_t configuration[2] = {0, 0x20004};
    request(6, 0, 0, 0, apm_session, configuration, sizeof configuration);
    assert(send(apm) == 1);
    request(6, 1, 0, 0, apm_session, configuration, 4);
    assert(send(apm) == 1 && read_word(48) == 0x20004);
    configuration[0] = 1;
    request(6, 1, 0, 0, apm_session, configuration, 4);
    assert(send(apm) == 1 && !read_word(48));
    configuration[0] = 2;
    request(6, 0, 0, 0, apm_session, configuration, sizeof configuration);
    assert(send(apm) == -1);
    request(6, 0, 0, 0, apm_session, configuration, 4);
    assert(send(apm) == -1);
    uint32_t app = service(sm, "appletOE");
    request(7, 0, 0, 0, 0, NULL, 0);
    assert(send(app) == 1 && read_word(32) == 1);
    uint64_t reserved = 0;
    request(6, 0, 1, 0, 1, &reserved, 8);
    assert(send(app) == -1);
    request(6, 0, 1, 1, 1, &reserved, 8);
    put_word(20, 0x100);
    assert(send(app) == -1);
    uint32_t proxy = child(app, 1, 0, 1);
    uint32_t common = child(app, proxy, 0, 0);
    uint32_t self = child(app, proxy, 1, 0);
    uint32_t window = child(app, proxy, 2, 0);
    uint32_t functions = child(app, proxy, 20, 0);
    request(6,21,0,0,functions,NULL,0);
    assert(send(app)==-1);
    assert(hades_ipc_set_display_version("0123456789abcdef"));
    assert(!hades_ipc_set_display_version("test-version"));
    assert(hades_ipc_set_display_version("changed"));
    request(6,21,0,0,functions,NULL,0);
    assert(send(app)==1 && !memcmp(message+48,"test-version",13));
    assert(!message[61] && !message[62] && !message[63]);
    request(6, 0, 0, 0, common, NULL, 0);
    assert(send(app) == 1 && read_word(8) == 2);
    uint32_t applet_event = read_word(12);
    uint64_t applet_wait[4] = {0, (uintptr_t)&applet_event, 1, 0};
    assert(hades_ipc_svc(0x18, applet_wait, NULL, 0) == 1 && !applet_wait[0]);
    request(6, 9, 0, 0, common, NULL, 0);
    assert(send(app) == 1 && message[48] == 1);
    request(6, 1, 0, 0, common, NULL, 0);
    assert(send(app) == 1 && read_word(48) == 15);
    applet_wait[1] = (uintptr_t)&applet_event;
    assert(hades_ipc_svc(0x18, applet_wait, NULL, 0) == 1 && applet_wait[0] == 0xEA01);
    request(6, 1, 0, 0, common, NULL, 0);
    assert(send_expected(app, 0x680) == 1);
    assert(hades_ipc_set_focus_state(3)==-1);
    assert(!hades_ipc_set_focus_state(2));
    applet_wait[1]=(uintptr_t)&applet_event;
    assert(hades_ipc_svc(0x18,applet_wait,NULL,0)==1 && !applet_wait[0]);
    request(6,9,0,0,common,NULL,0);
    assert(send(app)==1 && message[48]==2);
    request(6,1,0,0,common,NULL,0);
    assert(send(app)==1 && read_word(48)==15);
    assert(!hades_ipc_set_focus_state(2));
    request(6,1,0,0,common,NULL,0);
    assert(send_expected(app,0x680)==1);
    assert(!hades_ipc_set_focus_state(1));
    request(6,9,0,0,common,NULL,0);
    assert(send(app)==1 && message[48]==1);
    request(6,1,0,0,common,NULL,0);
    assert(send(app)==1 && read_word(48)==15);
    request(6, 10, 0, 0, window, NULL, 0);
    assert(send(app) == 1);
    request(6, 1, 0, 0, window, NULL, 0);
    assert(send(app) == 1);
    uint64_t resource;
    memcpy(&resource, message + 48, 8);
    assert(resource == 0x5678 && resource != spoofed_pid);
    request(6, 91, 0, 0, self, NULL, 0);
    assert(send(app) == 1 && read_word(8) == 2);
    uint32_t suspend_event = read_word(12);
    request(6, 130, 0, 0, functions, NULL, 0);
    assert(send(app) == 1 && read_word(8) == 2);
    gpu_event = read_word(12);
    uint32_t handles[2] = {suspend_event, gpu_event};
    uint64_t registers[4] = {0, (uintptr_t)handles, 2, 0};
    assert(hades_ipc_svc(0x18, registers, NULL, 0) == 1 && registers[0] == 0xEA01);
    registers[3] = 1000000;
    assert(hades_ipc_svc(0x18, registers, NULL, 0) == 1 && registers[0] == 0xEA01);
    assert(!hades_ipc_set_suspended(1));
    assert(hades_ipc_set_focus_state(1)==-1);
    request(6,9,0,0,common,NULL,0);
    assert(send(app)==1 && message[48]==3);
    struct timespec pause = {.tv_nsec=1000000};
    nanosleep(&pause, NULL);
    assert(!hades_ipc_set_suspended(0));
    registers[3] = 0;
    assert(hades_ipc_svc(0x18, registers, NULL, 0) == 1 && !registers[0] && !registers[1]);
    request(6, 90, 0, 0, self, NULL, 0);
    assert(send(app) == 1);
    memcpy(&resource, message + 48, 8);
    assert(resource > 0);
    registers[0] = suspend_event;
    assert(hades_ipc_svc(0x17, registers, NULL, 0) == 1);
    registers[1] = (uintptr_t)handles;
    assert(hades_ipc_svc(0x18, registers, NULL, 0) == 1 && registers[0] == 0xEA01);
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++)
        assert(!pthread_create(&threads[i], NULL, wait_event, NULL));
    while (atomic_load(&ready) < 2)
        sched_yield();
    assert(!hades_ipc_report_gpu_error());
    for (unsigned i = 0; i < 2; i++)
        assert(!pthread_join(threads[i], NULL));
    uint8_t policy[3] = {0, 0, 1};
    request(6, 13, 0, 0, self, policy, sizeof policy);
    assert(send(app) == 1);
    uint8_t enabled = 0;
    request(6, 16, 0, 0, self, &enabled, sizeof enabled);
    assert(send(app) == 1);
    assert(hades_ipc_set_suspended(1) == -1);
    assert(!hades_ipc_set_focus_state(2));
    assert(!hades_ipc_set_focus_state(1));
    assert(hades_ipc_set_focus_state(3)==-1);
    policy[0]=1;policy[2]=0;
    request(6,13,0,0,self,policy,sizeof policy);
    assert(send(app)==1);
    assert(hades_ipc_set_focus_state(2)==-1);
    request(6,9,0,0,common,NULL,0);
    assert(send(app)==1 && message[48]==1);
    policy[0]=0;policy[2]=1;
    request(6,13,0,0,self,policy,sizeof policy);
    assert(send(app)==1);
    request(6, 9, 0, 0, common, NULL, 0);
    assert(send(app) == 1 && message[48] == 1);
    enabled = 2;
    request(6, 16, 0, 0, self, &enabled, sizeof enabled);
    assert(send(app) == -1);
    handles[1] = 0xABC;
    registers[1] = (uintptr_t)handles;
    assert(hades_ipc_svc(0x18, registers, NULL, 0) == -1);
    close_handle(gpu_event);
    close_handle(suspend_event);
    close_handle(applet_event);
    close_handle(app);
    request(7, 3, 0, 0, 0, NULL, 0);
    assert(send(app) == -1);
    close_handle(apm);
    close_handle(lm);
    close_handle(sm);
    uint32_t all[64];
    for (unsigned i = 0; i < 64; i++) {
        all[i] = connect();
        assert(all[i] != sm);
    }
    char port[] = "sm:";
    registers[1] = (uintptr_t)port;
    assert(hades_ipc_svc(0x1F, registers, NULL, 0) == -1);
    for (unsigned i = 0; i < 64; i++)
        close_handle(all[i]);
    assert(!hades_ipc_shutdown() && !hades_memory_shutdown());
    printf("IPC_OK: actual CMIF shapes, owned handles/domain objects, authenticated process copy, live event wait/reset/host-source notifications; not guest concurrency proof\n");
    return 0;
}
