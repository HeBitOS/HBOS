# chromium-headless-shell 安装依赖清单(HBOS / `--dump-dom`)

这份文档把"跑起 headless shell 到底需要哪些文件"固定下来,并给出**静态校验**入口。
目标只有一个:**下次镜像重建时,不要再因为漏了某个文件白烧一轮 QEMU**。

配套工具:

| 工具 | 作用 | 退出码 |
|---|---|---|
| `scripts/check_headless_deps.py` | 直接读镜像,查依赖闭包/资源布局/版本符号 | 0=全在,1=有缺失,2=工具或环境错 |
| `scripts/fix_headless_layout.sh` | 幂等补齐 shell 目录里的资源(符号链接) | 0=成功(含"本来就对"),1=有失败 |
| `scripts/headless_deps_expect.json` | 期望清单(包/资源/符号),校验与修复都读它 | — |
| `scripts/headless_img.py` | 两个工具共用的"只读镜像 + debugfs"底层 | — |

三条命令(装完 chromium 相关 deb 之后):

```sh
bash scripts/fix_headless_layout.sh          # 1. 幂等补齐资源布局(缺什么补什么)
python3 scripts/check_headless_deps.py       # 2. 闸门:缺文件/缺符号就退出码非 0
bash scripts/fix_headless_layout.sh --include-optional --dry-run   # 3. 看 GPU/crashpad 辅助文件缺哪些
```

镜像约定:ext2 带 MBR,文件系统从 **offset 1048576** 开始(`debugfs` 必须加 `?offset=1048576`);
工具默认值写在 `expect` 里,`--img/--offset` 可覆盖;裸 ext2(offset=0)也能自动识别。

---

## 1. 系统库清单(headless shell 的直接 `NEEDED`,42 个)

shell 在镜像里的位置:`/opt/chromium-headless-shell/usr/lib/chromium/chromium-headless-shell`
(版本 `156.0.8078.73-1`,229 878 616 字节)。

HBOS 的库搜索顺序(`src/tools/app.c` → `build_linux_env()`):
`/lib64` → `/lib/x86_64-linux-gnu` → `/usr/lib/x86_64-linux-gnu` → `/usr/lib` → `/lib`
→ 然后才是 `/opt/<包>/usr/lib/x86_64-linux-gnu`、`/opt/<包>/usr/lib64`、`/opt/<包>/usr/lib`。
当前镜像这套拼出来的 `LD_LIBRARY_PATH` 是 **239 个目录 / 7919 字节**(内核上限已从 1 KB 扩到 16 KB,见坑 5)。

"已验证版本"= **build/debs/ 里那个 `.deb` 的文件名版本**(装进镜像的就是它),并用镜像内实际加载的
`.so` 文件名交叉核对;标了 attachments / /tmp/debs 的是后来单独补的包。

| soname | Debian 包 | 已验证版本 | 镜像里实际加载的文件 |
|---|---|---|---|
| `ld-linux-x86-64.so.2` | — | HBOS 自带(`libc.so.6`/`libm.so.6` 同理) | — |
| `libFLAC.so.14` | `libflac14` | 1.5.0+ds-5+b1 (attachments;build/tmp/debs 另有 1.5.0+ds-2) | libFLAC.so.14.0.0 |
| `libX11.so.6` | `libx11-6` | 1.8.4-2+deb12u2 | libX11.so.6.4.0 |
| `libXNVCtrl.so.0` | `libxnvctrl0` | 525.85.05-3~deb12u1 | libXNVCtrl.so.0.0.0 |
| `libXcomposite.so.1` | `libxcomposite1` | 0.4.5-1 | libXcomposite.so.1.0.0 |
| `libXdamage.so.1` | `libxdamage1` | 1.1.6-1 | libXdamage.so.1.1.0 |
| `libXext.so.6` | `libxext6` | 1.3.4-1+b1 | libXext.so.6.4.0 |
| `libXfixes.so.3` | `libxfixes3` | 6.0.0-2 | libXfixes.so.3.1.0 |
| `libXrandr.so.2` | `libxrandr2` | 1.5.2-2+b1 | libXrandr.so.2.2.0 |
| `libasound.so.2` | `libasound2` | 1.2.8-1+b1 | libasound.so.2.0.0 |
| `libatk-1.0.so.0` | `libatk1.0-0` | 2.46.0-5 | libatk-1.0.so.0.24609.1 |
| `libatspi.so.0` | `libatspi2.0-0` | 2.46.0-5 | libatspi.so.0.0.1 |
| `libc.so.6` | — | HBOS 自带 | — |
| `libdav1d.so.7` | `libdav1d7` | 1.5.4-1 (attachments;build/tmp/debs 另有 1.5.1-1) | libdav1d.so.7.0.0 |
| `libdbus-1.so.3` | `libdbus-1-3` | 1.14.10-1~deb12u1 | libdbus-1.so.3.32.4 |
| `libdouble-conversion.so.3` | `libdouble-conversion3` | 3.2.1-1 | libdouble-conversion.so.3.1 |
| `libexpat.so.1` | `libexpat1` | 2.5.0-1+deb12u2 | libexpat.so.1.8.10 |
| `libfontconfig.so.1` | `libfontconfig1` | 2.14.1-4 | libfontconfig.so.1.12.0 |
| `libfreetype.so.6` | `libfreetype6` | 2.12.1+dfsg-5+deb12u4 | libfreetype.so.6.18.3 |
| `libgbm.so.1` | `libgbm1` | 22.3.6-1+deb12u2 | libgbm.so.1.0.0 |
| `libgcc_s.so.1` | `libgcc-s1` | 12.2.0-14+deb12u1 | libgcc_s.so.1 |
| `libgio-2.0.so.0` / `libglib-2.0.so.0` / `libgobject-2.0.so.0` | `libglib2.0-0` | 2.74.6-2+deb12u9 | …so.0.7400.6 |
| `libharfbuzz-subset.so.0` | `libharfbuzz-subset0` | 6.0.0+dfsg-3 | libharfbuzz-subset.so.0.60000.0 |
| `libharfbuzz.so.0` | **`libharfbuzz0b`** | **10.2.0-1+deb13u1** (/tmp/debs;build/debs 里的 6.0.0+dfsg-3 是踩坑版本) | libharfbuzz.so.0.61020.0 |
| `libjpeg.so.62` | `libjpeg62-turbo` | 2.1.5-2 | libjpeg.so.62.3.0 |
| `liblcms2.so.2` | `liblcms2-2` | 2.14-2+deb12u1 | liblcms2.so.2.0.14 |
| `libm.so.6` | — | HBOS 自带 | — |
| `libminizip.so.1` | `libminizip1` **或** `libminizip1t64` | 1.1-8+deb12u1 / 1.3.dfsg+really1.3.2-4 | libminizip.so.1.0.0(两份都在,见坑 6) |
| `libnspr4.so` | `libnspr4` | 4.35-1 | libnspr4.so |
| `libnss3.so` / `libnssutil3.so` | `libnss3` | 3.87.1-1+deb12u2 | libnss3.so / libnssutil3.so |
| `libopenh264.so.8` | **`libopenh264-8`** | 2.6.0+dfsg-2+b2 (/tmp/debs) | libopenh264.so.2.6.0 |
| `libopenjp2.so.7` | `libopenjp2-7` | 2.5.0-2+deb12u3 | libopenjp2.so.2.5.0 |
| `libopus.so.0` | `libopus0` | 1.3.1-3 | libopus.so.0.8.0 |
| `libpulse.so.0` | `libpulse0` | 16.1+dfsg1-2+b1 | libpulse.so.0.24.2 |
| `libudev.so.1` | `libudev1` | 252.39-1~deb12u2 | libudev.so.1.7.5 |
| `libxcb.so.1` | `libxcb1` | 1.15-1 | libxcb.so.1.1.0 |
| `libxkbcommon.so.0` | `libxkbcommon0` | 1.5.0-1 | libxkbcommon.so.0.0.0 |
| `libz.so.1` | `zlib1g` | 1.2.13.dfsg-1 | libz.so.1.2.13 |
| `libzstd.so.1` | `libzstd1` | 1.5.4+dfsg2-5 | libzstd.so.1.5.4 |

**HBOS 自带、镜像里不该出现、也不算缺失**的 soname(写在 `expect.hbos_provided_sonames`):
`ld-linux-x86-64.so.2`、`libc.so.6`、`libm.so.6`、`libpthread.so.0`、`librt.so.1`、
`libdl.so.2`、`libutil.so.1`、`libresolv.so.2`(仓库根的 `lib*.hax`)。
查找顺序是**先镜像后自带**:镜像里真有就用镜像那份,免得"包装丢了"被误判成"系统自带"。

闭包校验默认展开到深度 3(直接 NEEDED 42 个 → 75 个 soname,其中 69 个落盘、4 个 HBOS 自带)。

### 哪些是 trixie/sid 的 t64 系列

t64 = Debian 64 位 `time_t` 迁移后改名的包(**只在 trixie/sid 存在**;bookworm 还是老名字)。
用本地 trixie 索引(`build/tmp/debs/Packages-trixie.xz`)核对,本清单里**被 t64 改名**的是:

| bookworm 名字(bookworm 里用这个) | trixie 名字(t64) | trixie 版本 |
|---|---|---|
| `libasound2` | `libasound2t64` | 1.2.14-1+deb13u1 |
| `libatk1.0-0` | `libatk1.0-0t64` | 2.56.2-1+deb13u2 |
| `libatspi2.0-0` | `libatspi2.0-0t64` | 2.56.2-1+deb13u2 |
| `libglib2.0-0` | `libglib2.0-0t64` | 2.84.4-3~deb13u5 |
| `libminizip1` | `libminizip1t64` | 1:1.3.dfsg+really1.3.1-1+b1 |

(image 里这四个包用的是 **bookworm** 名字/版本;`libminizip1t64` 是唯一真按 trixie 名字装的,
且它和 bookworm 的 `libminizip1` **同时在镜像里**,见坑 6。)

其余"非要 trixie/sid 不可"的包(bookworm 版本太旧,headless shell 156 需要新 ABI/新符号),
按上面表里的版本装,都是同名包、不走 t64 改名:

- `libharfbuzz0b` 10.2.0-1+deb13u1 —— bookworm 的 6.0.0 缺 `hb_face_set_get_table_tags_func`(坑 1)
- `libdav1d7` 1.5.4-1(bookworm 只有 `libdav1d6`)
- `libflac14` 1.5.0+ds-5+b1(bookworm 只有 `libflac12`)
- `libopenh264-8` 2.6.0+dfsg-2+b2(`libopenh264-7` 是 bookworm 的旧 soname)

`libc6 >= 2.43` 这条是 headless shell 156 的硬要求(bookworm 的 glibc 2.36 不够),
所以这些库整体按 trixie/sid 取;HBOS 运行期用的是自己的 libc(镜像里不需要 libc6 包)。

---

## 2. 资源文件清单(shell 自己目录里必须有)

shell 目录:`/opt/chromium-headless-shell/usr/lib/chromium/`
数据来源目录:`/opt/chromium-common/usr/lib/chromium/`(同内容的另一份在 `/opt/chromium/usr/lib/chromium/`)

| 文件 | 大小 | 来源 | 缺了会怎样 |
|---|---|---|---|
| `chromium-headless-shell` | 229 878 616 | 本体(headless-shell deb 自带) | 没有可运行的东西 |
| `icudtl.dat` | 10 876 560 | chromium-common | `Invalid file descriptor to ICU data received` → `int3` |
| `snapshot_blob.bin` | 365 488 | chromium-common | `FATAL:gin/v8_initializer.cc:640 Error loading V8 startup snapshot file` |
| `v8_context_snapshot.bin` | 740 048 | chromium-common | 同上 |
| `headless_command_resources.pak` | 3 109 | headless-shell deb 自带 | 命令行资源缺失 |
| `headless_lib_data.pak` | 742 327 | headless-shell deb 自带 | headless UI 资源缺失 |
| `headless_lib_strings.pak` | 5 017 195 | headless-shell deb 自带 | 字符串表缺失 |
| `resources.pak` | 13 046 519 | chromium-common | 通用资源缺失 |
| `chrome_100_percent.pak` | 696 632 | chromium-common | 图标/缩放资源缺失 |
| `chrome_200_percent.pak` | 1 198 296 | chromium-common | 同上(2x) |
| `locales/en-US.pak` | 577 990 | chromium-common | 语言包缺失;要别的语言再链对应 `locales/<lang>.pak` |

`optional_resources`(只告警不阻塞,Chromium 按 `dirname(/proc/self/exe)` 去 dlopen/exec 的):
`libEGL.so`、`libGLESv2.so`、`libvk_swiftshader.so`、`libvulkan.so.1`、`vk_swiftshader_icd.json`、
`chrome_crashpad_handler` —— 目前**都不在 shell 目录**(都在 chromium-common),
用 `bash scripts/fix_headless_layout.sh --include-optional` 一条命令可以补齐。

### 为什么用符号链接而不是复制

`scripts/fix_headless_layout.sh` 给资源建 `shell目录/<name> → /opt/chromium-common/usr/lib/chromium/<name>`
的符号链接。读内核代码确认过没有额外风险:

- `src/vfs.c` `vfs_expand_symlinks()` 会展开路径里的**每一段**(含最后一段),上限 8 层,
  也支持相对目标;这里链长最多 2 层。
- `src/ext2.c` `ext2_readlink()` 同时支持 fast(≤60 字节内联在 inode)和 slow(目标放数据块)
  两种布局。本脚本产出的链接两种都有:`resources.pak`(51 字节)是 fast,
  `chrome_100_percent.pak`(恰好 60 字节)会被 `debugfs` 写成 slow —— 两种 HBOS 都读得出来。
- `debugfs symlink` 写的目录项 `file_type=7`(实测扫描原始目录块确认),正是内核要求的 S_IFLNK。
- 代价:复制要多占约 **27 MB**(icudtl 10.9 + resources.pak 13.0 + v8 snapshot 0.7 +
  percent paks 1.9 + snapshot_blob 0.4 + en-US.pak 0.6;只算 icudtl/snapshot/v8 这三样约 12 MB),
  而链接几乎不占空间;更关键的是**升级 chromium-common 后链接自动指向新文件**,复制不会。

---

## 3. 版本符号检查(最有价值的一条)

"库在不在"和"库能不能用"是两件事。已知踩过的 `exit 127 undefined symbol` 就是后者。
工具做两层:

1. **必检符号**(`expect.required_symbols`,钉死的版本敏感符号)
   `libharfbuzz.so.0` → `hb_face_set_get_table_tags_func`。
   实测:镜像里同时躺着 `libharfbuzz.so.0.60000.0`(6.0.0,bookworm)和
   `libharfbuzz.so.0.61020.0`(10.2.0,trixie),把 6.0.0 与 shell 需要的 88 个 `hb_*`
   符号对差,**6.0.0 恰好只缺这一个符号** —— 就是当初 `exit 127` 的根因。
2. **符号族**(`expect.symbol_families`):从 shell 的未定义动态符号(1641 个)里按前缀
   抽出"这个库该提供什么",再要求该族的库真的导出它们。当前覆盖 26 个族、共 900+ 个符号:
   `hb_`(88)、`hb_subset_`(5)、`Wels`(2)、`dav1d_`(11)、`FLAC__`(11)、`unz|zip`(14)、
   `ZSTD_`(18)、zlib(20)、`jpeg_`(33)、`opj_`(23)、`cms`(8)、`XML_`(11)、`Fc`(39)、
   `FT_`(62)、`xkb_`(20)、`snd_`(125)、`pa_`(57)、`udev_`(31)、`gbm_`(20)、`xcb_`(28)、
   `atk_`(64)、`atspi_`(23)、`dbus_`(83)、`opus_`(29)、`g_`(glib/gobject/gio 联合,124)、
   `X`(7 个 X 库联合,65)、NSS 系(`PR_/PL_/NSS_/SEC_/PKII_…` 联合,36)。
   前缀撞车过两次(`XML_` 撞 `X` 族、`hb_subset_` 撞 `hb_` 族)已用 `exclude_prefixes` 消掉。

做法:从镜像 `debugfs dump` 出 `.so`,不执行它,纯静态查符号(`nm -D -P`,只认大写类型的动态符号)。

---

## 4. 踩过的坑 ↔ 症状 ↔ 现在能不能静态发现

| # | 症状 | 根因 | 能否静态发现 |
|---|---|---|---|
| 1 | `exit 127` + `undefined symbol: hb_face_set_get_table_tags_func` | 装了 bookworm 的 libharfbuzz 6.0.0;瞎补 10.2.0 后同目录还留着 6.0.0 文件,soname 链接指向旧文件 | ✅ 第 3 节必检符号(6.0.0 只缺这一个符号,已实测) |
| 2 | `Invalid file descriptor to ICU data received` → `int3` | `icudtl.dat` 不在 shell 自己目录 | ✅ 第 2 节资源清单 |
| 3 | `FATAL:gin/v8_initializer.cc:640 Error loading V8 startup snapshot file` | `snapshot_blob.bin` / `v8_context_snapshot.bin` 不在 shell 目录 | ✅ 第 2 节(实测最近 3 次运行已无此错) |
| 4 | `exit 127`,没有任何解释 | `libopenh264-8` / `libdav1d7` / `libminizip1t64` / `libflac14` 整个包没装 | ✅ 第 1/2 节,直接点名缺哪个 soname |
| 5 | `exit 127`,一堆库"明明装了却找不到" | `LD_LIBRARY_PATH` 1 KB 缓冲装不下 148 个包的路径,后半个列表被静默截断(现在 239 目录 7919 字节,旧上限必炸) | ✅ 间接:第 1 节会打印搜索目录数与解析结果;内核已扩到 16 KB(不归本清单管) |
| 6 | 随机性地加载到旧实现 | 同一个 soname 有两份实现:`libminizip1`(1.1)与 `libminizip1t64`(1.3.2)同时在 `/opt`,内核按 `/opt` readdir 顺序取第一个 | ✅ 第 2 节 WARN(当前实测两份都导出所需 14 个符号,所以只告警) |
| 7 | 装了"库"却仍然 `exit 127` | `libopenh264-cisco8` 是 Debian **contrib 的空壳包**,只有版权文件,不含 `libopenh264.so.8` | ✅ 第 1 节直接报 `libopenh264.so.8` 缺失(包名对不对不靠猜,靠 soname) |
| 8 | 重启后链接"变成普通文件" | ext2 目录项 `file_type` 不是 7,内核不再当符号链接 | ✅ 第 2 节(顺带记录:`debugfs symlink` 写的 file_type=7,实测原始目录块确认) |
| 9 | 悬空链接:看着文件在,一读就报错 | 手工 `debugfs symlink` 目标写错/目标被删 | ✅ 第 2 节把"缺失"和"悬空/成环"分开报 |
| 10 | 工具自身:刚写完的链接被判成"读不出目标" | 工作区挂在 **fuseblk(NTFS-3G)** 上,长命句柄在别的进程写完后仍读到旧页(实测老句柄读全 0、新开句柄读到正确内容) | — 已修:`headless_img.py` 裸读每次新开句柄 |
| 11 | 目标恰好 60 字节时 `debugfs stat` 不给 `Fast link dest` | ext2 fast symlink 上限 60 字节,`debugfs` 不打印边界情况的目标;且 `BLOCKS` 里那些"块号"其实是内联目标字节 | — 已修:工具两种布局都自己还原(`i_blocks==0 && size<=60` 走内联,否则读 `i_block[0]`) |

### 版本错配(已记录,暂时不影响)

`/opt/chromium-common` 里的数据文件尺寸与 **chromium-common 150.0.7871.100-1~deb12u1** 逐项一致
(156 的 chromium/headless-shell deb 都不含这些数据文件),而 shell 是 156.0.8078.73-1;
`--dump-dom` 实测能过 ICU/V8 快照加载(最近 3 次运行无相关报错)。要彻底干净就装
chromium-common 156 —— 装完后**不用改脚本**,链接自动指向新文件。

---

## 5. 从零重建:可复现步骤

```sh
# 1) bookworm 基线依赖闭包(下到 build/debs/)
python3 scripts/fetch_deb_closure.py chromium --max-packages 400

# 2) 单独补 trixie/sid 的那几个(版本见第 1 节),放 build/tmp/debs/
#    libharfbuzz0b_10.2.0-1+deb13u1、libdav1d7_1.5.4-1、libflac14_1.5.0+ds-5+b1、
#    libopenh264-8_2.6.0+dfsg-2+b2、libminizip1t64_1.3.dfsg+really1.3.2-4
#    (attachments 里有只读副本;也可从 https://deb.debian.org/debian/pool/... 取)

# 3) 装包(--img 就是磁盘镜像;--no-launch 关掉装完自动开 QEMU)
python3 scripts/install_app.py --deb build/tmp/debs/<pkg>.deb --img build/c3-chromium.img --no-launch

# 4) (可选)系统目录里的 soname 兜底链接;LD_LIBRARY_PATH 已扩到 16 KB,不是必需
python3 scripts/link_flat_libs.py --img build/c3-chromium.img \
    --lib libharfbuzz.so.0=/opt/libharfbuzz0b/usr/lib/x86_64-linux-gnu/libharfbuzz.so.0.61020.0

# 5) 幂等固化资源布局
bash scripts/fix_headless_layout.sh
#    (GPU/SwiftShader/crashpad 辅助文件一起补:加 --include-optional)

# 6) 闸门:非 0 就别开 QEMU
python3 scripts/check_headless_deps.py
```

`install_app.py` 在装 `chromium*` 包时会自动跑第 5、6 步(见脚本末尾 `post_install_chromium()`),
所以正常路径下只需要第 3 步。

---

## 6. 静态校验的边界 / 还需要 QEMU 验证什么

校验器**全绿 ≠ 能跑**。它只保证"文件、符号、资源布局都在",管不到的:

1. **运行期 GPU 进程**:当前 `hs-dom14` 的失败点是
   `FATAL:content/browser/gpu/gpu_data_manager_impl_private.cc:389 GPU process isn't usable. Goodbye.`
   (伴随 `GPU process launch failed: error_code=1002`)。这不是文件缺失 —— 校验器全绿。
   两个待验证的候选动作:
   - `bash scripts/fix_headless_layout.sh --include-optional` 把 `libEGL.so`/`libGLESv2.so`/
     `libvk_swiftshader.so`/`vk_swiftshader_icd.json`/`chrome_crashpad_handler` 链进 shell 目录,
     再跑一轮 `--dump-dom`(Chromium 找这些文件是按 `dirname(exe)`,不在别的包目录里找);
   - 或在命令行加 `--in-process-gpu --disable-gpu`,绕开 GPU 子进程,先确认渲染/HTML 输出链路。
2. **dlopen 的非 NEEDED 库**:符号检查只看直接依赖;`dlopen("libwayland-client.so.0")` 这类
   HBOS 侧没有对应实现的库,静态看不出来。
3. **数据文件版本匹配**:ICU/V8 快照是否与 shell 版本严格匹配,只有跑起来才知道
   (当前 150/156 错配实测可用,记录在第 4 节末尾)。
4. **ABI 层面**:符号存在但不是同一个 ABI(比如结构体布局变了)静态查不出来。

据此,**下一轮 QEMU 只需要验两件事**:(a) `--include-optional` 补齐后 GPU 进程是否还能起;
(b) 若仍失败,`--in-process-gpu --disable-gpu` 下 `--dump-dom` 是否吐出 HTML。
文件缺失这一整类问题应当已经不用再验了。
