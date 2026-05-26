#include <k64/libc.h>

int main(int argc, char** argv) {
    const char msg[] = "ok";

    (void)argc;
    (void)argv;
    if (k64_write_file("/tmp/bgspawn-ok", msg, sizeof(msg)) != K64_OK) {
        return 2;
    }
    return 11;
}
