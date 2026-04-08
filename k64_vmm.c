// k64_vmm.c
#include "k64_vmm.h"
#include "k64_log.h"
#include "k64_pmm.h"

#define K64_SERVICE_VM_BASE      0x0000000040000000ULL
#define K64_SERVICE_VM_STRIDE    0x0000000001000000ULL
#define K64_SERVICE_VM_ROOT_SIZE 0x0000000001000000ULL
#define K64_SERVICE_VM_STACK_SIZE 0x0000000000008000ULL
#define K64_SERVICE_VM_HEAP_SIZE  0x0000000000100000ULL
#define K64_SERVICE_VM_MAX_SLOTS  256
#define K64_SERVICE_STACK_FRAMES  (K64_SERVICE_VM_STACK_SIZE / 4096ULL)

static bool service_slot_used[K64_SERVICE_VM_MAX_SLOTS];

static void vmm_clear_space(k64_vm_space_t* space) {
    if (!space) {
        return;
    }

    space->present = false;
    space->root_base = 0;
    space->root_size = 0;
    space->stack_base = 0;
    space->stack_size = 0;
    space->heap_base = 0;
    space->heap_size = 0;
    space->phys_frame_count = 0;
    for (size_t i = 0; i < 8; ++i) {
        space->phys_frames[i] = 0;
    }
}

void k64_vmm_init(void) {
    for (size_t i = 0; i < K64_SERVICE_VM_MAX_SLOTS; ++i) {
        service_slot_used[i] = false;
    }

    K64_LOG_INFO("VMM: initialized service VM reservation pool.");
}

bool k64_vmm_alloc_service_space(uint64_t pid, k64_vm_space_t* out_space) {
    size_t slot;

    if (!out_space) {
        return false;
    }

    vmm_clear_space(out_space);

    if (pid == 0) {
        out_space->present = true;
        out_space->root_base = 0;
        out_space->root_size = 0x40000000ULL;
        return true;
    }

    slot = (size_t)(pid % K64_SERVICE_VM_MAX_SLOTS);
    if (service_slot_used[slot]) {
        return false;
    }

    service_slot_used[slot] = true;
    out_space->present = true;
    out_space->root_base = K64_SERVICE_VM_BASE + ((uint64_t)slot * K64_SERVICE_VM_STRIDE);
    out_space->root_size = K64_SERVICE_VM_ROOT_SIZE;
    out_space->heap_base = out_space->root_base + 0x00100000ULL;
    out_space->heap_size = K64_SERVICE_VM_HEAP_SIZE;
    out_space->stack_size = K64_SERVICE_VM_STACK_SIZE;
    out_space->stack_base = out_space->root_base + out_space->root_size - out_space->stack_size;

    for (size_t i = 0; i < K64_SERVICE_STACK_FRAMES && i < 8; ++i) {
        void* frame = k64_pmm_alloc_frame();
        if (!frame) {
            k64_vmm_release_service_space(out_space);
            return false;
        }
        out_space->phys_frames[out_space->phys_frame_count++] = (uint64_t)(uintptr_t)frame;
    }

    return true;
}

void k64_vmm_release_service_space(k64_vm_space_t* space) {
    size_t slot;

    if (!space || !space->present) {
        return;
    }

    for (size_t i = 0; i < space->phys_frame_count; ++i) {
        if (space->phys_frames[i] != 0) {
            k64_pmm_free_frame((void*)(uintptr_t)space->phys_frames[i]);
        }
    }

    if (space->root_base >= K64_SERVICE_VM_BASE) {
        slot = (size_t)((space->root_base - K64_SERVICE_VM_BASE) / K64_SERVICE_VM_STRIDE);
        if (slot < K64_SERVICE_VM_MAX_SLOTS) {
            service_slot_used[slot] = false;
        }
    }

    vmm_clear_space(space);
}
