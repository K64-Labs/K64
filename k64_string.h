#pragma once
#include <stddef.h>

size_t k64_strlen(const char* s);
int    k64_strncmp(const char* a, const char* b, size_t n);
int    k64_strcmp(const char* a, const char* b);
int    k64_streq(const char* a, const char* b);
