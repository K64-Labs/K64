// k64_vmm.c
#include "k64_vmm.h"
#include "k64_log.h"
#include "k64_pmm.h"

#define K64_PAGE_SIZE             4096ULL
#define K64_PAGE_MASK             (~(K64_PAGE_SIZE - 1ULL))
#define K64_PAGE_PRESENT          0x001ULL
#define K64_PAGE_RW               0x002ULL
#define K64_PAGE_TABLE_FLAGS      (K64_PAGE_PRESENT | K64_PAGE_RW)
#define K64_SERVICE_VM_BASE       0x0000000040000000ULL
#define K64_SERVICE_VM_STRIDE     0x0000000001000000ULL
#define K64_SERVICE_VM_ROOT_SIZE  0x0000000001000000ULL
#define K64_SERVICE_VM_STACK_SIZE 0x0000000000008000ULL
#define K64_SERVICE_VM_HEAP_SIZE  0x0000000000100000ULL
#define K64_SERVICE_VM_MAX_SLOTS  256
#define K64_SERVICE_STACK_FRAMES  (K64_SERVICE_VM_STACK_SIZE / K64_PAGE_SIZE)

static bool service_slot_used[K64_SERVICE_VM_MAX_SLOTS];
static uint64_t kernel_cr3 = 0;

extern uint64_t k64_vmm_call_asm(uint64_t new_cr3,
                                 uint64_t new_rsp,
                                 uint64_t entry,
                                 uint64_t arg0,
                                 uint64_t arg1,
                                 uint64_t arg2);

static uint64_t vmm_read_cr3(void) {
    uint64_t value;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(value));
    return value & K64_PAGE_MASK;
}

static void vmm_clear_page(void* page) {
    uint8_t* bytes = (uint8_t*)page;
    for (size_t i = 0; i < K64_PAGE_SIZE; ++i) {
        bytes[i] = 0;
    }
}

static void vmm_clear_space(k64_vm_space_t* space) {
    if (!space) {
        return;
    }

    space->present = false;
    space->cr3 = 0;
    space->root_base = 0;
    space->root_size = 0;
    space->stack_base = 0;
    space->stack_size = 0;
    space->heap_base = 0;
    space->heap_size = 0;
    space->page_table_frame_count = 0;
    space->phys_frame_count = 0;
    for (size_t i = 0; i < 16; ++i) {
        space->page_table_frames[i] = 0;
    }
    for (size_t i = 0; i < 512; ++i) {
        space->phys_frames[i] = 0;
    }
}

static bool vmm_record_frame(uint64_t* list, size_t* count, size_t capacity, uint64_t frame) {
    if (!list || !count || *count >= capacity) {
        return false;
    }
    list[*count] = frame;
    (*count)++;
    return true;
}

static uint64_t* vmm_alloc_table_frame(k64_vm_space_t* space) {
    void* frame = k64_pmm_alloc_frame();

    if (!frame) {
        return NULL;
    }
    if (!vmm_record_frame(space->page_table_frames,
                          &space->page_table_frame_count,
                          sizeof(space->page_table_frames) / sizeof(space->page_table_frames[0]),
                          (uint64_t)(uintptr_t)frame)) {
        k64_pmm_free_frame(frame);
        return NULL;
    }

    vmm_clear_page(frame);
    return (uint64_t*)(uintptr_t)frame;
}

static uint64_t* vmm_next_table(k64_vm_space_t* space, uint64_t* table, size_t index) {
    uint64_t entry = table[index];
    uint64_t* next;

    if ((entry & K64_PAGE_PRESENT) != 0) {
        if ((entry & (1ULL << 7)) != 0) {
            return NULL;
        }
        return (uint64_t*)(uintptr_t)(entry & K64_PAGE_MASK);
    }

    next = vmm_alloc_table_frame(space);
    if (!next) {
        return NULL;
    }
    table[index] = ((uint64_t)(uintptr_t)next) | K64_PAGE_TABLE_FLAGS;
    return next;
}

static bool vmm_map_page(k64_vm_space_t* space, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    uint64_t* pml4;
    uint64_t* pdpt;
    uint64_t* pdt;
    uint64_t* pt;
    size_t pml4_index;
    size_t pdpt_index;
    size_t pdt_index;
    size_t pt_index;

    if (!space || !space->present || space->cr3 == 0) {
        return false;
    }

    pml4 = (uint64_t*)(uintptr_t)space->cr3;
    pml4_index = (size_t)((virt_addr >> 39) & 0x1FFULL);
    pdpt_index = (size_t)((virt_addr >> 30) & 0x1FFULL);
    pdt_index = (size_t)((virt_addr >> 21) & 0x1FFULL);
    pt_index = (size_t)((virt_addr >> 12) & 0x1FFULL);

    pdpt = vmm_next_table(space, pml4, pml4_index);
    if (!pdpt) {
        return false;
    }
    pdt = vmm_next_table(space, pdpt, pdpt_index);
    if (!pdt) {
        return false;
    }
    pt = vmm_next_table(space, pdt, pdt_index);
    if (!pt) {
        return false;
    }

    pt[pt_index] = (phys_addr & K64_PAGE_MASK) | (flags & 0xFFFULL) | K64_PAGE_PRESENT;
    return true;
}

static bool vmm_clone_kernel_root(k64_vm_space_t* space) {
    uint64_t* new_pml4;
    uint64_t* new_pdpt;
    uint64_t* kernel_pml4;
    uint64_t* kernel_pdpt;

    if (kernel_cr3 == 0) {
        return false;
    }

    kernel_pml4 = (uint64_t*)(uintptr_t)kernel_cr3;
    if ((kernel_pml4[0] & K64_PAGE_PRESENT) == 0) {
        return false;
    }
    kernel_pdpt = (uint64_t*)(uintptr_t)(kernel_pml4[0] & K64_PAGE_MASK);

    new_pml4 = vmm_alloc_table_frame(space);
    new_pdpt = vmm_alloc_table_frame(space);
    if (!new_pml4 || !new_pdpt) {
        return false;
    }

    for (size_t i = 0; i < 512; ++i) {
        new_pml4[i] = kernel_pml4[i];
        new_pdpt[i] = kernel_pdpt[i];
    }

    new_pml4[0] = ((uint64_t)(uintptr_t)new_pdpt) | K64_PAGE_TABLE_FLAGS;
    space->cr3 = (uint64_t)(uintptr_t)new_pml4;
    return true;
}

static bool vmm_map_zeroed_page(k64_vm_space_t* space, uint64_t virt_addr) {
    void* frame = k64_pmm_alloc_frame();

    if (!frame) {
        return false;
    }
    if (!vmm_record_frame(space->phys_frames,
                          &space->phys_frame_count,
                          sizeof(space->phys_frames) / sizeof(space->phys_frames[0]),
                          (uint64_t)(uintptr_t)frame)) {
        k64_pmm_free_frame(frame);
        return false;
    }

    vmm_clear_page(frame);
    return vmm_map_page(space, virt_addr, (uint64_t)(uintptr_t)frame, K64_PAGE_RW);
}

void k64_vmm_init(void) {
    kernel_cr3 = vmm_read_cr3();
    for (size_t i = 0; i < K64_SERVICE_VM_MAX_SLOTS; ++i) {
        service_slot_used[i] = false;
    }

    K64_LOG_INFO("VMM: initialized isolated address-space pool.");
}

bool k64_vmm_alloc_service_space(uint64_t pid, k64_vm_space_t* out_space) {
    size_t start_slot;
    size_t slot;

    if (!out_space) {
        return false;
    }

    vmm_clear_space(out_space);

    if (pid == 0) {
        out_space->present = true;
        out_space->cr3 = kernel_cr3;
        out_space->root_base = 0;
        out_space->root_size = 0x40000000ULL;
        return true;
    }

    start_slot = (size_t)(pid % K64_SERVICE_VM_MAX_SLOTS);
    slot = start_slot;
    while (service_slot_used[slot]) {
        slot = (slot + 1) % K64_SERVICE_VM_MAX_SLOTS;
        if (slot == start_slot) {
            return false;
        }
    }

    service_slot_used[slot] = true;
    out_space->present = true;
    out_space->root_base = K64_SERVICE_VM_BASE + ((uint64_t)slot * K64_SERVICE_VM_STRIDE);
    out_space->root_size = K64_SERVICE_VM_ROOT_SIZE;
    out_space->heap_base = out_space->root_base + 0x00100000ULL;
    out_space->heap_size = K64_SERVICE_VM_HEAP_SIZE;
    out_space->stack_size = K64_SERVICE_VM_STACK_SIZE;
    out_space->stack_base = out_space->root_base + out_space->root_size - out_space->stack_size;

    if (!vmm_clone_kernel_root(out_space)) {
        k64_vmm_release_service_space(out_space);
        return false;
    }

    for (size_t i = 0; i < K64_SERVICE_STACK_FRAMES; ++i) {
        if (!vmm_map_zeroed_page(out_space, out_space->stack_base + (i * K64_PAGE_SIZE))) {
            k64_vmm_release_service_space(out_space);
            return false;
        }
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
    for (size_t i = 0; i < space->page_table_frame_count; ++i) {
        if (space->page_table_frames[i] != 0) {
            k64_pmm_free_frame((void*)(uintptr_t)space->page_table_frames[i]);
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

bool k64_vmm_map_private_range(k64_vm_space_t* space,
                               uint64_t virt_addr,
                               const uint8_t* data,
                               size_t file_size,
                               size_t mem_size) {
    uint64_t page_start;
    uint64_t page_end;

    if (!space || !space->present || mem_size == 0) {
        return false;
    }

    page_start = virt_addr & K64_PAGE_MASK;
    page_end = (virt_addr + mem_size + K64_PAGE_SIZE - 1ULL) & K64_PAGE_MASK;

    for (uint64_t page = page_start; page < page_end; page += K64_PAGE_SIZE) {
        void* frame = k64_pmm_alloc_frame();
        uint8_t* bytes;
        size_t copy_start;
        size_t copy_end;

        if (!frame) {
            return false;
        }
        if (!vmm_record_frame(space->phys_frames,
                              &space->phys_frame_count,
                              sizeof(space->phys_frames) / sizeof(space->phys_frames[0]),
                              (uint64_t)(uintptr_t)frame)) {
            k64_pmm_free_frame(frame);
            return false;
        }

        vmm_clear_page(frame);
        bytes = (uint8_t*)frame;

        copy_start = page < virt_addr ? (size_t)(virt_addr - page) : 0;
        copy_end = K64_PAGE_SIZE;
        if (page + copy_end > virt_addr + file_size) {
            if (virt_addr + file_size <= page) {
                copy_end = copy_start;
            } else {
                copy_end = (size_t)((virt_addr + file_size) - page);
            }
        }
        if (copy_end > copy_start && data) {
            size_t src_offset = (size_t)((page + copy_start) - virt_addr);
            size_t copy_size = copy_end - copy_start;
            for (size_t i = 0; i < copy_size; ++i) {
                bytes[copy_start + i] = data[src_offset + i];
            }
        }

        if (!vmm_map_page(space, page, (uint64_t)(uintptr_t)frame, K64_PAGE_RW)) {
            return false;
        }
    }

    return true;
}

uint64_t k64_vmm_call_isolated(const k64_vm_space_t* space,
                               uint64_t entry,
                               uint64_t arg0,
                               uint64_t arg1,
                               uint64_t arg2) {
    typedef uint64_t (*k64_call3_fn)(uint64_t, uint64_t, uint64_t);
    uint64_t stack_top;

    if (entry == 0) {
        return 0;
    }
    if (!space || !space->present || space->cr3 == 0 || space->cr3 == kernel_cr3) {
        return ((k64_call3_fn)(uintptr_t)entry)(arg0, arg1, arg2);
    }

    stack_top = (space->stack_base + space->stack_size) & ~0xFULL;
    return k64_vmm_call_asm(space->cr3, stack_top, entry, arg0, arg1, arg2);
}
