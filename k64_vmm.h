// k64_vmm.h – per-service virtual memory reservations
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void k64_vmm_init(void);

typedef struct {
    bool     present;
    uint64_t root_base;
    uint64_t root_size;
    uint64_t stack_base;
    uint64_t stack_size;
    uint64_t heap_base;
    uint64_t heap_size;
    uint64_t phys_frames[8];
    size_t   phys_frame_count;
} k64_vm_space_t;

bool k64_vmm_alloc_service_space(uint64_t pid, k64_vm_space_t* out_space);
void k64_vmm_release_service_space(k64_vm_space_t* space);
