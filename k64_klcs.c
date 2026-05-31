#include "k64_klcs.h"
#include "k64_string.h"
#ifndef K64_HOST_TEST
#include "k64_terminal.h"
#endif

#define KLCS_ELF_MAGIC0 0x7f
#define KLCS_ELF_MAGIC1 'E'
#define KLCS_ELF_MAGIC2 'L'
#define KLCS_ELF_MAGIC3 'F'
#define KLCS_ELFCLASS64 2
#define KLCS_ELFDATA2LSB 1
#define KLCS_EM_X86_64 62
#define KLCS_ET_EXEC 2
#define KLCS_ET_DYN 3
#define KLCS_PT_LOAD 1
#define KLCS_PT_INTERP 3

typedef struct {
    uint64_t nr;
    const char* name;
    bool implemented;
} klcs_syscall_entry_t;

static klcs_state_t klcs;

static const klcs_syscall_entry_t syscall_table[] = {
    {0, "read", true},
    {1, "write", true},
    {2, "open", false},
    {3, "close", true},
    {8, "lseek", false},
    {9, "mmap", false},
    {10, "mprotect", false},
    {11, "munmap", false},
    {12, "brk", false},
    {21, "access", false},
    {39, "getpid", true},
    {60, "exit", true},
    {63, "uname", false},
    {89, "readlink", false},
    {102, "getuid", true},
    {104, "getgid", true},
    {107, "geteuid", true},
    {108, "getegid", true},
    {158, "arch_prctl", false},
    {202, "futex", false},
    {217, "getdents64", false},
    {218, "set_tid_address", false},
    {228, "clock_gettime", false},
    {231, "exit_group", true},
    {257, "openat", false},
    {262, "newfstatat", false},
    {318, "getrandom", false},
};

static void klcs_copy(char* dst, size_t dst_size, const char* src) {
    size_t i = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (src[i] && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void klcs_append(char* dst, size_t dst_size, const char* src) {
    size_t pos = 0;
    size_t i = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    while (dst[pos] && pos + 1 < dst_size) {
        pos++;
    }
    while (src && src[i] && pos + 1 < dst_size) {
        dst[pos++] = src[i++];
    }
    dst[pos] = '\0';
}

static void klcs_append_u64(char* dst, size_t dst_size, uint64_t value) {
    char tmp[32];
    size_t n = 0;

    if (value == 0) {
        klcs_append(dst, dst_size, "0");
        return;
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (n > 0) {
        char c[2] = { tmp[--n], '\0' };
        klcs_append(dst, dst_size, c);
    }
}

static uint16_t read_u16le(const uint8_t* p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t read_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64le(const uint8_t* p) {
    uint64_t lo = read_u32le(p);
    uint64_t hi = read_u32le(p + 4);
    return lo | (hi << 32);
}

void klcs_init(void) {
    memset(&klcs, 0, sizeof(klcs));
    (void)klcs_fd_alloc(KLCS_FD_STDIN, "stdin", 0);
    (void)klcs_fd_alloc(KLCS_FD_STDOUT, "stdout", 1);
    (void)klcs_fd_alloc(KLCS_FD_STDERR, "stderr", 2);
}

klcs_state_t* klcs_state(void) {
    return &klcs;
}

void klcs_trace_set(bool enabled) {
    klcs.trace_enabled = enabled;
}

bool klcs_trace_enabled(void) {
    return klcs.trace_enabled;
}

const char* klcs_syscall_name(uint64_t nr) {
    for (size_t i = 0; i < sizeof(syscall_table) / sizeof(syscall_table[0]); ++i) {
        if (syscall_table[i].nr == nr) {
            return syscall_table[i].name;
        }
    }
    return "unknown";
}

bool klcs_syscall_supported(uint64_t nr) {
    for (size_t i = 0; i < sizeof(syscall_table) / sizeof(syscall_table[0]); ++i) {
        if (syscall_table[i].nr == nr) {
            return syscall_table[i].implemented;
        }
    }
    return false;
}

void klcs_trace_record(uint64_t pid, uint64_t nr, const char* name, int64_t result) {
    char* line;
    uint32_t slot;

    if (!klcs.trace_enabled) {
        return;
    }
    slot = klcs.next_trace++ % KLCS_TRACE_LINES;
    line = klcs.last_trace[slot];
    line[0] = '\0';
    klcs_append(line, 96, "[klcs] pid=");
    klcs_append_u64(line, 96, pid);
    klcs_append(line, 96, " ");
    klcs_append(line, 96, name ? name : "unknown");
    klcs_append(line, 96, "(");
    klcs_append_u64(line, 96, nr);
    klcs_append(line, 96, ") -> ");
    if (result < 0) {
        klcs_append(line, 96, "-");
        klcs_append_u64(line, 96, (uint64_t)(-result));
    } else {
        klcs_append_u64(line, 96, (uint64_t)result);
    }
}

void klcs_status(char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    klcs_append(out, out_size, "KLCS: running\n");
    klcs_append(out, out_size, "personality: linux-x86_64\n");
    klcs_append(out, out_size, "ELF64 loader: validation enabled\n");
    klcs_append(out, out_size, "syscall routing: service table enabled\n");
    klcs_append(out, out_size, "filesystem bridge: foundation\n");
    klcs_append(out, out_size, "memory bridge: planned\n");
    klcs_append(out, out_size, "process bridge: partial\n");
    klcs_append(out, out_size, "trace: ");
    klcs_append(out, out_size, klcs.trace_enabled ? "on\n" : "off\n");
    klcs_append(out, out_size, "total syscalls: ");
    klcs_append_u64(out, out_size, klcs.total_syscalls);
    klcs_append(out, out_size, "\nunsupported syscalls: ");
    klcs_append_u64(out, out_size, klcs.unsupported_syscalls);
    klcs_append(out, out_size, "\n");
}

void klcs_syscalls(char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(syscall_table) / sizeof(syscall_table[0]); ++i) {
        klcs_append_u64(out, out_size, syscall_table[i].nr);
        klcs_append(out, out_size, " ");
        klcs_append(out, out_size, syscall_table[i].name);
        klcs_append(out, out_size, syscall_table[i].implemented ? " implemented\n" : " planned\n");
    }
}

void klcs_trace_dump(char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    for (uint32_t i = 0; i < KLCS_TRACE_LINES; ++i) {
        uint32_t idx = (klcs.next_trace + i) % KLCS_TRACE_LINES;
        if (klcs.last_trace[idx][0]) {
            klcs_append(out, out_size, klcs.last_trace[idx]);
            klcs_append(out, out_size, "\n");
        }
    }
    if (!out[0]) {
        klcs_append(out, out_size, "KLCS trace: empty\n");
    }
}

int64_t klcs_linux_errno_from_k64(int64_t k64_status) {
    if (k64_status >= 0) {
        return k64_status;
    }
    switch (k64_status) {
        case K64_ERR_NOENT: return -KLCS_LINUX_ENOENT;
        case K64_ERR_ACCESS: return -KLCS_LINUX_EACCES;
        case K64_ERR_NOMEM: return -KLCS_LINUX_ENOMEM;
        case K64_ERR_FAULT: return -KLCS_LINUX_EFAULT;
        case K64_ERR_NOSYS: return -KLCS_LINUX_ENOSYS;
        case K64_ERR_BADFD: return -KLCS_LINUX_EBADF;
        case K64_ERR_INVAL: return -KLCS_LINUX_EINVAL;
        default: return -KLCS_LINUX_EINVAL;
    }
}

int klcs_fd_alloc(klcs_fd_kind_t kind, const char* path, int native_fd) {
    int start = native_fd >= 0 && native_fd < 3 ? native_fd : 3;

    for (int i = start; i < KLCS_LINUX_FD_MAX; ++i) {
        if (!klcs.fds[i].used) {
            klcs.fds[i].used = true;
            klcs.fds[i].kind = kind;
            klcs.fds[i].native_fd = native_fd;
            klcs.fds[i].cloexec = false;
            klcs_copy(klcs.fds[i].path, sizeof(klcs.fds[i].path), path ? path : "");
            return i;
        }
    }
    return -KLCS_LINUX_EMFILE;
}

bool klcs_fd_close(int fd) {
    if (fd < 0 || fd >= KLCS_LINUX_FD_MAX || fd < 3 || !klcs.fds[fd].used) {
        return false;
    }
    memset(&klcs.fds[fd], 0, sizeof(klcs.fds[fd]));
    return true;
}

bool klcs_translate_path(const char* linux_path, char* out, size_t out_size) {
    if (!linux_path || !out || out_size == 0 || !linux_path[0]) {
        return false;
    }
    if (k64_streq(linux_path, "/")) {
        klcs_copy(out, out_size, "/compat/linux/root");
    } else if (k64_streq(linux_path, "/tmp") || k64_strncmp(linux_path, "/tmp/", 5) == 0) {
        klcs_copy(out, out_size, linux_path);
    } else if (k64_strncmp(linux_path, "/home", 5) == 0) {
        klcs_copy(out, out_size, linux_path);
    } else if (k64_streq(linux_path, "/dev/null") ||
               k64_streq(linux_path, "/dev/zero") ||
               k64_streq(linux_path, "/dev/random") ||
               k64_streq(linux_path, "/dev/urandom") ||
               k64_streq(linux_path, "/proc/self/exe")) {
        klcs_copy(out, out_size, linux_path);
    } else {
        klcs_copy(out, out_size, "/compat/linux/root");
        if (linux_path[0] != '/') {
            klcs_append(out, out_size, "/");
        }
        klcs_append(out, out_size, linux_path);
    }
    return out[0] != '\0';
}

bool klcs_validate_elf64(const uint8_t* data, size_t size, klcs_elf_info_t* out) {
    uint64_t phoff;
    uint16_t phentsize;

    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!data || size < 64) {
        klcs_copy(out->message, sizeof(out->message), "ELF too small");
        return false;
    }
    if (data[0] != KLCS_ELF_MAGIC0 || data[1] != KLCS_ELF_MAGIC1 ||
        data[2] != KLCS_ELF_MAGIC2 || data[3] != KLCS_ELF_MAGIC3) {
        klcs_copy(out->message, sizeof(out->message), "bad ELF magic");
        return false;
    }
    if (data[4] != KLCS_ELFCLASS64 || data[5] != KLCS_ELFDATA2LSB) {
        klcs_copy(out->message, sizeof(out->message), "unsupported ELF class/data");
        return false;
    }
    out->type = read_u16le(data + 16);
    out->machine = read_u16le(data + 18);
    out->entry = read_u64le(data + 24);
    phoff = read_u64le(data + 32);
    phentsize = read_u16le(data + 54);
    out->phnum = read_u16le(data + 56);
    if ((out->type != KLCS_ET_EXEC && out->type != KLCS_ET_DYN) || out->machine != KLCS_EM_X86_64) {
        klcs_copy(out->message, sizeof(out->message), "unsupported ELF target");
        return false;
    }
    if (phentsize < 56 || out->phnum == 0 ||
        phoff > size || (uint64_t)out->phnum * phentsize > size - phoff) {
        klcs_copy(out->message, sizeof(out->message), "bad program header table");
        return false;
    }
    for (uint16_t i = 0; i < out->phnum; ++i) {
        const uint8_t* ph = data + phoff + (uint64_t)i * phentsize;
        uint32_t type = read_u32le(ph);
        uint64_t off = read_u64le(ph + 8);
        uint64_t filesz = read_u64le(ph + 32);
        uint64_t memsz = read_u64le(ph + 40);

        if (type == KLCS_PT_INTERP) {
            out->dynamic = true;
        }
        if (type == KLCS_PT_LOAD &&
            (filesz > memsz || off > size || filesz > size - off)) {
            klcs_copy(out->message, sizeof(out->message), "bad PT_LOAD bounds");
            return false;
        }
    }
    out->valid = true;
    klcs_copy(out->message,
              sizeof(out->message),
              out->dynamic ? "dynamic ELF not supported yet by KLCS MVP" : "ELF64 static candidate");
    return true;
}

int64_t klcs_dispatch_syscall(const klcs_linux_syscall_frame_t* frame) {
    int64_t rc = -KLCS_LINUX_ENOSYS;
    const char* name;

    if (!frame) {
        return -KLCS_LINUX_EFAULT;
    }
    name = klcs_syscall_name(frame->nr);
    klcs.total_syscalls++;
    switch (frame->nr) {
        case 1:
#ifndef K64_HOST_TEST
            if ((frame->arg0 == 1 || frame->arg0 == 2) && frame->arg1 && frame->arg2 < 4096) {
                const char* p = (const char*)(uintptr_t)frame->arg1;
                for (uint64_t i = 0; i < frame->arg2 && p[i]; ++i) {
                    k64_term_putc(p[i]);
                }
                rc = (int64_t)frame->arg2;
            } else
#endif
            {
                rc = -KLCS_LINUX_EBADF;
            }
            break;
        case 3:
            rc = klcs_fd_close((int)frame->arg0) ? 0 : -KLCS_LINUX_EBADF;
            break;
        case 39:
            rc = (int64_t)frame->pid;
            break;
        case 60:
        case 231:
            rc = 0;
            break;
        case 102:
        case 104:
        case 107:
        case 108:
            rc = 0;
            break;
        default:
            klcs.unsupported_syscalls++;
            rc = -KLCS_LINUX_ENOSYS;
            break;
    }
    klcs_trace_record(frame->pid, frame->nr, name, rc);
    return rc;
}
