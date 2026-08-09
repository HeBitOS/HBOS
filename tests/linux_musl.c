#include <unistd.h>

/*
 * Keep the on-disk ELF above the former 512 KiB execve ceiling.  Both ends
 * are read at runtime so a truncated/partial PT_LOAD copy cannot pass.
 */
#define STREAM_PADDING_SIZE (600U * 1024U)
static const unsigned char stream_padding[STREAM_PADDING_SIZE]
    __attribute__((used)) = {
        [0] = 0x48,
        [STREAM_PADDING_SIZE - 1] = 0x54
    };

int main(void) {
    static const char message[] = "LINUX_MUSL: PASS\n";
    if (stream_padding[0] != 0x48 ||
        stream_padding[STREAM_PADDING_SIZE - 1] != 0x54)
        return 2;
    return write(STDOUT_FILENO, message, sizeof(message) - 1) ==
           (long)(sizeof(message) - 1) ? 0 : 1;
}
