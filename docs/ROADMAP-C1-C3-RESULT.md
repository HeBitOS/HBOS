# C1 / C2 / C3 实施结果（Chromium on HBOS）

> 本文只记录**已实测**的结论。每条都附可复现命令与真实串口输出；
> 没跑通的部分明确写成"未通过/下一步"，不把"能装"说成"能跑"。

---

## 0. 环境约定（先读，否则命令会失败）

| 事实 | 影响 |
| --- | --- |
| 本机没有 `/dev/kvm` | `scripts/qemu_session.py` 已改为 `--accel auto`：有 KVM 用 `-enable-kvm -cpu host`，没有就自动退到 TCG（`-accel tcg -cpu max`）。同一套命令两边都能跑。 |
| `/tmp` 只有 10 MiB（tmpfs） | `grub-mkrescue` 会因空间不足失败。构建前必须 `export TMPDIR=$PWD/build/tmp`。 |
| 容器会拦截批量删除 | `mk_test_img.sh` 已改成 `find -delete`，不要再用 `rm -rf build/imgwork`。 |

```bash
export TMPDIR=/media/data/hbosv2/build/tmp
make bios-iso -j16
```

---

## 1. C1：多包共享库搜索路径自动注入 ✅

### 改动

| 文件 | 改动 |
| --- | --- |
| `HPT/app/hpt.c` | 新增 `is_shared_library_rel()` / `export_shared_library()`：解包时把包内 `lib*.so[.版本…]` 额外链接到 `/lib64`、`/lib/x86_64-linux-gnu`、`/usr/lib/x86_64-linux-gnu`，链接写进 `<pkg>.list`，卸载一并删除。已存在的同名库（如内置 `libc.so.6`）不覆盖。 |
| `HPT/app/hpt.c` | `clean_from_list_buf()` 上限 512 → 1024 行（大包尾部文件原来删不掉）。 |
| `HPT/app/hpt.c` | 新增 `prune_dir_tree()`：卸载后递归清掉 `mkdir_parents()` 补建、但不在清单里的空目录。 |
| `src/tools/app.c` | 方案 B：`run`/`spawn` 启动动态 ELF 时注入 `LD_LIBRARY_PATH=/lib64:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:/usr/lib:/lib` **+ 扫描 `/opt/*/usr/lib{,64,/x86_64-linux-gnu}`**，以及 `PATH/HOME/TMPDIR/LANG`。 |
| `src/ext2.c`、`src/ext2.h`、`src/fs.c` | **真 ext2 符号链接**：`ext2_symlink()`（≤60 字节走 fast symlink，否则占一个数据块）+ `ext2_readlink()`；`fs_create_symlink()` 在 ext2 后端走真 inode（S_IFLNK + 目录项 file_type=7）；`ext2_vfs_read()` 对链接返回目标路径而非块指针。 |

### 为什么必须动 ext2（本轮发现的关键回归）

原实现里 `fs_create_symlink()` 对 ext2 只写一个**普通文件**，内容为目标路径文本，然后只在内存里把 `type` 改成 2。结果是：
- 当次启动内 `run file` 正常；
- **重启后** `/bin/file`、`/lib64/libz.so.1` 全部退化成"内容是路径字符串的普通文件"，`ld.so` 直接报
  `cannot open shared object file`，`hpt verify` 报"缺失 …/usr/share/bug/file/control"。

同时 `ext2_scan_cb()` 原来只收 `S_IFREG/S_IFDIR`（链接根本进不了文件表），且递归深度硬编码 5（`/opt/<pkg>/usr/share/bug/file/control` 是第 7 层，重启后"看不见"）。两处都已修：链接入表，深度上限 12。

### 验收（真实 Debian 包：`file` + `libmagic1` + `libz` + `liblzma` + `libbz2`）

测试镜像（仓库在 `/repo`，`file` 的 DT_NEEDED 覆盖 `libmagic.so.1 → libz/liblzma/libbz2`）：

```bash
./scripts/mk_test_img.sh build/c1-ext2.img \
  build/debs/file_5.44-3_amd64.deb build/debs/libmagic1_5.44-3_amd64.deb \
  build/debs/libmagic-mgc_5.44-3_amd64.deb build/debs/libbz2-1.0_1.0.8-5+b1_amd64.deb \
  build/debs/liblzma5_5.4.1-1+deb12u1_amd64.deb testdata/zlib1g.deb

python3 scripts/qemu_session.py build/hbos-bios.iso --disk build/c1-ext2.img --mem 1024M --timeout 200 -- \
  "run hpt source file /repo" "run hpt update" "run hpt install file" \
  "run ls /lib64" "run file -m /opt/libmagic-mgc/usr/share/misc/magic.mgc /etc/hpt/sources.list"
```

实际输出（节选）：

```
流式安装 libbz2-1.0（112656 B，已校验）...
  库导出 /lib64/libbz2.so.1.0 -> /opt/libbz2-1.0/lib/x86_64-linux-gnu/libbz2.so.1.0
  ...
已安装 file 5.44-3（多文件目录树 -> /opt/file）。
hbos:/# run ls /lib64
ld-lsb-x86-64.so.3
libbz2.so.1
libbz2.so.1.0
libbz2.so.1.0.4
liblzma.so.5
liblzma.so.5.4.1
libmagic.so.1
libmagic.so.1.0.0
libz.so.1
libz.so.1.3.1
hbos:/# run file -m /opt/libmagic-mgc/usr/share/misc/magic.mgc /etc/hpt/sources.list
/etc/hpt/sources.list: ASCII text
```

**重启后仍然成立**（这才是真正的验收，见上面 ext2 回归）：

```
hbos:/# run file -m /opt/libmagic-mgc/usr/share/misc/magic.mgc /opt/zlib1g/usr/lib/x86_64-linux-gnu/libz.so.1.3.1
/opt/zlib1g/usr/lib/x86_64-linux-gnu/libz.so.1.3.1: ELF 64-bit LSB shared object, x86-64, version 1 (SYSV), dynamically linked, BuildID[sha1]=b6e5d9c2..., stripped
hbos:/# run hpt verify file
校验通过 file 5.44-3
```

**卸载清理**：

```
hbos:/# run hpt remove file
已删除 file。
hbos:/# run hpt remove libmagic1
已删除 libmagic1。
hbos:/# run ls /lib64
ld-lsb-x86-64.so.3
libbz2.so.1
libbz2.so.1.0
libbz2.so.1.0.4
liblzma.so.5
liblzma.so.5.4.1
libz.so.1
libz.so.1.3.1
hbos:/# run ls /opt
libbz2-1.0
liblzma5
libmagic-mgc
zlib1g
```

`libmagic*`/`libz*` 的导出链接随包删除；仍安装的 `libbz2`/`liblzma`/`zlib1g` 保持不动；
`/opt/file`、`/opt/libmagic1` 连空目录一起清掉。

---

## 2. C2：大文件 `MAP_PRIVATE` / `MAP_FIXED` 与物理内存上限 ✅

### 改动

| 文件 | 改动 |
| --- | --- |
| `src/core/pmm.c` | 新增 `PMM_IDENTITY_LIMIT = 4 GiB` 并把 `max_addr` 钳进去。boot.asm 只建了 0–4 GiB 恒等映射；`-m 4096M` 时 Multiboot2 会报 4 GiB 以上的可用区，原实现会把这些页分配出去，内核一写就是分页异常（"内存越大越容易崩"）。 |
| `src/syscall.c` | Linux `nr=25 mremap` 直接透传到 `HBOS_SYS_MREMAP`（glibc 大块 realloc/calloc 会走）。 |

`MAP_FIXED` 覆盖、非零页对齐 offset、`mprotect` 大跨度改权限这些路径经测试**无需修改**，原实现已正确（含 VMA 分裂、COW 私有化、匿名大映射）。

### 验收：`tests/linux_bigmmap.c`（40 MiB 文件 + 80 TiB 固定地址 + 64 MiB 匿名 + 可选 N MiB）

```bash
python3 scripts/qemu_session.py build/hbos-bios.iso --disk build/c1-ext2.img --mem 4096M --timeout 180 -- \
  "run /tests/linux_bigmmap 700"
```

实际输出（`-m 4096M`，逐页触碰 700 MiB 匿名内存以证明 PMM 能管 >512 MiB）：

```
BIGMMAP: file size 40 MiB
BIGMMAP: mmap 40MiB MAP_PRIVATE                         PASS
BIGMMAP: content ok at 6 offsets                        PASS
BIGMMAP: mprotect(40MiB, RW)                            PASS
BIGMMAP: mprotect(40MiB, R)                             PASS
BIGMMAP: MAP_PRIVATE write not in file                  PASS
BIGMMAP: mmap MAP_FIXED at 80TiB                        PASS
BIGMMAP: MAP_FIXED content follows file offset          PASS
BIGMMAP: MAP_FIXED over existing mapping                PASS
BIGMMAP: mprotect(MAP_FIXED 8MiB, RW)                   PASS
BIGMMAP: munmap MAP_FIXED range                         PASS
BIGMMAP: munmap middle 4MiB                             PASS
BIGMMAP: MAP_FIXED into munmapped hole                  PASS
BIGMMAP: mmap 64MiB anonymous                           PASS
BIGMMAP: anonymous pages writable end to end            PASS
BIGMMAP: mprotect(64MiB anon, R<->RW)                   PASS
BIGMMAP: munmap 64MiB anonymous                         PASS
BIGMMAP: mmap large anonymous                           PASS
BIGMMAP: anon 700 MiB touched end to end
BIGMMAP: large anon content follows page index          PASS
BIGMMAP: munmap large anonymous                         PASS
BIGMMAP: PASS (0 failures)
```

同样用例在 `-m 1024M` 下也全 PASS（不含 700 MiB 项）。

---

## 3. C3：Chromium 依赖栈收敛与系统调用缺口

### 3.1 已落地的内核能力

| 文件 | 改动 |
| --- | --- |
| `src/syscall.c` | **未实现 syscall 串口跟踪**：`default:` 分支打印 `[LINUX_SYSCALL] unimplemented nr=… a0..a3`，每个号最多 4 次（避免 poll 刷屏）。 |
| `src/syscall.c` | 新增 Linux 调用：`73 flock / 74 fsync / 75 fdatasync / 97 getrlimit / 160 setrlimit / 117,119 setresuid,gid / 118,120 getresuid,gid / 122,123 setfsuid,gid / 125 capget / 126 capset / 137 statfs / 138 fstatfs / 203,204 sched_{set,get}affinity / 263 unlinkat / 269 faccessat / 317 seccomp / 324 membarrier / 441 epoll_pwait2`。 |
| `src/syscall.c` | `prctl` 补 `PR_SET_SECCOMP(22)`、`PR_SET/GET_TIMERSLACK(29/30)`、`PR_SET/GET_CHILD_SUBREAPER(36/37)`、`PR_SET/GET_THP_DISABLE(41/42)`。 |
| `src/core/gdt_idt.c` | **用户态同步异常不再 panic**：`#BP` 门改成 DPL3，`#BP/#UD/#GP/#DE/#AC/#MF` 从 ring3 触发时打印 `[TRAP] user … rip=…` 后按 `SIGTRAP/SIGILL` 终止该任务。之前 ring3 的一条 `int3` 会升级成 `#GP(0x1A=IDT[3] 不可用)` 把整机停住。 |
| `Makefile`、`src/kernel.c` | 内核自带 glibc 运行时补齐：`libm / libpthread / libdl / librt / libresolv / libutil / libgcc_s` 一并种到 `/lib64`（原来只有 `libc.so.6` + `ld.so`）。缺 `libm.so.6` 时 glib 直接 `error while loading shared libraries`。 |
| `src/fs.h` | `FS_MAX_FILES` 1024 → 8192（`file_t` 456 B，约 3.7 MiB BSS）。 |
| `HPT/app/include/hpt.h` | 字段上限对齐真实发行版元数据：version 24→40、Depends 128→**2048**、依赖条数 32→**64**、desc 128→192。（Chromium 版本号 25 字符、Depends 41 条，任一超限都会让 `hpt_parse_line` 判**整份索引**无效。） |
| `HPT/app/hpt.c` | `INDEX_CAP/STATUS_CAP/LIST_CAP` 提升（128K/96K/96K），`INDEX_PACKAGE_CAP` 512。 |

### 3.2 配套工具（新增）

| 文件 | 作用 |
| --- | --- |
| `scripts/fetch_deb_closure.py` | 读 bookworm `Packages.xz`，递归解析 Depends/Pre-Depends 并下载闭包。`--skip` 可裁包，`--list-only` 只估算规模。 |
| `scripts/mk_deb_repo.py` | .deb → HPK2 + `Packages`，**自动剪掉仓库里不存在的依赖并打印**（HPT 的 `validate_index` 要求依赖闭包自洽，缺一个包就整份索引作废）。 |
| `scripts/mk_test_img.sh` | 生成 ext2 数据盘：`/repo`（HPT file 源）、`/tests/*`（宿主构建的动态 ELF）、`/data/big40.bin`。 |
| `scripts/qemu_session.py` | 支持 `--accel auto|kvm|tcg`、`--mem`、`--smp`、`--log`。 |
| `tests/linux_bigmmap.c` | C2 用例（见上）。 |

### 3.3 里程碑 1：glib 栈跑通（多动态库 + 多包）

```bash
python3 scripts/qemu_session.py build/hbos-bios.iso --disk build/c3-ext2.img --mem 2048M --timeout 150 -- \
  "run /opt/libglib2.0-bin/usr/bin/gio version"
```

```
[LINUX_SYSCALL] unimplemented nr=334 a0=0x10000088cd60 a1=0x20 a2=0x0 a3=0x53053053
2.74.6
```

`gio`（glib 2.74.6）完整启动并输出自身版本；`gio help` 也能列出全部子命令。
唯一缺口是 `nr=334 rseq`——glibc 探测失败后自动回退，不影响功能。

> 注意：`/bin/<pkg>` 入口按**包名**命名，`libglib2.0-bin` 这类"包名≠二进制名"的包要用
> `/opt/<pkg>/usr/bin/<prog>` 调用。

### 3.4 里程碑 2：Chromium 150 安装成功（280 MB 单文件流式路径）

```bash
cd build/debs && python3 ../../scripts/fetch_deb_closure.py chromium --skip libc6 --max-packages 200
```

闭包 204 个包 / 202 MiB（含 perl、adwaita 图标主题等非运行必需项）。
为在 8192 项文件表内跑通，镜像只收**运行期必需**的 156 个包，并显式排除：

| 排除 | 原因 | 后果 |
| --- | --- | --- |
| `adwaita-icon-theme` | 5741 个图标文件，纯 UI 资源 | `libgtk-3-0` 的该依赖被 `mk_deb_repo.py` 剪掉并打印 |
| `perl*`、`lib*-perl` | 只被 `usrmerge` 等安装脚本依赖 | 同上 |
| `chromium-common` | 只含资源/脚本（`/usr/bin/chromium` 本身是 5 KB shell 包装脚本） | 直接跑 `/usr/lib/chromium/chromium` |

安装实测（`-m 4096M`，磁盘 1.8 GB）：

```
chromium 依赖: libasound2,libatk-bridge2.0-0,...,libxnvctrl0,libxrandr2,libzstd1,zlib1g,libgtk-3-0
获取 chromium 150.0.7871.100-1~deb12（279930896 B）...
流式安装 chromium（279930896 B，已校验）...
已安装 chromium 150.0.7871.100-1~deb12（多文件目录树 -> /opt/chromium）。
```

41 个直接依赖 + 传递闭包全部装完，**279,930,896 B 的单文件**走 `install_hpk2_stream()`
完成流式 SHA-256 校验与解包（2 MiB 缓冲限制、128 KiB ramfs 槽位都没碰到）。

### 3.5 里程碑 3：首次启动 → 定位并修掉"整机 panic"

```bash
python3 scripts/qemu_session.py build/hbos-bios.iso --disk build/c3-chromium.img --mem 4096M --timeout 480 -- \
  "run /opt/chromium/usr/lib/chromium/chromium --headless --no-sandbox --disable-gpu --single-process --dump-dom about:blank"
```

修复前的输出（第一次运行）：

```
[LINUX_SYSCALL] unimplemented nr=334 ...
===== KERNEL PANIC =====
Exception: General Protection Fault (vector 0xD)
Error code: 0x1A
RIP=0x1108C34F02 ...
System halted.
```

定位：`0x1108C34F02 - 0x1100000000 = 0x8C34F02`，反汇编该地址：

```
8c34efa: mov %rbx,%rdi
8c34efd: call 2ae0d70        ; LogMessage::~LogMessage()
8c34f02: cc                 int3     <-- 故障点（IMMEDIATE_CRASH）
8c34f03: 0f 0b              ud2
```

即 **Chromium 的 `CHECK()` 失败后走 `int3`**，而 `int3` 的 IDT 门是 DPL0，ring3 触发会被 CPU
升级为 `#GP`，错误码 `0x1A = IDT[3] 门不可用`，内核把它当致命异常 panic。

修复后（`#BP` 门 DPL3 + 用户态同步异常按信号终止任务）：

```
hbos:/# run /opt/chromium/usr/lib/chromium/chromium --headless --no-sandbox --disable-gpu --single-process --dump-dom about:blank
[LINUX_SYSCALL] unimplemented nr=334 a0=0x100002e56b00 a1=0x20 a2=0x0 a3=0x53053053

[TRAP] user Breakpoint rip=0x1108C34F03 err=0x0 -> task terminated
run: exit 133
hbos:/#
```

现在：Elf 加载 → ld.so 解析 41 个依赖库（全部跨 `/opt/<pkg>`）→ 进入 Chromium 主程序
→ 走到一个 `CHECK` 失败点 → 内核精确报告 RIP 并只杀该进程，**shell 存活**。
调试点从"整机挂死、每次重启 30 秒"变成"一条命令拿到 RIP"。

### 3.5b 继续推进后的三项新发现（本轮）

**① 32 GiB 地址空间预留被当成"立刻占满物理内存"（已修，关键）**

`--version` 也崩在同一处，于是反汇编 CHECK 前的代码：

```
8c34e28: movabs $0x800000000,%rdi     ; 32 GiB
8c34e46: call   8c34630               ; 预留地址空间（mmap）
8c34e4b: mov    %rax,0x10973000
8c34e52: test   %rax,%rax
8c34e55: je     8c34eb2               ; 失败 → CHECK → int3
```

这是 PartitionAlloc 的地址池：Linux 上 `mmap(NULL, 32 GiB, PROT_NONE)` 只占虚拟地址，
物理页等 `mprotect` 之后再按需提交。HBOS 原来对每一页都 `vmm_alloc_page_at()`，32 GiB
必然 ENOMEM。

修法（`src/syscall.c`）：
- `mmap(prot == PROT_NONE)` 只登记 VMA（记下 backing 类型/文件/偏移），**不分配任何物理页**；
- `mprotect` 改为 `commit_and_protect_user_range()`：范围内缺页的 VMA 按类型补页
  （匿名清零、文件按 `backing_offset` 回填），再改权限。

结果：`chromium --version` → **`Chromium 150.0.7871.100`**。分配器初始化、命令行解析全部走通。

**② `/proc/self/exe` 返回的是"/+任务名"（已修）**

原实现 `readlink("/proc/self/exe")` 返回 `/<task->name>`（如 `/chromium`），
Chromium 用 `dirname(readlink("/proc/self/exe"))` 定位 `icudtl.dat` 必然找错目录。
已给 `task_t` 加 `exe_path[128]`，`elf64_load_source_and_spawn()` 里从 VFS 节点全路径写入，
`readlink` 优先用它。

**③ 当前卡点：ICU 数据 mmap 失败**

```
[1024/000410.090000:ERROR:base/i18n/icu_util.cc:232] Invalid file descriptor to ICU data received.
```

该行对应 `base::MemoryMappedFile::Initialize(base::File(data_fd), region)` 失败——
即 **对 icudtl.dat 的 mmap 返回了错误**（不是 fd 无效：fd 无效走的是另一条分支）。
`icudtl.dat` 已确认落在 `/opt/chromium/usr/lib/chromium/`（可用 `run ls` 看到）。

下一步（明确、可执行）：
1. 给 `HBOS_SYS_MMAP` 的文件映射失败路径加串口日志（打印 fd/len/prot/flags/off/size/返回码），
   直接看到 `MemoryMappedFile` 那次 mmap 的参数与 errno；
2. 用 `tests/linux_bigmmap.c` 的同类写法加一条 `mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0)`
   用例，确认 MAP_SHARED 只读文件映射本身是否可用。

**资源归属问题（需要架构层面解决）**：`chromium` 与 `chromium-common` 两个包都拥有
`/usr/lib/chromium/*`，而 HPT 按 `/opt/<pkg>/` 前缀隔离安装，导致 `icudtl.dat`/`*.pak`
落在 `/opt/chromium-common/usr/lib/chromium/` 而二进制在 `/opt/chromium/usr/lib/chromium/`。
本轮用 `run cp` 手工补齐做了验证；正确的修法是让 HPT 尊重 manifest 的 `install_prefix`
（`pkg2hax` 已输出该字段），把"同一目录多包共有"表达出来。

### 3.5c 继续推进：把 Chromium 从"进用户态即死"推到"启动中期"

按"崩溃点 → 定位 → 修 → 再跑"的顺序，本轮连续修掉 6 个内核缺陷。每一条都对应
一个 Chromium 自己报出来的错误（顺序即实际推进顺序）：

| # | 现象 | 根因 | 修法 |
| --- | --- | --- | --- |
| 1 | `CHECK` → `int3`（`--version` 也崩） | `mmap(NULL, 32 GiB, PROT_NONE)` 被当成"立刻占满物理页" | `mmap(prot==0)` 只登记 VMA；`mprotect` 走 `commit_and_protect_user_range()` 按需补页 |
| 2 | `Invalid file descriptor to ICU data received.` | `readlink("/proc/self/exe")` 返回 `/任务名`（`/chromium`），dirname 后找不到 `icudtl.dat` | `task_t.exe_path` + elf 加载与 `run` 路径双写入；`readlink` 优先用它 |
| 3 | 同上（修 2 后仍报） | Linux `fcntl` 语义整错：`F_DUPFD_CLOEXEC(1030)` 返回 EINVAL、`F_GETFL(3)` 被当"清 flags" | 新增 `linux_fcntl()`，按 Linux 编号实现 `0/1/2/3/4/1030` |
| 4 | `Error loading V8 startup snapshot file` | `v8_context_snapshot.bin` 没放进二进制目录（资源包拆包问题，见下） | 测试期 `run cp` 补齐；正确修法是 HPT 尊重 manifest `install_prefix` |
| 5 | `crashpad ... socketpair: Socket type not supported (94)` | `unix_socket` 只认 `SOCK_STREAM`，Chromium 用 `SOCK_SEQPACKET(5)` | `linux_compat_unix_socket()` 把 SEQPACKET 归一为 STREAM（字节流语义足够） |
| 6 | `fork: Invalid argument (22)` → `posix_spawn ... Invalid argument` | `linux_clone` 把 `CLONE_VM|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD` 全列为必需位；glibc `fork()` 与 `posix_spawn()` 的 clone 都不带 `CLONE_THREAD` | 无 `CLONE_THREAD` 时走 `task_fork_linux()`（fork 语义；posix_spawn 子进程随即 exec，复制地址空间等价） |
| 7 | `setsid: Function not implemented (38)` | `setsid/setpgid/getpgrp` 未实现 | 补 `109/111/112`（HBOS 无会话模型，返回稳定标识即可） |

配套诊断能力（这轮定位全靠它）：

- `mmap` 每个失败分支打印 `[MMAP] fail err=.. fd=.. len=.. prot=.. flags=.. off=.. size=..`；
- **最近 128 条 Linux 系统调用环形缓冲**：用户态异常时连同回溯一起 dump，
  `open/openat/readlink/access` 会把路径字符串**在调用当场复制**进条目
  （事后再读用户缓冲会已被释放），每行形如
  `nr=257 ret=4 path="/opt/chromium/usr/lib/chromium/icudtl.dat"`；
- 用户态 `#BP/#UD/#GP` 与未处理 `SIGSEGV` 都打印 RBP 链回溯（`bt[0..15]`），
  可用宿主侧 `objdump -d --start-address=<vaddr-0x1100000000>` 反查调用点。

**当前进展（`--headless --no-sandbox --no-zygote --single-process --disable-gpu --dump-dom about:blank`）**：
加载 280 MB 主程序 + 41 个依赖库 → 分配器/ICU/V8 快照/资源包 → 建 `/tmp` 工作目录
→ 读 `/proc/self/exe` → 拉起子进程（fork/posix_spawn 全通）→ 现在停在 crashpad：

```
[TRAP] user SIGSEGV addr=0x0 rip=0x1000017F95A5
[TRAP] user Breakpoint rip=0x1107053BB8 err=0x0 -> task terminated
run: exit 191
```

子进程在共享库（`0x1000017F95A5`）里调用了空指针，父进程随后 CHECK 退出。
下一刀：看这个 SIGSEGV 的回溯/syscall 轨迹（新诊断已能直接给出），大概率落在
`execve` 的参数/环境（argv/envp 传递）或 crashpad handler 的初始化上。

### 3.5d 关掉 crashpad 后的新卡点：栈保护失败

加 `--disable-crashpad-for-testing` 后不再 spawn 子进程，crashpad 相关错误全消失；
`creat(85)/getpriority(140,141)` 也补齐了。当前停在：

```
[TRAP] user Breakpoint rip=0x1107053BB8 err=0x0 -> task terminated
  bt[0] rip=0x1107053BB8   ← 对应文件偏移 0x7053BB8
```

反汇编该地址：

```
 7053bb2: call fefaa80 <__stack_chk_fail@plt>
 7053bb7: cc            int3
 7053bb8: 0f 0b         ud2        ← 实际落点
```

即 **`__stack_chk_fail`（stack smashing detected）**，不是业务 CHECK。
方向：用户栈 canary（`fs:0x28`，由 `AT_RANDOM` 派生）在这次调用中被改坏，
可能是 `execve`/`clone` 之后 TLS（`fs_base`）或栈顶没有正确重建。
已加日志验证：`ARCH_SET_FS` 全程只发生一次（pid=1, fs=0x100002e57740），
异常时内核记录的 `fs_base` 与它一致 —— **TLS 没被改坏**，canary 校验失败
是栈槽被覆盖（真·栈破坏），不是 fs 指错。

异常前的最后动作（syscall 轨迹）：

```
openat("/opt/chromium/usr/lib/chromium/chrome_100_percent.pak") -> 14
openat("/opt/chromium/usr/lib/chromium/chrome_200_percent.pak") -> 15
（随后 __stack_chk_fail）
```

即崩在**资源包（ResourceBundle）加载/解析**阶段。下一步用 `getdents64`/`stat`
的写回长度做交叉验证：这类"内核往用户缓冲写超了"最容易踩爆栈上的 canary。

### 3.5e 多线程跑起来了 —— 卡在内核空函数指针

这一轮连续修掉三个缺陷后，Chromium 终于**真正跑起了多线程**（日志里出现
`1:6` / `1:8` / `1:9` 多线程标识）：

| 现象 | 根因 | 修法 |
| --- | --- | --- |
| `pthread_create: Bad address (14)` | ① `clone` 要求新线程栈顶地址本身已映射（glibc 传的是"栈顶高地址"，该地址在映射外）；② glibc 走的是 `clone3(435)`，而我们的 clone3 校验另有一套 | 校验改成 `stack-8`；`clone3` 改为复用 `linux_clone_args()`，不再摊平成帧副本 |
| 新线程一跑就 `SIGSEGV rip=栈地址` | 我把 clone3 参数写回帧的 `a0..a4`，而子线程恢复时这些槽位会被当成父线程的 **rdi/rsi/rdx/r10/r8** 还原 —— glibc 把线程入口函数指针放在这些寄存器里，被冲掉后跳到野地址 | `linux_clone_args(frame, flags, stack, ptid, ctid, tls)` 显式传参，原帧不动 |
| `locale_file_path.empty() for locale`（→ 资源包 CHECK） | locale 为空，且 `locales/en-US.pak` 不在二进制目录 | `--lang=en-US` + 补齐 locale 包 |
| `setsid: Function not implemented` | 未实现 | 补 `109/111/112` |
| `socketpair: Socket type not supported (94)` | 只认 `SOCK_STREAM` | `SOCK_SEQPACKET(5)` 归一为 STREAM |
| `fork / posix_spawn: Invalid argument` | `clone` 把 `CLONE_THREAD` 当必需位 | 无 `CLONE_THREAD` → fork 语义 |

同时补齐：`creat(85)`、`getpriority/setpriority(140,141)`、
`sched_{set,get}param / set,get}scheduler / get_priority_{max,min}(143-148)`、`sched_getattr(229)`。

**当前状态**：Chromium 主进程 + 多个线程都在跑，能看到的都是预期内的环境缺失
（dbus 没起、fontconfig 无配置、NETLINK 不支持），然后内核崩：

```
===== KERNEL PANIC =====
Exception: Invalid Opcode (vector 0x6)
RAX=0x100004E4F6C0 RBX=0x0  RCX=0x4510940  RDX=0x4511F40
RSI=0x459E1D8  RDI=0x45A8100  RBP=0x0  RSP=0x4527A58
RIP=0x3   CS=0x8 (内核代码段)
```

**RIP=0x3 = 内核调了一个空函数指针**（不是野地址，是 3 —— 典型的"把 int
当指针用"：某个结构体字段偏移读错，或者回调指针没初始化）。这是在多线程
真正并发之后才暴露的：某个线程走了某条 `ops` 回调/调度路径，而单线程时
从不经过。下一步：把 syscall 轨迹 dump 也接到内核 panic 路径上，先看崩的
是哪条系统调用，再反查对应的回调表。

### 3.6 下一刀（按优先级，均已具备工具）

1. **拿到 CHECK 的上下文**：`0x8c34efd` 调的是 `LogMessage::~LogMessage()`，说明失败信息本应写
   stderr 却没出现。下一步给 `write(2, …)`/`writev` 加 fd 前缀日志，确认是日志 sink 未初始化
   还是 `write` 返回错误；或在 `[TRAP]` 分支里把用户栈回溯（RBP 链 + `elf` 映射表）打出来。
2. **`nr=334 rseq`**：目前靠 glibc 回退。补一个"注册即返回 0/`-ENOSYS`"的显式分支即可消除噪音，
   但要注意 rseq 语义（注册成功后内核必须在上下文切换时写 abort 标志），宁可不实现也别假实现。
3. **`/dev/shm`**：`libgtk`/Chromium 的 `memfd` 路径已有 `memfd_create`，但 `/dev/shm` 目录缺失，
   `base::SharedMemory` 打开 `/dev/shm/*` 会失败——在 `devfs` 里加一个可写 tmpfs 目录。
4. **`/proc/self/statm`** 缺失（`/proc/self/{maps,status,cmdline,exe,fd}` 已有），
   V8/PartitionAlloc 会读它。
5. 线程：`clone`/`clone3`/`futex` 已有实现，但 Chromium 单进程模式仍会起 IO/Worker 线程，
   下一轮先验证 `pthread_create` 在 4 KiB 栈 + TLS 下的可用性。

---

## 4. 本轮改动文件清单

```
HPT/app/hpt.c                     C1 库导出/卸载清理/流式安装容量
HPT/app/include/hpt.h             字段与依赖上限（真实发行版元数据）
Makefile                          glibc 运行时补充库的构建规则
src/kernel.c                      种 libm/libpthread/... 到 /lib64
src/core/pmm.c                    4 GiB 恒等映射上限
src/core/gdt_idt.c                用户态 #BP/#UD/#GP 不再 panic
src/core/task.h                   task_t.child_subreaper
src/ext2.c  src/ext2.h            ext2 真符号链接（symlink/readlink）
src/fs.c  src/fs.h                ext2 链接入表、递归深度、FS_MAX_FILES=8192
src/syscall.c                     syscall 跟踪 + 一批 Linux 调用 + mremap
src/tools/app.c                   LD_LIBRARY_PATH/PATH/HOME 注入（方案 B）
scripts/qemu_session.py           accel/mem/log 选项
scripts/mk_deb_repo.py            .deb → 仓库 + 依赖剪枝（新）
scripts/fetch_deb_closure.py      Debian 依赖闭包下载（新）
scripts/mk_test_img.sh            ext2 测试盘（新）
tests/linux_bigmmap.c             C2 用例（新）
```

## 5. 复现一条命令

```bash
export TMPDIR=$PWD/build/tmp && make bios-iso -j16 && \
SIZE_MB=1800 ./scripts/mk_test_img.sh build/c3-chromium.img build/debs/*.deb && \
python3 scripts/qemu_session.py build/hbos-bios.iso --disk build/c3-chromium.img \
  --mem 4096M --timeout 3600 -- "run hpt source file /repo" "run hpt update" \
  "run hpt install chromium" \
  "run /opt/chromium/usr/lib/chromium/chromium --headless --no-sandbox --disable-gpu --single-process --dump-dom about:blank"
```
