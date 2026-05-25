#include <k64/libc.h>

int main(int argc, char** argv) {
    int64_t pid = k64_getpid();

    if (pid < 0) {
        return 1;
    }
    if (argc > 1 && argv && argv[1]) {
        (void)argv[1];
    }
    return 0;
}
