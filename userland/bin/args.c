#include <k64/libc.h>

int main(int argc, char** argv) {
    k64_puts("args: argc=");
    k64_put_i64(argc);
    k64_puts("\n");

    for (int i = 0; i < argc; ++i) {
        k64_puts("argv[");
        k64_put_i64(i);
        k64_puts("]=");
        k64_puts(argv && argv[i] ? argv[i] : "(null)");
        k64_puts("\n");
    }
    return 0;
}
