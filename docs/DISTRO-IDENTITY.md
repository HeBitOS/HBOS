# 发行版身份方案（os-release / Debian 12 / Arch）

> 目标：让 HBOS 能装 Debian 包和 AUR 包，并让**装进来的程序以为自己跑在
> 对应发行版上**；同时在所有"人看得到"的地方仍然明确写着 HBOS。

---

## 1. 问题

镜像里目前**没有 `/etc/os-release`**（`/etc` 下只有 `hpt/`）。后果：

| 场景 | 现状 |
|---|---|
| `fastfetch` / `neofetch` | 读不到 os-release，显示 unknown 或直接报错 |
| 安装脚本 `if [ -f /etc/debian_version ]` | 判定失败，走错分支 |
| 程序按 `ID` 选 logo / 调 apt | 全部落空 |
| 用户想知道这是什么系统 | 只能靠 `uname` 看内核名 |

而 `pkg2hax.py` 已经能转换三种包格式（`.deb` / `.pkg.tar.zst` / `PKGBUILD`），
manifest 里也有 `source_format` —— **格式转换这层已经通了，缺的是身份层**。

---

## 2. 根本约束：os-release 是全局单文件

这是整个方案最重要的前提，必须先说清楚：

- `/etc/os-release` 是**进程全局唯一**的一份，进程启动时读一次。
- 不可能让 A 程序看到 Debian、B 程序看到 Arch（除非每个进程 mount namespace，代价太大）。
- 所以**"同时装 deb 和 AUR 包"时，只能有一个当前身份**。

由此推导出的策略：

1. **按安装动作切换**：装了 Debian 包 → 身份切成 Debian；装了 AUR 包 → 切成 Arch。
2. **可手动锁定**：`hpt distro use debian|arch|hbos`，避免装个小工具就把整个身份改掉。
3. **HBOS 身份不被覆盖**：无论切到哪个，os-release 里始终保留 `HBOS_*` 字段。

---

## 3. 三个身份档案

| 档案 | `ID` | 用途 |
|---|---|---|
| `hbos` | `hbos` | 默认；不装外来包时的原生身份 |
| `debian` | `debian` | 装 `.deb` 后 |
| `arch` | `arch` | 装 `.pkg.tar.zst` / `PKGBUILD` 后 |

### 3.1 `/etc/os-release` — debian 档案

```ini
PRETTY_NAME="Debian GNU/Linux 12 (bookworm)"
NAME="Debian GNU/Linux"
VERSION_ID="12"
VERSION="12 (bookworm)"
VERSION_CODENAME=bookworm
ID=debian
HOME_URL="https://www.debian.org/"
SUPPORT_URL="https://www.debian.org/support"
BUG_REPORT_URL="https://bugs.debian.org/"

# HBOS 扩展字段：符合 os-release 规范（未定义字段允许存在），
# 不影响任何标准解析器，但能让人一眼看出真实身份。
HBOS_VERSION="0.2-beta1-pre1-gui.4"
HBOS_KERNEL="hbos"
HBOS_COMPAT_DISTRO="debian"
HBOS_COMPAT_VERSION="12"
```

### 3.2 `/etc/os-release` — arch 档案

```ini
NAME="Arch Linux"
PRETTY_NAME="Arch Linux"
ID=arch
BUILD_ID=rolling
ANSI_COLOR="38;2;23;147;209"
HOME_URL="https://archlinux.org/"
DOCUMENTATION_URL="https://wiki.archlinux.org/"
SUPPORT_URL="https://bbs.archlinux.org/"
BUG_REPORT_URL="https://gitlab.archlinux.org/groups/archlinux/-/issues"
LOGO=archlinux-logo

HBOS_VERSION="0.2-beta1-pre1-gui.4"
HBOS_KERNEL="hbos"
HBOS_COMPAT_DISTRO="arch"
```

### 3.3 `/etc/os-release` — hbos 档案（默认）

```ini
NAME="HBOS"
PRETTY_NAME="HBOS 0.2-beta1 (gui.4)"
ID=hbos
ID_LIKE=linux
VERSION_ID="0.2"
VERSION="0.2-beta1-pre1-gui.4"
ANSI_COLOR="0;36"
HOME_URL="https://github.com/hbos-project"   # 占位，替换成真实地址
LOGO=hbos-logo
HBOS_VERSION="0.2-beta1-pre1-gui.4"
HBOS_KERNEL="hbos"
```

---

## 4. 配套标识文件矩阵

很多程序不看 os-release，而是探测**标志文件**。必须一起造：

| 文件 | hbos | debian | arch |
|---|---|---|---|
| `/etc/os-release` | 见 3.3 | 见 3.1 | 见 3.2 |
| `/etc/lsb-release` | `DISTRIB_ID=HBOS` | `DISTRIB_ID=Debian` / `DISTRIB_RELEASE=12` / `DISTRIB_CODENAME=bookworm` | `DISTRIB_ID=Arch` |
| `/etc/debian_version` | 不存在 | `12.11` | 不存在 |
| `/etc/arch-release` | 不存在 | 不存在 | 空文件 |
| `/etc/issue` | `HBOS \r (\l)` | `Debian GNU/Linux 12 \n \l` | `Arch Linux \r (\l)` |
| `/etc/hbos-release` | **始终存在** | 始终存在 | 始终存在 |

`/etc/hbos-release` 是硬标识，任何身份下都不删 —— 这是"注明 HBOS"最可靠的落点，
比 os-release 里的扩展字段更不容易被覆盖。

### 4.1 包管理器的最小骨架（P1，非本次必须）

某些程序会直接调 `dpkg -l` / `pacman -Q`。P0 先不做，但要预留：

- debian 档案：`/var/lib/dpkg/status`（最小合法骨架）、`/var/lib/dpkg/arch`
- arch 档案：`/etc/pacman.conf`、`/var/lib/pacman/local/`

⚠️ 这一层**不要假装得太像**：如果 `dpkg -l` 返回空表但 `fastfetch` 说有包，
排查会很痛苦。P0 阶段宁可让这些命令报 "command not found"。

---

## 5. 实现落点

### 5.1 `pkg2hax.py`：manifest 增加 `distro` 字段

现有 manifest 已有 `source_format`，按它推导：

| source_format | distro |
|---|---|
| `deb` | `debian` |
| `pkg.tar.zst` / `pkgbuild` | `arch` |
| 其他 | `hbos` |

新增字段（schema 2 向后兼容，加字段不破坏旧安装器）：

```json
"distro": {
  "id": "debian",
  "version_id": "12",
  "codenamed": "bookworm",
  "confidence": "inferred"     // inferred | declared | overridden
}
```

`confidence` 很重要：`inferred` 表示从格式猜的，`declared` 表示包内带了
`/etc/os-release` 或 control 字段，`overridden` 表示用户显式指定。
安装器遇到 `inferred` 且当前身份是 `overridden` 时**不切换**。

版本号怎么来：
- `.deb`：control 里没有发行版字段，默认 `12`（bookworm）；
  可用 `--distro-version=11` 覆盖
- `.pkg.tar.zst`：Arch 是 rolling，`version_id` 留空，`BUILD_ID=rolling`
- PKGBUILD：读 `pkgver` 无意义，同上

### 5.2 权限收敛：只有一个地方写这些文件

**所有身份文件只允许由 `/bin/hbos-distro` 一个程序写。**

理由：分散写必然出现"os-release 改了、debian_version 没改"的半吊子状态，
而这种不一致最难查。

```
hpt install foo.deb     → 解析 manifest.distro → hbos-distro use debian --version 12
hpt install bar.pkg.tar.zst → hbos-distro use arch
hpt distro              → 打印当前身份 + 来源包
hpt distro use debian   → 手动锁定（confidence 变 overridden）
hpt distro reset        → 回到 hbos
```

`hbos-distro` 的实现就是个写文件的小工具（P0 可以用 shell 脚本 + 模板，
但镜像里没有 bash 的话就用 C 写，约 200 行）。

### 5.3 首次启动预置

内核 seed 阶段（`[KERN] seed: ...` 那套）写入 `hbos` 档案 +
`/etc/hbos-release`。这样即使不装任何包，`fastfetch` 也能正常工作。

### 5.4 embed 包内的 os-release 怎么办

有些包（如 `base-files`）自带 `/etc/os-release`。规则：

- **不覆盖**：安装器跳过包内的 `/etc/os-release`、`/etc/lsb-release`、
  `/etc/debian_version`、`/etc/arch-release`
- 记录到 manifest 的 `skipped_files`，`hpt info` 能查
- 否则装个 `base-files` 就把身份改了，行为完全不可预测

---

## 6. 验证清单

装完 `fastfetch` 之后逐条过：

```sh
cat /etc/os-release                 # ID 正确 + HBOS_* 字段在
cat /etc/hbos-release               # 任何身份下都在
fastfetch                           # logo 与 OS 行匹配当前身份
fastfetch --logo none               # 纯文本模式，确认 OS 字段
uname -a                            # 仍是 Linux ... hbos
```

三个身份的预期输出：

| 身份 | fastfetch 的 OS 行 |
|---|---|
| debian | `Debian GNU/Linux 12 (bookworm) x86_64` |
| arch | `Arch Linux x86_64` |
| hbos | `HBOS 0.2-beta1 (gui.4) x86_64` |

---

## 7. 分阶段

| 阶段 | 内容 | 产出 |
|---|---|---|
| **P0** | 三份档案 + `hbos-distro` 工具 + 内核 seed 预置 + `hpt distro` 命令 | `fastfetch` 能正确显示 Debian 12 / Arch |
| **P1** | `pkg2hax` 写 `distro` 字段；`hpt install` 按包切换 | 装包自动切身份 |
| **P2** | `dpkg` / `pacman` 最小数据库骨架 | `dpkg -l` 不报错 |
| **P3** | APT / pacman 真实语义（依赖求解、卸载、升级） | 真正的包管理 |

P0 和 P1 是这次要做的；P2/P3 依赖包管理器的整体进度。

---

## 8. 风险与已知取舍

1. **全局单身份的局限**：同时装 deb 和 AUR 包时只能二选一。这不是实现缺陷，
   是 os-release 的固有约束。缓解：`hpt distro` 显示当前身份的来源包，
   切换有明确日志。
2. **装得太像反而难查**：P0 阶段**不**伪造 dpkg/pacman 数据库，
   让 `dpkg` 干脆不存在 —— "命令找不到"比"命令返回空表"好排查得多。
3. **`ID` 字段不能撒谎过头**：`ID=debian` 时 `NAME` 也必须是 Debian，
   否则 fastfetch 的 logo 和文字会打架。HBOS 身份全部收在 `HBOS_*`
   自定义字段和 `/etc/hbos-release`。
4. **包内自带 os-release 的情况**必须跳过，否则装什么变什么。
5. **`/etc/issue` 会被登录程序读**：HBOS 目前没有 getty 抢这个文件，
   但如果以后加，要注意别让 `agetty` 覆盖。

---

## 9. 待确认

- `HOME_URL` / `BUG_REPORT_URL` 的真实值（现在用占位）
- `.deb` 的发行版版本号：固定 `12`，还是从仓库的 `Release` 文件读？
  （本地 `dpkg-deb -x` 解出来的包里没有这个信息）
- AUR 包的 `BUILD_ID`：用安装日期，还是固定 `rolling`？
