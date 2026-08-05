# HIVE Chromium 后端

这里保存可审计的小型移植层、固定版本和构建参数，不把 Chromium 的超大上游
源码提交进 HBOS/HIVE 仓库。源码工作树由 `scripts/chromium_source.sh` 管理，默认
位置是外部大容量盘上的 `hbos-browser-work/chromium/src`。

当前固定 Linux Stable `151.0.7922.71`，精确提交见 `VERSION`。固定提交的目的
是让 Blink、V8、Skia、FFmpeg 及其依赖可重复同步，而不是跟随滚动的 `main`。

## 两阶段构建

1. `args.host.gn` 先生成 Linux 主机参考壳。它用于验证源码、GN/Ninja、Blink、
   V8、Vue 验收页和 Bilibili 页面能力，不是最终 HBOS 产物。
2. `args.hbos.gn` 是 HBOS 目标配置。只有 Chromium 上游源码中的 GN 平台识别、
   HBOS sysroot 和 `//hbos` Ozone/IPC 适配补丁就绪后才会成功生成。

这样可把“网页引擎自身是否工作”和“HBOS 平台层是否完整”分开定位，Lite 后端
始终保留为低资源与故障回退路径。

## 常用命令

```sh
HBOS_CHROMIUM_WORKDIR=/path/to/hbos-browser-work \
  scripts/chromium_source.sh status

HBOS_CHROMIUM_WORKDIR=/path/to/hbos-browser-work \
  scripts/chromium_source.sh pin

HBOS_CHROMIUM_WORKDIR=/path/to/hbos-browser-work \
  scripts/chromium_source.sh hooks

HBOS_CHROMIUM_WORKDIR=/path/to/hbos-browser-work \
  scripts/chromium_source.sh host-gen

HBOS_CHROMIUM_WORKDIR=/path/to/hbos-browser-work \
HBOS_CHROMIUM_JOBS=4 scripts/chromium_source.sh host-build
```

`pin` 会把 `src` 切到 `VERSION` 中的精确提交并同步对应依赖；`hooks` 单独运行
上游 hooks，便于网络中断后继续。工作目录至少预留 100 GB。
若官方 Artifact Registry 在本机网络被中断，脚本会优先使用外部工作区的
`bin/vpython3` 启动器；该启动器只属于本机工具缓存，不提交到 HBOS 仓库。

## 五项验收

- G0：Lite 能力诊断及离线回归。
- G1：动态 ELF、pthread/futex、IPC、IPv6/X.509、窗口与音频基础。
- G2：Blink/V8 执行 HTML、完整 CSS、ES modules、fetch、Vue 3 hydration。
- G3：Bilibili 首页完成 SSR hydration、导航、图片和交互。
- G4：Bilibili 视频页完成 MSE、解码、音频输出与 720p 连续播放。

通过必须由自动验收页或启动日志证明，不能仅以“能够编译”代替。
