#pragma once
#include <stddef.h>

size_t k64_strlen(const char* s);
int    k64_strncmp(const char* a, const char* b, size_t n);
int    k64_strcmp(const char* a, const char* b);
int    k64_streq(const char* a, const char* b);

void*  memcpy(void* dst, const void* src, size_t n);
void*  memset(void* dst, int value, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
