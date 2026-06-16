# Input — 输入子系统 (`src/input/`)

> PS/2 键盘/鼠标, USB HID 框架 (xHCI 控制器未实现)

## 文件清单

| 文件 | 职责 |
|------|------|
| `mouse.c` + `mouse.h` | PS/2 鼠标驱动 + USB fallback |
| `src/usb_hid.c` + `usb_hid.h` | USB HID 框架 (解析报告描述符) |
| `src/xhci.c` + `xhci.h` | xHCI USB 控制器框架 (部分实现) |
| `src/usb_msc.c` + `usb_msc.h` | USB Mass Storage 框架 |

## 键盘 (键盘 ISR 在 `src/core/gdt_idt.c` 的 kb_irq_handler)

键盘数据处理：
- **ISR** (`kb_irq_handler`): 读 0x60 端口, 入队 scancode ring buffer (`kb_irq_enqueue_scancode`)
- **任务上下文** (`kb_poll_key`): 从 ring buffer 出队, 解析 scancode→ASCII (US QWERTY layout, shift/ctrl 处理)
- 实现分散在 `src/graphics/graphics.c` 和 shell 相关代码中

```c
// PS/2 端口
#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

// 键盘 LED 控制
// 写 0xED 命令到 0x60 端口, 等待 ACK (0xFA), 再写 LED 位掩码
```

## 鼠标 (`mouse.c`) — 当前最新版本 (刚重构)

```c
int mouse_init(void);         // 先试 PS/2, 再试 USB; 返回 0=成功
void mouse_shutdown(void);    // 发送 0xF5 停止数据报告
int mouse_poll(mouse_event_t *ev);  // 轮询一次鼠标事件

typedef struct {
    int dx, dy;      // 相对位移
    int dz;          // 滚轮 (PS/2 时来自第 4 字节 Z 轴)
    int buttons;      // MOUSE_LEFT=0x01, MOUSE_RIGHT=0x02, MOUSE_MIDDLE=0x04
    int abs_x, abs_y; // USB 绝对坐标 (-1 = 相对模式)
    int64_t tick;     // 帧序号
} mouse_event_t;
```

### PS/2 鼠标初始化流程 (重构后)

```
int_disable();
// 1. 读 8042 config byte (cmd 0x20, data port 0x60)
// 2. 清除 bit 5 (disable auxiliary clock) ← 必须在 0xA8 之前!
// 3. 设置 bit 1 (enable IRQ12)              ← 防止数据走 IRQ1
// 4. 写回 8042 config (cmd 0x60)
// 5. 发 0xA8 使能 aux port
// 6. flush_output
// 7. 发 0xF4 (enable data reporting) 到 aux
//    失败 → 0xFF reset → 等 0xAA bat pass → 重试
int_enable();
```

### PS/2 鼠标轮询 (重构后)

```c
int_disable();
while (0x64 status bit 0 && bit 5) { // aux data available?
    b = inb(0x60);
    // 等 3 (或 4) 字节完整 packet
    // 解析: byte0=flags(B1=right,B2=left,B4=dx sign,B5=dy sign,B6=B7=x/y overflow)
    //       byte1=dx, byte2=dy, byte3=dz (有 scroll wheel 时)
}
int_enable();
// 返回累计位移 (多 packet 累加)
```

### USB mouse fallback

```c
int usb_mouse_init(void);    // 调用 xHCI 扫描 HID 设备 (目前返回 -1, 无控制器)
int usb_hid_init(void);      // USB HID 子系统初始化
int usb_hid_poll(void);      // 轮询 USB HID 报告
// 注意: xHCI 仅框架, usb_mouse_init 总是返回 -1!
```

## 当前状态与 TODO

| 项 | 状态 |
|----|------|
| PS/2 键盘 | ✅ 可用, ISR ring buffer 方式 |
| PS/2 鼠标 | ✅ 可用, 含加速曲线 (在 gui.c 的 clamp_delta) |
| USB 键盘 | ❌ xHCI 框架未实现 |
| USB 鼠标 | ❌ 同上 |
| USB 存储 | ❌ xHCI 框架未实现 |

## 调试技巧

- 检查 `drivers` 命令输出:
  - `PS/2 keyboard: ready` → 正常
  - `mouse backend: ps2` → 鼠标 PS/2 正常
  - `mouse backend: none` → 鼠标初始化失败
- VirtualBox: 确保 System→Pointing Device 设为 "PS/2 Mouse" (非 USB Tablet!)
- QEMU: `-device pci-ohci` 或 `-usb -device usb-mouse` 仅 USB HID 测试用
- 鼠标乱动/点不到: 检查 gui.c 的 `clamp_delta` 加速度曲线
