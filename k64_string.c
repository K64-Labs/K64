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
