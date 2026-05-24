#include <k64/libc.h>

#define K64_SYSCALL_EXIT   0ULL
#define K64_SYSCALL_WRITE  1ULL
#define K64_SYSCALL_YIELD  2ULL
#define K64_SYSCALL_SLEEP  3ULL
#define K64_SYSCALL_OPEN   4ULL
#define K64_SYSCALL_READ   5ULL
#define K64_SYSCALL_CLOSE  6ULL
#define K64_SYSCALL_GETPID 7ULL
#define K64_SYSCALL_UPTIME 8ULL
#define K64_SYSCALL_WRITEFILE 9ULL
#define K64_SYSCALL_CLEAR 10ULL
#define K64_SYSCALL_READKEY 11ULL
#define K64_SYSCALL_CURSOR 12ULL
#define K64_SYSCALL_TERMSIZE 13ULL
#define K64_SYSCALL_FBINFO 14ULL
#define K64_SYSCALL_FBBLIT 15ULL
#define K64_SYSCALL_SPAWN 16ULL
#define K64_SYSCALL_READKEY_NB 17ULL
#define K64_SYSCALL_LISTDIR 18ULL
#define K64_SYSCALL_MOVE 19ULL
#define K64_SYSCALL_PROCINFO 20ULL
#define K64_SYSCALL_WAITPID 21ULL
#define K64_SYSCALL_PIPE 22ULL
#define K64_SYSCALL_WRITEFD 23ULL

int64_t k64_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    int64_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

void k64_exit(int code) {
    (void)k64_syscall3(K64_SYSCALL_EXIT, (uint64_t)(int64_t)code, 0, 0);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

int64_t k64_write_fd(int fd, const void* data, size_t len) {
    uint64_t nr = fd <= K64_STDERR ? K64_SYSCALL_WRITE : K64_SYSCALL_WRITEFD;
    return k64_syscall3(nr, (uint64_t)(int64_t)fd, (uint64_t)(uintptr_t)data, (uint64_t)len);
}

int64_t k64_write(const void* data, size_t len) {
    return k64_write_fd(K64_STDOUT, data, len);
}

void k64_yield(void) {
    (void)k64_syscall3(K64_SYSCALL_YIELD, 0, 0, 0);
}

void k64_sleep(uint64_t ticks) {
    (void)k64_syscall3(K64_SYSCALL_SLEEP, ticks, 0, 0);
}

int64_t k64_open(const char* path) {
    return k64_syscall3(K64_SYSCALL_OPEN, (uint64_t)(uintptr_t)path, 0, 0);
}

int64_t k64_read(int fd, void* data, size_t len) {
    return k64_syscall3(K64_SYSCALL_READ, (uint64_t)(int64_t)fd, (uint64_t)(uintptr_t)data, (uint64_t)len);
}

int64_t k64_close(int fd) {
    return k64_syscall3(K64_SYSCALL_CLOSE, (uint64_t)(int64_t)fd, 0, 0);
}

int64_t k64_read_stdin(void* data, size_t len) {
    return k64_read(K64_STDIN, data, len);
}

int64_t k64_write_file(const char* path, const void* data, size_t len) {
    return k64_syscall3(K64_SYSCALL_WRITEFILE,
                        (uint64_t)(uintptr_t)path,
                        (uint64_t)(uintptr_t)data,
                        (uint64_t)len);
}

void k64_clear_screen(void) {
    (void)k64_syscall3(K64_SYSCALL_CLEAR, 0, 0, 0);
}

int64_t k64_read_key(void) {
    return k64_syscall3(K64_SYSCALL_READKEY, 0, 0, 0);
}

int64_t k64_read_key_nonblock(void) {
    return k64_syscall3(K64_SYSCALL_READKEY_NB, 0, 0, 0);
}

void k64_set_cursor(int x, int y) {
    (void)k64_syscall3(K64_SYSCALL_CURSOR, (uint64_t)(int64_t)x, (uint64_t)(int64_t)y, 0);
}

int k64_term_cols(void) {
    int64_t packed = k64_syscall3(K64_SYSCALL_TERMSIZE, 0, 0, 0);
    return packed < 0 ? 80 : (int)(packed & 0xFFFF);
}

int k64_term_rows(void) {
    int64_t packed = k64_syscall3(K64_SYSCALL_TERMSIZE, 0, 0, 0);
    return packed < 0 ? 25 : (int)((packed >> 16) & 0xFFFF);
}

int64_t k64_fb_info(k64_fb_info_t* info) {
    return k64_syscall3(K64_SYSCALL_FBINFO, (uint64_t)(uintptr_t)info, 0, 0);
}

int64_t k64_fb_blit(const k64_fb_blit_t* blit) {
    return k64_syscall3(K64_SYSCALL_FBBLIT, (uint64_t)(uintptr_t)blit, 0, 0);
}

int64_t k64_spawn(const char* path, const char* args) {
    return k64_syscall3(K64_SYSCALL_SPAWN,
                        (uint64_t)(uintptr_t)path,
                        (uint64_t)(uintptr_t)args,
                        0);
}

int64_t k64_list_dir(const char* path, char* out, size_t len) {
    return k64_syscall3(K64_SYSCALL_LISTDIR,
                        (uint64_t)(uintptr_t)path,
                        (uint64_t)(uintptr_t)out,
                        (uint64_t)len);
}

int64_t k64_move(const char* src, const char* dst) {
    return k64_syscall3(K64_SYSCALL_MOVE,
                        (uint64_t)(uintptr_t)src,
                        (uint64_t)(uintptr_t)dst,
                        0);
}

int64_t k64_getpid(void) {
    return k64_syscall3(K64_SYSCALL_GETPID, 0, 0, 0);
}

int64_t k64_proc_info(uint64_t pid, k64_proc_info_t* info) {
    return k64_syscall3(K64_SYSCALL_PROCINFO,
                        pid,
                        (uint64_t)(uintptr_t)info,
                        0);
}

int64_t k64_waitpid(uint64_t pid, int64_t* exit_code) {
    return k64_waitpid_flags(pid, exit_code, K64_WAIT_BLOCK);
}

int64_t k64_waitpid_flags(uint64_t pid, int64_t* exit_code, uint64_t flags) {
    return k64_syscall3(K64_SYSCALL_WAITPID,
                        pid,
                        (uint64_t)(uintptr_t)exit_code,
                        flags);
}

int64_t k64_pipe(int fds[2]) {
    return k64_syscall3(K64_SYSCALL_PIPE,
                        (uint64_t)(uintptr_t)fds,
                        0,
                        0);
}

uint64_t k64_uptime_ticks(void) {
    int64_t ticks = k64_syscall3(K64_SYSCALL_UPTIME, 0, 0, 0);
    return ticks < 0 ? 0 : (uint64_t)ticks;
}

size_t k64_strlen(const char* text) {
    size_t len = 0;
    if (!text) {
        return 0;
    }
    while (text[len]) {
        len++;
    }
    return len;
}

int k64_strcmp(const char* a, const char* b) {
    unsigned char ca;
    unsigned char cb;

    if (!a) {
        a = "";
    }
    if (!b) {
        b = "";
    }
    while (*a || *b) {
        ca = (unsigned char)*a;
        cb = (unsigned char)*b;
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        a++;
        b++;
    }
    return 0;
}

int k64_strncmp(const char* a, const char* b, size_t len) {
    unsigned char ca;
    unsigned char cb;

    if (!a) {
        a = "";
    }
    if (!b) {
        b = "";
    }
    for (size_t i = 0; i < len; ++i) {
        ca = (unsigned char)a[i];
        cb = (unsigned char)b[i];
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        if (ca == '\0') {
            return 0;
        }
    }
    return 0;
}

int k64_memcmp(const void* a, const void* b, size_t len) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;

    for (size_t i = 0; i < len; ++i) {
        if (pa[i] != pb[i]) {
            return pa[i] < pb[i] ? -1 : 1;
        }
    }
    return 0;
}

void* k64_memcpy(void* dst, const void* src, size_t len) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;

    for (size_t i = 0; i < len; ++i) {
        d[i] = s[i];
    }
    return dst;
}

void* k64_memset(void* dst, int value, size_t len) {
    unsigned char* d = (unsigned char*)dst;

    for (size_t i = 0; i < len; ++i) {
        d[i] = (unsigned char)value;
    }
    return dst;
}

char* k64_strcpy(char* dst, const char* src) {
    size_t i = 0;

    if (!dst) {
        return dst;
    }
    if (!src) {
        src = "";
    }
    do {
        dst[i] = src[i];
    } while (src[i++] != '\0');
    return dst;
}

char* k64_strncpy(char* dst, const char* src, size_t len) {
    size_t i = 0;

    if (!dst || len == 0) {
        return dst;
    }
    if (!src) {
        src = "";
    }
    while (i < len && src[i]) {
        dst[i] = src[i];
        i++;
    }
    while (i < len) {
        dst[i++] = '\0';
    }
    return dst;
}

char* k64_strcat(char* dst, const char* src) {
    size_t pos;

    if (!dst) {
        return dst;
    }
    pos = k64_strlen(dst);
    k64_strcpy(dst + pos, src ? src : "");
    return dst;
}

int k64_atoi(const char* text) {
    int sign = 1;
    int value = 0;

    while (text && (*text == ' ' || *text == '\t')) {
        text++;
    }
    if (text && *text == '-') {
        sign = -1;
        text++;
    }
    while (text && *text >= '0' && *text <= '9') {
        value = value * 10 + (*text - '0');
        text++;
    }
    return value * sign;
}

void k64_putc(char ch) {
    (void)k64_write(&ch, 1);
}

void k64_puts(const char* text) {
    (void)k64_write(text, k64_strlen(text));
}

void k64_put_u64(uint64_t value) {
    char buf[21];
    size_t pos = sizeof(buf);

    buf[--pos] = '\0';
    if (value == 0) {
        buf[--pos] = '0';
    } else {
        while (value > 0 && pos > 0) {
            buf[--pos] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    k64_puts(&buf[pos]);
}

void k64_put_i64(int64_t value) {
    if (value < 0) {
        k64_puts("-");
        k64_put_u64((uint64_t)(-value));
        return;
    }
    k64_put_u64((uint64_t)value);
}

void k64_put_hex64(uint64_t value) {
    static const char hex[] = "0123456789abcdef";
    char buf[19];

    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        unsigned shift = (unsigned)(60 - (i * 4));
        buf[2 + i] = hex[(value >> shift) & 0xFULL];
    }
    buf[18] = '\0';
    k64_puts(buf);
}
