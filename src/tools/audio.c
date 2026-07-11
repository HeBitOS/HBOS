#include "../ac97.h"
#include "../wav.h"
#include "../fs.h"
#include "../graphics/graphics.h"
#include "tool.h"

static void print_uint(uint32_t v) {
    char buf[16];
    int n = 0;
    do {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (n--) console_putchar(buf[n]);
}

/* AC97 是懒初始化的：没有声卡（比如没挂 -device AC97）的机器上，一直
 * 探测失败也没关系，playwav 用到的时候才报错，不影响没有声卡时的正常
 * 启动。 */
static int g_ac97_tried = 0;

static int ensure_ac97(void) {
    if (ac97_present()) return 1;
    if (g_ac97_tried) return 0;
    g_ac97_tried = 1;
    return ac97_init() == 0;
}

/* 目前一次性把整个文件读进这块静态缓冲——WAV 播放上限本来就卡在
 * ac97_play_pcm16() 的 buffer descriptor 容量（32 条 * 0xFFFE 采样点 ≈
 * 4MB PCM 数据，48kHz 立体声下约 22 秒），不需要流式读取，缓冲区按这个
 * 上限留一点余量就够。 */
#define PLAYWAV_MAX_BYTES (5 * 1024 * 1024)
static uint8_t g_playwav_buf[PLAYWAV_MAX_BYTES];

static void cmd_playwav(int argc, char **argv) {
    if (argc < 2) {
        console_puts("usage: playwav <path>\n");
        return;
    }
    if (!ensure_ac97()) {
        console_puts("\x1b[31mplaywav: 未找到 AC97 声卡\x1b[0m\n");
        return;
    }

    file_t *f = fs_find_file(argv[1]);
    if (!f) {
        console_puts("\x1b[31mplaywav: 文件不存在\x1b[0m\n");
        return;
    }
    uint32_t n = f->size;
    if (n > PLAYWAV_MAX_BYTES) {
        console_puts("\x1b[31mplaywav: 文件太大\x1b[0m\n");
        return;
    }
    uint32_t got = fs_read_file_data(f, 0, g_playwav_buf, n);
    if (got != n) {
        console_puts("\x1b[31mplaywav: 读取失败\x1b[0m\n");
        return;
    }

    wav_info_t info;
    if (wav_parse(g_playwav_buf, n, &info) < 0) {
        console_puts("\x1b[31mplaywav: 不是 PCM/16 位 WAV 文件\x1b[0m\n");
        return;
    }

    console_puts("playwav: ");
    print_uint(info.sample_rate);
    console_puts("Hz ");
    print_uint(info.channels);
    console_puts("ch ");
    print_uint(info.frame_count);
    console_puts(" 帧");
    if (info.sample_rate != 48000) {
        console_puts("\x1b[33m（非 48000Hz，音调/速度会不对，未实现重采样）\x1b[0m");
    }
    console_puts("\n");

    if (ac97_play_pcm16(info.pcm, info.frame_count, info.channels) < 0) {
        console_puts("\x1b[31mplaywav: 播放失败（可能太长，一次最多约 44 秒立体声）\x1b[0m\n");
        return;
    }
    console_puts("\x1b[32mplaywav: 播放完成\x1b[0m\n");
}

void tool_audio_init(void) {
    static const command_t cmds[] = {
        {"playwav", CMD_GROUP_GRAPHICS, "Play a 16-bit PCM WAV file via AC97", "playwav <path>", cmd_playwav},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
