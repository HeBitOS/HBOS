#define STREAM_PADDING_SIZE (600U * 1024U)

static const unsigned char stream_padding[STREAM_PADDING_SIZE]
    __attribute__((used)) = {
        [0] = 0x12,
        [STREAM_PADDING_SIZE - 1] = 0x2a
    };

__attribute__((visibility("default")))
int hbos_large_answer(void) {
    const volatile unsigned char *bytes = stream_padding;
    return bytes[0] == 0x12 && bytes[STREAM_PADDING_SIZE - 1] == 0x2a ?
           42 : -1;
}
