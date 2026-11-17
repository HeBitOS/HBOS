# HBOS GPU 加速方案：现状评估、四方案对比、最小实现计划

> 状态：调研 + 设计（**本轮没有启动任何 QEMU**，也没有改任何被主代理占用的文件）
> 范围：KDE/KWin 出图链路要不要上 `virtio-gpu`，以及不上 GPU 时最划算的替代路径
> 证据基线：仓库 `/media/data/hbosv2` @ `Experimental`（2026-11-17 工作树），
> 所有引用都带 `文件:行号`；我自己跑出来的数字标「本轮实测」。
> 配套工具：`tools/fb_bench.c`（本轮新增，客机侧帧缓冲写带宽探针）

---

## 0. 一句话结论

**现在不要做 virtio-gpu（方案 A/B）。** 三条实测/实证理由：

1. **整帧拷贝不是瓶颈**：1920×1080 一帧 7.9 MB，宿主机同形状拷贝 **0.43 ms/帧**
   （本轮实测，18 GB/s）；`hbos_wl_fb` 当前每帧做 2 次（上屏 + 存副本）≈ 0.9 ms。
   即使做到绝对零拷贝，也只能省 <1 ms/帧 —— 而 KWin 的 QPainter 一帧是几十 ms 量级。
2. **QPainter 是 CPU 光栅化，GPU 对它无效**。要用上 GPU 必须让 KWin 走 OpenGL/EGL
   合成器，而 KWin 6.5 的 DRM 后端只走 atomic（`docs/wiki/KWIN-PLAN.md:68`），
   且 QPainter 后端在 6.8 已被上游删除（同文件 `:26`）。这是一条 3–6 人月的路。
3. **400~600 s 花在"单核争用 + 阻塞等待 + 软件光栅化"，不是花在搬运像素上**：
   会话脚本自己写着「`-smp 1` 下 plasmashell 需要 ~190 s 才能画出面板」
   （`build/tmp/kwin_root/usr/bin/kde_session.sh:135`），且 plasmashell 卡在
   `plasma_waitforname` 等 D-Bus 名字（`docs/HANDOFF-to-gemini.md:349` 附近）。

**推荐走 D 路线**：先量（Step 0），再砍像素（Step 3）、砍阻塞（Step 1）、等 SMP（Step 2），
最后才轮到拷贝路径（Step 4）和 GPU 立项（Step 5，带硬门槛）。

---

## 1. 现状评估：`src/graphics/drm/` 到哪一步了

### 1.1 一句话

**它是一个「能跑通、能自测、但从未接进启动流程」的 legacy KMS 状态机**——
ABI 层可用，架构方向（legacy 而非 atomic）与 KWin 6.5 不匹配，对当前 KDE 出图链路
（KWin nested 走 `wl_shm` + `/dev/fb0`）**完全不参与**。

### 1.2 已经做到的部分（可复用）

| 能力 | 证据 |
|---|---|
| 25 个 ioctl 的 uapi 定义，与 Linux/libdrm 逐字节对齐 | `src/graphics/drm/drm_uapi.h:25`（`grep -c 'define DRM_IOCTL'` = 25） |
| VERSION / GET_CAP / GETRESOURCES / GETCONNECTOR / GETENCODER / GETCRTC | `drm_card.c:372/406/450/481/508/520`，分发在 `:697` |
| CREATE_DUMB（连续物理页）/ MAP_DUMB / DESTROY_DUMB | `drm_card.c:312/664/357` |
| ADDFB2 / GETFB2 / SETCRTC / PAGE_FLIP / WAIT_VBLANK / 事件环 | `drm_card.c:598/640/540/577/673/169` |
| 上屏 = 一次逐行 memcpy 到真实 framebuffer | `drm_card.c:192-210`（`drm_scanout`） |
| 用户态 mmap 到 dumb buffer 物理页（syscall 钩子已预留） | `src/syscall.c:5427-5441` |
| 宿主单测（不需要 QEMU）**本轮我实跑过，全部 PASS** | 见 1.6 |

### 1.3 没做到的（三条硬缺口）

1. **从未初始化，也没有设备节点。**
   - `drm_card_init()` 在整个仓库里**没有任何调用点**（`grep -rn drm_card_init src/` 只命中
     `src/graphics/drm/drm.h` 的声明与本文件自身）。
   - `devfs.c` 里没有 `/dev/dri` 节点（`grep -n 'dri\|card0' src/devfs.c` = 0 命中）；
     对比 `evdev` 是**已接线**的（`src/devfs.c:27` include、`:704` `evdev_register_nodes`）。
   - 所以 `drm_card.c` 只是被 `Makefile:400` 的 wildcard 编译进来，**运行时是死代码**。
2. **符号错配：dumb buffer 的 mmap 永远走不到。**
   `src/syscall.c:4101` 的弱壳叫 `drm_card_is_node()`，`:5429` 调的也是它；
   而 `drm_card.c` 导出的强符号是 `drm_card_is_card_node()`（`drm.h:97`）。
   两者不同名 → 即使将来把 `drm_card_init()` 接上，`mmap(/dev/dri/card0)` 依然只命中弱壳、
   永远返回 0。这是接线的第一件事，不是一个"其实已经能用"的地方。
3. **只有 legacy KMS，没有 atomic。** `MODE_ATOMIC`(0xBC) 明确返回 `-EINVAL`
   （`drm_card.c:751`），`SET_CLIENT_CAP(ATOMIC)` 也拒绝（`drm_uapi.h:359` 注释 +
   `drm_card.c:430`）。同时没有 plane/property/blob 对象模型
   （`drm_card.c:751-756` 一整段显式 `-EINVAL`/`-ENOTTY`）。
   → 这套东西**喂不动 KWin 6.5 的 DRM 后端**（`docs/wiki/KWIN-PLAN.md:68`：
   KWin 6.5 的提交全是 `DRM_MODE_ATOMIC_*`）。

### 1.4 `EGL_DECISION.md` 已有的结论（我认可，不重复论证）

- §0（`:10-14`）：**先不要上 EGL**；QPainter 不需要 EGL/GL；现有 KMS+dumb 足以跑起来。
- §2（`:33-37`）：A=QPainter 首选；需要 GL 时唯一推荐 Mesa **softpipe + kms_swrast**；
  ANGLE+SwiftShader 明确否掉（`:85-98`：ANGLE 无 GBM 平台、依赖更重、HBOS 没有 Vulkan）。
- §3（`:100-107`）：**virgl/virtio-gpu 3D 被显式列为"当前不做"**，理由含"QEMU 单实例归 Lead 管"。
- §4（`:109-117`）：判定顺序 = selftest 全绿 → 用户态最小程序跑完 modeset → 查目标 KWin
  有没有 QPainter → 有就收口不做 EGL。
- §5（`:119-128`）：libinput 需要 libudev 枚举设备，是**独立于 EGL 的阻塞项**。

我补一条它没写的：`EGL_DECISION` 成文时 KWin 还是"要不要用 DRM 后端"的假设；现在实际路线是
**KWin nested 在自研 Wayland 服务端上**（`start_kwin.sh:17` 的 `--wayland-display wayland-fb`），
所以 `/dev/dri` 这条路连"可选"都算不上了。

### 1.5 可复用程度（分档结论）

| 用途 | 判定 |
|---|---|
| 当 ABI 参考实现（ioctl 号、结构体布局、错误码语义） | **高**：25 个 ioctl 全对，注释解释清楚，直接可查 |
| 当宿主可跑的 KMS 状态机实验室 | **高**：`drm_host_test.c` 一条 gcc 命令就能跑，我实跑 PASS |
| 当"给 KWin 用的 KMS 驱动" | **低**：缺 atomic 全栈（plane/property/blob/commit），等于重写上层 |
| 当 virtio-gpu 的宿主驱动骨架 | **低**：它是"伪造硬件"（单 CRTC、无 DMA、无真实寄存器），与 virtio-gpu 的 GEM/资源模型是两套模型 |

**建议**：保留，重命名为"legacy KMS lab"，**不要**接启动流程、不要为它补 atomic
（YAGNI：当前 KDE 路线不需要它）。若一年内都没人用，按 `docs/agents/PROGRESS.md:439`
的待决项直接删掉，别再当成"半成品资产"反复评估。

### 1.6 本轮实测（不需要 QEMU，可在任何一台开发机复现）

```
$ gcc -O2 -w -Isrc -Isrc/graphics -Isrc/core -Isrc/shell -Isrc/gui -Isrc/user \
      -Isrc/graphics/drm -DDRM_HOST_TEST=1 \
      src/graphics/drm/drm_host_test.c src/graphics/drm/drm_card.c \
      src/graphics/drm/evdev.c -o build/tmp/gpuplan/drm_host_test
$ ./build/tmp/gpuplan/drm_host_test
== drm_card_selftest (host) ==
[DRM] PASS drm_card_ioctl(NULL, DRM_IOCTL_VERSION, &ver) == 0
[DRM] PASS strcmp(name, "hive-drm") == 0
...
[DRM] PASS drm_card_ioctl(NULL, DRM_IOCTL_MODE_ADDFB2, &fb) == 0
...
[EVDEV] selftest: all checks passed
== ALL PASS ==
```
注：`drm_host_test.c` 头注释里的命令**漏了 `evdev.c`**，照抄会 `undefined reference to evdev_selftest`。
这是文档 bug，改起来一行。

---

## 2. 现在的出图链路与真实成本账

### 2.1 链路（当前实际在跑的）

```
KWin(QPainter 软件合成, 1920×1080)
  → wl_shm buffer（客户端 memfd，普通内存）
  → hbos_wl_fb::surf_commit  memcpy  → /dev/fb0（mmap 到 QEMU VRAM 物理页）
  → QEMU stdvga 扫描输出 → screendump → 宿主 PPM/PNG
```

证据链：
- KWin 以 Wayland 客户端身份跑在自研服务端上，输出尺寸 1920×1080：
  `build/tmp/kwin_root/usr/bin/start_kwin.sh:17`
- 服务端只提供 `wl_shm`（`tools/hbos_wl_fb.c:476` `wl_display_init_shm`），没有
  dmabuf/EGL → KWin 只能用 QPainter 后端。
- `g_fb` 是 `/dev/fb0` 的 `mmap`（`tools/hbos_wl_fb.c:467`），而 `/dev/fb0` 的 mmap
  走的是**真实帧缓冲物理页直映射**（`src/graphics/fbdev.c:208` `fbdev_page_phys`
  → `src/syscall.c:5427-5441` `vmm_map_page(...)`）。
  **所以"上屏不经过中间缓冲"这一半已经是现状，不是待做的优化。**

### 2.2 每帧到底拷了几次

`tools/hbos_wl_fb.c:110-186` 的 `surf_commit`：

| 分支 | 拷贝 |
|---|---|
| `attach + commit`（有新 buffer） | `shm → g_fb`（`:134-136`）**+** `shm → s->last_px`（`:145-150`）= **2 帧拷贝** |
| 只 `commit`（无新 buffer） | `s->last_px → g_fb`（`:171-173`）= **1 帧拷贝**（重放） |
| `wl_surface.damage` | **被忽略**（`:93-96` 空函数；`damage_buffer` 同样 `:193-196`） |

注意 2.2 的一个常见误判：**补上 damage 追踪没有收益**。KWin 的 QPainter 后端每帧都整屏
重绘并整屏 commit，damage 恒等于全屏。这条优化看起来很像"该做的事"，实际收益 ~0。

### 2.3 拷贝成本的量级（本轮实测 + 估算）

本轮在宿主机（与客机同为 KVM/host CPU 直通）用与 `surf_commit` **完全相同形状**的循环测：

```
frame 1920x1080 = 7.91 MB
row-wise memcpy : 0.428 ms  (18 GB/s)
single memcpy   : 0.460 ms
nt-store AVX2   : 0.370 ms
```

→ 单次整帧搬运 **0.4 ms 量级**；当前每帧 2 次 ≈ **0.9 ms**。
即使客机里因为 VRAM/脏页跟踪慢 5 倍，也只有 ~4.5 ms/帧。
**这是"帧率上限"级别的成本，不是"400 秒启动"级别的成本。**

### 2.4 那 400~600 s 花在哪

| 证据 | 说明 |
|---|---|
| `build/tmp/kwin_root/usr/bin/kde_session.sh:135` | 「`-smp 1` 下 plasmashell 需要 ~190 s 才能画出面板」，且明确写"Chromium 软件光栅化很吃 CPU，面板抢不到时间片" |
| `docs/HANDOFF-to-gemini.md:341-349` | plasmashell **从不建窗口**（根因收敛到 plasmashell 内部，不是 KWin/不是 hbos_wl_fb）；新线索：`plasma_waitforname` 阻塞等 D-Bus 名字 |
| `docs/HANDOFF-to-gemini.md:268` | KWin 只提交 **1 个** 1920×1080 surface（合成器本身工作正常） |
| `build/tmp/kde_verify_runs/20261115-142604/report.md` | 首个合成帧 29.5 s，但 `plasmashell alive: 0`、`frames committed: 1` |
| `kwin-dbg-3` 同目录报告 | 帧数 960 —— 合成循环一旦被激活是能跑起来的 |

结论：**瓶颈是单核 CPU 时间片的争用 + plasmashell 的阻塞式启动序列（D-Bus/UDisks2/PipeWire
一路超时）+ 软件光栅化，而不是像素搬运带宽**。这三样都不是 GPU 能修的：
QPainter 光栅化本来就在 CPU 上；阻塞等待与 GPU 无关；单核争用要靠 SMP。

---

## 3. 四方案对比

| 方案 | 实现工作量 | 对 KDE 首次出面板的实际改善 | 风险 | 要改 KWin/Plasma？ |
|---|---|---|---|---|
| **A. virtio-gpu + 内核 DRM + Mesa virgl** | **3–6 人月**（下面拆开） | **近 0**（首次出面板是 CPU/阻塞问题）；稳态帧率可能 2–5× | 极高：要 atomic KMS、GEM、virgl 上下文、Mesa 子集、libgbm shim | **要**：必须让 KWin 走 OpenGL 合成器（6.8 已删 QPainter，锁版本） |
| **B. virtio-gpu 只做 dumb framebuffer** | 3–5 人周 | **0，甚至负**：比现状多一次 TRANSFER_TO_HOST_2D | 中高：virtio-pci 传输层 + 控制队列 + 资源生命周期 | 不要 |
| **C. 保持 `-vga std`，直写 framebuffer** | **≈ 0（已经实现）** | 0（它已经是现状） | 无 | 不要 |
| **D. 不换硬件：砍像素 / 砍阻塞 / 砍拷贝** | 每项 0.5–2 天 | **最大的一档**（SMP + 解锁 plasmashell + 降分辨率） | 低 | 不要 |

### 3.1 A：virtio-gpu + virgl（真正的 GPU 加速）——**否**

要做完需要（缺一不可）：

1. `virtio-pci` 传输层（现代/legacy cap 解析、virtqueue、notify）——可参考
   `src/net_virtio.c`（478 行，legacy + 轮询），但 GPU 控制队列是 request/response 语义，
   不能只轮询环。
2. `virtio-gpu` 控制队列：`GET_DISPLAY_INFO`、`RESOURCE_CREATE_2D`、`ATTACH_BACKING`、
   `SET_SCANOUT`、`TRANSFER_TO_HOST_2D`、`RESOURCE_FLUSH`；3D 还要 `CTX_CREATE` +
   `SUBMIT_3D`（virgl 命令流）。估 1500–2500 行。
3. **GEM 对象模型 + atomic KMS**（plane/property/blob/COMMIT）。KWin 6.5 只走 atomic，
   `drm_card.c` 现有的 legacy 模型等于重写上层，估 +800–1200 行。
4. 用户态：libdrm（发行版二进制 ABI 已对齐，可直接搬）、libgbm 薄封装、
   Mesa `virgl_dri.so` + `libEGL`（发行版有现成的，不用自建）。
5. 镜像里锁 KWin ≤ 6.7 并使用 OpenGL 合成器；Qt RHI 选 GL 后端才能真正吃到 GPU。
6. 调试：GPU 路径失败是黑屏/硬挂，没有 QPainter 那样"慢但能看见"的中间态。

**为什么它救不了当前的痛点**：首次出面板的时间花在 plasmashell 的 QML 构造、
阻塞式 D-Bus 等待、字体/图标加载上——这些是 CPU + I/O，GPU 不参与。
GPU 加速的是"合成/上屏"这一段，而这一段现在已经是 1 ms/帧。

**什么时候值得重新评估**：见 Step 5 的门槛（SMP 落地后、profile 显示 ≥40% 在 QPainter 光栅化）。

### 3.2 B：virtio-gpu 只做 dumb framebuffer ——**否（技术上还会更慢）**

关键事实：`-vga std` 的 VRAM 是 QEMU 侧的一块 RAM MemoryRegion，BAR 直接映射给客机，
**客机写 VRAM 就是普通内存写、不产生 vmexit**。而 virtio-gpu 的资源内存在**客机 RAM** 里，
上屏必须 `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`：多一次 QEMU 侧 memcpy、多一次 trap、
且这个 memcpy 在 vCPU 线程上同步做。**用 B 替换现状，是把 0.4 ms 变成 0.4 ms + 一次陷阱。**

B 的唯一价值是"为 A 铺路"。既然 A 现在不做，B 就不该先做（YAGNI）。

唯一值得单独验证的假设是"VRAM 写是否被 QEMU 脏页跟踪放大"——这就是 Step 0 的 `fb_bench`
要回答的，**用一次测量代替一周的驱动开发**。

### 3.3 C：直写 framebuffer（零拷贝上屏）——**已经是现状，只剩 ~0.5 ms 的浪费**

- `hbos_wl_fb` 的 `g_fb` 就是 `/dev/fb0` 的真实物理页（2.1 节证据），**没有中间缓冲**。
- 剩下的浪费只有两处，都是 `hbos_wl_fb.c` 内部的小事：
  1. `s->last_px` 整帧副本（每帧多拷一次，`:145-150`）；
  2. 无 attach 时的整帧重放（`:169-180`，其实屏幕内容已经在屏上，可以只还 frame callback）。
- 另外发现一个**潜在隐患**（不是性能问题，是正确性）：内核把真实帧缓冲映射成写合并
  （`src/graphics/graphics.c:1200` `vmm_set_range_wc`），而用户态 mmap 同一批物理页时
  用的是普通可缓存属性（`src/syscall.c:5441` 只带 `VMM_P|VMM_U|VMM_W`）。
  **同一物理页两种内存类型（WB/WC）在 x86 上属于未定义行为**，目前能用是因为客机侧
  写多读少且 console 已被抑制（`src/graphics/fbdev.c:210`）。要么两边统一成 WC，
  要么用户态改用非临时存储（movntdq）。这一条建议**顺手统一成 WC**，成本 1 行。

### 3.4 D：推荐路线（不换硬件，先把时间省下来）

按"每 1 人天能省多少秒"排序：

1. **SMP**（主代理正在做）：1 核 → 4 核，KDE 启动的三个线程（KWin / plasmashell / kded）
   从"抢一个核"变成并行。这是唯一一个有数量级潜力的杠杆。
2. **解锁 plasmashell 的阻塞等待**：`plasma_waitforname` 换成 `true`、把 UDisks2/PipeWire
   相关等待改成有上限的非阻塞探测。这是"400 s → 数十 s"的直接候选，
   `docs/HANDOFF-to-gemini.md:349` 已经点名。
3. **降分辨率到 1280×720**：QPainter 的像素量降到 44%，拷贝量同比。零风险 A/B，
   一行 `--width/--height` + 帧缓冲模式。`EGL_DECISION.md:82-83` 也是这么建议的。
4. **不要在 plasmashell 画完面板之前起 Chromium**：`kde_session.sh:134-141` 已经踩过这个坑
   并加了轮询，确认这个保护还在、且不是用自旋实现的。
5. **present 路径小改**（可选，收益 ~0.5 ms/帧）：见附录 B，**只在 Step 0 触发的条件下做**。

---

## 4. 最小可行实现计划（D 路线），每步怎么验证

> 我**没有**跑 QEMU。下面每条都写成"给主代理执行的命令 + 观察点"。
> 唯一需要新 VM 独占的步骤是 Step 0 和 Step 3；其余可以先在代码侧准备好。

### Step 0（30 分钟，1 次启动）—— 量清楚"上屏拷贝"到底贵不贵

这是**整份计划的判决点**：如果客机里 `fb-memcpy` 与 `ram-memcpy` 同速，方案 A/B/C 全部关闭。

```sh
# 宿主：编译 + 装进镜像（和 wayland 测试程序同一套参数）
gcc -O2 -fPIE -pie -Wl,-z,max-page-size=0x1000 \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
    -o build/tests/fb_bench tools/fb_bench.c
python3 scripts/install_app.py --img-put build/tests/fb_bench --img-dest /bin/fb_bench

# 客机（串口 shell 里）
/bin/fb_bench 200
```

**观察点**（按顺序判定）：

| 输出 | 判定 |
|---|---|
| `fb-memcpy` / `ram-memcpy` ≈ 1–2× | **拷贝不是问题**，关闭 A/B/C 的性能理由 —— 直接跳到 Step 1 |
| 比值 > 3× | VRAM 写有额外代价（脏页/EPT），值得做 Step 4 的双缓冲或写合并映射 |
| `fb-nt-store` 明显快于 `fb-memcpy` | 用非临时存储替换 memcpy（附录 B） |
| `coherence: READBACK MISMATCH` | **先修内存类型别名**（3.3 节），这是 bug 不是性能 |

### Step 1（0.5–1 人天，1 次启动）—— 解锁 plasmashell 的阻塞

- 把镜像里的 `/bin/plasma_waitforname` 换成 `true`（仓库里已有 `.orig` 备份的痕迹），
  或让它 `timeout 5`。
- 重新跑 `python3 scripts/kde_verify_run.py ...`。

**观察点**（都在 `build/tmp/kde_verify_runs/<run>/report.md`）：

1. `startup milestones` 里是否**首次出现** `plasmashell_exec`（当前多次 run 都是
   `n/a (missing: plasmashell_exec)`）。
2. `Screenshots` 表最后一列 `panel?` 是否从 `False` 变 `True`。
3. `boot->first composited frame` **不要**变差（当前 29–37 s，这是健康基线）。

### Step 2（主代理在做，不在本计划内）—— SMP

**观察点**：同一份 `report.md` 的 `boot->plasmashell` 段；对比 `-smp 1` 与 `-smp 4`。
这一步落地后 Step 0/1 的结论要重跑一次（单核结论不必然外推到多核）。

### Step 3（0.5 人天，A/B 各 1 次启动）—— 1280×720

需要改两处（**都在我无权修改的 `build/tmp/kwin_root/` 里**，请主代理改）：

1. `usr/bin/start_kwin.sh:17`：`--width 1280 --height 720`
2. 帧缓冲模式：limine 取的是 EDID 首选模式（`limine.cfg` 里没有显式的 resolution）。
   两条路二选一：
   - QEMU 侧：`-vga std,xres=1280,yres=720`（EDID 首选模式，bootloader 自动选）
   - 运行期：任一进程 `ioctl(/dev/fb0, FBIOPUT_VSCREENINFO, 1280×720)`
     —— `src/graphics/fbdev.c:149-161` 已支持，内部走 `fb_set_resolution()`（BGA）

**观察点**：`boot->first composited frame`、`boot->plasmashell`、面板是否仍然出现；
外加 `HBOS_WL_FB_FRAME` 的尺寸应变成 `1280x720`（`tools/hbos_wl_fb.c:162` 的打印）。

### Step 4（可选，仅在 Step 0 显示拷贝贵时做）—— present 路径 / 双缓冲

- 4a：`hbos_wl_fb` 去掉 `last_px` 整帧副本、无 attach 时只还回调（附录 B 补丁）。
- 4b：`-vga std,vgamem_mb=64` + `fbdev` 暴露 `yres_virtual = 2*h` + `FBIOPAN_DISPLAY`
  走 BGA `Y_OFFSET`，`hbos_wl_fb` 交替写两块再 pan —— 消除撕裂，且不扫到正在写的区域。
  工作量 ~150 行（`fbdev.c` + `graphics.c` 的 BGA ypan + `hbos_wl_fb`）。
- 4c：统一帧缓冲的缓存属性（3.3 节的 WB/WC 别名）。

**观察点**：`fb_bench` 重跑（应有改善）；`HBOS_WL_FB_FRAME count` 增速不变或更快；
连续 3 张 screendump 的 sha256 与改动前**内容一致**（除时间戳类 UI）。

### Step 5（GPU 立项门槛，**不满足就不做**）

三条同时成立才开 A 路线：

1. SMP ≥ 4 核已合并且稳定，KDE 首次出面板 < 120 s；
2. 用 `sysprof`/`perf`（客机内）证明**剩余时间 ≥ 40% 落在 QPainter/软件光栅化**，
   而不是 I/O、D-Bus、加载；
3. 有人愿意投入 3–6 人月，且接受"锁 KWin ≤ 6.7"。

先做**一周的探针**：`virtio-gpu` 发现 + 控制队列 + `GET_DISPLAY_INFO` + 一个
1024×768 的 2D 资源 `SET_SCANOUT`。**一周内做不到扫屏就停**（kill criterion），
不要顺手把 atomic KMS 一起写。

---

## 5. 如果将来要做 A/B：复用 `src/graphics/drm/` 的具体清单

### 5.1 可以直接复用（不要重写）

| 文件 | 复用什么 |
|---|---|
| `drm_uapi.h` | 全部 ioctl 号与结构体（25 个），virtio-gpu 只需再加 4 个 virtio-gpu uapi 头 |
| `drm_card.c:372-696` | VERSION/GET_CAP/GETRESOURCES/GETCONNECTOR 的**返回语义**（KWin 只要求自洽） |
| `drm_card.c:312-371` | dumb buffer 分配（连续物理页）——virtio-gpu 的 2D 资源也必须连续 |
| `drm_host_test.c` | 宿主回归骨架，改造成"virtio-gpu 后端"的同一套断言 |

### 5.2 必须补/改

| 文件（新增或修改） | 内容 | 关键接口 |
|---|---|---|
| `src/syscall.c:4101` | **修符号错配**：弱壳 `drm_card_is_node` → `drm_card_is_card_node`（1 行） | — |
| `src/devfs.c` | 新增 `/dev/dri/card0`（+ `renderD128` 若走 GBM render node） | `drm_card_register_node(node)` |
| 新增 `src/graphics/drm/drm_atomic.c` | plane/property/blob 对象模型 + `DRM_IOCTL_MODE_ATOMIC` | `drm_atomic_commit(req) -> int` |
| 新增 `src/graphics/drm/drm_gem.c` | handle↔物理页、mmap 钩子、DMA 预留 | `drm_gem_create(size) -> handle`；`drm_gem_phys(handle, off)` |
| 新增 `src/drivers/virtio_pci.c` | 传输层（cap 解析、virtqueue、notify） | `virtio_pci_probe(dev_id) -> vq*` |
| 新增 `src/drivers/virtio_gpu.c` | 控制队列 2D 六件套 + 3D 上下文 | `vgpu_set_scanout(res, w, h) -> int`；`vgpu_flush(res, rect)` |
| 初始化挂点 | 注意 `src/kernel.c` 是禁改文件 → 挂在 `devfs_init()`（`src/devfs.c`）或 `graphics_init()` 尾部 | — |
| 用户态 | libdrm（搬发行版二进制）、`libgbm` 薄封装（300 行）、Mesa `virgl_dri.so`+`libEGL` | `gbm_create_device(fd)` / `gbm_surface_*` |

> 提醒：`virtual`/`headless` 出不了画面（`docs/wiki/KWIN-PLAN.md:31-36`），
> 所以"先 `--virtual` 再接 GPU"这条路不存在，必须一路做到真扫屏。

---

## 6. 需要主代理配合的事项（按优先级）

1. **决策**：确认"现在不做 A/B"（本文 §3）。若接受，把 `src/graphics/drm/` 标注为
   legacy lab 并从 `docs/agents/PROGRESS.md:439` 的待决项里结掉。
2. **跑 Step 0**：我写好了 `tools/fb_bench.c`（已用客机同参数编译通过）。需要你：
   `install_app.py --img-put` 装进镜像，在串口 shell 里跑一次 `/bin/fb_bench 200`，
   把输出贴回来。**这是唯一能判定"要不要碰拷贝路径"的测量**。
3. **Step 1**：把镜像里的 `plasma_waitforname` 换成 `true`（或 `timeout 5 ...`），
   重跑 `kde_verify_run.py`，看 `plasmashell_exec` 是否出现、`panel?` 是否变 True。
4. **Step 3（可选）**：`build/tmp/kwin_root/usr/bin/start_kwin.sh:17` 的
   `--width 1920 --height 1080` → `1280 720`，配合 `-vga std,xres=1280,yres=720`。
   （这两个位置都在你占用的目录里，我没有动。）
5. **测量卫生**：做 A/B 时**不要同时开** `scripts/kde_monitor.py`（它每 0.4 s
   拉一次 screendump 并做 PPM→PNG，会和 vCPU 抢宿主 CPU），否则数字不可比。
6. **只有当 Step 5 三条门槛都成立时**才需要改 QEMU 命令行：
   `-vga none -device virtio-gpu-pci,virgl=on`（注意会改变 screendump 的取图路径，
   验证工具链要先单独验一次）。

---

## 7. 我做了什么 / 没做什么（诚实交代）

**做了**：
- 通读 `src/graphics/drm/` 全部 6 个文件 + `EGL_DECISION.md`、`graphics.c`、`fb.c`、
  `fbdev.c`、`tools/hbos_wl_fb.c`，以及 KWin/KDE 相关的既有文档与验证报告；
- **实跑**了 `drm_host_test`（宿主，无需 QEMU）：ALL PASS —— 所以"可复用"不是猜的；
- **实测**了整帧拷贝成本（与 `surf_commit` 同形状的循环）；
- 新增 `tools/fb_bench.c`（客机探针，已用客机同参数编译通过、`-Wall -Wextra` 无警告）。

**没做**（受约束）：
- 没有启动任何 QEMU，没有 `pgrep -f` / `pkill -f`；
- 没有改任何禁改文件（`task.c`/`smp.c`/`smp.h`/`gdt_idt.c`/`interrupt_asm.asm`/
  `kernel.c`/`syscall.c`/`Makefile`/`build/tmp/kwin_root/`）；
- 没有改 `tools/hbos_wl_fb.c`（补丁以附录 B 的形式给出，等你确认 Step 0 后再落）；
- 没有 commit / push。
- **没有写 virtio-gpu 驱动的代码骨架**：本轮不允许开 QEMU，写出来就是无法验证的死代码，
  而它恰好是 §3.1 里最多坑的一段。Step 5 的门槛成立后再写，届时的接口清单见 §5.2。

---

## 附录 A：`tools/fb_bench.c` 用法速查

```
宿主：gcc -O2 -fPIE -pie -Wl,-z,max-page-size=0x1000 \
          -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
          -o build/tests/fb_bench tools/fb_bench.c
装镜像：python3 scripts/install_app.py --img-put build/tests/fb_bench --img-dest /bin/fb_bench
客机：/bin/fb_bench [迭代次数=200]
```
输出五项：`ram-memcpy` / `fb-memcpy` / `fb-rows`（当前 `surf_commit` 的形状）/
`fb-nt-store`（非临时存储）/ `coherence`（写完读回是否一致）。

## 附录 B：present 路径补丁（**只在 Step 0 显示拷贝贵时才用**）

`tools/hbos_wl_fb.c` 的 `surf_commit`，三处改动，合计约 -20 行：

```c
/* 1) 连续快路径：省掉逐行 memcpy 的循环开销（stride 与 pitch 相同的常见情况） */
if (stride == (int32_t)g_fb_pitch && cw == g_fb_w) {
    memcpy(g_fb, src, (size_t)g_fb_pitch * ch);
} else {
    for (uint32_t y = 0; y < ch; y++)
        memcpy(g_fb + y * g_fb_pitch, src + y * stride, cw * 4);
}

/* 2) 删掉 s->last_px 的整帧副本（:138-150 整段）以及 struct 里的 last_px/last_w/last_h */

/* 3) 无 attach 时只还 frame callback，不再重放（:169-180 整段替换为注释） */
```

> 风险与回滚：第 3 条依赖"屏幕内容已经在屏上"这个前提。若出现**画面闪回上一帧/黑屏**，
> 说明有别的东西在写 fb（console 未被抑制、或出现第二个 surface）——回滚第 3 条即可。

## 附录 C：证据索引

| 主题 | 位置 |
|---|---|
| DRM 自检与宿主测试 | `src/graphics/drm/drm_host_test.c`、`drm_card.c:802` |
| ioctl 覆盖与 atomic 拒绝 | `src/graphics/drm/drm_card.c:697-757` |
| 镜像里未接线的证据 | `Makefile:400`、`src/devfs.c`（无 `/dev/dri`）、`src/syscall.c:4101/4105` |
| 帧提交与拷贝 | `tools/hbos_wl_fb.c:110-186` |
| `/dev/fb0` 直映射物理页 | `src/graphics/fbdev.c:208`、`src/syscall.c:5427-5441` |
| 帧缓冲写合并映射 | `src/graphics/graphics.c:1200` |
| KWin 版本/后端约束 | `docs/wiki/KWIN-PLAN.md:26,31-36,68` |
| 软件 GL 路线结论 | `src/graphics/drm/EGL_DECISION.md:10-14,33-37,100-117` |
| plasmashell 阻塞与 190 s | `build/tmp/kwin_root/usr/bin/kde_session.sh:135`、`docs/HANDOFF-to-gemini.md:341-349` |
| 运行报告 | `build/tmp/kde_verify_runs/*/report.md` |
