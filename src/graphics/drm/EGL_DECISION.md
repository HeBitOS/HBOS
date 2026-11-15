# HBOS 软件 GL（EGL）通路结论

> 范围：KWin/Plasma 的 DRM 后端在 HBOS 上需要什么图形栈，哪条软件 GL 路线
> 值得做、哪条明确不做。
> 状态：结论 + 依赖测量，不含实现。所有测量都是在宿主（Arch x86_64，
> Mesa 26.2.6）上做的，见文末"证据"。

## 0. 一句话结论

**先不要上 EGL。** KWin 的合成器有三种后端，其中 **QPainter（CPU 合成）
不需要 EGL/GL**；只要目标 Plasma 版本仍保留该后端，我们现有的
KMS + dumb buffer 就足以把 KWin 跑起来（慢，但完整）。

如果目标版本强制 OpenGL 合成，唯一值得做的是
**Mesa gallium `softpipe`（`kms_swrast`）+ 自写的 `libdrm`/`libgbm` 薄封装**；
**不要**移植 llvmpipe，**不要**选 ANGLE + SwiftShader。

## 1. 为什么"KWin 能认出 KMS 设备"是第一验收点

KWin 的 DRM 后端启动顺序是：

1. `open("/dev/dri/card0")` → `drmGetVersion/GetCapabilities/GetResources`
2. `drmModeGetConnector` 取模式，`drmModeCreateDumbBuffer` 探测 dumb 支持
3. `gbm_create_device(fd)` → `gbm_surface_create`/`gbm_bo_create`
4. `eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_device)` → `eglInitialize`
5. `eglCreateContext` + `eglMakeCurrent` → 查询 `GL_RENDERER`

第 1–2 步全在我们已实现的 ioctl 里；第 3 步起才进入 GBM/EGL。**前两步过不去，
后面什么都谈不上**，所以先把 1–2 做成可自检的（`drm_card_selftest`）。

## 2. 三条路线对比

| 路线 | 组成 | 依赖 | 工作量 | 判定 |
|---|---|---|---|---|
| A. QPainter（CPU 合成） | KWin `QRasterBackend` + DRM dumb buffer | 无 EGL、无 GL、无 GBM | 只需我们的 KMS + dumb 可见 | **首选，先验证版本可用性** |
| B. Mesa softpipe | `libEGL`/`libGLESv2` + `kms_swrast_dri.so` + 自写 `libdrm`/`libgbm` | 纯 C、无 JIT；需 pthread/TLS/mmap/exec-free | 大（Mesa 子集构建）但可控 | 需要 EGL 时的**唯一**推荐 |
| C. ANGLE + SwiftShader | `libEGL(ANGLE)`→GLES→Vulkan→`libvk_swiftshader.so` | C++17、Vulkan loader、无 GBM 后端 | 极大且路径不通 | **不建议** |

### 2.1 A. QPainter：为什么它是最省的一条

- KWin 的 QPainter 后端把窗口内容画进 QImage，然后走 **dumb buffer + page
  flip** 上屏 —— 正是我们已经做好的东西。它不调用 EGL，不需要 GBM。
- 风险：**Plasma 6 / KWin 6 是否还保留 QPainter 后端需要按目标版本确认**。
  KWin 5.27 明确支持（`KWIN_COMPOSE=Q`）；KWin 6 的精简里它被反复讨论删除。
  这条验证不需要 QEMU：解包目标镜像的 `kwin_wayland`，反查字符串
  `QPainter` / `QRasterBackend` / `OpenGL` 即可判定。
- 若可用，本任务之后应该做的是"KWin 探测全绿 + dumb 上屏"，而不是 EGL。

### 2.2 B. Mesa softpipe：可行，但要做三块胶水

测量（Arch all-drivers 构建，属"上限"，自建可更小）：

| 组件 | 体积 | 备注 |
|---|---|---|
| `libdrm.so.2` | 92 KB | 我们只需要它的 ioctl 封装层，可自写 |
| `libgbm.so.1.0.0` | 18 KB | 只做 dumb buffer + bo 映射，可自写 |
| `libEGL_mesa.so` | 396 KB | Mesa 提供 |
| `libgallium-*.so` | 55 MB（含全部驱动） | 自建只留 `swrast` 会小一个数量级 |
| `libLLVM.so` | **172 MB** | **只有 llvmpipe/zink 需要**；关闭 LLVM 后 softpipe 不需要 |

`kms_swrast_dri.so` 的 `ldd` 只要求 `libgbm/libm/libc/libdrm/libexpat/libgcc_s`，
**不直接依赖 LLVM**；LLVM 是 `libgallium.so` 引入的。这证明"关掉 LLVM 的
softpipe 构建"在依赖上是成立的。

落地分解（按可验收粒度）：

1. **libdrm 薄封装**：`drmIoctl/drmModeGetResources/drmModeGetConnector/
   drmModeAddFB2/drmModeSetCrtc/drmModePageFlip/drmHandleEvent`。
   我们的 ioctl ABI 已与 libdrm 逐字节对齐（20 个 ioctl 号 + 14 个结构体
   尺寸实测一致），所以这层只是"把参数搬过去"。
2. **libgbm 薄封装**：`gbm_create_device` / `gbm_bo_create`（→ CREATE_DUMB）/
   `gbm_bo_map`（→ MAP_DUMB + mmap）/ `gbm_surface_*` 的 front/back buffer
   轮换（→ ADDFB2 + PAGE_FLIP）。
3. **Mesa 子集构建**：`meson -Dgallium-drivers=swrast -Dllvm=disabled
   -Dplatforms= -Dglx=disabled -Degl=enabled -Dgles2=enabled`，只保留
   EGL/GLESv2 + softpipe。需要 pthread、TLS、`mmap`、`clock_gettime`；
   HBOS 已能跑 Chromium，这些原语基本齐备，但要确认 **不需要写执行页**
   （softpipe 不 JIT，这是它相对 llvmpipe 的决定性优势）。
4. **镜像接线**：把 `libEGL.so.1`/`libGLESv2.so.2`/`kms_swrast_dri.so` 放
   进镜像，`LIBGL_ALWAYS_SOFTWARE=1`、`GALLIUM_DRIVER=softpipe`。

预期性能：softpipe 在 1080p 合成下通常只有个位数 fps。建议 **1280x720** 作为
KWin 的默认输出尺寸，直到有真正的 GPU 3D。

### 2.3 C. ANGLE + SwiftShader：明确不建议

三个独立原因，任何一个都足以否掉：

1. **路径不通**：ANGLE 的上游 Linux 后端是 X11/Wayland-Surface/Vulkan，
   **没有 GBM 平台**。KWin 要的是 `EGL_PLATFORM_GBM_KHR`，ANGLE 给不了。
   Chromium 里能用是因为 Chromium 自己写 ozone 平台层，不是 ANGLE 的能力。
2. **依赖更重**：SwiftShader 是 C++17 的 Vulkan ICD，要先有 `libvulkan`
   loader（694 KB）+ ICD（数十 MB）+ ANGLE 的 GLES 前端，比 Mesa softpipe
   多两层翻译、多一套 C++ 运行时。
3. **我们自己没有 Vulkan**：目前 HBOS 连 1.0 级 Vulkan 驱动都没有，
   SwiftShader 作为 ICD 还得让 loader 相信它，这条链每一环都要新写 shim。

结论：除非将来目标是"用 Vulkan 做 3D"，否则不要在 SwiftShader 上投入。

## 3. 不建议的其它选项（写明原因，防止后来者重走）

| 选项 | 不推荐原因 |
|---|---|
| llvmpipe | 需要 LLVM（172 MB 共享库 + JIT + C++ + 运行期写执行页）。HBOS 的内核 NX 策略与 W^X 路线会直接冲突，且移植 LLVM 本身是另一个项目 |
| 自己写 GLES2 实现 | 覆盖 KWin 合成器用到的着色器/纹理/framebuffer 语义是数万行量级，且 Mesa 已经免费提供 softpipe |
| zink（GL→Vulkan） | 先要有 Vulkan 驱动；同理依赖 SwiftShader，回到 C 路线 |
| virgl/virtio-gpu 3D | 依赖 QEMU 侧 virgl 与 guest 驱动；不是软件通路，且当前 QEMU 单实例归 Lead 管 |

## 4. 建议的判定顺序（每一步都能独立验收）

1. `drm_card_selftest` 全绿（已达成，见 `src/graphics/drm/`）。
2. devfs 注册 `/dev/dri/card0` 后，用户态一个最小程序跑完
   VERSION→GETRESOURCES→GETCONNECTOR→CREATE_DUMB→ADDFB2→SETCRTC。
3. 确认目标 KWin 是否有 QPainter 后端（查 `kwin_wayland` 字符串）。
   - 有 → 走 A，**本任务到此收口，不做 EGL**。
   - 没有 → 走 B，按 §2.2 的 1→4 顺序做，每步以 `eglInitialize` 成功为门槛。
4. 只有在 B 的第 3 步卡住时，才重新评估 C。

## 5. 未验证 / 风险

- KWin 6 是否保留 QPainter 后端：**未验证**（本机没装 KWin，不能凭记忆下结论）。
- Mesa `-Dllvm=disabled` 下 `kms_swrast` 是否仍完整导出 EGL 1.5：需要实际
  构建验证；上游文档只保证 swrast 可用，未逐项列 EGL 扩展。
- libinput 需要 `libudev` 枚举设备（`/dev/input/event*` 只是设备节点，
  KWin 通过 udev 发现它们）。这是**独立于 EGL 的阻塞项**，需要一个
  libudev 客户端 API 的 shim（`udev_new`/`udev_enumerate_*`/
  `udev_device_get_property_value` 等），规模远小于 udev 守护进程。
- 性能数字（个位数 fps）是按 softpipe 的历史经验估计，**未在 HBOS 上实测**。

## 6. 证据（本轮实测）

```
$ ls -lL /usr/lib/libdrm.so.2 /usr/lib/libgbm.so.1.0.0 \
         /usr/lib/libEGL_mesa.so.0.0.0 /usr/lib/libgallium-26.2.6-arch1.1.so
   92104  libdrm.so.2
   18416  libgbm.so.1.0.0
  396424  libEGL_mesa.so.0.0.0
55269560  libgallium-26.2.6-arch1.1.so

$ ldd /usr/lib/dri/kms_swrast_dri.so
  libgbm.so.1  libm.so.6  libc.so.6  libdrm.so.2  libexpat.so.1  libgcc_s.so.1

$ ldd /usr/lib/libgallium-26.2.6-arch1.1.so | head
  libdrm.so.2  libLLVM.so.23.1(172MB)  libexpat  libz  libzstd
  libSPIRV-Tools  libsensors  libxcb*  libelf  libdrm_amdgpu  libstdc++
```

第二段与第三段的对比就是本结论的支点：**软件 KMS 驱动本身不依赖 LLVM，
是打包出来的 libgallium 把 LLVM 拖进来的**。自建时把 `llvmpipe` 关掉，
依赖表就回到"libdrm + libgbm + libc + libm"这一档。
