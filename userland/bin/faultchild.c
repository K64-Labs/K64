#include <k64/libc.h>

int main(int argc, char** argv) {
    volatile uint64_t* bad = (volatile uint64_t*)(uintptr_t)0x00000000FFFFF000ULL;

    (void)argc;
    (void)argv;
    *bad = 0x1234;
    return 1;
}
