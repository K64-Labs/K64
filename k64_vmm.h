// k64_vmm.h – per-service virtual memory reservations
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void k64_vmm_init(void);

typedef struct {
    bool     present;
    uint64_t cr3;
    uint64_t root_base;
    uint64_t root_size;
    uint64_t stack_base;
    uint64_t stack_size;
    uint64_t heap_base;
    uint64_t heap_size;
    uint64_t page_table_frames[16];
    size_t   page_table_frame_count;
    uint64_t phys_frames[512];
    size_t   phys_frame_count;
} k64_vm_space_t;

bool k64_vmm_alloc_service_space(uint64_t pid, k64_vm_space_t* out_space);
void k64_vmm_release_service_space(k64_vm_space_t* space);
bool k64_vmm_map_private_range(k64_vm_space_t* space,
                               uint64_t virt_addr,
                               const uint8_t* data,
                               size_t file_size,
                               size_t mem_size);
uint64_t k64_vmm_call_isolated(const k64_vm_space_t* space,
                               uint64_t entry,
                               uint64_t arg0,
                               uint64_t arg1,
                               uint64_t arg2);
