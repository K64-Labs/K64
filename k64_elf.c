#include "k64_elf.h"
#include <stddef.h>
#include <stdint.h>
#include "k64_fs.h"
#include "k64_log.h"
#include "k64_terminal.h"
#include "k64_usermode.h"
#include "k64_vmm.h"

#define K64_ELF64 2
#define K64_ELF_LITTLE 1
#define K64_ELF_EXEC 2
#define K64_ELF_DYN 3
#define K64_ELF_X86_64 62
#define K64_PT_LOAD 1
#define K64_ELF_PAGE 4096ULL
#define K64_ELF_VM_BASE 0x0000000040000000ULL
#define K64_ELF_VM_STRIDE 0x0000000001000000ULL
#define K64_ELF_VM_MAX_SLOTS 256ULL
#define K64_ELF_PID_BASE 0x100000000ULL
#define K64_ELF_ARG_MAX 16
#define K64_ELF_ARG_TEXT_MAX 384

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
    int argc;
    char text[K64_ELF_ARG_TEXT_MAX];
    char* argv[K64_ELF_ARG_MAX];
} k64_elf_args_t;

static uint64_t elf_align_up(uint64_t value, uint64_t align) {
    return (value + align - 1ULL) & ~(align - 1ULL);
}

static uint64_t elf_pid_for_entry(uint64_t fallback_pid, uint64_t entry) {
    if (entry >= K64_ELF_VM_BASE) {
        uint64_t slot = (entry - K64_ELF_VM_BASE) / K64_ELF_VM_STRIDE;
        if (slot < K64_ELF_VM_MAX_SLOTS) {
            return K64_ELF_PID_BASE + slot;
        }
    }
    return fallback_pid;
}

static size_t elf_strlen(const char* s) {
    size_t len = 0;

    while (s && s[len]) {
        len++;
    }
    return len;
}

static void elf_copy(char* dst, size_t dst_size, const char* src) {
    size_t i = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    while (src[i] && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void elf_parse_args(k64_elf_args_t* out, const char* path, const char* args) {
    size_t pos = 0;

    if (!out) {
        return;
    }
    out->argc = 0;
    for (int i = 0; i < K64_ELF_ARG_MAX; ++i) {
        out->argv[i] = NULL;
    }
    out->text[0] = '\0';

    out->argv[out->argc++] = &out->text[pos];
    elf_copy(&out->text[pos], sizeof(out->text) - pos, path ? path : "");
    pos += elf_strlen(&out->text[pos]) + 1;

    while (args && *args && out->argc < K64_ELF_ARG_MAX && pos + 1 < sizeof(out->text)) {
        while (*args == ' ' || *args == '\t') {
            args++;
        }
        if (!*args) {
            break;
        }
        out->argv[out->argc++] = &out->text[pos];
        while (*args && *args != ' ' && *args != '\t' && pos + 1 < sizeof(out->text)) {
            out->text[pos++] = *args++;
        }
        out->text[pos++] = '\0';
    }
}

static bool elf_write_initial_stack(const k64_vm_space_t* space,
                                    uint64_t stack_top,
                                    const k64_elf_args_t* args,
                                    uint64_t* out_stack) {
    uint64_t argv_user[K64_ELF_ARG_MAX + 1];
    uint64_t sp = stack_top;
    uint64_t value;

    if (!space || !args || !out_stack || args->argc < 1) {
        return false;
    }

    for (int i = args->argc - 1; i >= 0; --i) {
        size_t len = elf_strlen(args->argv[i]) + 1;
        sp -= len;
        if (!k64_vmm_write_user(space, sp, args->argv[i], len)) {
            return false;
        }
        argv_user[i] = sp;
    }
    argv_user[args->argc] = 0;

    sp &= ~0xFULL;
    for (int i = args->argc; i >= 0; --i) {
        sp -= sizeof(uint64_t);
        value = argv_user[i];
        if (!k64_vmm_write_user(space, sp, &value, sizeof(value))) {
            return false;
        }
    }
    sp -= sizeof(uint64_t);
    value = (uint64_t)(uint32_t)args->argc;
    if (!k64_vmm_write_user(space, sp, &value, sizeof(value))) {
        return false;
    }

    *out_stack = sp;
    return true;
}

static bool elf_execute_impl(const char* path, bool user_mode, const char* args_text) {
    const uint8_t* file_data = NULL;
    size_t file_size = 0;
    const k64_elf64_ehdr_t* ehdr;
    const k64_elf64_phdr_t* phdrs;
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    k64_vm_space_t app_space;
    static uint64_t next_ephemeral_pid = K64_ELF_PID_BASE;
    uint64_t app_pid;
    k64_elf_args_t args;
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

    app_pid = elf_pid_for_entry(next_ephemeral_pid++, ehdr->e_entry);
    if ((user_mode ? !k64_vmm_alloc_user_space(app_pid, &app_space)
                   : !k64_vmm_alloc_service_space(app_pid, &app_space))) {
        K64_LOG_WARN("ELF: allocation failed.");
        return false;
    }

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const k64_elf64_phdr_t* ph = &phdrs[i];

        if (ph->p_type != K64_PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (!(user_mode ? k64_vmm_map_user_range(&app_space,
                                                 ph->p_vaddr,
                                                 file_data + ph->p_offset,
                                                 (size_t)ph->p_filesz,
                                                 (size_t)ph->p_memsz)
                        : k64_vmm_map_private_range(&app_space,
                                                    ph->p_vaddr,
                                                    file_data + ph->p_offset,
                                                    (size_t)ph->p_filesz,
                                                    (size_t)ph->p_memsz))) {
            K64_LOG_WARN("ELF: segment mapping failed.");
            k64_vmm_release_service_space(&app_space);
            return false;
        }
    }

    k64_term_write("ELF: executing ");
    k64_term_write(path);
    k64_term_putc('\n');
    if (user_mode && !k64_vmm_is_mapped(&app_space, ehdr->e_entry, true)) {
        k64_term_write("ELF: entry page is not mapped\n");
        k64_vmm_release_service_space(&app_space);
        return false;
    }
    if (user_mode) {
        uint64_t user_stack_top = app_space.stack_base + app_space.stack_size - 16ULL;
        elf_parse_args(&args, path, args_text);
        if (!elf_write_initial_stack(&app_space, user_stack_top, &args, &user_stack_top)) {
            k64_term_write("ELF: failed to prepare process arguments\n");
            k64_vmm_release_service_space(&app_space);
            return false;
        }
        rc = (int)k64_usermode_execute_named(&app_space, ehdr->e_entry, user_stack_top, path);
    } else {
        rc = (int)k64_vmm_call_isolated(&app_space, ehdr->e_entry, 0, 0, 0);
    }
    k64_term_write("ELF: exit code ");
    k64_term_write_dec((uint64_t)(uint32_t)rc);
    k64_term_putc('\n');
    k64_vmm_release_service_space(&app_space);
    return true;
}

bool k64_elf_execute_path(const char* path) {
    return elf_execute_impl(path, false, "");
}

bool k64_elf_execute_user_path(const char* path) {
    return elf_execute_impl(path, true, "");
}

bool k64_elf_execute_user_path_args(const char* path, const char* args) {
    return elf_execute_impl(path, true, args);
}

bool k64_elf_spawn_user_path(const char* path) {
    return k64_elf_spawn_user_path_args(path, "");
}

bool k64_elf_spawn_user_path_args(const char* path, const char* args) {
    k64_fs_stat_t st;

    if (!path || !path[0]) {
        return false;
    }
    if (!k64_fs_stat(path, &st) || !st.exists || st.is_dir) {
        K64_LOG_WARN("ELF: file unavailable.");
        return false;
    }
    return k64_elf_execute_user_path_args(path, args);
}
