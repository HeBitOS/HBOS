# OpenCubeOS 对标分析（HBOS vs Open Cube OS）

> 外部对标文档。对象是 GitHub 上的开源 x86_64 教学/实验内核 **Open Cube OS**
> （`cubestudio-dev/OpenCubeOS`），目标是把「它做对了而我们没做」的部分挑出来，
> 并给出可落地的补全清单与代价评估。
>
> 分析日期：2026-11-07 · 对应 HBOS `v0.1-beta5-pre18`（`version.mk`）

---

## 0. 抓取方法与元信息

| 项 | 值 |
|---|---|
| 仓库 | https://github.com/cubestudio-dev/OpenCubeOS |
| 许可 | **Apache-2.0**（`LICENSE` + 每个文件 SPDX 头 + `NOTICE` 三方声明） |
| 创建 / 最后推送 | 2026-09-26 / 2026-11-07（非常活跃，单人主导） |
| 热度 | 3 star / 1 fork（小众，但工程记录密度远超同类） |
| 体积 | ~264 MB（含 `website/` 发布产物与生成的数据 blob） |
| 语言 | C（+ 少量 nasm / Python 工具） |
| 默认分支 | `main` |

**抓取过程中的关键发现（影响对标结论，必须先说）：**

`main` HEAD **不是**一个可运行的系统，而是一次重写（内部代号 **NAK**）的目录骨架：
非 `archive/` 的条目只有 **43 个**，其中 `ocos/kernel/kmain.c` 只有 1128 字节的
kernel stub，`ocos/drivers/`、`ocos/services/`、`ocos/l1apps/` 全是 `.gitkeep` 空目录，
`ocbl/`（引导器）、`ocse/`（安装环境）、`ocire/`（恢复内核）的 README 都写着
*"NAK-01-tree only establishes the directory; no bootloader code exists yet."*

真正的实现冻结在 **`archive/v3.0.0/`**（3726 个条目，632 个源文件 / 131,230 行）。
所以本文的「对方做法」一律指 **v3.0.0 归档树**，而不是 main 骨架。

抓取方式（raw.githubusercontent / codeload 在本机被 TLS 拦截，git clone 走
gh-proxy 也被拦截）：

```bash
# 1) 元数据 + 全量文件树
curl -sSL https://api.github.com/repos/cubestudio-dev/OpenCubeOS            # -> build/tmp/oc_api.json
curl -sSL "https://api.github.com/repos/cubestudio-dev/OpenCubeOS/git/trees/main?recursive=1" \
     -o build/tmp/oc_tree.json
# 2) 文件内容走 jsDelivr 镜像（gh 直连不通）
curl -sSL "https://cdn.jsdelivr.net/gh/cubestudio-dev/OpenCubeOS@main/archive/v3.0.0/ARCHITECTURE.md"
```

本地缓存：`build/tmp/oc/`（已抓取的 23 个关键文档）、`build/tmp/oc_tree.json`（全树）。

---

## 1. 对方项目定位与架构摘要

**一句话定位**：一个**服务化的两层内核**——L0 是「纯服务内核」（core + drivers +
root services + 扩展接口），L1 是纯 Ring3 用户世界；核心设计信条是
**"everything is extensible"**（连 shell、TUI、字体引擎都是扩展面）。
明确声明 **"Is it Linux? No. ... It does not run Linux binaries"**（`FAQ.md`）。

### 1.1 两层与两扇门

```
              OCBL（自研引导器：BIOS stage1 El Torito/MBR + EFI entry，无 GRUB）
                              ↓ 16→32→64 bit long mode
   L0  PHASE1 core（IDT/PIC/PMM/VMM/heap/sched/sync/usermode/registry/ACPI/PCIe/wdt）
       PHASE2 drivers + 无驱动服务（VFS/ramfs；block/usb/wireless/sd/bt/camera/misc）
       PHASE3 依赖驱动的服务（FAT/exFAT/ext4、config、A/B、wireless、net、snd）
       PHASE4 接口就绪（crash dump/crashlog/soft wdt、health、update check、OCIRE embed）
                              ↓ "L0 ready, waiting for L1"
   L1  Ring3：275 个程序（242 个 app 目录 + 8 个平铺 .c + 25 个 .asm），
       默认条目 ush（OC_L1_APP_DECL），285 条命令全部在 Ring3，L0 零命令
```

Ring3 只能通过两个门进内核：

1. **经典 syscall ABI —— 54 个 syscall**（`src/L0/services/sysapi/core_syscall.h`）；
2. **`SYS_L1SVC(110)` 能力通道** —— `(category, op, arg, out, cap)` 交给 registry
   路由到拥有该服务的 `l1_dispatch`；类别/操作号是冻结的 ABI 表
   （`services/sysapi/l1svc_defs.h`，**20 类 / 190 个编号 op**）。

### 1.2 声明式注册（本仓库最值得抄的一条）

没有任何运行时注册调用：驱动/服务/L1 程序**在实现自己的那个 .c 文件里**用
`OC_DRIVER_DECL` / `OC_SVC_DECL` / `OC_L1_APP_DECL` 宏往专用 linker section
（`.oc_drvs` / `.oc_svcs` / `.oc_apps`）放一个 const 描述符，boot 时
`core_registry_collect()` 扫段边界全部收编。"加一个驱动 = 加一个 .c 文件 + 一行声明"，
不碰任何 boot 文件。**v1 时代那批 `driver_*_register()` 运行期注册函数已被删除**。

### 1.3 引导 / 恢复 / 升级三件套（HBOS 完全没有对应物）

| 组件 | 做法 |
|---|---|
| **OCBL** | 自研引导器替代 GRUB：BIOS 512B stage1 + EFI entry，磁盘扫描、10 秒倒计时菜单、`ocbl-install`；独立 freestanding 构建（`ocbl/ocbl.mk`） |
| **boot-state monitor** | 共享结构 `include/ocbl_state.h`：状态区 `0x98000`（6 个状态点 + 心跳，内核只写不读）+ 保留区 `0x9B000`（`BOOT_FAILED`/`USER_REQUEST` 记录 + 反码校验）；分阶段预算 30s/(10s×n)/15s 心跳；**i6300ESB 硬件看门狗中继**——内核每阶段重臂预算，卡死则硬件复位；OCBL 重启后测沉默时长 ≥8s 且无 CLEAN 标记 → 判定失败 → 写记录 → 再次重启**自动引导 OCIRE** |
| **OCIRE** | 独立恢复内核（自己的 `ocire.mk`/linker/引导），读失败记录打印"卡在哪一阶段 + 修复建议"，kexec 回切 OCOS；整盘写保护 + `ocire_guard` 受保护进程（`kill` 被 `sys_kill` 拒绝） |
| **A/B OTA** | 槽位 A/B + 启动槽标志（`services/ota/ota_ab.*`），OTA 包 = kernel ELF + manifest，`checkupdate`/`rollback` 命令；`make ota` 产出可复现包 |

### 1.4 规模与覆盖面（v3.0.0）

| 层 | 目录 | 文件 | 行 |
|---|---|---|---|
| core | `src/L0/core` | 64 | 13,366 |
| drivers | `src/L0/drivers` | 125 | 40,411 |
| services | `src/L0/services` | 179 | 66,110 |
| boot | `src/L0/boot` | 9 | 1,670 |
| L1 | `src/L1` | 255 | 9,673 |
| **合计** | | **632** | **131,230** |

驱动族 15 个：`acpi block bluetooth camera i2c input misc nic pci scsi sd snd time usb wireless`
（ACPI 含 **76 KB 的 AML 解释器**；NIC 有 e1000e/igb/ixgbe/rtl8139/rtl8169/bcm57xx；
存储有 ATA/ATA-DMA/AHCI/NVMe/virtio-blk/SCSI/分区/块缓存；另有 xHCI、无线、蓝牙、
UVC 摄像头 + 内核态 MJPEG 解码、I2C/SMBus、HD-Audio/ES1370/AC97）。
服务层 24 个：`audio block camera crash crypto diag display encoding font fs health ipc
klog l1 loader net ocembed power selfhost sysapi time tui usb watchdog`——
其中 `net` 自带 TCP（含拥塞控制）/DNS/TLS1.2+1.3/X.509/HTTPS，且**自带 ssh 客户端和 sshd 服务端**；
`crypto` 自带 AES-GCM/SHA/HMAC/RSA/EC/X25519/AEAD。

### 1.5 工程侧（这块才是 OpenCubeOS 真正的强项）

- **构建**：组件化 Makefile（`ocos/ ocbl/ ocse/ ocire/` 各自独立）+ `tools/oc_cc.sh`
  **内容寻址编译缓存**（key = 编译器身份 + 完整 flag + 源文件 hash + 每个头文件依赖 hash；
  实测冷 46.6s → 热 14.4s，186/186 命中，产物字节一致）+ `make reproducible-check`
  （两次干净构建字节比对）+ `defconfig/menuconfig` + `CONFIG_TUI/CONFIG_CHINESE/...`
  多配置 + `make ARCH=arm64|riscv` 跨架构 bring-up shell + `build.ps1`(WSL2/MSYS2) +
  `tools/build.sh` 可 source 的函数式 API + CI `-Wall -Wextra -Werror` 零告警门禁。
- **测试**：`make test` 跑 `tests/host_*.c` **19 个 harness**（直接在宿主机上编译真实内核
  源文件，部分开 ASAN；缺 fixture 的显式报 `SKIP-FIXTURE`）+ pexpect 驱动真实 QEMU 的
  E2E 套件（watchdog/logging/health 14 场景、TUI、中文控制台、桌面 app）+ 每个 L1 程序的
  逐 app 记录；**证据落到 `evidence/`（452 个条目，含真实串口日志）**；
  `tools/check_deps.py` 是**分层依赖门禁**（core 不得依赖 drivers/services，drivers 不得依赖 services）。
- **文档**：`docs/` **830 个文件**，EN/zh 双镜像；251 条命令手册页 **由 `make api-docs`
  从代码生成**（`docs/data/commands.json`、`interfaces.json`、`stats.json`）；另有
  `KNOWN_ISSUES.md` 逐条记录「2.0.0 的 17 个 P0/P1 缺陷 + 每条被什么修掉」这种级别的诚实记录。
- **诚实提醒**：对方自己也存在文档漂移——`FAQ.md` 还写着「17 类 / 158 op」「251 条命令」，
  而 `ARCHITECTURE.md` 已是「20 类 / 190 op」「285 条命令」；`BUILDING.md` 仍描述
  GRUB 构建，而 `ARCHITECTURE.md` 说引导链里「no GRUB」。**生成式文档（C8）能治的是
  数字类漂移，治不了叙述类漂移**——两边都得靠"改代码的人同时改文档"这条纪律。

---

## 2. 能力矩阵对比

图例：✅ 有且实打实 · 🟡 有但弱/部分/只在计划里 · ❌ 无

| 维度 | Open Cube OS v3.0.0 | HBOS `v0.1-beta5-pre18` | 谁强 |
|---|---|---|---|
| **内核类型** | 宏内核式「服务内核」+ 两层（L0/L1），**54 syscall** + `SYS_L1SVC` 20 类/190 op | 宏内核 + **Linux ABI 兼容层**（`src/syscall.c` 5488 行，365 个 `case`） | HBOS（用户面兼容性）/ OCO（接口纪律） |
| 用户态生态 | ❌ 不跑 Linux 二进制；自带 275 个小程序（ush shell、编辑器、TUI 桌面） | ✅ 未改 Debian 二进制（glibc/musl、Chromium + 41 依赖、sqlite、tcc 运行时） | **HBOS** |
| 动态链接 | 🟡 自研 `ld_so`（`.so` + `dlsym`，自制格式） | ✅ 真 Linux `ld.so` 闭包（`src/elf.c` 1062 行 + `src/user/ldso.c`） | **HBOS** |
| SMP / 多核 | 🟡 未见 SMP 启动路径（`core_sched` 单核调度） | ✅ `src/smp.c`（AP 启动，`src/kernel.c:99`） | **HBOS** |
| **驱动** | ✅ 15 个驱动族、125 文件 40k 行：ATA/AHCI/NVMe/virtio-blk/SCSI、e1000e/igb/ixgbe/rtl8139/rtl8169/bcm57xx、xHCI、无线、蓝牙(HCI/L2CAP/SDP/RFCOMM)、SD、UVC+MJPG、I2C/SMBus、HD-Audio/AC97/ES1370、ACPI(含 AML 解释器) | 🟡 `src/*.c` 平铺：`ahci ata nvme usb_* xhci net_virtio ac97 pci acpi`；**无**蓝牙/无线/摄像头/SD/I2C/SMBus；ACPI 仅解析 `_S5`（`src/acpi.c`，348 行） | **OCO**（广度）；HBOS 够用但只覆盖 QEMU |
| **图形 / 桌面** | ❌ 无 GPU、无合成器；只有 framebuffer 文本控制台 + 字符 TUI（`services/tui`） | ✅ `src/graphics`(CJK 渲染/字体/图标/壁纸) + `src/gui`（wm/compositor/effects/glass/HTML 引擎/browser_backend）+ Wayland 协议骨架（`src/gui/wl/`）+ `WAYLAND_PLAN.md` P0–P5 计划 | **HBOS** |
| 输入 / 中文 | ✅ IME + 中文控制台 + 编码层（`services/encoding`、`services/font`） | 🟡 CJK 字体渲染 + IME 实验（`src/input`、`src/graphics/font_cjk.c`） | 平（方向不同） |
| **包管理** | ❌ 只有整机 OTA（A/B 槽 + update.tar.gz），无第三方包、无仓库、无依赖解析 | ✅ **HPT**（`HPT/app/hpt.c` 1896 行）：本地/HTTP/HTTPS 仓库、X.509 CA 链 + 主机名校验、SHA-256 完整性、依赖闭环、staged 升级 + 回滚、`/var/lib/hpt` 数据库；`HPT/tools/pkg2hax.py` 把 Debian `.deb` 转 HAX | **HBOS**（决定性优势） |
| **引导** | ✅ 自研 OCBL（BIOS+UEFI、安装器、失败判定） | 🟡 外购 Limine v12（`.gitmodules`）+ GRUB 用于 CI BIOS ISO | OCO（自主）/ HBOS 够用 |
| 崩溃/看门狗恢复 | ✅ 三段式：boot-state monitor + i6300ESB 硬看门狗中继 + OCIRE 自动引导 + 失败记录落盘 | ❌ `grep -ri watchdog src` 零命中；崩溃只能靠串口日志 + 人工 `scripts/symbolize_rip.py` | **OCO** |
| **构建系统** | ✅ 组件化 + 内容寻址编译缓存 + 可复现门禁 + defconfig/menuconfig + 跨架构 + `build.ps1` + 零告警 CI | 🟡 单文件 `Makefile` **1968 行 / 100 个 target**；有 `run-*` 矩阵（bios/uefi/hdd/img/nogui/headless）和 `scripts/build-windows.ps1`，但**无编译缓存、无可复现门禁、无配置系统（`CONFIG_` 仅 1 处）** | **OCO** |
| **测试** | ✅ 19 host harness（ASAN）+ pexpect E2E 多套 + `evidence/` 452 条证据 + 分层依赖门禁 + 每 app 记录 | 🟡 39 个 `tests/host/*`（`make test` 挂 18 个子套）+ 46 个 `tests/*` 内核 ABI 用例 + CI 三 job（host-tests / build-bios / qemu-tests，`.github/workflows/ci.yml:20/35/73`）；无分层门禁、无证据归档规范 | OCO（纪律）/ HBOS（ABI 覆盖） |
| **文档** | ✅ 830 文件、EN/zh 镜像、251 条命令手册页**由代码生成**、`commands.json`/`interfaces.json`、诚实的 KNOWN_ISSUES | 🟡 `docs/` 20 篇 + 根目录 8 篇（中英 README、性能、路线图、HAX API PDF）；手写为主，**已出现漂移**：`README.md` 仍写「tty/termios/pty missing」，而 `src/tty.c:14/172/192` 已实现 pty + `TCGETS` + `TIOCGWINSZ` | **OCO** |
| 许可 | Apache-2.0（宽松，带 NOTICE 三方清单） | GPL-3.0（含 HPT/limine/mbedtls/lwip 子模块，`.gitmodules` 6 个） | 取决于目标（见 §6） |
| 跨架构 | 🟡 arm64 / riscv 仅 UART bring-up shell | ❌ x86_64 only | OCO（象征性） |
| 第三方依赖 | ❌ 几乎全自研（TLS、TCP、AML、MJPEG 都在树内） | mbedtls + lwip + limine + HPT + HIVE + HAX-Compile（6 个子模块） | 中性（HBOS 换速度，OCO 换可控） |

---

## 3. HBOS 相对 OpenCubeOS 的优势

1. **Linux 二进制兼容是降维打击**。对方 FAQ 明说「不跑 Linux 二进制」，全部用户程序要
   自己写；HBOS 的 `src/syscall.c`（5488 行 / 365 个 syscall case）+ `src/linux_compat.c`
   （2692 行）+ `src/elf.c` 已经能跑 glibc/musl/Chromium/sqlite 和真 `ld.so` 依赖闭包。
   这是**生态量级**的差距，不是功能条数的差距。
2. **真实图形栈 + 浏览器**。对方只有 framebuffer 文本控制台和字符 TUI；HBOS 有合成器、
   脏区、特效、CJK 渲染、HTML/CSS 引擎与 Chromium 目标平台（`src/gui/browser_backend.c`、
   `gui_html.c`）。**「跑真浏览器」这件事对方连门都没进。**
3. **包管理已经闭环**。HPT 支持 HTTPS 仓库 + CA 链校验 + SHA-256 + 依赖解析 + staged
   升级回滚 + Debian `.deb` 转换管线（`HPT/tools/pkg2hax.py`、`scripts/mk_deb_repo.py`）；
   对方只有整机 OTA，装任何软件都要重刷系统。
4. **SMP 已落地**（`src/smp.c` + `src/kernel.c:99`），对方调度器是单核形态。
5. **在客户机里能编译**（tcc 运行时 seed：`src/kernel.c:148` `tcc_runtime_seed_init()`），
   对方没有任何 guest 内工具链。

## 4. HBOS 相对 OpenCubeOS 的劣势

1. **没有任何「启动失败自愈」链路**：无硬件看门狗、无失败判定、无恢复内核。当前
   Chromium bring-up 遇到 #PF 只能靠串口日志 + `scripts/symbolize_rip.py` 人工定位，
   一次定位就是一整轮 QEMU（TCG 1–2 分钟/次，且全局串行）。
2. **构建工程化落后一代**：1968 行单体 Makefile、无编译缓存、无可复现门禁、无配置矩阵。
   对方「冷 46.6s → 热 14.4s」的缓存和「两次构建字节一致」的门禁，对 HBOS 这种
   「改一行要重链 ISO + 起 QEMU」的工作流是直接的吞吐量差距。
3. **分层没有被机器强制**。对方 `tools/check_deps.py` 把「core 不得依赖 drivers/services」
   变成 CI 门禁；HBOS 的 `src/core/**` 与 `src/*.c` 靠约定，主代理这一轮正在改
   `src/core/**`、`src/syscall.c`、`src/elf.c`——正是最容易发生反向依赖的区域。
4. **文档会漂移且无生成机制**。对方 251 条命令手册页由 `make api-docs` 从代码生成；
   HBOS 的 `README.md` 兼容性表已经和 `src/tty.c` 的现状矛盾（详见 §2 文档行）。
5. **驱动广度窄**：缺 I2C/SMBus、HD-Audio、SD、摄像头、蓝牙、无线；ACPI 只解析 `_S5`
   （`src/acpi.c:187-194` 只做 AML 整数解析用于关机），真机上的电池/热区/EC/`_DSM` 都拿不到。
6. **无健康监控/崩溃转储落盘**：对方 1 Hz 采样的 health monitor + crashlog 落盘 +
   klog ring 轮转（`kernel.log`/`kernel.log.1`）；HBOS 的 `dmesg` 只在 syscall 层
   （`src/linux_compat.c`），没有持久化与看门狗策略。

---

## 5. 可补全项清单

优先级定义：**P0** = 直接服务当前主线（Chromium bring-up / 主代理本周工作）；
**P1** = 一两个迭代内能显著提升吞吐或正确性；**P2** = 有价值但不紧急。

| ID | 补全项 | 对方做法 | 移植代价 | 优先级 |
|---|---|---|---|---|
| C1 | 启动状态点 + 硬件看门狗中继 + 失败记录 | boot-state monitor（见 §1.3） | 中 | **P0** |
| C2 | 恢复/急救运行环境（OCIRE 等价） | 独立 freestanding 恢复内核 + kexec 回切 | 大 | P1 |
| C3 | 内核 A/B 槽 + OTA 回滚 | `services/ota/ota_ab.*` + `make ota` | 中 | P1 |
| C4 | 内容寻址编译缓存 | `tools/oc_cc.sh` | 小 | **P0** |
| C5 | 可复现构建门禁 | `make reproducible-check` | 小 | P1 |
| C6 | 分层依赖门禁 | `tools/check_deps.py` | 小 | **P0** |
| C7 | 链接段声明式注册表 | `OC_*_DECL` + `.oc_drvs/.oc_svcs/.oc_apps` | 中 | P2 |
| C8 | 文档/接口清单由代码生成 | `make api-docs` → `commands.json`/`interfaces.json`/手册页 | 中 | P1 |
| C9 | E2E 套件 + 证据归档规范 | `tools/*_e2e.py` + `evidence/` | 中 | P1 |
| C10 | i18n 资源层（en/zh JSON） | `src/L1/<app>/i18n/{en,zh}.json` + `L1/_i18n` | 中 | P2 |
| C11 | 健康监控（1 Hz 采样 + CPU 告警） | `services/health`（JSON 导出 + `CPU-LOCKED` 告警） | 中 | P2 |
| C12 | 崩溃转储落盘 + klog 轮转 | `services/crash` + `lib_klog`（`kernel.log`/`.1` 轮转） | 中 | P1 |
| C13 | ACPI AML 解释器（不全量，按需） | `driver_acpi_aml.c`（76 KB） | 大 | P2 |
| C14 | 多配置构建（defconfig/menuconfig） | `tools/oc_menuconfig.sh` + `config_gen.h` | 小/中 | P2 |
| C15 | 跨架构 bring-up | `core/arch/{arm64,riscv}` UART shell | 大 | P3（暂不做） |
| C16 | 驱动族补课（I2C/SMBus、HDA、SD、UVC、BT、无线） | 见 §1.4 驱动清单 | 大/族 | P2/P3 |

### C1 · 启动状态点 + 硬件看门狗中继 + 失败记录（P0）

- **HBOS 现状**：`src/kernel.c:84-242` 是线性 init（serial → graphics → gdt_idt →
  pit → acpi → smp → pmm → vmm → heap → vfs → task → net → seeds → shell），
  **没有任何阶段标记、预算或看门狗**；卡死 = 静默挂住，只能等人发现串口不再输出。
  `grep -ri watchdog src` 零命中。
- **对方做法**：状态区 `0x98000` 写 6 个状态点（KM_AIN_ENTERED → SCHED_READY →
  DRIVERS_READY → SERVICES_READY → L1_LAUNCHED → L1_READY）+ 1 秒心跳；每阶段
  重臂 i6300ESB 预算（启动 30s / 中段 10s / L1 30s / 运行 15s）；硬件复位后
  由引导器测「沉默 ≥8s 且无 CLEAN 标记」判定失败并写 `BOOT_FAILED` 记录（含停滞阶段、
  时间戳、实测沉默），再自动引导恢复内核。驱动侧只需一个 PCI watchdog 设备 +
  一条 I/O 写 `0x98000`。
- **移植代价：中**。HBOS 用 Limine（不改引导器就无法在重启后判定），最小可行版是
  「内核写状态点 + i6300ESB 驱动 + 串口/磁盘失败记录 + 一个能读记录的恢复命令」，
  跳过「自动引导 OCIRE」（那是 C2）。约 300–500 行 + 一个 QEMU 场景脚本。
- **为什么 P0**：当前主线的痛点正是「一次 QEMU 1–2 分钟且全局串行」，卡死时要靠人工
  读串口尾部。有失败记录后，崩溃现场可以**机器判定 + 落盘 + 下次开机自述**，
  与 `CHROMIUM_COMPAT_ROADMAP.md` 的调试闭环直接咬合。

### C2 · 恢复/急救运行环境（P1）

- **HBOS 现状**：无。ISO 引导失败就结束；`scripts/qemu_session.py` 只能事后读日志。
- **对方做法**：`ocire/` 是**独立第二内核**（自己的 linker `linker_ocire.ld`、`ocire.mk`、
  ATA/FAT/串口/键盘/shell 自足），被引导器在失败时自动拉起，读失败记录打印
  「卡在哪 + 修复建议」，还能 kexec 回切主系统与整盘写保护。
- **移植代价：大**（一个第二内核 + 引导器联动）。
- **降级版（代价小）**：不做第二内核，改做「**内核内置的 panic/rescue shell**」——
  panic 时进入一个只用轮询 ATA + FAT32 + 串口的极小命令集（`dmesg`、`ls`、`cat`、
  `dump`），把崩溃现场（寄存器 + 栈 + 最近日志）写到磁盘 EFI 分区。收益的 80%，
  代价的 20%。

### C3 · 内核 A/B 槽 + OTA 回滚（P1）

- **HBOS 现状**：`Makefile:767 release:` 只产 ISO/VMDK/VDI；HPT 的 staged upgrade +
  rollback 只覆盖 `/opt` 下的包（`HPT/app/hpt.c`），**内核本体没有版本化槽位**，
  升级 = 重刷镜像，刷坏 = 没有回退路径。
- **对方做法**：分区 A/B + 启动槽标志 + `ota_ab.c/h`；`checkupdate`/`rollback` 命令；
  `make ota` 产 `dist/OpenCubeOS-<ver>-update.tar.gz`（kernel ELF + manifest，含构建日）。
- **移植代价：中**（分区表 + 槽标志 + 安装器写另一槽 + 引导器选槽；HBOS 用 Limine 时
  靠 limine.conf 的条目切换即可，不需要改引导器代码）。
- **优先级 P1**：只有在「有真实用户跑裸机」时才紧急；但在 QEMU 侧可以先做
  「内核 ELF 版本化 + 上一版自动回退」，成本能压到小。

### C4 · 内容寻址编译缓存（P0）

- **HBOS 现状**：`Makefile` 1968 行、100 个 target，每次 `make bios-iso` 从零编译 +
  limine/xorriso 打包；没有 ccache 类机制，也没有 `make cache-stats`。
- **对方做法**：`tools/oc_cc.sh` 包住每条编译规则，key = 编译器身份 + 完整 flag +
  源文件内容 hash + **每个头文件依赖的内容 hash**（`gcc -MM -MG` 探依赖）；
  实测冷 46.6s → 热 14.4s（186/186 命中，3.2×），热产物与冷产物字节一致。
- **移植代价：小**（约 150 行 shell，HBOS 的 Makefile 已经是统一变量驱动，替换 `CC`
  即可；注意 `hbos-gcc.specs` 与 `.hax` 产物也要纳入 key）。
- **为什么 P0**：这是纯吞吐收益，没有语义风险，直接放大每一轮「改 → 编 → 起 QEMU」的
  迭代速度；对全局串行的 QEMU 约束是唯一不受硬件限制的放大器。

### C5 · 可复现构建门禁（P1）

- **HBOS 现状**：无 `reproducible` 相关 target（`grep -rl reproducible Makefile scripts` 空）。
- **对方做法**：`make reproducible-check` 两次干净构建后字节比对 ELF；配套约束——
  全树禁 `__DATE__/__TIME__/__TIMESTAMP__`（make 里 grep 把关）、固定链接顺序、
  `strip --strip-debug` 去掉带路径的 section、`SOURCE_DATE_EPOCH` 透传。
- **移植代价：小**（一个 target + 三处约束 + CI 一步）。附带好处：能精确验证
  「这次改动到底有没有改变二进制」，对 `src/core/**` 的重构期特别有用。

### C6 · 分层依赖门禁（P0）

- **HBOS 现状**：无。`src/core/`（16 文件 / 5.8k 行）与 `src/*.c` 之间靠人守约定；
  `docs/REPOSITORY_SPLIT_BOUNDARIES.md` 写了规则但没有机器执行。
- **对方做法**：`tools/check_deps.py` 扫 `#include` 图，强制 `core/` 不得依赖
  `drivers/`、`services/`，`drivers/` 不得依赖 `services/`；**跑在测试矩阵里**，
  违反即红。为满足它，对方把「平台控制台」下沉到 `core/console`、把 klog 环留在 core
  而把落盘 sink 从 `services/klog` 以 emit hook 注入。
- **移植代价：小**（一个 ~200 行 Python + CI 一步）。
- **为什么 P0**：主代理这一轮同时动 `src/core/**`、`src/syscall.c`、`src/elf.c`、
  `src/linux_compat.c`，正是层间反向依赖最容易滋生的时刻；门禁先立，后面每笔改动都免费受保护。

### C7 · 链接段声明式注册表（P2）

- **HBOS 现状**：`src/kernel.c` 里手写 init 调用序列（含 `*_seed_init()` 一族），
  加驱动/服务必须改 `kernel.c`（正是主代理在动的文件）。
- **对方做法**：`OC_DRIVER_DECL` 等宏把 const 描述符放进专用 section，boot 时扫段收编；
  新增一个文件 = 新增一个驱动，零 boot 改动。
- **移植代价：中**（要定义 3 个 section + 收集器 + 把现有 init 逐个改造成描述符；
  `hbos-gcc.specs` 与 linker 脚本要加 section 边界符号）。收益是**消除 `kernel.c` 的
  单点冲突**——正好是当前并行开发的痛处。

### C8 · 文档/接口清单由代码生成（P1）

- **HBOS 现状**：`docs/` 20 篇手写 + 根目录 8 篇；命令清单、syscall 表、包格式全靠手写，
  **已经漂移**（`README.md` 兼容性表说 tty/termios/pty 缺失，`src/tty.c:172/192` 已实现
  `TCGETS`/`TIOCGWINSZ`，`:14` 已有 pty 池）。
- **对方做法**：`make api-docs` 从真实源码生成 `docs/data/{commands,interfaces,stats}.json`，
  再据此生成 251 条命令手册页（EN+zh 镜像），网站数字直接读 JSON——**文档不可能与代码脱节**。
- **移植代价：中**（先做最小版：从 `src/syscall.c` 的 case 表和 shell 命令表 dump 出
  `docs/data/syscalls.json` + `commands.json` + `stats.json`，手册页生成留到下一步）。

### C9 · E2E 套件 + 证据归档规范（P1）

- **HBOS 现状**：`scripts/` 有 46 个脚本（`auto_test.py`、`qemu_session.py`、`test_*.sh`、
  `headless_matrix.sh`），但**没有统一的套件命名/矩阵文档/证据归档目录**；
  `.github/workflows/ci.yml` 有 qemu-tests job，但在无 KVM 的环境只能 TCG 串行。
- **对方做法**：每个子系统一个 `tools/*_e2e.py`，pexpect 驱动串口；`evidence/<WP-id>/`
  存真实日志；`docs/dev/e2e-suites.md` 列出「发布门禁矩阵」并注明每个套件的 skip 计数。
- **移植代价：中**（不需要新框架，把现有脚本归拢成矩阵 + 建 `evidence/` 约定 +
  在 `docs/` 里写一张门禁表；收益是"改完到底证明过什么"可审计）。

### C10 · i18n 资源层（P2）

- **HBOS 现状**：双语只在 README（`README.zh-CN.md`）与少数 `lang` 字段
  （`src/gui/desktop_entry.c`）；系统字符串硬编码中文/英文混排。
- **对方做法**：每个 L1 app 目录下 `i18n/{en,zh}.json`，`L1/_i18n` 集中管理，
  诊断类技术文本刻意保留英文。
- **移植代价：中**（要求所有用户可见字符串走查表 API；可先只覆盖 shell + GUI 菜单）。

### C11 / C12 · 健康监控与崩溃转储（P2 / P1）

- **HBOS 现状**：`dmesg` 只在 syscall 兼容层（`src/linux_compat.c`）；异常处理在
  `src/core/gdt_idt.c` 打印寄存器到串口；**无 ring buffer 落盘、无轮转、无健康采样**。
- **对方做法**：`services/klog` 环形缓冲 + `/var/log/kernel.log` 轮转（阈值 4096 →
  `.log` + `.log.1`）；`services/health` 1 秒采样 + JSON 导出 + `CPU-LOCKED` 三 spinner
  告警；`services/crash` 捕获 core dump + crashlog；软/硬看门狗槽位。
- **移植代价**：C11 中、C12 中。建议 C12 先做（崩溃转储对 Chromium bring-up 直接有用），
  C11 后做。
- 注意：崩溃转储落盘依赖 C1 的失败记录格式，两者可合并成一个工作包。

### C13 · ACPI AML 解释器（P2）

- **HBOS 现状**：`src/acpi.c` 348 行，只解析 RSDT/XSDT/FADT/DSDT 并从 `_S5` 取关机类型
  （`:187-194` 仅 `aml_int`）。
- **对方做法**：`driver_acpi_aml.c` 76 KB 的解释器 + `driver_acpi_power.c`，
  能跑 `_DSM`/电池/热区/EC。
- **移植代价：大**（一个独立子系统）。**只在目标转向真机时才值得**；QEMU 环境收益接近 0。

### C14 · 多配置构建（P2）

- **HBOS 现状**：靠一堆并列 target（`bios-iso` / `nogui-bios` / `uefi` / `core-only` /
  `headless`）表达配置，`CONFIG_` 在 Makefile 里仅出现 1 次。
- **对方做法**：`make defconfig` 写 `.config`、`make menuconfig` 交互、`CONFIG_TUI=n`
  一次性覆盖，生成 `core/generated/config_gen.h`；关闭的特性路由到 no-op 等价实现
  而不是删代码（`CONFIG_CHINESE=n` 强制 `CONFIG_TUI=n`）。
- **移植代价：小/中**（HBOS 的 target 矩阵其实已经在做同一件事，只是没有统一配置面；
  可以先做 `config_gen.h` 生成 + 把现有 target 改写成配置组合）。

### C16 · 驱动族补课（P2/P3，按需）

- 有真机价值、代价中等：**I2C/SMBus**（笔记本触摸板/传感器）、**HD-Audio**（QEMU
  `intel-hda` 即可验证，且 HBOS 只有 AC97）。
- 代价大且当前无验证手段：**SD/eMMC、UVC 摄像头、蓝牙、无线**——QEMU 无法验证，
  引入即不可测代码。**除非有真机目标，否则不碰。**

---

## 6. 明确不值得移植的项

| 项 | 理由 |
|---|---|
| **自研引导器（OCBL）** | HBOS 用 Limine v12（`.gitmodules`）已解决 BIOS+UEFI，且 limine.conf 支持条目切换（正好服务 C3 的 A/B 选槽）。重造引导器是纯成本、零收益，还会引入「自己写的引导器在自己机器上不可测」的风险。 |
| **两层 `SYS_L1SVC` 能力 ABI（20 类/190 op）** | HBOS 的用户面是 **Linux ABI**（365 个 syscall case）。再叠一层自有能力通道 = 双轨维护，且没有任何现成程序能消费它。要的是它的**纪律**（冻结 ABI 表 + 文档生成，见 C8），不是它的**形态**。 |
| **把 275 个 Ring3 小程序搬过来** | 它们的存在意义是"没有 Linux 生态所以自己写一遍"；HBOS 能直接跑 Debian 二进制（且已有 tcc 运行时）。搬过来等于同时维护两套用户世界。 |
| **整机 OTA + website 发布链** | HBOS 已有 HPT 的包级分发（HTTPS 仓库 + CA 校验 + 回滚），比 OTA 强。**只借 A/B 槽的内核回滚**（C3），不借它的发布树。 |
| **arm64 / riscv bring-up** | 对方自己也只是 UART shell + 心跳。HBOS 当前主线是 x86_64 Chromium 兼容，投入产出比接近 0。 |
| **其全量驱动族（无线/BT/摄像头/SD）** | 本机无真机、QEMU 无法验证 → 写完即不可测代码。只挑 QEMU 可验证的（I2C 有 `qemu -device` 覆盖有限、HDA 可验证）。 |
| **它的 `archive/` 兼容层包袱与 WP-XX 工作包文档体系** | 对方 452 条 `evidence/` 与「WP-03 G5 把 XX 下沉」的记录有归档价值，但那是**流程考古**，不是代码资产。只借「建一个 evidence 目录」这个习惯（C9）。 |
| **Apache-2.0 换牌** | HBOS 是 GPL-3.0 + 6 个子模块（limine/mbedtls/lwip/HPT/HIVE/HAX-Compile），整体换许可不可行也无意义；**仅**在"想复用 OCO 的某段代码"时注意：Apache-2.0 与 GPL-3.0 单向兼容（Apache 代码可进 GPL 项目，反向不行），所以抄 OCO 代码到 HBOS 在法律上没有障碍，但要保留 SPDX 与 NOTICE 归属。 |

---

## 7. 建议执行顺序

```
P0（可与当前 Chromium 主线并行，均为低风险工程改动）
  C6 分层依赖门禁        —— 先把"哪层不许碰哪层"变红绿灯（保护 src/core 重构）
  C4 编译缓存           —— 同一轮改动的迭代速度放大器（3× 量级）
  C1 状态点+看门狗+失败记录 —— 把"卡死"从人工读串口变成机器判定+落盘
P1（1–2 个迭代）
  C12 崩溃转储落盘（与 C1 合并工作包）
  C5 可复现门禁 / C8 文档生成（最小版）/ C9 E2E 矩阵与 evidence 约定
  C3 内核 A/B 回滚（降级版：版本化 ELF + 自动回退）
P2（按需）
  C7 声明式注册表 / C10 i18n / C11 健康监控 / C14 多配置 / C13 AML / C16 按真机需求挑
```

**验收锚点**（每项都要能跑出来，不接受"看起来做了"）：

- C4：`make cache-stats` 报命中率，热构建产物与冷构建字节一致。
- C5：`make reproducible-check` 打印 `PASS`。
- C6：故意在 `src/core/` 里 include 一个 drivers 头，门禁必须红。
- C1：QEMU 注入一个「sched 后挂死」场景，重启后能读到 `BOOT_FAILED: stage=SCHED_READY`。
- C8：`make api-docs` 生成的 `syscalls.json` 条目数与 `src/syscall.c` 的 case 数一致。

---

## 8. 一句话结论

OpenCubeOS 的价值**不在功能条数**（它的用户态生态和图形栈远不如 HBOS），
而在**工程纪律三件套**——可复现/可缓存的构建、机器强制的分层与门禁、以及
「启动失败能被判定、记录并自愈」的可靠性闭环；HBOS 该抄的正是这三件，
而 Linux ABI 兼容、图形栈、HPT 包管理这三块要守住，一条都不该被对方的形态带偏。
