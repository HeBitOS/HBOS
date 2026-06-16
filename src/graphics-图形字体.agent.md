# Graphics — 图形与字体 (`src/graphics/`)

> Framebuffer 图形输出, CJK 位图字体渲染, VESA/VGA 初始化

## 文件清单

| 文件 | 职责 |
|------|------|
| `graphics.c` + `.h` | Framebuffer 初始化, 像素/矩形/线条绘制, 控制台输出, 键盘输入 |
| `font_cjk.c` + `.h` | CJK 位图字体加载和渲染 (从 `fonts/ZhengGeDianHei-16.ttf` 预生成) |
| `cjk_glyph.S` / `.asm` | CJK 字形位图数据 (汇编嵌入) |

## Framebuffer 初始化

```c
// graphics.c
void graphics_init(void *mbi);     // 从 Multiboot2 获取 framebuffer tag
void console_clear(void);          // 清除整个屏幕
void console_print(const char *s); // 打印字符串 (自动 \n 换行)
void console_putc(char c);         // 打印单个字符 (支持 CJK)
void console_set_cursor(int x, int y);
int kb_poll_key(void);             // 阻塞读取一个按键 (ASCII 码)

// fb_info_t (帧缓冲信息):
typedef struct {
    uint32_t *fb;        // framebuffer 基地址 (虚拟地址)
    int width, height;   // 分辨率
    int pitch;           // 每行字节数
    int bpp;             // 位深 (24 bpp → 32-bit 像素)
} fb_info_t;
```

## 绘制 API

```c
// 像素
void gfx_draw_pixel(int x, int y, uint32_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);

// 形状
void gfx_draw_hline(int x, int y, int w, uint32_t color);
void gfx_draw_vline(int x, int y, int h, uint32_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);
void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color);

// 文字
void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);  // 8x16 ASCII
void gfx_draw_cjk(int x, int y, uint32_t codepoint, uint32_t fg, uint32_t bg); // CJK

// 颜色
#define RGB(r,g,b) (((r)<<16)|((g)<<8)|(b))
```

## CJK 字体渲染

```c
// font_cjk.c
void font_cjk_init(void);                                     // 初始化字形查找表
int  font_cjk_render(uint32_t codepoint, uint8_t bitmap[32]); // 渲染一个 CJK 字符→16x16 位图 (32 bytes)
// 字形数据: cjk_glyph.S 中包含预生成的位图数组
// 编码范围: 覆盖常用汉字 3000+
// 字符宽度: 16x16 像素
```

## 控制台功能

```c
// graphics.c 内部:
// 光标位置: cursor_x, cursor_y (字符坐标, 8x16 每格)
// 自动换行和滚屏
// 转义序列: 基础支持
// 颜色: 白色文字/黑色背景, 彩色横幅
```

## 分辨率支持

| 分辨率 | 状态 |
|--------|------|
| 1024x768 | ✅ 默认 (VirtualBox VESA 兼容) |
| 800x600 | ✅ 回退 |
| 1440x900 | ⚠️ 暂不优先 (VirtualBox 不支持) |
| VGA 文本 | ✅ 无 framebuffer 时回退 |

## 与 GUI 的关系

GUI (`tools/gui.c`) 直接从 `fb_info_t->fb` 绘制, 不经过控制台层:
```c
// gui.c 直接操作:
fb_info_t *fb = graphics_get_fb();
uint32_t *fb32 = fb->fb;
// 直接写入 fb32[y * fb->pitch/4 + x]
```
