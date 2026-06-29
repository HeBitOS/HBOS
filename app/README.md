# HBOS 应用目录（./app）

把你的 HAX 应用源码（`*.c`）放进这个目录。`make` 会自动：

1. 用 HAX 运行时把每个 `app/*.c` 编译为 `build/app/<名字>.hax`（标准 ELF64）。
2. 读取每个 `.hax` 的 `.haxmeta` 自描述段，把它们打包进内核。
3. 系统启动时自动注册——**只要 `./app` 里有编译产物，就加入系统**，无需手动登记。

应用随后出现在 `apps` 列表，可用 `run <名字>` 运行；标记为 GUI 的应用还会出现在
图形桌面的启动器中（在终端窗口运行）。

## 最小模板

```c
#include <hax.h>

HAX_APP("myapp", "一句话描述", HAX_KIND_TUI);   // 或 HAX_KIND_GUI / HAX_KIND_BOTH

int main(int argc, char **argv) {
    hax_println("Hello from my .hax app!");
    return 0;
}
```

完整 API 见根目录的《HBOS 应用开发手册》（HBOS_HAX_API.pdf）。
SDK 头文件：[`app/include/hax.h`](include/hax.h)。

## 直接放预编译的 .hax

如果你已经有别处编译好的 `.hax`（标准 ELF64 + `.haxmeta` 段），直接丢进
`app/` 即可，下次 `make` 会一并打包进系统。
