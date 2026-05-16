#include <k64/libc.h>

#define K64_SYSCALL_EXIT   0ULL
#define K64_SYSCALL_WRITE  1ULL
#define K64_SYSCALL_OPEN   4ULL
#define K64_SYSCALL_READ   5ULL
#define K64_SYSCALL_CLOSE  6ULL
#define K64_SYSCALL_GETPID 7ULL
#define K64_SYSCALL_UPTIME 8ULL

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

int64_t k64_write(const void* data, size_t len) {
    return k64_syscall3(K64_SYSCALL_WRITE, (uint64_t)(uintptr_t)data, (uint64_t)len, 0);
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

int64_t k64_getpid(void) {
    return k64_syscall3(K64_SYSCALL_GETPID, 0, 0, 0);
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
