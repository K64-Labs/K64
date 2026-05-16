#include "k64_string.h"

size_t k64_strlen(const char* s) {
    size_t n = 0;

    if (!s) {
        return 0;
    }

    while (s[n]) {
        n++;
    }

    return n;
}

int k64_strncmp(const char* a, const char* b, size_t n) {
    while (n-- && *a && *b) {
        if (*a != *b) {
            return (unsigned char)*a - (unsigned char)*b;
        }
        a++;
        b++;
    }

    if ((int)n >= 0 && (*a || *b)) {
        return (unsigned char)*a - (unsigned char)*b;
    }

    return 0;
}

int k64_strcmp(const char* a, const char* b) {
    size_t max_len = k64_strlen(a);
    size_t b_len = k64_strlen(b);

    if (b_len > max_len) {
        max_len = b_len;
    }

    return k64_strncmp(a ? a : "", b ? b : "", max_len + 1);
}

int k64_streq(const char* a, const char* b) {
    return k64_strcmp(a, b) == 0;
}

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;

    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dst;
}

void* memset(void* dst, int value, size_t n) {
    unsigned char* d = (unsigned char*)dst;

    for (size_t i = 0; i < n; ++i) {
        d[i] = (unsigned char)value;
    }
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;

    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        for (size_t i = 0; i < n; ++i) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}
