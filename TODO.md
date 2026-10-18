# HBOS 路线图 / TODO

> 基线：`v0.1-beta5-pre18`，工作分支 `Experimental`（原 `refactor/perf-and-code-quality`）。
> 这份清单是**执行顺序**，不是愿望清单：每一阶段都有可验证的验收标准，
> 没通过验收就不进下一阶段。

---

## P0 · 质量门禁（先止血）

| # | 任务 | 现状 | 验收标准 |
|---|---|---|---|
| P0-1 | GitHub Actions：PR / push 自动构建 | ✅ 本轮完成（`.github/workflows/ci.yml`） | `bios-iso` + `uefi-iso` 在 CI 构建通过 |
| P0-2 | GitHub Actions：宿主机单测 | ✅ 本轮完成（`make test`） | `make test` 在 CI 全绿（ui/crypto/ext2/inflate/browser/hbfs/hive） |
| P0-3 | `make deps` 前置检查接入 CI | 脚本已有，CI 待接 | 缺依赖时 CI 直接失败并打印安装命令，而不是编译到一半报错 |
| P0-4 | 提交前钩子：禁止 `.claude/`、`.agent-teams/`、`build/` 进库 | 已有 .gitignore | `git ls-files` 里不出现这些路径 |

## P1 · Bug 清零（issue #8 剩余项）

| # | 任务 | 关键文件 | 验收标准 |
|---|---|---|---|
| P1-1 | 24px 大字体下控件/开始菜单自适应 | `src/ui/ui_controls.c`、`src/tools/gui.c`（SM_* 布局） | 三档字号（16/20/24）下无重叠、无溢出；截图对比 |
| P1-2 | PS/2 鼠标丢帧、速度非线性 | `src/input/mouse.c`、`src/core/task.c` | 鼠标移动与屏幕刷新同频；连续 10s 画圈无跳点 |
| P1-3 | 任务栏/日历弹窗时间显示 | `src/tools/gui.c`（draw_taskbar/cal） | 弹窗内显示完整 时:分:秒 + 时区 |
| P1-4 | VMware `dhcp discover timeout` | `src/net.c`、`src/net_virtio.c` | VMware NAT 下 DHCP 100% 成功（脚本化复现） |
| P1-5 | 时钟与宿主机偏差 | `src/rtc_tz.c` | RTC 为 UTC 时显示本地时间正确；可配置时区 |
| P1-6 | 资源管理器数值不刷新 | `src/gui/apps/app_taskmgr.c` | 每秒刷新，数值随开/关应用变化 |

## P2 · 多语言与 UTF-8

| # | 任务 | 说明 | 验收标准 |
|---|---|---|---|
| P2-1 | 文案表（zh-CN / en-US） | 新增 `src/i18n/`：`i18n.c` + `lang/*.c`，ID 枚举 + 查表，禁止裸字符串散落 | 所有 UI 文案来自表；`grep` 检查无硬编码中文残留 |
| P2-2 | 运行时切换语言 | 设置应用增加语言项，保存到 `/hive-settings.bin` | 切换后立即生效，重启保持 |
| P2-3 | UTF-8 全链路 | 输入（PS/2 按键 → UTF-8）→ 存储 → 渲染（CJK 位图 + 回退） | 中文/日文/emoji 输入、保存、重新打开一致；非法序列不崩 |
| P2-4 | 字体覆盖 | 补 Latin-1 重音、日文假名、常用符号 | 抽样 500 字形渲染正确（脚本比对） |

## P3 · 存储栈（分盘 + 文件系统）

顺序很重要：**先分区表，再文件系统，最后热插拔**。

| # | 任务 | 说明 | 验收标准 |
|---|---|---|---|
| P3-1 | 分区表解析 | ✅ 解析已完成（`src/partition.c`，MBR+EBR+GPT+CRC，宿主机单测）；**待办**：块层按分区读写 + 挂载指定分区 | `diskmgr` 已列出表类型/类型名/GPT 名字；挂载仍未分区化 |
| P3-2 | USB 外接盘发现 | 现有 USB MSC 只认第一个 LUN；补枚举 + 插拔事件 | 插入 U 盘 3s 内出现在 `disks`；拔出后不崩 |
| P3-3 | exFAT（读写） | 新增 `src/exfat.c`：目录项、簇链、FAT | 与 Linux `mkfs.exfat` 镜像双向互认（脚本差分） |
| P3-4 | ext3（ext2 + journal 回放） | 在 `src/ext2.c` 上加 journal 只读回放 | 挂载 dirty 的 ext3 镜像不损坏数据 |
| P3-5 | ext4（extent + 64bit） | 扩展 ext2 驱动：extent 树、flex_bg | 读 Ubuntu 默认 ext4 镜像（`debugfs` 差分） |
| P3-6 | XFS（只读优先） | 新增 `src/xfs.c`：B+ 树、inode | 只读挂载 + 列目录/读文件与 `xfs_db` 一致 |
| P3-7 | FAT32 写入补齐 | 现有 FAT32 偏只读 | 创建/删除/改名/长文件名全通 |

> ext4/XFS 是大工程，先做**只读**满足安装器"读源盘"的需求，写支持放到 P5 之后。

## P4 · 安装器（自定义安装）

| # | 任务 | 说明 | 验收标准 |
|---|---|---|---|
| P4-1 | 分区器 UI | 图形化：选磁盘 → 新建/删除分区 → 设文件系统 → 挂载点 | 分区结果与 `fdisk -l` 一致 |
| P4-2 | 自定义文件系统 | 安装时可选 FAT32 / ext2 / ext3 / ext4 / XFS / exFAT | 每种格式装完能启动 |
| P4-3 | 引导安装 | GRUB BIOS + UEFI（ESP 自动识别/创建） | 两种固件各装一次，重启进系统 |
| P4-4 | 保留数据安装 | 只格式化根分区，不动其它分区 | 其它分区数据校验和不变 |
| P4-5 | 安装日志与回滚 | 失败可回滚分区表（备份 MBR/GPT） | 模拟写入失败后磁盘可恢复 |

## P5 · 网络与加固

| # | 任务 | 说明 | 验收标准 |
|---|---|---|---|
| P5-1 | 网卡驱动加固 | 收包长度/描述符边界校验、DMA 缓冲区所有权检查 | 畸形包 fuzz 1 万条不崩、不越界 |
| P5-2 | 最小防火墙 | 入站默认拒绝 + 规则表（端口/协议） | 未放行端口不可达，放行端口可达 |
| P5-3 | TLS 证书校验收紧 | 禁止空证书链/过期证书 | 自签证书连接被拒绝 |
| P5-4 | 权限边界 | 用户态不能直接碰 MMIO/端口 | ring3 越权访问触发异常而非静默成功 |

## P6 · 长期

- SMP 抢占式调度与同步原语
- AHCI/libahci 行为对齐（GPL-2.0-or-later，可直接参考）
- NVMe 多队列 + PRP list + 中断（当前单队列轮询）
- 浏览器内核兼容（见 `CHROMIUM_COMPAT_ROADMAP.md`）

---

## 依赖关系

```
P0 ──► P1 ──► P2
        │
        └──► P3-1 ──► P3-2 ──► P3-7
                 └──► P3-3 ─┐
                 └──► P3-4 ─┼──► P4-2 ──► P4-3
                 └──► P3-5 ─┤
                 └──► P3-6 ─┘
P5 可与 P3/P4 并行（不同子系统）
```

## 本轮已完成

- `Experimental` 分支（原重构分支改名），已推送
- NVMe 后端（真机优先），QEMU 已验证读写
- 构建依赖自检 `make deps`、全新 clone 修复（PR #9）
- 开始菜单裁剪、吸附/缩放/任务栏/#8 部分修复
- `make test` 宿主机测试总入口；修复失效的 winsrv 测试（P4 窗口统一后字段改名，测试仍在用旧字段）
- CI：PR 跑宿主机测试 + 构建 BIOS ISO
- 分区表解析（MBR/EBR/GPT + CRC 负例）与 diskmgr 分区显示
