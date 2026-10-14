#pragma once
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint64_t resident, resident_peak, physical_footprint, virtual_size;
} hades_memory_observation;

static inline int hades_observe_memory(hades_memory_observation *result) {
    if (!result) {
        fprintf(stderr,"[hades-memory-observation] Missing output\n");
        return -1;
    }
    task_vm_info_data_t info={0};
    mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
    kern_return_t status=task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&info,&count);
    if (status!=KERN_SUCCESS || count<TASK_VM_INFO_REV1_COUNT) {
        fprintf(stderr,"[hades-memory-observation] TASK_VM_INFO unavailable status=%d count=%u\n",status,count);
        return -1;
    }
    *result=(hades_memory_observation){info.resident_size,info.resident_size_peak,
                                     info.phys_footprint,info.virtual_size};
    return 0;
}
static inline void hades_log_memory(const char *phase,uint64_t frame,int metal_observed,uint64_t metal_allocated) {
    hades_memory_observation value;
    if (hades_observe_memory(&value))
        return;
    char metal[32]="unobserved";
    if (metal_observed)
        snprintf(metal,sizeof metal,"%llu",(unsigned long long)metal_allocated);
    fprintf(stderr,"[hades-memory-observation] phase=%s frame=%llu resident=%llu resident-peak=%llu footprint=%llu virtual=%llu metal-allocated=%s bytes; overlapping accounts, do not sum\n",
            phase,(unsigned long long)frame,(unsigned long long)value.resident,
            (unsigned long long)value.resident_peak,(unsigned long long)value.physical_footprint,
            (unsigned long long)value.virtual_size,metal);
}
