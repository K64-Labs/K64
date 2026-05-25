#include <k64/libc.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    k64_puts("hello from K64 user mode\n");
    return 42;
}
