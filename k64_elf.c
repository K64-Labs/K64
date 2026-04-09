#include "k64_elf.h"
#include <stddef.h>
#include <stdint.h>
#include "k64_fs.h"
#include "k64_log.h"
#include "k64_terminal.h"
#include "k64_vmm.h"

#define K64_ELF64 2
#define K64_ELF_LITTLE 1
#define K64_ELF_EXEC 2
#define K64_ELF_DYN 3
#define K64_ELF_X86_64 62
#define K64_PT_LOAD 1
#define K64_ELF_PAGE 4096ULL

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

static uint64_t elf_align_up(uint64_t value, uint64_t align) {
    return (value + align - 1ULL) & ~(align - 1ULL);
}

bool k64_elf_execute_path(const char* path) {
    const uint8_t* file_data = NULL;
    size_t file_size = 0;
    const k64_elf64_ehdr_t* ehdr;
    const k64_elf64_phdr_t* phdrs;
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    k64_vm_space_t app_space;
    static uint64_t next_ephemeral_pid = 0x100000000ULL;
    int rc;

    if (!path || !path[0]) {
        return false;
    }
    if (!k64_fs_read_file_raw(path, &file_data, &file_size) || !file_data || file_size < sizeof(k64_elf64_ehdr_t)) {
        K64_LOG_WARN("ELF: file unavailable.");
        return false;
    }

    ehdr = (const k64_elf64_ehdr_t*)file_data;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F' ||
        ehdr->e_ident[4] != K64_ELF64 || ehdr->e_ident[5] != K64_ELF_LITTLE ||
        (ehdr->e_type != K64_ELF_EXEC && ehdr->e_type != K64_ELF_DYN) ||
        ehdr->e_machine != K64_ELF_X86_64) {
        K64_LOG_WARN("ELF: invalid executable.");
        return false;
    }
    if (ehdr->e_phentsize != sizeof(k64_elf64_phdr_t) ||
        ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(k64_elf64_phdr_t) > file_size) {
        K64_LOG_WARN("ELF: bad program header table.");
        return false;
    }

    phdrs = (const k64_elf64_phdr_t*)(file_data + ehdr->e_phoff);
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const k64_elf64_phdr_t* ph = &phdrs[i];
        uint64_t seg_start;
        uint64_t seg_end;

        if (ph->p_type != K64_PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (ph->p_offset + ph->p_filesz > file_size) {
            K64_LOG_WARN("ELF: segment exceeds file.");
            return false;
        }
        seg_start = ph->p_vaddr & ~(K64_ELF_PAGE - 1ULL);
        seg_end = elf_align_up(ph->p_vaddr + ph->p_memsz, K64_ELF_PAGE);
        if (seg_start < min_vaddr) {
            min_vaddr = seg_start;
        }
        if (seg_end > max_vaddr) {
            max_vaddr = seg_end;
        }
    }

    if (min_vaddr == UINT64_MAX || max_vaddr <= min_vaddr || ehdr->e_entry < min_vaddr || ehdr->e_entry >= max_vaddr) {
        K64_LOG_WARN("ELF: no loadable image.");
        return false;
    }

    if (!k64_vmm_alloc_service_space(next_ephemeral_pid++, &app_space)) {
        K64_LOG_WARN("ELF: allocation failed.");
        return false;
    }

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const k64_elf64_phdr_t* ph = &phdrs[i];

        if (ph->p_type != K64_PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (!k64_vmm_map_private_range(&app_space,
                                       ph->p_vaddr,
                                       file_data + ph->p_offset,
                                       (size_t)ph->p_filesz,
                                       (size_t)ph->p_memsz)) {
            K64_LOG_WARN("ELF: segment mapping failed.");
            k64_vmm_release_service_space(&app_space);
            return false;
        }
    }

    k64_term_write("ELF: executing ");
    k64_term_write(path);
    k64_term_putc('\n');
    rc = (int)k64_vmm_call_isolated(&app_space, ehdr->e_entry, 0, 0, 0);
    k64_term_write("ELF: exit code ");
    k64_term_write_dec((uint64_t)(uint32_t)rc);
    k64_term_putc('\n');
    k64_vmm_release_service_space(&app_space);
    return true;
}
