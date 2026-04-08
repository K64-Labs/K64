#include "k64_hotreload.h"
#include <stddef.h>
#include <stdint.h>
#include "k64_fs.h"
#include "k64_log.h"
#include "k64_multiboot.h"
#include "k64_pmm.h"
#include "k64_string.h"
#include "k64_terminal.h"

extern uint32_t k64_mb_magic;
extern uint32_t k64_mb_info;
extern uint8_t k64_hotreload_trampoline_start;
extern uint8_t k64_hotreload_trampoline_end;

#define K64_ELF64 2
#define K64_ELF_LITTLE 1
#define K64_ELF_EXEC 2
#define K64_ELF_X86_64 62
#define K64_PT_LOAD 1
#define K64_HOTRELOAD_MAX_OPS 8

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) k64_elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) k64_elf64_phdr_t;

typedef struct {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed)) k64_elf64_sym_t;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} __attribute__((packed)) k64_elf64_shdr_t;

typedef struct {
    uint64_t dst;
    uint64_t src;
    uint64_t copy_size;
    uint64_t zero_size;
} k64_hotreload_op_t;

typedef struct {
    uint64_t op_count;
    uint64_t ops_ptr;
    uint64_t mb_magic_addr;
    uint64_t mb_info_addr;
    uint32_t mb_magic_value;
    uint32_t mb_info_value;
    uint64_t entry;
    uint64_t stack_top;
} k64_hotreload_plan_t;

static void hotreload_copy(uint8_t* dst, const uint8_t* src, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        dst[i] = src[i];
    }
}

static const char* hotreload_strtab_string(const uint8_t* file_data,
                                           size_t file_size,
                                           const k64_elf64_shdr_t* strtab,
                                           uint32_t off) {
    if (!strtab || strtab->sh_offset >= file_size || off >= strtab->sh_size) {
        return NULL;
    }
    if (strtab->sh_offset + off >= file_size) {
        return NULL;
    }
    return (const char*)(file_data + strtab->sh_offset + off);
}

static bool hotreload_find_symbol_value(const uint8_t* file_data,
                                        size_t file_size,
                                        const char* symbol,
                                        uint64_t* value_out) {
    const k64_elf64_ehdr_t* ehdr;
    const k64_elf64_shdr_t* shdrs;

    if (!file_data || file_size < sizeof(k64_elf64_ehdr_t) || !symbol || !value_out) {
        return false;
    }

    ehdr = (const k64_elf64_ehdr_t*)file_data;
    if (ehdr->e_shoff == 0 || ehdr->e_shentsize != sizeof(k64_elf64_shdr_t) ||
        ehdr->e_shoff + (uint64_t)ehdr->e_shnum * sizeof(k64_elf64_shdr_t) > file_size) {
        return false;
    }

    shdrs = (const k64_elf64_shdr_t*)(file_data + ehdr->e_shoff);
    for (uint16_t i = 0; i < ehdr->e_shnum; ++i) {
        const k64_elf64_shdr_t* sh = &shdrs[i];
        const k64_elf64_shdr_t* strtab;
        const k64_elf64_sym_t* syms;
        size_t count;

        if (sh->sh_type != 2 || sh->sh_entsize != sizeof(k64_elf64_sym_t)) {
            continue;
        }
        if (sh->sh_link >= ehdr->e_shnum) {
            continue;
        }
        if (sh->sh_offset + sh->sh_size > file_size) {
            continue;
        }

        strtab = &shdrs[sh->sh_link];
        syms = (const k64_elf64_sym_t*)(file_data + sh->sh_offset);
        count = (size_t)(sh->sh_size / sh->sh_entsize);
        for (size_t si = 0; si < count; ++si) {
            const char* name = hotreload_strtab_string(file_data, file_size, strtab, syms[si].st_name);
            if (name && name[0] && k64_strcmp(name, symbol) == 0) {
                *value_out = syms[si].st_value;
                return true;
            }
        }
    }

    return false;
}

static bool hotreload_parse_plan(const uint8_t* file_data,
                                 size_t file_size,
                                 k64_hotreload_op_t* ops,
                                 size_t* op_count_out,
                                 uint64_t* entry_out,
                                 uint64_t* mb_magic_addr_out,
                                 uint64_t* mb_info_addr_out) {
    const k64_elf64_ehdr_t* ehdr;
    const k64_elf64_phdr_t* phdrs;
    size_t op_count = 0;
    uint64_t hole_start = 0;
    uint64_t hole_end = 0;

    if (!file_data || file_size < sizeof(k64_elf64_ehdr_t) || !ops || !op_count_out || !entry_out) {
        return false;
    }

    ehdr = (const k64_elf64_ehdr_t*)file_data;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F' ||
        ehdr->e_ident[4] != K64_ELF64 || ehdr->e_ident[5] != K64_ELF_LITTLE ||
        ehdr->e_type != K64_ELF_EXEC || ehdr->e_machine != K64_ELF_X86_64) {
        return false;
    }
    if (ehdr->e_phentsize != sizeof(k64_elf64_phdr_t) ||
        ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(k64_elf64_phdr_t) > file_size) {
        return false;
    }

    phdrs = (const k64_elf64_phdr_t*)(file_data + ehdr->e_phoff);
    (void)hotreload_find_symbol_value(file_data, file_size, "pml4_table", &hole_start);
    (void)hotreload_find_symbol_value(file_data, file_size, "k64_hotreload_trampoline_end", &hole_end);
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const k64_elf64_phdr_t* ph = &phdrs[i];
        uint64_t seg_start;
        uint64_t seg_file_end;
        uint64_t seg_mem_end;

        if (ph->p_type != K64_PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (ph->p_offset + ph->p_filesz > file_size || op_count >= K64_HOTRELOAD_MAX_OPS) {
            return false;
        }
        seg_start = ph->p_vaddr;
        seg_file_end = ph->p_vaddr + ph->p_filesz;
        seg_mem_end = ph->p_vaddr + ph->p_memsz;

        if (hole_start && hole_end > hole_start &&
            seg_start < hole_end && seg_file_end > hole_start) {
            uint64_t pre_copy_end = hole_start > seg_start ? hole_start : seg_start;
            uint64_t post_copy_start = hole_end < seg_file_end ? hole_end : seg_file_end;

            if (pre_copy_end > seg_start) {
                ops[op_count].dst = seg_start;
                ops[op_count].src = (uint64_t)(uintptr_t)(file_data + ph->p_offset);
                ops[op_count].copy_size = pre_copy_end - seg_start;
                ops[op_count].zero_size = 0;
                op_count++;
            }
            if (post_copy_start < seg_file_end) {
                if (op_count >= K64_HOTRELOAD_MAX_OPS) {
                    return false;
                }
                ops[op_count].dst = post_copy_start;
                ops[op_count].src = (uint64_t)(uintptr_t)(file_data + ph->p_offset + (post_copy_start - seg_start));
                ops[op_count].copy_size = seg_file_end - post_copy_start;
                ops[op_count].zero_size = 0;
                op_count++;
            }
            if (seg_mem_end > seg_file_end) {
                if (op_count >= K64_HOTRELOAD_MAX_OPS) {
                    return false;
                }
                ops[op_count].dst = seg_file_end;
                ops[op_count].src = 0;
                ops[op_count].copy_size = 0;
                ops[op_count].zero_size = seg_mem_end - seg_file_end;
                op_count++;
            }
            continue;
        }

        ops[op_count].dst = seg_start;
        ops[op_count].src = (uint64_t)(uintptr_t)(file_data + ph->p_offset);
        ops[op_count].copy_size = ph->p_filesz;
        ops[op_count].zero_size = ph->p_memsz - ph->p_filesz;
        op_count++;
    }

    if (op_count == 0) {
        return false;
    }
    if (!hotreload_find_symbol_value(file_data, file_size, "k64_mb_magic", mb_magic_addr_out) ||
        !hotreload_find_symbol_value(file_data, file_size, "k64_mb_info", mb_info_addr_out)) {
        return false;
    }

    if (!hotreload_find_symbol_value(file_data, file_size, "k64_kernel_main", entry_out) &&
        !hotreload_find_symbol_value(file_data, file_size, "long_mode_entry", entry_out)) {
        *entry_out = ehdr->e_entry;
    }

    *op_count_out = op_count;
    return true;
}

bool k64_hotreload_kernel(void) {
    const uint8_t* kernel_file;
    size_t kernel_size;
    size_t stage_size;
    size_t kernel_frames;
    size_t tramp_size;
    size_t tramp_frames;
    size_t stack_frames = 4;
    void* staged_kernel;
    void* tramp_buf;
    void* tramp_stack;
    k64_hotreload_plan_t* plan;
    k64_hotreload_op_t* ops;
    size_t op_count = 0;
    uint64_t entry = 0;
    uint64_t mb_magic_addr = 0;
    uint64_t mb_info_addr = 0;
    void (*trampoline)(k64_hotreload_plan_t*) = NULL;

    if (!k64_fs_read_file_raw("/boot/k64_kernel.elf", &kernel_file, &kernel_size) || !kernel_file || kernel_size == 0) {
        K64_LOG_ERROR("Hotreload: /boot/k64_kernel.elf is unavailable.");
        return false;
    }

    stage_size = kernel_size + sizeof(k64_hotreload_op_t) * K64_HOTRELOAD_MAX_OPS;
    kernel_frames = (stage_size + 4095u) / 4096u;
    staged_kernel = k64_pmm_alloc_contiguous(kernel_frames);
    if (!staged_kernel) {
        K64_LOG_ERROR("Hotreload: unable to allocate staged kernel buffer.");
        return false;
    }
    hotreload_copy((uint8_t*)staged_kernel, kernel_file, kernel_size);

    ops = (k64_hotreload_op_t*)((uint8_t*)staged_kernel + kernel_size);
    if (!hotreload_parse_plan((const uint8_t*)staged_kernel,
                              kernel_size,
                              ops,
                              &op_count,
                              &entry,
                              &mb_magic_addr,
                              &mb_info_addr)) {
        K64_LOG_ERROR("Hotreload: kernel ELF parsing failed.");
        k64_pmm_free_contiguous(staged_kernel, kernel_frames);
        return false;
    }

    tramp_size = (size_t)(&k64_hotreload_trampoline_end - &k64_hotreload_trampoline_start);
    tramp_frames = (tramp_size + sizeof(k64_hotreload_plan_t) + 4095u) / 4096u;
    tramp_buf = k64_pmm_alloc_contiguous(tramp_frames);
    tramp_stack = k64_pmm_alloc_contiguous(stack_frames);
    if (!tramp_buf || !tramp_stack) {
        K64_LOG_ERROR("Hotreload: unable to allocate trampoline memory.");
        if (tramp_buf) {
            k64_pmm_free_contiguous(tramp_buf, tramp_frames);
        }
        if (tramp_stack) {
            k64_pmm_free_contiguous(tramp_stack, stack_frames);
        }
        k64_pmm_free_contiguous(staged_kernel, kernel_frames);
        return false;
    }

    hotreload_copy((uint8_t*)tramp_buf, &k64_hotreload_trampoline_start, tramp_size);
    plan = (k64_hotreload_plan_t*)((uint8_t*)tramp_buf + tramp_size);
    plan->op_count = op_count;
    plan->ops_ptr = (uint64_t)(uintptr_t)ops;
    plan->mb_magic_addr = mb_magic_addr;
    plan->mb_info_addr = mb_info_addr;
    plan->mb_magic_value = k64_mb_magic;
    plan->mb_info_value = k64_mb_info;
    plan->entry = entry;
    plan->stack_top = (uint64_t)(uintptr_t)((uint8_t*)tramp_stack + stack_frames * 4096u - 16u);

    trampoline = (void (*)(k64_hotreload_plan_t*))(uintptr_t)tramp_buf;
    k64_term_write("Hotreload: loading /boot/k64_kernel.elf\n");
    k64_term_write("Hotreload: entering new kernel image\n");
    __asm__ __volatile__("cli");
    trampoline(plan);
    return false;
}
