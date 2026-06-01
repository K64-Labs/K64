#include "k64_elf.h"
#include <stddef.h>
#include <stdint.h>
#include "k64_fs.h"
#include "k64_log.h"
#include "k64_user.h"
#include "k64_terminal.h"
#include "k64_usermode.h"
#include "k64_string.h"
#include "k64_vmm.h"

#define K64_ELF64 2
#define K64_ELF_LITTLE 1
#define K64_ELF_EXEC 2
#define K64_ELF_DYN 3
#define K64_ELF_X86_64 62
#define K64_PT_LOAD 1
#define K64_ELF_DYN_LOAD_BIAS 0x0000000040000000ULL
#define K64_ELF_MAIN_DYN_LOAD_BIAS 0x0000000000400000ULL
#define K64_ELF_PAGE 4096ULL
#define K64_ELF_VM_BASE 0x0000000040000000ULL
#define K64_ELF_VM_STRIDE 0x0000000001000000ULL
#define K64_ELF_VM_MAX_SLOTS 256ULL
#define K64_ELF_PID_BASE 0x100000000ULL
#define K64_ELF_USER_MIN 0x0000000000010000ULL
#define K64_ELF_USER_MAX 0x0000000080000000ULL
#define K64_ELF_SEGMENT_MAX 0x0000000001000000ULL
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

#define K64_ELF_CONTEXT_MAX 4

typedef struct {
    k64_vm_space_t space;
    k64_elf_args_t args;
} k64_elf_context_t;

static k64_elf_context_t elf_contexts[K64_ELF_CONTEXT_MAX];
static int elf_context_depth;

static uint64_t elf_align_up(uint64_t value, uint64_t align) {
    return (value + align - 1ULL) & ~(align - 1ULL);
}

static bool elf_add_overflows(uint64_t a, uint64_t b) {
    return a + b < a;
}

static bool elf_range_valid(uint64_t start, uint64_t size, uint64_t min, uint64_t max) {
    uint64_t end;

    if (size == 0 || elf_add_overflows(start, size)) {
        return false;
    }
    end = start + size;
    return start >= min && end <= max;
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
    path = path ? path : "";
    while (*path && *path != ' ' && *path != '\t' && pos + 1 < sizeof(out->text)) {
        out->text[pos++] = *path++;
    }
    out->text[pos] = '\0';
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
                                    uint64_t entry,
                                    uint64_t phdr,
                                    uint64_t phnum,
                                    uint64_t phentsize,
                                    uint64_t at_base,
                                    uint64_t* out_stack) {
    uint64_t argv_user[K64_ELF_ARG_MAX + 1];
    const char* env[] = {
        "TERM=xterm",
        "PATH=/bin:/usr/bin:/usr/games:/compat/linux/bin",
        "HOME=/",
    };
    uint64_t env_user[sizeof(env) / sizeof(env[0])];
    uint64_t random_user;
    uint8_t random_bytes[16];
    uint64_t auxv[][2] = {
        {3, phdr},
        {4, phentsize},
        {5, phnum},
        {6, K64_ELF_PAGE},
        {7, at_base},
        {8, 0},
        {9, entry},
        {11, k64_user_real_uid()},
        {12, k64_user_effective_uid()},
        {13, k64_user_effective_gid()},
        {14, k64_user_effective_gid()},
        {17, 100},
        {23, 0},
        {25, 0},
        {31, 0},
        {0, 0},
    };
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
    for (int i = (int)(sizeof(env) / sizeof(env[0])) - 1; i >= 0; --i) {
        size_t len = elf_strlen(env[i]) + 1;
        sp -= len;
        if (!k64_vmm_write_user(space, sp, env[i], len)) {
            return false;
        }
        env_user[i] = sp;
    }
    for (size_t i = 0; i < sizeof(random_bytes); ++i) {
        random_bytes[i] = (uint8_t)(0x5Au + i);
    }
    sp -= sizeof(random_bytes);
    random_user = sp;
    if (!k64_vmm_write_user(space, sp, random_bytes, sizeof(random_bytes))) {
        return false;
    }
    auxv[13][1] = random_user;
    auxv[14][1] = argv_user[0];

    sp &= ~0xFULL;
    {
        uint64_t table_bytes = ((uint64_t)(sizeof(auxv) / sizeof(auxv[0])) * 16ULL) +
                               8ULL +
                               ((uint64_t)(sizeof(env) / sizeof(env[0]))) * 8ULL +
                               ((uint64_t)args->argc + 1ULL) * 8ULL +
                               8ULL;
        uint64_t final_mod = (sp - table_bytes) & 0xFULL;
        uint64_t desired_mod = 0ULL;
        if (final_mod != desired_mod) {
            sp -= (final_mod - desired_mod) & 0xFULL;
        }
        for (int i = (int)(sizeof(auxv) / sizeof(auxv[0])) - 1; i >= 0; --i) {
            sp -= sizeof(uint64_t);
            value = auxv[i][1];
            if (!k64_vmm_write_user(space, sp, &value, sizeof(value))) {
                return false;
            }
            sp -= sizeof(uint64_t);
            value = auxv[i][0];
            if (!k64_vmm_write_user(space, sp, &value, sizeof(value))) {
                return false;
            }
        }
    }
    sp -= sizeof(uint64_t);
    value = 0;
    if (!k64_vmm_write_user(space, sp, &value, sizeof(value))) {
        return false;
    }
    for (int i = (int)(sizeof(env) / sizeof(env[0])) - 1; i >= 0; --i) {
        sp -= sizeof(uint64_t);
        value = env_user[i];
        if (!k64_vmm_write_user(space, sp, &value, sizeof(value))) {
            return false;
        }
    }
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

static bool elf_header_valid(const uint8_t* file_data,
                             size_t file_size,
                             const k64_elf64_ehdr_t** out_ehdr,
                             const k64_elf64_phdr_t** out_phdrs) {
    const k64_elf64_ehdr_t* ehdr;

    if (!file_data || file_size < sizeof(k64_elf64_ehdr_t) || !out_ehdr || !out_phdrs) {
        return false;
    }
    ehdr = (const k64_elf64_ehdr_t*)file_data;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F' ||
        ehdr->e_ident[4] != K64_ELF64 || ehdr->e_ident[5] != K64_ELF_LITTLE ||
        (ehdr->e_type != K64_ELF_EXEC && ehdr->e_type != K64_ELF_DYN) ||
        ehdr->e_machine != K64_ELF_X86_64) {
        return false;
    }
    if (ehdr->e_phentsize != sizeof(k64_elf64_phdr_t) ||
        ehdr->e_phnum == 0 ||
        elf_add_overflows(ehdr->e_phoff, (uint64_t)ehdr->e_phnum * sizeof(k64_elf64_phdr_t)) ||
        ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(k64_elf64_phdr_t) > file_size) {
        return false;
    }
    *out_ehdr = ehdr;
    *out_phdrs = (const k64_elf64_phdr_t*)(file_data + ehdr->e_phoff);
    return true;
}

static bool elf_scan_loads(size_t file_size,
                           const k64_elf64_ehdr_t* ehdr,
                           const k64_elf64_phdr_t* phdrs,
                           uint64_t load_bias,
                           uint64_t* out_min_vaddr,
                           uint64_t* out_max_vaddr) {
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const k64_elf64_phdr_t* ph = &phdrs[i];
        uint64_t load_vaddr;
        uint64_t seg_start;
        uint64_t seg_end;
        uint64_t seg_limit;

        if (ph->p_type != K64_PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (ph->p_filesz > ph->p_memsz ||
            ph->p_memsz > K64_ELF_SEGMENT_MAX ||
            elf_add_overflows(ph->p_offset, ph->p_filesz) ||
            ph->p_offset + ph->p_filesz > file_size) {
            return false;
        }
        load_vaddr = ph->p_vaddr + load_bias;
        if (!elf_range_valid(load_vaddr, ph->p_memsz, 0, K64_ELF_USER_MAX)) {
            return false;
        }
        if (ph->p_align != 0 && (ph->p_align & (ph->p_align - 1ULL)) != 0) {
            return false;
        }
        if (elf_add_overflows(load_vaddr, ph->p_memsz)) {
            return false;
        }
        seg_limit = load_vaddr + ph->p_memsz;
        seg_start = load_vaddr & ~(K64_ELF_PAGE - 1ULL);
        if (elf_add_overflows(seg_limit, K64_ELF_PAGE - 1ULL)) {
            return false;
        }
        seg_end = elf_align_up(seg_limit, K64_ELF_PAGE);
        if (seg_start < min_vaddr) {
            min_vaddr = seg_start;
        }
        if (seg_end > max_vaddr) {
            max_vaddr = seg_end;
        }
    }

    if (min_vaddr == UINT64_MAX || max_vaddr <= min_vaddr) {
        return false;
    }
    if (out_min_vaddr) {
        *out_min_vaddr = min_vaddr;
    }
    if (out_max_vaddr) {
        *out_max_vaddr = max_vaddr;
    }
    return true;
}

static bool elf_map_loads(k64_vm_space_t* space,
                          const uint8_t* file_data,
                          const k64_elf64_ehdr_t* ehdr,
                          const k64_elf64_phdr_t* phdrs,
                          uint64_t load_bias,
                          bool user_mode) {
    uint64_t mapped_pages[512];
    size_t mapped_count = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const k64_elf64_phdr_t* ph = &phdrs[i];
        uint64_t load_vaddr = ph->p_vaddr + load_bias;

        if (ph->p_type != K64_PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (user_mode) {
            uint64_t page_start = load_vaddr & ~(K64_ELF_PAGE - 1ULL);
            uint64_t page_end = elf_align_up(load_vaddr + ph->p_memsz, K64_ELF_PAGE);

            for (uint64_t page = page_start; page < page_end; page += K64_ELF_PAGE) {
                bool already_mapped_by_image = false;
                for (size_t j = 0; j < mapped_count; ++j) {
                    if (mapped_pages[j] == page) {
                        already_mapped_by_image = true;
                        break;
                    }
                }
                if (!already_mapped_by_image) {
                    if (mapped_count >= sizeof(mapped_pages) / sizeof(mapped_pages[0])) {
                        k64_term_write("ELF: too many mapped pages in image\n");
                        return false;
                    }
                    if (!k64_vmm_map_user_anon(space, page, K64_ELF_PAGE)) {
                        k64_term_write("ELF: failed to map page ");
                        k64_term_write_hex(page);
                        k64_term_putc('\n');
                        return false;
                    }
                    mapped_pages[mapped_count++] = page;
                }
            }
            if (ph->p_filesz != 0 &&
                !k64_vmm_write_user(space,
                                    load_vaddr,
                                    file_data + ph->p_offset,
                                    (size_t)ph->p_filesz)) {
                k64_term_write("ELF: failed to copy segment at ");
                k64_term_write_hex(load_vaddr);
                k64_term_putc('\n');
                return false;
            }
        } else {
            if (!k64_vmm_map_private_range(space,
                                           load_vaddr,
                                           file_data + ph->p_offset,
                                           (size_t)ph->p_filesz,
                                           (size_t)ph->p_memsz)) {
                return false;
            }
        }
    }
    return true;
}

static const char* elf_loader_process_path(const char* path, const char* args, char* out, size_t out_size) {
    const char* p = args;

    if (!path || !args || !out || out_size == 0 ||
        k64_strncmp(path, "/compat/linux/lib64/ld-linux", 28) != 0) {
        return path;
    }
    out[0] = '\0';
    while (p && *p) {
        char token[128];
        size_t i = 0;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        while (p[i] && p[i] != ' ' && p[i] != '\t' && i + 1 < sizeof(token)) {
            token[i] = p[i];
            i++;
        }
        token[i] = '\0';
        p += i;
        if (k64_streq(token, "--library-path") ||
            k64_streq(token, "--preload") ||
            k64_streq(token, "--audit") ||
            k64_streq(token, "--argv0")) {
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
            continue;
        }
        if (token[0] == '-' && token[1] == '-') {
            continue;
        }
        elf_copy(out, out_size, token);
        return out;
    }
    return path;
}

static uint64_t elf_phdr_vaddr(const k64_elf64_ehdr_t* ehdr,
                               const k64_elf64_phdr_t* phdrs,
                               uint64_t load_bias) {
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const k64_elf64_phdr_t* ph = &phdrs[i];

        if (ph->p_type != K64_PT_LOAD) {
            continue;
        }
        if (ehdr->e_phoff >= ph->p_offset &&
            ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize <= ph->p_offset + ph->p_filesz) {
            return load_bias + ph->p_vaddr + (ehdr->e_phoff - ph->p_offset);
        }
    }
    return load_bias + ehdr->e_phoff;
}

static bool elf_execute_impl_ex(const char* path,
                                bool user_mode,
                                const char* args_text,
                                uint64_t parent_pid,
                                uint64_t pid) {
    const uint8_t* file_data = NULL;
    size_t file_size = 0;
    const k64_elf64_ehdr_t* ehdr;
    const k64_elf64_phdr_t* phdrs;
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    uint64_t load_bias = 0;
    uint64_t entry;
    static uint64_t next_ephemeral_pid = K64_ELF_PID_BASE;
    uint64_t app_pid;
    k64_elf_context_t* ctx;
    int rc;
    char process_path[128];

    if (!path || !path[0]) {
        return false;
    }
    if (user_mode) {
        k64_fs_stat_t st;
        if (!k64_fs_stat(path, &st) ||
            !k64_user_can_access(st.uid, st.gid, st.mode, K64_ACCESS_READ | K64_ACCESS_EXEC)) {
            K64_LOG_WARN("ELF: execute permission denied.");
            return false;
        }
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
        ehdr->e_phnum == 0 ||
        elf_add_overflows(ehdr->e_phoff, (uint64_t)ehdr->e_phnum * sizeof(k64_elf64_phdr_t)) ||
        ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(k64_elf64_phdr_t) > file_size) {
        K64_LOG_WARN("ELF: bad program header table.");
        return false;
    }

    phdrs = (const k64_elf64_phdr_t*)(file_data + ehdr->e_phoff);
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const k64_elf64_phdr_t* ph = &phdrs[i];
        uint64_t seg_start;
        uint64_t seg_end;
        uint64_t seg_limit;

        if (ph->p_type != K64_PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (ph->p_filesz > ph->p_memsz ||
            ph->p_memsz > K64_ELF_SEGMENT_MAX ||
            elf_add_overflows(ph->p_offset, ph->p_filesz) ||
            ph->p_offset + ph->p_filesz > file_size) {
            K64_LOG_WARN("ELF: segment exceeds file.");
            return false;
        }
        uint64_t load_vaddr = ph->p_vaddr;
        if (ehdr->e_type == K64_ELF_DYN) {
            load_bias = K64_ELF_DYN_LOAD_BIAS;
            load_vaddr += load_bias;
        }
        if (!elf_range_valid(load_vaddr, ph->p_memsz, K64_ELF_USER_MIN, K64_ELF_USER_MAX)) {
            K64_LOG_WARN("ELF: segment address is outside user range.");
            return false;
        }
        if (ph->p_align != 0 && (ph->p_align & (ph->p_align - 1ULL)) != 0) {
            K64_LOG_WARN("ELF: segment alignment is invalid.");
            return false;
        }
        if (elf_add_overflows(load_vaddr, ph->p_memsz)) {
            K64_LOG_WARN("ELF: segment address overflow.");
            return false;
        }
        seg_limit = load_vaddr + ph->p_memsz;
        seg_start = load_vaddr & ~(K64_ELF_PAGE - 1ULL);
        if (elf_add_overflows(seg_limit, K64_ELF_PAGE - 1ULL)) {
            K64_LOG_WARN("ELF: segment alignment overflow.");
            return false;
        }
        seg_end = elf_align_up(seg_limit, K64_ELF_PAGE);
        if (seg_start < min_vaddr) {
            min_vaddr = seg_start;
        }
        if (seg_end > max_vaddr) {
            max_vaddr = seg_end;
        }
    }

    entry = ehdr->e_entry + load_bias;
    if (min_vaddr == UINT64_MAX || max_vaddr <= min_vaddr ||
        entry < min_vaddr || entry >= max_vaddr ||
        entry < K64_ELF_USER_MIN || entry >= K64_ELF_USER_MAX) {
        K64_LOG_WARN("ELF: no loadable image.");
        return false;
    }

    app_pid = elf_pid_for_entry(next_ephemeral_pid++, entry);
    if (elf_context_depth < 0 || elf_context_depth >= K64_ELF_CONTEXT_MAX) {
        K64_LOG_WARN("ELF: nested execution limit reached.");
        return false;
    }
    ctx = &elf_contexts[elf_context_depth++];

    if ((user_mode ? !k64_vmm_alloc_user_space(app_pid, &ctx->space)
                   : !k64_vmm_alloc_service_space(app_pid, &ctx->space))) {
        elf_context_depth--;
        K64_LOG_WARN("ELF: allocation failed.");
        return false;
    }

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const k64_elf64_phdr_t* ph = &phdrs[i];
        uint64_t load_vaddr = ph->p_vaddr + load_bias;

        if (ph->p_type != K64_PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (!(user_mode ? k64_vmm_map_user_range(&ctx->space,
                                                 load_vaddr,
                                                 file_data + ph->p_offset,
                                                 (size_t)ph->p_filesz,
                                                 (size_t)ph->p_memsz)
                        : k64_vmm_map_private_range(&ctx->space,
                                                    load_vaddr,
                                                    file_data + ph->p_offset,
                                                    (size_t)ph->p_filesz,
                                                    (size_t)ph->p_memsz))) {
            K64_LOG_WARN("ELF: segment mapping failed.");
            k64_vmm_release_service_space(&ctx->space);
            elf_context_depth--;
            return false;
        }
    }

    k64_term_write("ELF: executing ");
    k64_term_write(path);
    k64_term_putc('\n');
    if (user_mode && !k64_vmm_is_mapped(&ctx->space, entry, true)) {
        k64_term_write("ELF: entry page is not mapped\n");
        k64_vmm_release_service_space(&ctx->space);
        elf_context_depth--;
        return false;
    }
    if (user_mode) {
        uint64_t user_stack_top = ctx->space.stack_base + ctx->space.stack_size - 16ULL;
        elf_parse_args(&ctx->args, path, args_text);
        if (!elf_write_initial_stack(&ctx->space,
                                     user_stack_top,
                                     &ctx->args,
                                     entry,
                                     elf_phdr_vaddr(ehdr, phdrs, load_bias),
                                     ehdr->e_phnum,
                                     ehdr->e_phentsize,
                                     0,
                                     &user_stack_top)) {
            k64_term_write("ELF: failed to prepare process arguments\n");
            k64_vmm_release_service_space(&ctx->space);
            elf_context_depth--;
            return false;
        }
        rc = (int)k64_usermode_execute_named_ex(&ctx->space,
                                                entry,
                                                user_stack_top,
                                                elf_loader_process_path(path, args_text, process_path, sizeof(process_path)),
                                                parent_pid,
                                                pid);
    } else {
        rc = (int)k64_vmm_call_isolated(&ctx->space, entry, 0, 0, 0);
    }
    k64_term_write("ELF: exit code ");
    k64_term_write_dec((uint64_t)(uint32_t)rc);
    k64_term_putc('\n');
    k64_vmm_release_service_space(&ctx->space);
    elf_context_depth--;
    return true;
}

bool k64_elf_execute_path(const char* path) {
    return elf_execute_impl_ex(path, false, "", 0, 0);
}

bool k64_elf_execute_user_path(const char* path) {
    return elf_execute_impl_ex(path, true, "", 0, 0);
}

bool k64_elf_execute_user_path_args(const char* path, const char* args) {
    return elf_execute_impl_ex(path, true, args, 0, 0);
}

bool k64_elf_execute_linux_dynamic(const char* main_path,
                                   const char* argv0_path,
                                   const char* args,
                                   const char* interp_path) {
    const uint8_t* main_data = NULL;
    const uint8_t* interp_data = NULL;
    size_t main_size = 0;
    size_t interp_size = 0;
    const k64_elf64_ehdr_t* main_ehdr;
    const k64_elf64_phdr_t* main_phdrs;
    const k64_elf64_ehdr_t* interp_ehdr;
    const k64_elf64_phdr_t* interp_phdrs;
    uint64_t main_bias;
    uint64_t interp_bias = 0;
    uint64_t main_min;
    uint64_t main_max;
    uint64_t interp_min;
    uint64_t interp_max;
    uint64_t entry;
    uint64_t main_entry;
    uint64_t user_stack_top;
    static uint64_t next_ephemeral_pid = K64_ELF_PID_BASE + 0x1000ULL;
    k64_elf_context_t* ctx;
    int rc;

    if (!main_path || !main_path[0] || !interp_path || !interp_path[0]) {
        return false;
    }
    if (!argv0_path || !argv0_path[0]) {
        argv0_path = main_path;
    }
    if (!k64_fs_read_file_raw(main_path, &main_data, &main_size) ||
        !k64_fs_read_file_raw(interp_path, &interp_data, &interp_size) ||
        !elf_header_valid(main_data, main_size, &main_ehdr, &main_phdrs) ||
        !elf_header_valid(interp_data, interp_size, &interp_ehdr, &interp_phdrs) ||
        interp_ehdr->e_type != K64_ELF_DYN) {
        K64_LOG_WARN("ELF: dynamic Linux image unavailable or invalid.");
        return false;
    }

    main_bias = main_ehdr->e_type == K64_ELF_DYN ? K64_ELF_MAIN_DYN_LOAD_BIAS : 0;
    main_entry = main_ehdr->e_entry + main_bias;
    entry = interp_ehdr->e_entry + interp_bias;

    if (!elf_scan_loads(main_size, main_ehdr, main_phdrs, main_bias, &main_min, &main_max) ||
        !elf_scan_loads(interp_size, interp_ehdr, interp_phdrs, interp_bias, &interp_min, &interp_max) ||
        main_entry < main_min || main_entry >= main_max ||
        entry < interp_min || entry >= interp_max) {
        K64_LOG_WARN("ELF: dynamic Linux load layout is invalid.");
        return false;
    }
    if (!(main_max <= interp_min || interp_max <= main_min)) {
        K64_LOG_WARN("ELF: dynamic Linux image overlaps interpreter.");
        return false;
    }
    if (elf_context_depth < 0 || elf_context_depth >= K64_ELF_CONTEXT_MAX) {
        K64_LOG_WARN("ELF: nested execution limit reached.");
        return false;
    }

    ctx = &elf_contexts[elf_context_depth++];
    if (!k64_vmm_alloc_user_space(next_ephemeral_pid++, &ctx->space)) {
        elf_context_depth--;
        K64_LOG_WARN("ELF: allocation failed.");
        return false;
    }
    if (!elf_map_loads(&ctx->space, main_data, main_ehdr, main_phdrs, main_bias, true) ||
        !elf_map_loads(&ctx->space, interp_data, interp_ehdr, interp_phdrs, interp_bias, true)) {
        K64_LOG_WARN("ELF: dynamic Linux segment mapping failed.");
        k64_vmm_release_service_space(&ctx->space);
        elf_context_depth--;
        return false;
    }

    k64_term_write("ELF: executing ");
    k64_term_write(main_path);
    k64_term_write(" via ");
    k64_term_write(interp_path);
    k64_term_putc('\n');

    user_stack_top = ctx->space.stack_base + ctx->space.stack_size - 16ULL;
    elf_parse_args(&ctx->args, argv0_path, args);
    if (!elf_write_initial_stack(&ctx->space,
                                 user_stack_top,
                                 &ctx->args,
                                 main_entry,
                                 elf_phdr_vaddr(main_ehdr, main_phdrs, main_bias),
                                 main_ehdr->e_phnum,
                                 main_ehdr->e_phentsize,
                                 interp_bias,
                                 &user_stack_top)) {
        k64_term_write("ELF: failed to prepare process arguments\n");
        k64_vmm_release_service_space(&ctx->space);
        elf_context_depth--;
        return false;
    }

    rc = (int)k64_usermode_execute_named_ex(&ctx->space,
                                            entry,
                                            user_stack_top,
                                            main_path,
                                            0,
                                            0);
    k64_term_write("ELF: exit code ");
    k64_term_write_dec((uint64_t)(uint32_t)rc);
    k64_term_putc('\n');
    k64_vmm_release_service_space(&ctx->space);
    elf_context_depth--;
    return true;
}

bool k64_elf_spawn_user_path(const char* path) {
    return k64_elf_spawn_user_path_args(path, "");
}

bool k64_elf_spawn_user_path_args(const char* path, const char* args) {
    if (!path || !path[0]) {
        return false;
    }
    return k64_elf_execute_user_path_args(path, args);
}

bool k64_elf_spawn_user_path_args_ex(const char* path, const char* args, uint64_t parent_pid, uint64_t pid) {
    if (!path || !path[0]) {
        return false;
    }
    return elf_execute_impl_ex(path, true, args, parent_pid, pid);
}
