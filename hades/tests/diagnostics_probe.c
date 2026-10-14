#include "../runtime/diagnostics.h"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
int main(void) {
    hades_memory_observation before,after;
    assert(hades_observe_memory(NULL)==-1);
    assert(!hades_observe_memory(&before));
    const size_t size=32u*1024*1024;
    volatile unsigned char *pages=malloc(size);
    assert(pages);
    long page=sysconf(_SC_PAGESIZE);
    assert(page>0);
    for (size_t offset=0;offset<size;offset+=(size_t)page)
        pages[offset]=42;
    assert(!hades_observe_memory(&after));
    assert(after.resident>=before.resident+24u*1024*1024);
    assert(after.physical_footprint>=before.physical_footprint+24u*1024*1024);
    assert(after.resident_peak>=after.resident);
    assert(after.virtual_size>=after.resident);
    hades_log_memory("synthetic-private-allocation",0,0,0);
    free((void *)pages);
    puts("ACTUAL_SELF_MEMORY_ACCOUNTING_OK");
    return 0;
}
