#define _DARWIN_C_SOURCE
#include "../runtime/host.h"
#include "../runtime/memory.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <CommonCrypto/CommonDigest.h>

static _Alignas(16384) unsigned char arena[16384];
static _Alignas(16384) unsigned char readonly[16384];
const unsigned char hades_host_stubs[2048] = {0};

static int call(unsigned index, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uint64_t *result) {
    uint64_t registers[31] = {a,b,c,d,e};
    int status = hades_host_call(index, registers);
    if (result)
        *result = registers[0];
    return status;
}

int main(int argc, char **argv) {
    assert(argc == 3);
    const size_t mib = 1024*1024;
    uint64_t result;
    hades_memory_config config = {4*mib,4*mib,8*mib,0,0x1234,1ull<<39,1ull<<44,1,0,0};
    assert(!hades_memory_initialize(&config));
    assert(!hades_memory_register((uintptr_t)arena, sizeof arena, 4, 3, 99));
    assert(!hades_memory_register((uintptr_t)readonly, sizeof readonly, 4, 1, 100));
    assert(!hades_host_initialize(argv[1],"720p",NULL,0));
    assert(hades_host_initialize(argv[1],"720p",NULL,0));
    uint32_t *npad=(uint32_t *)(arena+128);
    *npad=0;
    memset(arena+132,0xa5,16);
    assert(!call(HADES_HOST_HID_VIBRATION_HANDLES,(uintptr_t)arena+132,4,(uintptr_t)npad,1,0,&result)
           && !result && arena[132]==0xa5);
    assert(call(HADES_HOST_HID_VIBRATION_HANDLES,(uintptr_t)readonly,1,(uintptr_t)npad,1,0,NULL));
    *npad=9;
    assert(call(HADES_HOST_HID_VIBRATION_HANDLES,(uintptr_t)arena+132,4,(uintptr_t)npad,1,0,NULL));
    *npad=0;
    assert(call(HADES_HOST_FS_ROM_CACHE,(uintptr_t)arena,0,0,0,0,NULL));
    assert(call(HADES_HOST_TIME_NOW,0,0,0,0,0,NULL));
    assert(!call(HADES_HOST_FS_INITIALIZE,0,0,0,0,0,NULL));
    assert(call(HADES_HOST_NETWORK_AVAILABLE,0,0,0,0,0,NULL));
    assert(!call(HADES_HOST_NETWORK_INITIALIZE,0,0,0,0,0,NULL));
    assert(!call(HADES_HOST_NETWORK_AVAILABLE,0,0,0,0,0,&result) && !result);
    assert(!call(HADES_HOST_NETWORK_SUBMIT,0,0,0,0,0,NULL));
    assert(!call(HADES_HOST_NETWORK_ON_HOLD,0,0,0,0,0,&result) && !result);
    memset(arena+256,0xA5,32);
    assert(!call(HADES_HOST_NETWORK_PROXY,(uintptr_t)arena+256,0,0,0,0,&result) && result==0x4201);
    assert(arena[256]==0xA5);
    assert(!call(HADES_HOST_NETWORK_CANCEL,0,0,0,0,0,NULL));
    assert(!call(HADES_HOST_PCTL_INITIALIZE,0,0,0,0,0,NULL));
    assert(!call(HADES_HOST_TIME_INITIALIZE,0,0,0,0,0,NULL));
    time_t before = time(NULL);
    assert(!call(HADES_HOST_TIME_NOW,0,0,0,0,0,&result));
    assert((int64_t)result >= before && (int64_t)result <= time(NULL));
    assert(!call(HADES_HOST_TIME_CURRENT,(uintptr_t)arena,0,0,0,0,&result) && !result);
    int64_t seconds = *(int64_t *)arena;
    assert(seconds >= before && seconds <= time(NULL));
    assert(call(HADES_HOST_TIME_CURRENT,1,0,0,0,0,NULL));
    assert(call(HADES_HOST_TIME_CURRENT,(uintptr_t)readonly,0,0,0,0,NULL));
    assert(!call(HADES_HOST_TIME_CALENDAR,(uintptr_t)arena+32,(uintptr_t)arena+64,(uintptr_t)arena,0,0,NULL));
    struct tm actual;
    assert(localtime_r((time_t *)arena,&actual));
    assert(*(int16_t *)(arena+32) == actual.tm_year+1900);
    assert(arena[34] == actual.tm_mon+1 && arena[35] == actual.tm_mday);
    assert(*(uint32_t *)(arena+64) == (uint32_t)actual.tm_wday && *(uint32_t *)(arena+68) == (uint32_t)actual.tm_yday);
    assert(*(int32_t *)(arena+84) == actual.tm_gmtoff);
    assert(call(HADES_HOST_TIME_CALENDAR,(uintptr_t)arena,1,(uintptr_t)arena,0,0,NULL));
    assert(call(HADES_HOST_ACCOUNT_INITIALIZE,0,0,0,0,0,NULL));
    assert(!hades_host_configure_user(argv[2]));
    assert(hades_host_configure_user(argv[2]));
    assert(!call(HADES_HOST_ACCOUNT_INITIALIZE,0,0,0,0,0,NULL));
    assert(!call(HADES_HOST_ACCOUNT_LAST,(uintptr_t)arena+96,0,0,0,0,NULL));
    unsigned char uid[16];
    memcpy(uid,arena+96,16);
    assert(!call(HADES_HOST_ACCOUNT_LIST,(uintptr_t)arena+128,(uintptr_t)arena+160,1,0,0,NULL));
    assert(*(int32_t *)(arena+128)==1 && !memcmp(uid,arena+160,16));
    assert(call(HADES_HOST_ACCOUNT_LIST,(uintptr_t)arena+128,(uintptr_t)arena+160,-1,0,0,NULL));
    assert(!call(HADES_HOST_ACCOUNT_PRESELECTED,(uintptr_t)arena+192,0,0,0,0,&result) && result==1);
    assert(!memcmp(arena+192,uid,16));
    assert(hades_host_shutdown());
    assert(!call(HADES_HOST_ACCOUNT_CLOSE,(uintptr_t)arena+192,0,0,0,0,NULL));
    assert(call(HADES_HOST_ACCOUNT_CLOSE,(uintptr_t)arena+192,0,0,0,0,NULL));
    assert(!call(HADES_HOST_FS_ENSURE_SAVE,(uintptr_t)arena+96,0,0,0,0,NULL));
    assert(!call(HADES_HOST_FS_ENSURE_SAVE,(uintptr_t)arena+96,0,0,0,0,NULL));
    arena[96] ^= 1;
    assert(call(HADES_HOST_FS_ENSURE_SAVE,(uintptr_t)arena+96,0,0,0,0,NULL));
    arena[96] ^= 1;
    strcpy((char *)arena+256,"save");
    assert(!call(HADES_HOST_FS_SAVE_MOUNT,(uintptr_t)arena+256,(uintptr_t)arena+96,0,0,0,NULL));
    strcpy((char *)arena+256,"save:/Data");
    assert(!call(HADES_HOST_FS_CREATE_DIRECTORY,(uintptr_t)arena+256,0,0,0,0,NULL));
    assert(!call(HADES_HOST_FS_CREATE_DIRECTORY,(uintptr_t)arena+256,0,0,0,0,&result) && result==0x402);
    strcpy((char *)arena+256,"save:/Data/Profile");
    assert(!call(HADES_HOST_FS_CREATE,(uintptr_t)arena+256,11,0,0,0,NULL));
    assert(!call(HADES_HOST_FS_OPEN,(uintptr_t)arena+384,(uintptr_t)arena+256,3,0,0,NULL));
    uint64_t saved_file = *(uint64_t *)(arena+384);
    memcpy(arena+512,"hello world",11);
    *(uint32_t *)(arena+416) = 1;
    assert(!call(HADES_HOST_FS_WRITE,saved_file,0,(uintptr_t)arena+512,11,(uintptr_t)arena+416,NULL));
    assert(!call(HADES_HOST_FS_FLUSH,saved_file,0,0,0,0,NULL));
    strcpy((char *)arena+256,"save");
    assert(!call(HADES_HOST_FS_COMMIT,(uintptr_t)arena+256,0,0,0,0,NULL));
    assert(call(HADES_HOST_FS_UNMOUNT,(uintptr_t)arena+256,0,0,0,0,NULL));
    assert(!call(HADES_HOST_FS_CLOSE,saved_file,0,0,0,0,NULL));
    strcpy((char *)arena+256,"save:/Data/Profile");
    assert(!call(HADES_HOST_FS_DELETE,(uintptr_t)arena+256,0,0,0,0,NULL));
    strcpy((char *)arena+256,"save");
    assert(!call(HADES_HOST_FS_UNMOUNT,(uintptr_t)arena+256,0,0,0,0,NULL));
    assert(!call(HADES_HOST_FS_SAVE_MOUNT,(uintptr_t)arena+256,(uintptr_t)arena+96,0,0,0,NULL));
    strcpy((char *)arena+256,"save:/Data/Profile");
    assert(!call(HADES_HOST_FS_OPEN,(uintptr_t)arena+384,(uintptr_t)arena+256,1,0,0,NULL));
    saved_file = *(uint64_t *)(arena+384);
    assert(!call(HADES_HOST_FS_READ,(uintptr_t)arena+400,saved_file,0,(uintptr_t)arena+512,11,NULL));
    assert(!memcmp(arena+512,"hello world",11));
    assert(!call(HADES_HOST_FS_CLOSE,saved_file,0,0,0,0,NULL));
    strcpy((char *)arena+256,"save:/Data");
    assert(!call(HADES_HOST_FS_OPEN_DIRECTORY,(uintptr_t)arena+384,(uintptr_t)arena+256,2,0,0,NULL));
    uint64_t directory = *(uint64_t *)(arena+384);
    assert(!call(HADES_HOST_FS_READ_DIRECTORY,(uintptr_t)arena+400,(uintptr_t)arena+1024,directory,1,0,NULL));
    assert(*(int64_t *)(arena+400)==1 && !strcmp((char *)arena+1024,"Profile"));
    assert(!call(HADES_HOST_FS_READ_DIRECTORY,(uintptr_t)arena+400,(uintptr_t)arena+1024,directory,1,0,NULL));
    assert(!*(int64_t *)(arena+400));
    assert(!call(HADES_HOST_FS_CLOSE_DIRECTORY,directory,0,0,0,0,NULL));
    assert(call(HADES_HOST_FS_CLOSE_DIRECTORY,directory,0,0,0,0,NULL));
    strcpy((char *)arena+256,"save");
    assert(!call(HADES_HOST_FS_UNMOUNT,(uintptr_t)arena+256,0,0,0,0,NULL));
    strcpy((char *)arena+256,"rom");
    assert(!call(HADES_HOST_FS_ROM_CACHE,(uintptr_t)arena+400,0,0,0,0,NULL));
    assert(*(uint64_t *)(arena+400)==4096);
    assert(!call(HADES_HOST_FS_ROM_MOUNT,(uintptr_t)arena+256,(uintptr_t)arena+12288,4096,0,0,NULL));
    strcpy((char *)arena+256,"rom:/Ordered");
    assert(!call(HADES_HOST_FS_OPEN_DIRECTORY,(uintptr_t)arena+384,(uintptr_t)arena+256,2,0,0,NULL));
    directory = *(uint64_t *)(arena+384);
    const char *ordered[] = {"10.shader","B.shader","a.shader","z.shader"};
    unsigned seen = 0;
    const char *order = getenv("HADES_FS_DIRECTORY_ORDER_PROBE");
    for (unsigned i=0;i<4;i++) {
        assert(!call(HADES_HOST_FS_READ_DIRECTORY,(uintptr_t)arena+400,(uintptr_t)arena+1024,directory,1,0,NULL));
        assert(*(int64_t *)(arena+400)==1 && arena[1024+0x304]==1
               && *(int64_t *)(arena+1024+0x308)==3);
        if (order && !strcmp(order,"1"))
            assert(!strcmp((char *)arena+1024,ordered[i]));
        unsigned j;
        for (j=0;j<4 && strcmp((char *)arena+1024,ordered[j]);j++);
        assert(j<4 && !(seen & (1u<<j)));
        seen |= 1u<<j;
    }
    assert(seen==15);
    assert(!call(HADES_HOST_FS_READ_DIRECTORY,(uintptr_t)arena+400,(uintptr_t)arena+1024,directory,1,0,NULL));
    assert(!*(int64_t *)(arena+400));
    assert(!call(HADES_HOST_FS_CLOSE_DIRECTORY,directory,0,0,0,0,NULL));
    assert(!call(HADES_HOST_FS_OPEN_DIRECTORY,(uintptr_t)arena+384,(uintptr_t)arena+256,1,0,0,NULL));
    directory = *(uint64_t *)(arena+384);
    assert(!call(HADES_HOST_FS_READ_DIRECTORY,(uintptr_t)arena+400,(uintptr_t)arena+1024,directory,1,0,NULL));
    assert(*(int64_t *)(arena+400)==1 && !strcmp((char *)arena+1024,"folder")
           && !arena[1024+0x304] && !*(int64_t *)(arena+1024+0x308));
    assert(!call(HADES_HOST_FS_CLOSE_DIRECTORY,directory,0,0,0,0,NULL));
    strcpy((char *)arena+256,"rom:/Hello.txt");
    assert(!call(HADES_HOST_FS_OPEN,(uintptr_t)arena+384,(uintptr_t)arena+256,1,0,0,NULL));
    uint64_t file = *(uint64_t *)(arena+384);
    assert(!call(HADES_HOST_FS_SIZE,(uintptr_t)arena+400,file,0,0,0,NULL));
    assert(*(int64_t *)(arena+400)==11);
    assert(!call(HADES_HOST_FS_ENABLE_CACHE,(uintptr_t)arena+8192,4096,0,0,0,NULL));
    assert(call(HADES_HOST_FS_ENABLE_CACHE,(uintptr_t)arena+8192,4096,0,0,0,NULL));
    assert(!call(HADES_HOST_FS_READ,(uintptr_t)arena+400,file,0,(uintptr_t)arena+512,32,NULL));
    assert(*(uint64_t *)(arena+400)==11 && !memcmp(arena+512,"hello world",11));
    assert(!memcmp(arena+8192,"hello world",11));
    assert(!call(HADES_HOST_FS_READ,(uintptr_t)arena+400,file,1,(uintptr_t)arena+512,5,NULL));
    assert(*(uint64_t *)(arena+400)==5 && !memcmp(arena+512,"ello ",5));
    assert(!call(HADES_HOST_FS_READ,(uintptr_t)arena+400,file,99,(uintptr_t)arena+512,5,NULL));
    assert(!*(uint64_t *)(arena+400));
    assert(call(HADES_HOST_FS_READ,(uintptr_t)arena+400,file,-1,(uintptr_t)arena+512,1,NULL));
    assert(call(HADES_HOST_FS_READ,(uintptr_t)arena+400,file,0,(uintptr_t)arena+8192,1,NULL));
    assert(!call(HADES_HOST_FS_DISABLE_CACHE,0,0,0,0,0,NULL));
    assert(!call(HADES_HOST_FS_READ,(uintptr_t)arena+400,file,0,(uintptr_t)arena+512,11,NULL));
    assert(!memcmp(arena+12288+32,"hello world",11));
    strcpy((char *)arena+256,"rom");
    assert(call(HADES_HOST_FS_UNMOUNT,(uintptr_t)arena+256,0,0,0,0,NULL));
    assert(hades_host_shutdown());
    assert(!call(HADES_HOST_FS_CLOSE,file,0,0,0,0,NULL));
    assert(call(HADES_HOST_FS_CLOSE,file,0,0,0,0,NULL));
    strcpy((char *)arena+256,"rom:/Hello.txt");
    assert(call(HADES_HOST_FS_OPEN,(uintptr_t)arena+384,(uintptr_t)arena+256,2,0,0,NULL));
    const char *missing[] = {"rom:/missing","rom:/hello.txt"};
    for (unsigned i=0;i<2;i++) {
        strcpy((char *)arena+256,missing[i]);
        assert(!call(HADES_HOST_FS_OPEN,(uintptr_t)arena+384,(uintptr_t)arena+256,1,0,0,&result) && result==0x202);
    }
    const char *invalid[] = {"other:/Hello.txt","rom:/../Hello.txt","rom:/./Hello.txt","rom:/link"};
    for (unsigned i=0;i<4;i++) {
        strcpy((char *)arena+256,invalid[i]);
        assert(call(HADES_HOST_FS_OPEN,(uintptr_t)arena+384,(uintptr_t)arena+256,1,0,0,NULL));
    }
    strcpy((char *)arena+256,"rom:/Hello.txt");
    assert(!call(HADES_HOST_FS_ENTRY_TYPE,(uintptr_t)arena+400,(uintptr_t)arena+256,0,0,0,NULL));
    assert(*(uint32_t *)(arena+400)==1);
    strcpy((char *)arena+256,"rom:/");
    assert(!call(HADES_HOST_FS_ENTRY_TYPE,(uintptr_t)arena+400,(uintptr_t)arena+256,0,0,0,NULL));
    assert(!*(uint32_t *)(arena+400));
    assert(call(HADES_HOST_COUNT,0,0,0,0,0,NULL));
    assert(!hades_host_shutdown());
    assert(!hades_host_initialize(argv[1],"720p",NULL,0) && !hades_host_configure_user(argv[2]));
    assert(!call(HADES_HOST_ACCOUNT_INITIALIZE,0,0,0,0,0,NULL));
    assert(!call(HADES_HOST_ACCOUNT_LAST,(uintptr_t)arena+96,0,0,0,0,NULL) && !memcmp(arena+96,uid,16));
    assert(!hades_host_shutdown());
    const unsigned char variant[]="X = 1170\n";
    hades_ui_overlay overlay={.path="Content/Test.sjson",.source_size=11,
                              .size=sizeof variant-1,.data=variant};
    CC_SHA256("hello world",11,overlay.source_digest);
    assert(hades_host_initialize(argv[1],"720p",&overlay,1));
    assert(hades_host_initialize(argv[1],"wide-720p",NULL,0));
    assert(!hades_host_initialize(argv[1],"wide-720p",&overlay,1));
    assert(!call(HADES_HOST_FS_INITIALIZE,0,0,0,0,0,NULL));
    strcpy((char *)arena+256,"rom");
    assert(!call(HADES_HOST_FS_ROM_MOUNT,(uintptr_t)arena+256,(uintptr_t)arena+12288,4096,0,0,NULL));
    strcpy((char *)arena+256,"rom:/Content/Test.sjson");
    overlay.source_digest[0]^=1;
    assert(call(HADES_HOST_FS_OPEN,(uintptr_t)arena+384,(uintptr_t)arena+256,1,0,0,NULL));
    overlay.source_digest[0]^=1;
    overlay.source_size++;
    assert(call(HADES_HOST_FS_OPEN,(uintptr_t)arena+384,(uintptr_t)arena+256,1,0,0,NULL));
    overlay.source_size--;
    assert(!call(HADES_HOST_FS_OPEN,(uintptr_t)arena+384,(uintptr_t)arena+256,1,0,0,NULL));
    file=*(uint64_t *)(arena+384);
    assert(!call(HADES_HOST_FS_SIZE,(uintptr_t)arena+400,file,0,0,0,NULL));
    assert(*(uint64_t *)(arena+400)==sizeof variant-1);
    assert(!call(HADES_HOST_FS_READ,(uintptr_t)arena+400,file,0,(uintptr_t)arena+512,64,NULL));
    assert(*(uint64_t *)(arena+400)==sizeof variant-1 && !memcmp(arena+512,variant,sizeof variant-1));
    assert(!call(HADES_HOST_FS_READ,(uintptr_t)arena+400,file,3,(uintptr_t)arena+512,4,NULL));
    assert(*(uint64_t *)(arena+400)==4 && !memcmp(arena+512,variant+3,4));
    assert(!call(HADES_HOST_FS_READ,(uintptr_t)arena+400,file,sizeof variant,(uintptr_t)arena+512,4,NULL));
    assert(!*(uint64_t *)(arena+400));
    assert(!call(HADES_HOST_FS_CLOSE,file,0,0,0,0,NULL));
    strcpy((char *)arena+256,"rom:/Content");
    assert(!call(HADES_HOST_FS_OPEN_DIRECTORY,(uintptr_t)arena+384,(uintptr_t)arena+256,2,0,0,NULL));
    directory=*(uint64_t *)(arena+384);
    assert(!call(HADES_HOST_FS_READ_DIRECTORY,(uintptr_t)arena+400,(uintptr_t)arena+1024,directory,1,0,NULL));
    assert(*(int64_t *)(arena+400)==1 && !strcmp((char *)arena+1024,"Test.sjson")
           && *(int64_t *)(arena+1024+0x308)==sizeof variant-1);
    assert(!call(HADES_HOST_FS_CLOSE_DIRECTORY,directory,0,0,0,0,NULL));
    assert(!hades_host_shutdown());
    assert(!hades_host_initialize(argv[1],"720p",NULL,0));
    assert(!call(HADES_HOST_FS_INITIALIZE,0,0,0,0,0,NULL));
    strcpy((char *)arena+256,"rom");
    assert(!call(HADES_HOST_FS_ROM_MOUNT,(uintptr_t)arena+256,(uintptr_t)arena+12288,4096,0,0,NULL));
    strcpy((char *)arena+256,"rom:/Content/Test.sjson");
    assert(!call(HADES_HOST_FS_OPEN,(uintptr_t)arena+384,(uintptr_t)arena+256,1,0,0,NULL));
    file=*(uint64_t *)(arena+384);
    assert(!call(HADES_HOST_FS_SIZE,(uintptr_t)arena+400,file,0,0,0,NULL));
    assert(*(int64_t *)(arena+400)==11);
    assert(!call(HADES_HOST_FS_READ,(uintptr_t)arena+400,file,0,(uintptr_t)arena+512,64,NULL));
    assert(*(uint64_t *)(arena+400)==11 && !memcmp(arena+512,"hello world",11));
    assert(!call(HADES_HOST_FS_CLOSE,file,0,0,0,0,NULL));
    assert(!hades_host_shutdown());
    assert(!hades_memory_unregister(99) && !hades_memory_unregister(100) && !hades_memory_shutdown());
    puts("HOST_BACKEND_OK: real file/cache/clock/calendar, durable local identity/save namespace, owned handles, invalid buffers/paths rejected");
    return 0;
}
