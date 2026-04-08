// k64_pmm.h – physical memory manager
#pragma once
#include <stddef.h>
#include <stdint.h>

void   k64_pmm_init(void);
void*  k64_pmm_alloc_frame(void);
void*  k64_pmm_alloc_contiguous(size_t frames);
void   k64_pmm_free_frame(void* frame);
void   k64_pmm_free_contiguous(void* frame, size_t frames);
size_t k64_pmm_total_frames(void);
size_t k64_pmm_used_frames(void);
