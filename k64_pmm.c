// k64_pmm.c
#include "k64_pmm.h"
#include "k64_multiboot.h"
#include "k64_log.h"
#include "k64_terminal.h"

#define K64_FRAME_SIZE 4096
#define K64_MAX_FRAMES (1024 * 1024) /* up to 4 GiB */

extern char _kernel_end;

static uint8_t  pmm_bitmap[K64_MAX_FRAMES / 8];
static size_t   total_frames = 0;
static size_t   used_frames  = 0;

static inline void set_frame(size_t frame) {
    pmm_bitmap[frame / 8] |= (1 << (frame % 8));
}

static inline void clear_frame(size_t frame) {
    pmm_bitmap[frame / 8] &= ~(1 << (frame % 8));
}

static inline int test_frame(size_t frame) {
    return pmm_bitmap[frame / 8] & (1 << (frame % 8));
}

static void reserve_range(uint64_t start, uint64_t end) {
    if (end <= start) {
        return;
    }

    start &= ~(uint64_t)(K64_FRAME_SIZE - 1);
    end = (end + K64_FRAME_SIZE - 1) & ~(uint64_t)(K64_FRAME_SIZE - 1);
    for (uint64_t addr = start; addr < end; addr += K64_FRAME_SIZE) {
        size_t frame = (size_t)(addr / K64_FRAME_SIZE);
        if (frame < K64_MAX_FRAMES && !test_frame(frame)) {
            set_frame(frame);
            used_frames++;
        }
    }
}

void k64_pmm_init(void) {
    if (k64_mb_magic != 0x2BADB002) {
        K64_LOG_ERROR("PMM: invalid Multiboot magic.");
        return;
    }

    multiboot_info_t* mb = (multiboot_info_t*)(uintptr_t)k64_mb_info;

    for (size_t i = 0; i < sizeof(pmm_bitmap); ++i) pmm_bitmap[i] = 0xFF;

    if (!(mb->flags & (1 << 6))) {
        K64_LOG_ERROR("PMM: no memory map.");
        return;
    }

    multiboot_mmap_entry_t* mmap = (multiboot_mmap_entry_t*)(uintptr_t)mb->mmap_addr;
    multiboot_mmap_entry_t* mmap_end = (multiboot_mmap_entry_t*)((uintptr_t)mmap + mb->mmap_length);

    total_frames = 0;
    used_frames  = 0;

    for (; mmap < mmap_end; mmap = (multiboot_mmap_entry_t*)((uintptr_t)mmap + mmap->size + sizeof(mmap->size))) {
        if (mmap->type != 1) continue;
        uint64_t region_start = mmap->addr;
        uint64_t region_end   = mmap->addr + mmap->len;

        for (uint64_t addr = region_start; addr < region_end; addr += K64_FRAME_SIZE) {
            size_t frame = (size_t)(addr / K64_FRAME_SIZE);
            if (frame >= K64_MAX_FRAMES) break;
            clear_frame(frame);
            total_frames++;
        }
    }

    reserve_range(0, (uint64_t)(uintptr_t)&_kernel_end);
    reserve_range((uint64_t)(uintptr_t)mb, (uint64_t)(uintptr_t)mb + sizeof(*mb));
    if (mb->flags & (1 << 6)) {
        reserve_range((uint64_t)mb->mmap_addr, (uint64_t)mb->mmap_addr + mb->mmap_length);
    }
    if (mb->flags & (1 << 3)) {
        multiboot_module_t* mods = (multiboot_module_t*)(uintptr_t)mb->mods_addr;
        reserve_range((uint64_t)(uintptr_t)mods, (uint64_t)(uintptr_t)mods + (uint64_t)mb->mods_count * sizeof(*mods));
        for (uint32_t i = 0; i < mb->mods_count; ++i) {
            reserve_range((uint64_t)mods[i].mod_start, (uint64_t)mods[i].mod_end);
        }
    }

    K64_LOG_INFO("PMM initialized.");
    k64_term_write("PMM: total frames=");
    k64_term_write_dec(total_frames);
    k64_term_putc('\n');
}

void* k64_pmm_alloc_frame(void) {
    for (size_t frame = 0; frame < total_frames && frame < K64_MAX_FRAMES; ++frame) {
        if (!test_frame(frame)) {
            set_frame(frame);
            used_frames++;
            return (void*)(uintptr_t)(frame * K64_FRAME_SIZE);
        }
    }
    return NULL;
}

void* k64_pmm_alloc_contiguous(size_t frames) {
    size_t limit;

    if (frames == 0 || frames > K64_MAX_FRAMES) {
        return NULL;
    }

    limit = total_frames < K64_MAX_FRAMES ? total_frames : K64_MAX_FRAMES;
    for (size_t frame = 0; frame + frames <= limit; ++frame) {
        bool free_run = true;

        for (size_t j = 0; j < frames; ++j) {
            if (test_frame(frame + j)) {
                free_run = false;
                frame += j;
                break;
            }
        }
        if (!free_run) {
            continue;
        }

        for (size_t j = 0; j < frames; ++j) {
            set_frame(frame + j);
            used_frames++;
        }
        return (void*)(uintptr_t)(frame * K64_FRAME_SIZE);
    }

    return NULL;
}

void k64_pmm_free_frame(void* frame_addr) {
    size_t frame = (size_t)((uintptr_t)frame_addr / K64_FRAME_SIZE);
    if (frame < K64_MAX_FRAMES && test_frame(frame)) {
        clear_frame(frame);
        used_frames--;
    }
}

void k64_pmm_free_contiguous(void* frame_addr, size_t frames) {
    size_t frame = (size_t)((uintptr_t)frame_addr / K64_FRAME_SIZE);

    for (size_t i = 0; i < frames; ++i) {
        size_t current = frame + i;
        if (current < K64_MAX_FRAMES && test_frame(current)) {
            clear_frame(current);
            used_frames--;
        }
    }
}

size_t k64_pmm_total_frames(void) {
    return total_frames;
}

size_t k64_pmm_used_frames(void) {
    return used_frames;
}
