#include "ext2.h"
#include "block.h"
#include "errno.h"
#include "rtc.h"
#include "string.h"

/*
 * 写路径的错误码约定
 * ══════════════════
 * 本文件对外接口成功返回 0（或新建的 inode 号），失败返回 **负的 errno**
 * （-ENOSPC / -EROFS / -EISDIR …）。旧实现一律 -1，调用方只能翻译成 EIO，
 * 用户态拿不到"磁盘满了"还是"只读卷"——Chromium 的 sqlite 就是靠 ENOSPC
 * 决定回滚事务的，EIO 会让它直接杀掉渲染进程。
 *
 * 分配器（ext2_alloc_block/inode）仍然用 0 表示"分配不到"：0 本身就是
 * 无效块号/inode 号，比负值更贴合契约；写路径在调用点翻译成 -ENOSPC。
 */

/** 参数/状态检查：非只读且已挂载返回 0，否则返回 -EINVAL / -EROFS。 */
static int ext2_writable(ext2_fs_t *fs) {
    if (!fs || !fs->mounted) return -EINVAL;
    if (fs->read_only) return -EROFS;
    return 0;
}

/**
 * @brief 当前 Unix 时间戳（秒），ext2 的 i_atime/i_mtime/i_ctime 用它。
 *
 * RTC 读失败时返回 1 而不是 0：i_*time == 0 在 e2fsck 眼里是"时间戳缺失"，
 * 会被判成 inode 损坏；1 至少是一个合法（且必然在过去）的时刻。
 * 时区维度按 RTC 本地时间直接折算，与内核 rtc_timestamp() 的口径一致。
 */
static uint32_t ext2_now(void) {
    uint64_t seconds = rtc_timestamp();
    return (uint32_t)(seconds ? seconds : 1);
}

/** 新建/修改 inode 时统一打时间戳。 */
static void ext2_touch(ext2_inode_t *inode) {
    uint32_t now = ext2_now();
    inode->i_atime = now;
    inode->i_mtime = now;
    inode->i_ctime = now;
}

/** 修改目录内容后刷新该目录自己的 mtime/ctime（atime 不动）。 */
static void ext2_touch_dir(ext2_fs_t *fs, uint32_t dir_ino, ext2_inode_t *dir) {
    uint32_t now = ext2_now();
    dir->i_mtime = now;
    dir->i_ctime = now;
    (void)ext2_write_inode(fs, dir_ino, dir);
}

/**
 * @brief 目录项的名字长度。
 *
 * ext2 的目录项布局有两种：置了 INCOMPAT_FILETYPE 时第 6 字节是 name_len、
 * 第 7 字节是 file_type；没有该特性时这两个字节合成 16 位 name_len。老卷
 * （mke2fs -O ^filetype）在文件名 >255 时才会用到高位，但按 8 位读会把
 * 文件名截断后匹配错项，所以统一走这个访问器。
 */
static uint32_t ext2_dirent_name_len(const ext2_fs_t *fs,
                                    const ext2_dir_entry_t *entry) {
    if (fs->has_filetype) return entry->name_len;
    return (uint32_t)entry->name_len | ((uint32_t)entry->file_type << 8);
}

/**
 * 位图栈缓冲容量（字节）。一个块组最多 8192 块 / 8192 inode，即 1024 字节
 * 位图；留到 4 KiB 与最大块尺寸一致，兼顾 64 KiB/组以下的卷。分配/释放
 * 路径在读取位图前都会校验 (bits+7)/8 ≤ 本常量，避免损坏超级块导致栈越界。
 */
#define EXT2_BITMAP_BUF_BYTES 4096u

/*
 * 间接块缓存（3 槽）—— 同时解决两个问题
 * ══════════════════════════════════════
 * 问题 1（性能）：`ext2_inode_block()` 每次调用都要把所在的间接块从磁盘读回来。
 * 顺序读一个大文件时，同一个间接块会被连续成百上千次重复读取（block_idx 只
 * +1，一级路径下 1024 个块共享同一张表），这是 I/O 量的成倍浪费。
 *
 * 问题 2（栈安全）：旧实现按路径在**栈上**放 1~3 个 4 KiB 缓冲（`ind_buf`/
 * `dind_buf`/`tind_buf`），三级路径合计 12 KiB —— 而任务栈（`TASK_STACK_SIZE`）
 * 只有 8 KiB，这是随时会踩爆栈的隐患。CODE_STYLE §4 明确写了"不要在栈上放
 * >4 KiB 的缓冲"。
 *
 * 方案：把这三张表放到文件级静态区，并给每个"层级深度"一个独立槽位，这样
 * 多级路径**不会互相驱逐**：
 *   - 一级路径用 slot 0；
 *   - 二级路径 slot 0 = 顶级表、slot 1 = 二级表；
 *   - 三级路径 slot 0/1/2 依次向下。
 * 顺序读时各槽的键几乎不变（一张顶级表覆盖 addr_per_block 个块），命中率接近
 * 100%，同时栈占用从 12 KiB 降到 0。
 *
 * 不变式：
 *  - 槽内容只取决于 (卷起始 LBA, 块大小, 间接块号)，三者都参与比较，换卷/
 *    换块大小必然 miss，不可能读到别的卷的数据；
 *  - `ext2_inode_block()` 只读间接块、从不改写它，因此缓存无需在写路径失效；
 *  - 内核单核协作式调度且本路径无让出点（无 task_yield/阻塞等待），文件级
 *    静态缓存不存在并发写入问题。
 */
#define EXT2_IND_SLOTS 3
/** @brief 单个间接块槽的容量（ext2 块上限 4 KiB；更大的块不缓存，走直读） */
#define EXT2_IND_SLOT_BYTES 4096u

static uint8_t  g_ind_slot[EXT2_IND_SLOTS][EXT2_IND_SLOT_BYTES] __attribute__((aligned(4)));
static uint32_t g_ind_slot_block[EXT2_IND_SLOTS]; /**< 0 = 无效（块 0 不是有效数据块） */
static uint32_t g_ind_slot_part[EXT2_IND_SLOTS];  /**< 卷起始 LBA，区分分区 */
static uint32_t g_ind_slot_bsize[EXT2_IND_SLOTS]; /**< 块大小，区分重新挂载 */

/**
 * @brief 读一个间接块到指定槽（命中则直接返回），`ext2_inode_block()` 专用
 *
 * @param slot 槽位（0..EXT2_IND_SLOTS-1），调用方按层级深度分配、互不复用
 * @param out  输出：指向块内容的指针（指向该槽的静态缓冲）
 * @return 0 成功，-1 失败
 */
static int ext2_read_ind_block(ext2_fs_t *fs, uint32_t block, int slot,
                               const uint8_t **out) {
    if (block == 0) return -1;
    if (slot < 0 || slot >= EXT2_IND_SLOTS) return -1;
    if (fs->block_size > EXT2_IND_SLOT_BYTES) return -1; /* 防御：ext2 上限 4 KiB */

    if (g_ind_slot_block[slot] == block &&
        g_ind_slot_part[slot] == fs->partition_lba &&
        g_ind_slot_bsize[slot] == fs->block_size) {
        *out = g_ind_slot[slot];
        return 0;
    }
    if (ext2_read_block(fs, block, g_ind_slot[slot]) < 0) return -1;
    g_ind_slot_block[slot] = block;
    g_ind_slot_part[slot] = fs->partition_lba;
    g_ind_slot_bsize[slot] = fs->block_size;
    *out = g_ind_slot[slot];
    return 0;
}

static uint32_t ext2_read_sector(uint32_t partition_lba, uint32_t lba, uint8_t *buf)
{
    return block_read_sector(partition_lba + lba, buf) == 0 ? 1 : 0;
}

static const char *g_ext2_error = "ok";

const char *ext2_last_error(void) { return g_ext2_error; }

int ext2_mount(uint32_t partition_lba, ext2_fs_t *fs)
{
    g_ext2_error = "ok";
    if (!fs) return -1;
    memset(fs, 0, sizeof(*fs));

    uint32_t sb_lba = EXT2_SUPERBLOCK_OFFSET / BLOCK_SECTOR_SIZE;
    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));

    if (!ext2_read_sector(partition_lba, sb_lba, sector)) return -1;
    memcpy(&fs->sb, sector, sizeof(ext2_superblock_t));

    if (fs->sb.s_magic != EXT2_SIGNATURE) {
        g_ext2_error = "bad magic";
        return -1;
    }

    /* ext4（extent / 64bit / flex_bg）和需要日志回放的 ext3 不能按 ext2 读：
     * 硬挂上去会把 extent 树当块指针解释，读出来是垃圾，写下去就是破坏。 */
    uint32_t incompat = fs->sb.s_feature_incompat;
    if (incompat & EXT2_INCOMPAT_RECOVER) {
        g_ext2_error = "journal needs replay (ext3 dirty)";
        return -1;
    }
    fs->feature_incompat = incompat;
    fs->desc_size = (incompat & EXT2_INCOMPAT_64BIT) ? 64 : 32;
    /* ext4（extent/64bit）只读：现有写路径只会写直接/间接块指针，往 extent
     * inode 上写等于破坏文件系统。
     *
     * 另外三类元数据校验特性同样必须只读——它们把校验和写进了块位图 /
     * 组描述符 / 目录块尾巴，我们的写路径不会重算，写下去就是"校验和错误"
     * 的卷：
     *   ro_compat GDT_CSUM(0x10)      组描述符校验和
     *   ro_compat METADATA_CSUM(0x400) 位图/inode/目录块校验和
     *   incompat META_BG(0x10)        组描述符被搬进元块组，read_bgd 会取错位置
     * 只读挂载仍然可用（读路径不校验校验和）。 */
    uint32_t ro_compat = fs->sb.s_feature_ro_compat;
    fs->read_only = (incompat & (EXT2_INCOMPAT_EXTENTS | EXT2_INCOMPAT_64BIT |
                                 EXT2_INCOMPAT_META_BG)) != 0 ||
                    (ro_compat & (EXT2_RO_COMPAT_GDT_CSUM |
                                  EXT2_RO_COMPAT_METADATA_CSUM)) != 0;
    fs->has_filetype = (incompat & EXT2_INCOMPAT_FILETYPE) != 0;

    /* 这些特性改变了"文件数据放在哪"或"目录项怎么编码"，按块指针读出来
     * 只会是垃圾。宁可挂载失败（错误串说明原因），也不要让上层读到错数据。 */
    if (incompat & (EXT2_INCOMPAT_INLINE_DATA | EXT2_INCOMPAT_ENCRYPT)) {
        g_ext2_error = "ext4 inline_data/encrypt unsupported";
        return -1;
    }

    /* 卷参数全部来自磁盘，必须在使用前校验：下面的组数/组内 inode 数计算
     * 是整数除法，s_blocks_per_group == 0 会直接 #DE 打死内核（不是返回错误）。
     * e2fsck 能容忍的"可疑但可修"超级块，我们也至少要能安全拒绝。 */
    fs->block_size = 1024U << fs->sb.s_log_block_size;
    fs->inode_size = fs->sb.s_inode_size ? fs->sb.s_inode_size : 128;
    if (fs->sb.s_blocks_per_group == 0 || fs->sb.s_inodes_per_group == 0 ||
        fs->sb.s_blocks_count == 0 || fs->sb.s_inodes_count == 0) {
        g_ext2_error = "corrupt superblock (zero counters)";
        return -1;
    }
    if (fs->block_size < 1024 || fs->block_size > 4096 ||
        fs->inode_size < 128 || fs->inode_size > fs->block_size) {
        g_ext2_error = "corrupt superblock (block/inode size)";
        return -1;
    }
    fs->block_group_count = (fs->sb.s_blocks_count + fs->sb.s_blocks_per_group - 1)
                            / fs->sb.s_blocks_per_group;
    fs->bgd_count = (fs->sb.s_inodes_count + fs->sb.s_inodes_per_group - 1)
                    / fs->sb.s_inodes_per_group;
    fs->partition_lba = partition_lba;
    fs->mounted = 1;
    return 0;
}

/**
 * @brief 读取一个 ext2 块（block_size 可能为 1/2/4 KiB）到 buf。
 *
 * 性能要点：块总是扇区的整数倍且 LBA 连续，因此整块一次下发给块层
 * （block_read_sectors）。AHCI 后端会把它合成**一条** DMA 命令；旧的逐扇区
 * 写法在 4 KiB 块上要多发 7 条命令、多等 7 次握手——这是 ext2 路径上
 * 最大的一笔 I/O 开销，目录查找与 inode 读取全都走这里。
 *
 * @param fs    已挂载的文件系统实例
 * @param block 文件系统内块号（非 LBA）
 * @param buf   输出缓冲，容量须 ≥ fs->block_size
 * @return 0 成功，-1 参数非法或底层读失败
 */
int ext2_read_block(ext2_fs_t *fs, uint32_t block, uint8_t *buf)
{
    if (!fs || !fs->mounted || !buf) return -1;

    uint32_t sectors_per_block = fs->block_size / BLOCK_SECTOR_SIZE;
    uint32_t start_lba = block * sectors_per_block;

    return block_read_sectors(fs->partition_lba + start_lba,
                              buf, sectors_per_block) == 0 ? 0 : -1;
}

static int ext2_read_bgd(ext2_fs_t *fs, uint32_t group, ext2_bgd_t *bgd)
{
    if (!fs || !fs->mounted || !bgd) return -1;

    uint32_t bgd_block = fs->sb.s_first_data_block + 1;
    uint32_t bgd_offset = group * (fs->desc_size ? fs->desc_size : sizeof(ext2_bgd_t));
    uint32_t block_offset = bgd_offset / fs->block_size;
    uint32_t byte_offset = bgd_offset % fs->block_size;

    uint8_t *tmp = (uint8_t *)0;
    (void)tmp;

    uint8_t block_buf[4096] __attribute__((aligned(2)));
    if (fs->block_size > 4096) return -1;

    if (ext2_read_block(fs, bgd_block + block_offset, block_buf) < 0) return -1;
    memcpy(bgd, block_buf + byte_offset, sizeof(ext2_bgd_t));
    return 0;
}

int ext2_read_inode(ext2_fs_t *fs, uint32_t inode_num, ext2_inode_t *inode)
{
    if (!fs || !fs->mounted || !inode || inode_num == 0) return -1;

    uint32_t group = (inode_num - 1) / fs->sb.s_inodes_per_group;
    uint32_t index = (inode_num - 1) % fs->sb.s_inodes_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, group, &bgd) < 0) return -1;

    uint32_t inode_table_block = bgd.bg_inode_table;
    uint32_t inode_offset = index * fs->inode_size;
    uint32_t block_offset = inode_offset / fs->block_size;
    uint32_t byte_offset = inode_offset % fs->block_size;

    uint8_t block_buf[4096] __attribute__((aligned(2)));
    if (ext2_read_block(fs, inode_table_block + block_offset, block_buf) < 0)
        return -1;

    memcpy(inode, block_buf + byte_offset, sizeof(ext2_inode_t));
    return 0;
}

/**
 * @brief 把文件内逻辑块号翻译成磁盘物理块号（ext2 间接块解析）。
 *
 * ext2 的 inode 直接指向 12 个数据块，超出后依次走一次/二次/三次间接块。
 * 每多一级就多一次同步块读，因此这里严格按「直→单→双→三」短路返回，
 * 任何一级读失败即返回 0（ext2 约定 0 号为孔洞/无效块）。
 *
 * 注意：间接块缓冲必须 4 字节对齐——它们随后被当作 uint32_t 数组索引，
 * 只声明 aligned(2) 属于对齐不足（x86 上侥幸可用，但违反 C 对齐要求）。
 *
 * @param fs        已挂载实例（需要 block_size 计算每块地址数）
 * @param inode     已读入内存的 inode
 * @param block_idx 文件内逻辑块号（0 起）
 * @return 物理块号；0 表示越界/空洞/读失败
 */

#include "lib/ext4_extent.h"

static int ext2_extent_rd(void *ctx, uint32_t block, void *buf) {
    return ext2_read_block((ext2_fs_t *)ctx, block, buf);
}

static uint32_t ext2_inode_block(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t block_idx)
{
    /* ext4：i_block 是 extent 树，不能按 15 个块指针解释 */
    if (inode->i_flags & EXT2_EXTENTS_FL)
        return ext4_extent_map_raw(ext2_extent_rd, fs,
                                   (const uint8_t *)inode->i_block,
                                   fs->block_size, block_idx);

    /* 每个间接块能装下的块地址数：ext2 块地址固定 4 字节 */
    const uint32_t addr_per_block = fs->block_size / 4;

    if (block_idx < EXT2_NDIR_BLOCKS) {
        return inode->i_block[block_idx];
    }
    block_idx -= EXT2_NDIR_BLOCKS;

    if (block_idx < addr_per_block) {
        const uint8_t *ind;
        if (ext2_read_ind_block(fs, inode->i_block[EXT2_IND_BLOCK], 0, &ind) < 0)
            return 0;
        return ((const uint32_t *)(const void *)ind)[block_idx];
    }
    block_idx -= addr_per_block;

    uint32_t dind_per = addr_per_block * addr_per_block;
    if (block_idx < dind_per) {
        uint32_t i1 = block_idx / addr_per_block;
        uint32_t i2 = block_idx % addr_per_block;
        const uint8_t *dind, *ind;
        if (ext2_read_ind_block(fs, inode->i_block[EXT2_DIND_BLOCK], 0, &dind) < 0)
            return 0;
        if (ext2_read_ind_block(fs, ((const uint32_t *)(const void *)dind)[i1], 1, &ind) < 0)
            return 0;
        return ((const uint32_t *)(const void *)ind)[i2];
    }
    block_idx -= dind_per;

    uint32_t tind_per = dind_per * addr_per_block;
    if (block_idx >= tind_per) return 0;

    uint32_t i1 = block_idx / dind_per;
    block_idx %= dind_per;
    uint32_t i2 = block_idx / addr_per_block;
    uint32_t i3 = block_idx % addr_per_block;

    const uint8_t *tind, *dind, *ind;
    if (ext2_read_ind_block(fs, inode->i_block[EXT2_TIND_BLOCK], 0, &tind) < 0)
        return 0;
    if (ext2_read_ind_block(fs, ((const uint32_t *)(const void *)tind)[i1], 1, &dind) < 0)
        return 0;
    if (ext2_read_ind_block(fs, ((const uint32_t *)(const void *)dind)[i2], 2, &ind) < 0)
        return 0;
    return ((const uint32_t *)(const void *)ind)[i3];
}

int ext2_read_file(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t offset,
                   uint8_t *buf, uint32_t count)
{
    if (!fs || !fs->mounted || !inode || !buf) return -1;
    if (offset >= inode->i_size) return 0;

    uint32_t remaining = inode->i_size - offset;
    if (count > remaining) count = remaining;

    uint32_t done = 0;
    uint8_t block_buf[4096] __attribute__((aligned(2)));

    while (done < count) {
        uint32_t abs = offset + done;
        uint32_t block_idx = abs / fs->block_size;
        uint32_t block_off = abs % fs->block_size;
        uint32_t chunk = fs->block_size - block_off;
        if (chunk > count - done) chunk = count - done;

        uint32_t phys_block = ext2_inode_block(fs, inode, block_idx);
        if (!phys_block) {
            memset(buf + done, 0, chunk);
            done += chunk;
            continue;
        }

        if (ext2_read_block(fs, phys_block, block_buf) < 0) return (int)done;
        memcpy(buf + done, block_buf + block_off, chunk);
        done += chunk;
    }
    return (int)done;
}

int ext2_readdir(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t index,
                 char *name, uint32_t *type)
{
    if (!fs || !fs->mounted || !inode || !name || !type) return -1;
    if ((inode->i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    uint8_t block_buf[4096] __attribute__((aligned(2)));
    if (fs->block_size > sizeof(block_buf)) return -1;

    uint32_t nblocks = (inode->i_size + fs->block_size - 1) / fs->block_size;
    uint32_t entry_idx = 0;

    /* 按"逻辑块"遍历，而不是按 rec_len 跨块串行推进。
     * ext4 的 htree 目录里夹着 dx_root/dx_node 索引块，它们的假目录项
     * rec_len 会覆盖整块，按链式推进会被带偏（实测 600 个文件只列出 126 个）。
     * 这里对每块单独解析，索引块（inode==0 且 rec_len==block_size）直接跳过。 */
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys = ext2_inode_block(fs, inode, b);
        if (!phys) continue;
        if (ext2_read_block(fs, phys, block_buf) < 0) return -1;

        ext2_dir_entry_t *first = (ext2_dir_entry_t *)block_buf;
        if (first->inode == 0 && first->rec_len == fs->block_size)
            continue;   /* dx_root / dx_node 索引块 */

        uint32_t off = 0;
        while (off + sizeof(ext2_dir_entry_t) <= fs->block_size) {
            ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(block_buf + off);
            if (entry->rec_len < 8 || off + entry->rec_len > fs->block_size) break;
            uint32_t entry_nlen = ext2_dirent_name_len(fs, entry);
            if (entry->inode != 0 && entry_nlen > 0) {
                if (entry_idx == index) {
                    uint32_t nlen = entry_nlen;
                    if (nlen > 31) nlen = 31;
                    memcpy(name, entry->name, nlen);
                    name[nlen] = '\0';
                    *type = (entry->file_type == 2) ? 1 : 0;
                    return 0;
                }
                entry_idx++;
            }
            off += entry->rec_len;
        }
    }
    return -1;
}

int ext2_iter_dir(ext2_fs_t *fs, ext2_inode_t *dir, ext2_dir_cb cb, void *ctx) {
    if (!fs || !fs->mounted || !dir || !cb) return -1;
    if ((dir->i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    uint8_t block_buf[4096] __attribute__((aligned(2)));
    if (fs->block_size > sizeof(block_buf)) return -1;

    uint32_t nblocks = (dir->i_size + fs->block_size - 1) / fs->block_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys = ext2_inode_block(fs, dir, b);
        if (!phys) continue;
        if (ext2_read_block(fs, phys, block_buf) < 0) return -1;

        ext2_dir_entry_t *first = (ext2_dir_entry_t *)block_buf;
        if (first->inode == 0 && first->rec_len == fs->block_size)
            continue;   /* htree 索引块 */

        uint32_t off = 0;
        while (off + sizeof(ext2_dir_entry_t) <= fs->block_size) {
            ext2_dir_entry_t *e = (ext2_dir_entry_t *)(block_buf + off);
            if (e->rec_len < 8 || off + e->rec_len > fs->block_size) break;
            uint32_t enlen = ext2_dirent_name_len(fs, e);
            if (e->inode && enlen) {
                char nm[64];
                uint32_t n = enlen > sizeof(nm) - 1 ? sizeof(nm) - 1 : enlen;
                memcpy(nm, e->name, n);
                nm[n] = '\0';
                if (cb(ctx, nm, e->file_type, e->inode)) return 0;
            }
            off += e->rec_len;
        }
    }
    return 0;
}

int ext2_lookup(ext2_fs_t *fs, ext2_inode_t *dir_inode,
                const char *name, uint32_t *out_inode)
{
    if (!fs || !fs->mounted || !dir_inode || !name || !out_inode) return -1;
    if ((dir_inode->i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    uint8_t block_buf[4096] __attribute__((aligned(2)));
    uint32_t offset = 0;
    uint32_t name_len = (uint32_t)strlen(name);

    while (offset < dir_inode->i_size) {
        uint32_t block_idx = offset / fs->block_size;
        uint32_t block_off = offset % fs->block_size;

        uint32_t phys_block = ext2_inode_block(fs, dir_inode, block_idx);
        if (!phys_block) return -1;

        if (ext2_read_block(fs, phys_block, block_buf) < 0) return -1;

        while (block_off < fs->block_size && offset < dir_inode->i_size) {
            ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(block_buf + block_off);
            if (entry->rec_len == 0) return -1;

            if (entry->inode != 0 &&
                ext2_dirent_name_len(fs, entry) == name_len &&
                memcmp(entry->name, name, name_len) == 0) {
                *out_inode = entry->inode;
                return 0;
            }
            offset += entry->rec_len;
            block_off += entry->rec_len;
        }
    }
    return -1;
}

int ext2_path_to_inode(ext2_fs_t *fs, const char *path, uint32_t *out_inode)
{
    if (!fs || !fs->mounted || !path || !out_inode) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, EXT2_ROOT_INO, &inode) < 0) return -1;

    if (path[0] == '/' && path[1] == '\0') {
        *out_inode = EXT2_ROOT_INO;
        return 0;
    }

    char component[64];
    const char *p = path;
    while (*p == '/') p++;

    while (*p) {
        uint32_t i = 0;
        while (*p && *p != '/' && i < 63) component[i++] = *p++;
        component[i] = '\0';
        while (*p == '/') p++;

        if (component[0] == '\0') break;

        if (ext2_lookup(fs, &inode, component, out_inode) < 0) return -1;

        if (*p) {
            if (ext2_read_inode(fs, *out_inode, &inode) < 0) return -1;
        }
    }
    return 0;
}
/* ── Write helpers ──────────────────────────────────────────── */

/**
 * @brief 写回一个 ext2 块（与 ext2_read_block 对称的合并写）。
 *
 * 同 ext2_read_block：整块一次下发，AHCI 上合成单条 DMA 写命令。
 *
 * @param fs    已挂载的文件系统实例
 * @param block 文件系统内块号（0 是合法块号，不作无效判断）
 * @param buf   待写入数据，长度 fs->block_size
 * @return 0 成功，-1 参数非法或底层写失败
 */
int ext2_write_block(ext2_fs_t *fs, uint32_t block, const uint8_t *buf)
{
    if (fs && fs->read_only) return -1;   /* ext4 布局：拒绝一切写入 */
    if (!fs || !fs->mounted || !buf || !block) return -1;
    for (int s = 0; s < EXT2_IND_SLOTS; s++) {
        if (g_ind_slot_block[s] == block)
            g_ind_slot_block[s] = 0;
    }
    uint32_t spb = fs->block_size / BLOCK_SECTOR_SIZE;
    uint32_t start = block * spb;
    return block_write_sectors(fs->partition_lba + start, buf, spb) == 0 ? 0 : -1;
}

static int ext2_write_bgd(ext2_fs_t *fs, uint32_t group, const ext2_bgd_t *bgd)
{
    uint32_t bgd_block = fs->sb.s_first_data_block + 1;
    uint32_t bgd_offset = group * (fs->desc_size ? fs->desc_size : sizeof(ext2_bgd_t));
    uint32_t block_offset = bgd_offset / fs->block_size;
    uint32_t byte_offset  = bgd_offset % fs->block_size;
    uint8_t block_buf[4096] __attribute__((aligned(2)));
    if (fs->block_size > 4096) return -1;
    if (ext2_read_block(fs, bgd_block + block_offset, block_buf) < 0) return -1;
    memcpy(block_buf + byte_offset, bgd, sizeof(ext2_bgd_t));
    return ext2_write_block(fs, bgd_block + block_offset, block_buf);
}

static int ext2_write_superblock(ext2_fs_t *fs)
{
    if (!fs || fs->read_only) return -1;
    /* 读出原始 1024 字节超级块，只覆盖我方结构体覆盖的头部 256 字节。
     * 早先直接写一个 512 字节栈缓冲，其中 256 字节之后是未初始化垃圾，
     * 会把 s_default_mount_opts / s_mkfs_time 等字段写坏 —— e2fsck 直接
     * 判"超级块损坏"（实测）。 */
    uint8_t sb_raw[EXT2_SUPERBLOCK_SIZE] __attribute__((aligned(2)));
    uint32_t sb_lba = EXT2_SUPERBLOCK_OFFSET / BLOCK_SECTOR_SIZE;
    uint32_t sectors = EXT2_SUPERBLOCK_SIZE / BLOCK_SECTOR_SIZE;
    if (block_read_sectors(fs->partition_lba + sb_lba, sb_raw, sectors) != 0)
        return -1;
    uint32_t copy_sz = sizeof(ext2_superblock_t);
    if (copy_sz > EXT2_SUPERBLOCK_SIZE) copy_sz = EXT2_SUPERBLOCK_SIZE;
    memcpy(sb_raw, &fs->sb, copy_sz);
    return block_write_sectors(fs->partition_lba + sb_lba, sb_raw, sectors) == 0 ? 0 : -1;
}

int ext2_write_inode(ext2_fs_t *fs, uint32_t inode_num, const ext2_inode_t *inode)
{
    if (!fs || !fs->mounted || !inode || inode_num == 0) return -EINVAL;
    if (fs->read_only) return -EROFS;
    uint32_t group = (inode_num - 1) / fs->sb.s_inodes_per_group;
    uint32_t index = (inode_num - 1) % fs->sb.s_inodes_per_group;
    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, group, &bgd) < 0) return -1;
    uint32_t inode_offset = index * fs->inode_size;
    uint32_t block_off = inode_offset / fs->block_size;
    uint32_t byte_off  = inode_offset % fs->block_size;
    uint8_t block_buf[4096] __attribute__((aligned(2)));
    if (fs->block_size > 4096) return -1;
    if (ext2_read_block(fs, bgd.bg_inode_table + block_off, block_buf) < 0) return -1;
    memcpy(block_buf + byte_off, inode, sizeof(ext2_inode_t));
    return ext2_write_block(fs, bgd.bg_inode_table + block_off, block_buf);
}

/* ── Block / inode allocator ────────────────────────────────── */

/**
 * @brief 在位图中查找第一个为 0（空闲）的位，按 64 位字批量跳过已占满区间。
 *
 * 分配器的实际负载：一个「几乎写满」的组里，逐位扫描要做 blocks_per_group
 * （常见 8192）次移位判断才能确认该组已满。整字比较后，同样只需 128 次
 * 比较，命中时再用 ctz 直接算出位号——语义与逐位扫描完全一致（都返回最小
 * 空闲位），只是常数小两个数量级。
 *
 * 用 memcpy 取值而非强转 uint64_t*：位图缓冲位于栈上、只保证 8 字节以下
 * 对齐，直接解引用会违反对齐要求。
 *
 * @param bm    位图缓冲区
 * @param limit 有效位数（组内块数 / 组内 inode 数，可能不是 64 的整数倍）
 * @param out   输出：第一个空闲位的位号
 * @return 1 找到，0 该组已满
 */
static int bitmap_first_free(const uint8_t *bm, uint32_t limit, uint32_t *out)
{
    uint32_t i = 0;
    for (; i + 64u <= limit; i += 64u) {
        uint64_t w;
        memcpy(&w, bm + (i >> 3), sizeof(w));
        if (w == ~0ULL) continue;                      /* 64 个全占用：整字跳过 */
        *out = i + (uint32_t)__builtin_ctzll(~w);      /* 最低 0 位即空闲位 */
        return 1;
    }
    for (; i < limit; i++) {                           /* 尾部不足 64 位 */
        if (!(bm[i >> 3] & (1u << (i & 7u)))) { *out = i; return 1; }
    }
    return 0;
}

/**
 * @brief 分配一个数据块：置位块位图、递减组/超级块空闲计数并落盘。
 *
 * 扫描顺序固定从组 0 开始（结果确定，便于测试与 fsck 互认）；组级
 * bg_free_blocks_count 为 0 的组在读位图前就被排除，因此满盘的代价只有
 * 每组一次 BGD 读，而非整卷位图扫描。
 *
 * @param fs 已挂载实例
 * @return 分配到的物理块号；0 表示磁盘已满或写回失败（0 是保留无效块号）
 */
uint32_t ext2_alloc_block(ext2_fs_t *fs)
{
    if (!fs || !fs->mounted || fs->read_only) return 0;
    if (fs->sb.s_free_blocks_count == 0) return 0;
    uint8_t bitmap[EXT2_BITMAP_BUF_BYTES] __attribute__((aligned(2)));
    for (uint32_t g = 0; g < fs->block_group_count; g++) {
        ext2_bgd_t bgd;
        if (ext2_read_bgd(fs, g, &bgd) < 0) continue;
        if (bgd.bg_free_blocks_count == 0) continue;   /* 组满：免读位图 */
        uint32_t bpg = fs->sb.s_blocks_per_group;
        /* 卷参数来自磁盘镜像，可能损坏或被恶意构造：位图必须装得进栈缓冲，
         * 否则宁可视为无效组，也不能越界读写栈。 */
        if (bpg == 0 || (bpg + 7u) / 8u > sizeof(bitmap)) continue;
        if (ext2_read_block(fs, bgd.bg_block_bitmap, bitmap) < 0) continue;
        uint32_t i;
        if (!bitmap_first_free(bitmap, bpg, &i)) continue;
        bitmap[i >> 3] |= (uint8_t)(1u << (i & 7u));
        if (ext2_write_block(fs, bgd.bg_block_bitmap, bitmap) < 0) return 0;
        /* 计数来自磁盘，可能已经被写坏：先夹住再减，避免无符号下溢成
         * 约 42 亿，让 e2fsck 看到"空闲块比卷还大"。 */
        if (bgd.bg_free_blocks_count) bgd.bg_free_blocks_count--;
        ext2_write_bgd(fs, g, &bgd);
        if (fs->sb.s_free_blocks_count) fs->sb.s_free_blocks_count--;
        ext2_write_superblock(fs);
        return g * bpg + i + fs->sb.s_first_data_block;
    }
    return 0;
}

/**
 * @brief 释放一个数据块：清位图位并回写组描述符与超级块的空闲计数。
 * @param fs    已挂载实例
 * @param block 待释放的物理块号（0 是保留无效块号，直接拒绝）
 * @return 0 成功，-1 参数非法、越界或底层读写失败
 */
int ext2_free_block(ext2_fs_t *fs, uint32_t block)
{
    if (!fs || !fs->mounted || !block) return -EINVAL;
    if (fs->read_only) return -EROFS;
    /* 卷内块号上界：fast symlink 的 i_block 里存的是目标路径文本，误当块号
     * 释放会把别的文件的块标成空闲（实测：'a.txt' 的第 5 字节 't'=0x74 →
     * 块 116，破坏 lost+found）。越界一律拒绝。 */
    if (block >= fs->sb.s_blocks_count) return -EINVAL;
    uint32_t bpg = fs->sb.s_blocks_per_group;
    if (bpg == 0) return -1;
    if ((bpg + 7u) / 8u > EXT2_BITMAP_BUF_BYTES) return -1;
    uint32_t adj = block - fs->sb.s_first_data_block;
    uint32_t g = adj / bpg, i = adj % bpg;
    /* 越界块号会算出超范围的组号，先拦下来再碰组描述符表 */
    if (g >= fs->block_group_count) return -1;
    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, g, &bgd) < 0) return -1;
    uint8_t bitmap[EXT2_BITMAP_BUF_BYTES] __attribute__((aligned(2)));
    if (ext2_read_block(fs, bgd.bg_block_bitmap, bitmap) < 0) return -1;
    bitmap[i / 8] &= ~(1u << (i % 8));
    if (ext2_write_block(fs, bgd.bg_block_bitmap, bitmap) < 0) return -1;
    bgd.bg_free_blocks_count++;
    ext2_write_bgd(fs, g, &bgd);
    fs->sb.s_free_blocks_count++;
    return ext2_write_superblock(fs);
}

/**
 * @brief 分配一个 inode（与 ext2_alloc_block 同构，扫 inode 位图）。
 *
 * inode 号从 1 起算且 1/2 号保留（坏块表/根目录），因此返回值恒 ≥ 3：
 * 组号 0 且位号 0、1 对应 inode 1、2，ext2 约定它们恒为占用位，正常卷上
 * 不会命中；这里保持与旧实现完全一致的取值公式 g * ipg + i + 1。
 *
 * @param fs 已挂载实例
 * @return 新 inode 号；0 表示 inode 表已满或写回失败
 */
uint32_t ext2_alloc_inode(ext2_fs_t *fs)
{
    if (!fs || !fs->mounted || fs->read_only) return 0;
    if (fs->sb.s_free_inodes_count == 0) return 0;
    uint8_t bitmap[EXT2_BITMAP_BUF_BYTES] __attribute__((aligned(2)));
    for (uint32_t g = 0; g < fs->block_group_count; g++) {
        ext2_bgd_t bgd;
        if (ext2_read_bgd(fs, g, &bgd) < 0) continue;
        if (bgd.bg_free_inodes_count == 0) continue;   /* 组满：免读位图 */
        uint32_t ipg = fs->sb.s_inodes_per_group;
        if (ipg == 0 || (ipg + 7u) / 8u > sizeof(bitmap)) continue;
        if (ext2_read_block(fs, bgd.bg_inode_bitmap, bitmap) < 0) continue;
        uint32_t i;
        if (!bitmap_first_free(bitmap, ipg, &i)) continue;
        bitmap[i >> 3] |= (uint8_t)(1u << (i & 7u));
        if (ext2_write_block(fs, bgd.bg_inode_bitmap, bitmap) < 0) return 0;
        if (bgd.bg_free_inodes_count) bgd.bg_free_inodes_count--;
        ext2_write_bgd(fs, g, &bgd);
        if (fs->sb.s_free_inodes_count) fs->sb.s_free_inodes_count--;
        ext2_write_superblock(fs);
        return g * ipg + i + 1;
    }
    return 0;
}

/**
 * @brief 释放一个 inode：清 inode 位图对应位并回写计数。
 *
 * s_inodes_per_group 来自磁盘超级块；若镜像损坏使其为 0，下面的除法会触发
 * #DE 除零异常直接打死内核，故必须先校验（块分配路径同样校验）。
 *
 * @param fs        已挂载实例
 * @param inode_num 待释放 inode 号（0 无效）
 * @return 0 成功，-1 参数非法或底层读写失败
 */
int ext2_free_inode(ext2_fs_t *fs, uint32_t inode_num)
{
    if (!fs || !fs->mounted || inode_num == 0) return -EINVAL;
    if (fs->read_only) return -EROFS;
    uint32_t ipg = fs->sb.s_inodes_per_group;
    if (ipg == 0 || (ipg + 7u) / 8u > EXT2_BITMAP_BUF_BYTES) return -1;
    uint32_t g = (inode_num - 1) / ipg, i = (inode_num - 1) % ipg;
    if (g >= fs->block_group_count) return -1;         /* 越界 inode 号 */
    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, g, &bgd) < 0) return -1;
    uint8_t bitmap[EXT2_BITMAP_BUF_BYTES] __attribute__((aligned(2)));
    if (ext2_read_block(fs, bgd.bg_inode_bitmap, bitmap) < 0) return -1;
    bitmap[i >> 3] &= (uint8_t)~(1u << (i & 7u));
    if (ext2_write_block(fs, bgd.bg_inode_bitmap, bitmap) < 0) return -1;
    if (bgd.bg_free_inodes_count < 0xFFFFu) bgd.bg_free_inodes_count++;
    ext2_write_bgd(fs, g, &bgd);
    if (fs->sb.s_free_inodes_count < fs->sb.s_inodes_count) fs->sb.s_free_inodes_count++;
    return ext2_write_superblock(fs);
}

/* ── Inode block map write ──────────────────────────────────── */

/*
 * 写路径的共享缓冲（BSS，不是栈）
 * ═══════════════════════════════
 * 内核任务栈只有 8 KiB，而 ext2 的块缓冲天然是 1~4 KiB。若在
 * ext2_write_file() 里放一个栈缓冲、它调用的 ext2_inode_set_block() 再放一个，
 * 单条写路径就吃掉 8 KiB —— 比任何逻辑 bug 都更早把内核打死。因此这里按用途
 * 分工固定 5 个静态槽：数据块 I/O、块映射、间接块回收（3 级各一）。
 *
 * 并发前提：内核是单核协作式调度，这些函数内部没有任何让出点（无
 * task_yield / 阻塞 I/O 等待），所以静态缓冲不会被另一个任务中途改写 ——
 * 与 g_ind_slot 那套缓存的假设完全相同。
 */
static uint8_t g_io_buf[4096]   __attribute__((aligned(8)));
static uint8_t g_map_buf[4096]  __attribute__((aligned(8)));
static uint8_t g_free_buf[3][4096] __attribute__((aligned(8)));

/** 目录项 file_type → 目录项类型字节（无 FILETYPE 特性时该字段另作他用）。 */
static void ext2_dirent_set_name(ext2_fs_t *fs, ext2_dir_entry_t *e,
                                 const char *name, uint32_t name_len,
                                 uint8_t file_type) {
    if (fs->has_filetype) {
        e->name_len = (uint8_t)name_len;
        e->file_type = file_type;
    } else {
        /* 老 ext2（无 INCOMPAT_FILETYPE）：name_len 是 16 位，第 7 字节是高位 */
        e->name_len = (uint8_t)(name_len & 0xFFu);
        e->file_type = (uint8_t)(name_len >> 8);
    }
    memcpy(e->name, name, name_len);
}

/** 由 inode 的 mode 推出目录项 file_type。 */
static uint8_t ext2_ftype_from_mode(uint16_t mode) {
    switch (mode & EXT2_S_IFMT) {
    case EXT2_S_IFDIR: return EXT2_FT_DIR;
    case EXT2_S_IFLNK: return EXT2_FT_SYMLINK;
    default:           return EXT2_FT_REG_FILE;
    }
}

/**
 * @brief 把逻辑块 idx 指向物理块 phys；必要时分配一级/二级/三级间接块。
 *
 * @param meta_out 输出：本调用顺带分配的**元数据块**数（间接块本身）。
 *                 调用方必须把它计入 i_blocks —— ext2 的 i_blocks 含间接块，
 *                 少算会让 e2fsck 报 "i_blocks is N, should be M"。
 * @return 0 成功；-ENOSPC 无空闲块；-EFBIG 逻辑块号超出 ext2 寻址范围；
 *         -EIO 底层读写失败。失败时 inode 内容可能已被部分修改（调用方按
 *         失败处理即可，不要继续写）。
 */
static int ext2_inode_set_block(ext2_fs_t *fs, ext2_inode_t *inode,
                                uint32_t block_idx, uint32_t phys,
                                uint32_t *meta_out)
{
    uint32_t apb = fs->block_size / 4;

    if (block_idx < EXT2_NDIR_BLOCKS) {
        inode->i_block[block_idx] = phys;
        return 0;
    }
    block_idx -= EXT2_NDIR_BLOCKS;

    if (block_idx < apb) {
        if (!inode->i_block[EXT2_IND_BLOCK]) {
            uint32_t nb = ext2_alloc_block(fs);
            if (!nb) return -ENOSPC;
            memset(g_map_buf, 0, fs->block_size);
            if (ext2_write_block(fs, nb, g_map_buf) < 0) {
                (void)ext2_free_block(fs, nb);
                return -EIO;
            }
            inode->i_block[EXT2_IND_BLOCK] = nb;
            if (meta_out) (*meta_out)++;
        }
        if (ext2_read_block(fs, inode->i_block[EXT2_IND_BLOCK], g_map_buf) < 0)
            return -EIO;
        ((uint32_t *)(void *)g_map_buf)[block_idx] = phys;
        return ext2_write_block(fs, inode->i_block[EXT2_IND_BLOCK], g_map_buf) < 0
                   ? -EIO : 0;
    }
    block_idx -= apb;

    if (block_idx < apb * apb) {
        if (!inode->i_block[EXT2_DIND_BLOCK]) {
            uint32_t nb = ext2_alloc_block(fs);
            if (!nb) return -ENOSPC;
            memset(g_map_buf, 0, fs->block_size);
            if (ext2_write_block(fs, nb, g_map_buf) < 0) {
                (void)ext2_free_block(fs, nb);
                return -EIO;
            }
            inode->i_block[EXT2_DIND_BLOCK] = nb;
            if (meta_out) (*meta_out)++;
        }
        uint32_t l1 = block_idx / apb;
        uint32_t l2 = block_idx % apb;
        if (ext2_read_block(fs, inode->i_block[EXT2_DIND_BLOCK], g_map_buf) < 0)
            return -EIO;
        uint32_t ind2 = ((uint32_t *)(void *)g_map_buf)[l1];
        if (!ind2) {
            ind2 = ext2_alloc_block(fs);
            if (!ind2) return -ENOSPC;
            ((uint32_t *)(void *)g_map_buf)[l1] = ind2;
            if (ext2_write_block(fs, inode->i_block[EXT2_DIND_BLOCK], g_map_buf) < 0)
                return -EIO;
            memset(g_map_buf, 0, fs->block_size);
            if (ext2_write_block(fs, ind2, g_map_buf) < 0) return -EIO;
            if (meta_out) (*meta_out)++;
        }
        if (ext2_read_block(fs, ind2, g_map_buf) < 0) return -EIO;
        ((uint32_t *)(void *)g_map_buf)[l2] = phys;
        return ext2_write_block(fs, ind2, g_map_buf) < 0 ? -EIO : 0;
    }
    block_idx -= apb * apb;

    if (block_idx >= apb * apb * apb) return -EFBIG;

    /* 三级间接：ext2 上限 4 TiB（4 KiB 块），实际很少走到 */
    if (!inode->i_block[EXT2_TIND_BLOCK]) {
        uint32_t nb = ext2_alloc_block(fs);
        if (!nb) return -ENOSPC;
        memset(g_map_buf, 0, fs->block_size);
        if (ext2_write_block(fs, nb, g_map_buf) < 0) {
            (void)ext2_free_block(fs, nb);
            return -EIO;
        }
        inode->i_block[EXT2_TIND_BLOCK] = nb;
        if (meta_out) (*meta_out)++;
    }
    uint32_t i1 = block_idx / (apb * apb);
    block_idx %= apb * apb;
    uint32_t i2 = block_idx / apb;
    uint32_t i3 = block_idx % apb;
    if (ext2_read_block(fs, inode->i_block[EXT2_TIND_BLOCK], g_map_buf) < 0)
        return -EIO;
    uint32_t dind = ((uint32_t *)(void *)g_map_buf)[i1];
    if (!dind) {
        dind = ext2_alloc_block(fs);
        if (!dind) return -ENOSPC;
        ((uint32_t *)(void *)g_map_buf)[i1] = dind;
        if (ext2_write_block(fs, inode->i_block[EXT2_TIND_BLOCK], g_map_buf) < 0)
            return -EIO;
        memset(g_map_buf, 0, fs->block_size);
        if (ext2_write_block(fs, dind, g_map_buf) < 0) return -EIO;
        if (meta_out) (*meta_out)++;
    }
    if (ext2_read_block(fs, dind, g_map_buf) < 0) return -EIO;
    uint32_t ind = ((uint32_t *)(void *)g_map_buf)[i2];
    if (!ind) {
        ind = ext2_alloc_block(fs);
        if (!ind) return -ENOSPC;
        ((uint32_t *)(void *)g_map_buf)[i2] = ind;
        if (ext2_write_block(fs, dind, g_map_buf) < 0) return -EIO;
        memset(g_map_buf, 0, fs->block_size);
        if (ext2_write_block(fs, ind, g_map_buf) < 0) return -EIO;
        if (meta_out) (*meta_out)++;
    }
    if (ext2_read_block(fs, ind, g_map_buf) < 0) return -EIO;
    ((uint32_t *)(void *)g_map_buf)[i3] = phys;
    return ext2_write_block(fs, ind, g_map_buf) < 0 ? -EIO : 0;
}

/**
 * @brief 递归回收一棵间接块子树里的全部数据块，最后归还间接块本身。
 *
 * @param blk   指向父节点（或 inode）里的块号字段；回收成功后写回 0
 * @param level 1=单级 2=双级 3=三级
 * @return 回收到的块数（数据块 + 元数据块）
 */
static uint32_t ext2_release_ind(ext2_fs_t *fs, uint32_t blk, int level)
{
    if (!blk || level < 1 || level > 3) return 0;
    uint8_t *buf = g_free_buf[level - 1];
    if (fs->block_size > sizeof(g_free_buf[0])) return 0;
    if (ext2_read_block(fs, blk, buf) < 0) return 0;

    uint32_t apb = fs->block_size / 4;
    uint32_t freed = 0;
    for (uint32_t i = 0; i < apb; i++) {
        uint32_t e = ((uint32_t *)(void *)buf)[i];
        if (!e) continue;
        if (level == 1) {
            if (ext2_free_block(fs, e) == 0) freed++;
        } else {
            freed += ext2_release_ind(fs, e, level - 1);
        }
    }
    if (ext2_free_block(fs, blk) == 0) freed++;
    return freed;
}

/** 释放 inode 引用的**全部**块（直接 12 + 三级间接），并清零块指针。 */
static uint32_t ext2_inode_release_all(ext2_fs_t *fs, ext2_inode_t *inode)
{
    uint32_t freed = 0;
    for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        if (!inode->i_block[i]) continue;
        if (ext2_free_block(fs, inode->i_block[i]) == 0) freed++;
        inode->i_block[i] = 0;
    }
    /* i_block 在 packed 结构里，不能对成员取地址（编译器按未对齐指针处理）；
     * 按值传进去，回来统一清零。 */
    freed += ext2_release_ind(fs, inode->i_block[EXT2_IND_BLOCK], 1);
    freed += ext2_release_ind(fs, inode->i_block[EXT2_DIND_BLOCK], 2);
    freed += ext2_release_ind(fs, inode->i_block[EXT2_TIND_BLOCK], 3);
    memset(inode->i_block, 0, sizeof(inode->i_block));
    inode->i_blocks = 0;
    return freed;
}

/**
 * @brief 回收已经全空的间接块（truncate 尾部清理用）。
 *
 * 与 ext2_release_ind 的分工：这个函数**不动数据块**（它们已经被逐块释放），
 * 只把因此变空的间接块还给文件系统。返回回收的元数据块数。
 *
 * @param blk   指向 inode 里的块号字段，回收后置 0
 * @param level 1=单级 2=双级 3=三级
 */
static uint32_t ext2_prune_ind(ext2_fs_t *fs, uint32_t blk, int level,
                               uint32_t *out_blk)
{
    if (out_blk) *out_blk = blk;
    if (!blk || level < 1 || level > 3) return 0;
    uint8_t *buf = g_free_buf[level - 1];
    if (fs->block_size > sizeof(g_free_buf[0])) return 0;
    if (ext2_read_block(fs, blk, buf) < 0) return 0;

    uint32_t apb = fs->block_size / 4;
    uint32_t freed = 0;
    int any = 0, dirty = 0;
    for (uint32_t i = 0; i < apb; i++) {
        uint32_t e = ((uint32_t *)(void *)buf)[i];
        if (!e) continue;
        if (level > 1) {
            uint32_t sub = 0;
            freed += ext2_prune_ind(fs, e, level - 1, &sub);
            if (sub != e) { ((uint32_t *)(void *)buf)[i] = sub; dirty = 1; }
            if (sub) any = 1;
        } else {
            any = 1;
        }
    }
    if (dirty && any) (void)ext2_write_block(fs, blk, buf);
    if (!any) {
        if (ext2_free_block(fs, blk) == 0) {
            if (out_blk) *out_blk = 0;
            freed++;
        }
    }
    return freed;
}

/* ── File write ─────────────────────────────────────────────── */

int ext2_write_file(ext2_fs_t *fs, uint32_t inode_num,
                    uint32_t offset, const uint8_t *buf, uint32_t count)
{
    int err = ext2_writable(fs);
    if (err) return err;
    if (!buf || !count) return -EINVAL;
    if (count > 0xFFFFFFFFu - offset) return -EFBIG;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, inode_num, &inode) < 0) return -EIO;
    if ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return -EISDIR;
    if (inode.i_flags & EXT2_EXTENTS_FL) return -EROFS;  /* extent inode 只读 */

    uint32_t done = 0;
    while (done < count) {
        uint32_t abs = offset + done;
        uint32_t bidx = abs / fs->block_size;
        uint32_t boff = abs % fs->block_size;
        uint32_t chunk = fs->block_size - boff;
        if (chunk > count - done) chunk = count - done;

        uint32_t phys = ext2_inode_block(fs, &inode, bidx);
        if (!phys) {
            phys = ext2_alloc_block(fs);
            if (!phys) return done ? (int)done : -ENOSPC;
            uint32_t meta = 0;
            int r = ext2_inode_set_block(fs, &inode, bidx, phys, &meta);
            if (r < 0) {
                (void)ext2_free_block(fs, phys);
                return done ? (int)done : r;
            }
            inode.i_blocks += (1u + meta) * (fs->block_size / 512);
            memset(g_io_buf, 0, fs->block_size);
        } else if (ext2_read_block(fs, phys, g_io_buf) < 0) {
            return done ? (int)done : -EIO;
        }
        memcpy(g_io_buf + boff, buf + done, chunk);
        if (ext2_write_block(fs, phys, g_io_buf) < 0)
            return done ? (int)done : -EIO;
        done += chunk;
    }

    if (offset + done > inode.i_size) inode.i_size = offset + done;
    inode.i_mtime = ext2_now();
    inode.i_ctime = inode.i_mtime;
    if (ext2_write_inode(fs, inode_num, &inode) < 0) return done ? (int)done : -EIO;
    return (int)done;
}

/* ── Directory entry helpers ────────────────────────────────── */

/**
 * @brief 往目录里加一个名字（必要时扩展目录块）。
 *
 * @param dir 父目录 inode（会被就地更新 i_size/i_blocks/时间戳并落盘）
 * @return 0 成功；-ENOSPC 目录块分配失败；-EIO 读写失败；-ENAMETOOLONG 名字太长
 *
 * 不变量：返回非 0 时目录内容未被修改。
 */
static int ext2_dir_add(ext2_fs_t *fs, uint32_t dir_ino, ext2_inode_t *dir,
                        const char *name, uint32_t name_len,
                        uint32_t ino, uint8_t file_type)
{
    uint32_t needed = (uint32_t)(sizeof(ext2_dir_entry_t) + name_len + 3u) & ~3u;
    if (needed > fs->block_size) return -EINVAL;   /* 没有 ENAMETOOLONG：名字长度按 EINVAL 拒绝 */

    uint32_t nblocks = (dir->i_size + fs->block_size - 1) / fs->block_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys = ext2_inode_block(fs, dir, b);
        if (!phys) continue;
        if (ext2_read_block(fs, phys, g_io_buf) < 0) return -EIO;

        uint32_t off = 0;
        while (off + sizeof(ext2_dir_entry_t) <= fs->block_size) {
            ext2_dir_entry_t *e = (ext2_dir_entry_t *)(void *)(g_io_buf + off);
            if (e->rec_len < sizeof(ext2_dir_entry_t) ||
                off + e->rec_len > fs->block_size)
                break;
            uint32_t nlen = ext2_dirent_name_len(fs, e);
            uint32_t real_len = (uint32_t)(sizeof(ext2_dir_entry_t) + nlen + 3u) & ~3u;
            uint32_t slack = e->rec_len - real_len;
            uint16_t old_rec = e->rec_len;
            if (e->inode == 0) { slack = e->rec_len; real_len = 0; }
            if (slack >= needed) {
                uint32_t entry_off = off;
                if (real_len) {
                    e->rec_len = (uint16_t)real_len;
                    entry_off += real_len;
                }
                ext2_dir_entry_t *ne =
                    (ext2_dir_entry_t *)(void *)(g_io_buf + entry_off);
                memset(ne, 0, sizeof(ext2_dir_entry_t));  /* 清掉旧名字残留 */
                ne->inode = ino;
                ne->rec_len = (uint16_t)(real_len ? slack : old_rec);
                ext2_dirent_set_name(fs, ne, name, name_len, file_type);
                if (ext2_write_block(fs, phys, g_io_buf) < 0) return -EIO;
                ext2_touch_dir(fs, dir_ino, dir);
                return 0;
            }
            off += e->rec_len;
        }
    }

    /* 现有块都塞不下：追加一个新目录块 */
    if (dir->i_size % fs->block_size) return -EIO;   /* 目录尺寸必须块对齐 */
    uint32_t new_blk = ext2_alloc_block(fs);
    if (!new_blk) return -ENOSPC;
    memset(g_io_buf, 0, fs->block_size);
    ext2_dir_entry_t *e = (ext2_dir_entry_t *)(void *)g_io_buf;
    e->inode = ino;
    e->rec_len = (uint16_t)fs->block_size;
    ext2_dirent_set_name(fs, e, name, name_len, file_type);
    if (ext2_write_block(fs, new_blk, g_io_buf) < 0) {
        (void)ext2_free_block(fs, new_blk);
        return -EIO;
    }
    uint32_t bidx = dir->i_size / fs->block_size;
    uint32_t meta = 0;
    int r = ext2_inode_set_block(fs, dir, bidx, new_blk, &meta);
    if (r < 0) {
        (void)ext2_free_block(fs, new_blk);
        return r;
    }
    dir->i_size += fs->block_size;
    dir->i_blocks += (1u + meta) * (fs->block_size / 512);
    ext2_touch_dir(fs, dir_ino, dir);
    return 0;
}

/**
 * @brief 从目录里摘掉一个名字，返回它指向的 inode 号。
 *
 * 摘除后与**前一个**项合并 rec_len（POSIX 目录块的标准回收方式）：不合并
 * 会让目录在反复 create/unlink 后碎片化成"每项独占一块"，e2fsck 虽然接受
 * 但目录会无限膨胀。
 *
 * @return 0 成功；-ENOENT 名字不存在；-EIO 读写失败
 */
static int ext2_dir_remove(ext2_fs_t *fs, uint32_t dir_ino, ext2_inode_t *dir,
                           const char *name, uint32_t *out_ino)
{
    uint32_t name_len = (uint32_t)strlen(name);
    uint32_t nblocks = (dir->i_size + fs->block_size - 1) / fs->block_size;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys = ext2_inode_block(fs, dir, b);
        if (!phys) continue;
        if (ext2_read_block(fs, phys, g_io_buf) < 0) return -EIO;

        ext2_dir_entry_t *first = (ext2_dir_entry_t *)(void *)g_io_buf;
        if (first->inode == 0 && first->rec_len == fs->block_size)
            continue;   /* htree 索引块 */

        ext2_dir_entry_t *prev = 0;
        uint32_t off = 0;
        while (off + sizeof(ext2_dir_entry_t) <= fs->block_size) {
            ext2_dir_entry_t *e = (ext2_dir_entry_t *)(void *)(g_io_buf + off);
            if (e->rec_len < sizeof(ext2_dir_entry_t) ||
                off + e->rec_len > fs->block_size)
                break;
            uint32_t nlen = ext2_dirent_name_len(fs, e);
            if (e->inode != 0 && nlen == name_len &&
                memcmp(e->name, name, name_len) == 0) {
                if (out_ino) *out_ino = e->inode;
                if (prev) {
                    prev->rec_len = (uint16_t)(prev->rec_len + e->rec_len);
                } else {
                    /* 块首项：清 inode 即可，rec_len 保持覆盖整块剩余空间 */
                    e->inode = 0;
                }
                if (ext2_write_block(fs, phys, g_io_buf) < 0) return -EIO;
                ext2_touch_dir(fs, dir_ino, dir);
                return 0;
            }
            prev = e;
            off += e->rec_len;
        }
    }
    return -ENOENT;
}

/** 目录是否只有 "." 与 ".."（rmdir 的前置条件）。 */
static int ext2_dir_is_empty(ext2_fs_t *fs, ext2_inode_t *dir)
{
    uint32_t nblocks = (dir->i_size + fs->block_size - 1) / fs->block_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys = ext2_inode_block(fs, dir, b);
        if (!phys) continue;
        if (ext2_read_block(fs, phys, g_io_buf) < 0) return -EIO;
        uint32_t off = 0;
        while (off + sizeof(ext2_dir_entry_t) <= fs->block_size) {
            ext2_dir_entry_t *e = (ext2_dir_entry_t *)(void *)(g_io_buf + off);
            if (e->rec_len < sizeof(ext2_dir_entry_t) ||
                off + e->rec_len > fs->block_size)
                break;
            if (e->inode != 0) {
                uint32_t nlen = ext2_dirent_name_len(fs, e);
                int is_dot = (nlen == 1 && e->name[0] == '.');
                int is_dotdot = (nlen == 2 && e->name[0] == '.' && e->name[1] == '.');
                if (!is_dot && !is_dotdot) return 0;
            }
            off += e->rec_len;
        }
    }
    return 1;
}

/** 把目录里已有名字的 inode 号改指到 ino（rename 更新 ".." 用）。 */
static int ext2_dir_repoint(ext2_fs_t *fs, ext2_inode_t *dir, const char *name,
                            uint32_t ino)
{
    uint32_t name_len = (uint32_t)strlen(name);
    uint32_t nblocks = (dir->i_size + fs->block_size - 1) / fs->block_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys = ext2_inode_block(fs, dir, b);
        if (!phys) continue;
        if (ext2_read_block(fs, phys, g_io_buf) < 0) return -EIO;
        uint32_t off = 0;
        while (off + sizeof(ext2_dir_entry_t) <= fs->block_size) {
            ext2_dir_entry_t *e = (ext2_dir_entry_t *)(void *)(g_io_buf + off);
            if (e->rec_len < sizeof(ext2_dir_entry_t) ||
                off + e->rec_len > fs->block_size)
                break;
            if (e->inode != 0 && ext2_dirent_name_len(fs, e) == name_len &&
                memcmp(e->name, name, name_len) == 0) {
                e->inode = ino;
                return ext2_write_block(fs, phys, g_io_buf) < 0 ? -EIO : 0;
            }
            off += e->rec_len;
        }
    }
    return -ENOENT;
}

/** 调整 inode 所属块组的 bg_used_dirs_count（mkdir/rmdir 维护）。 */
static int ext2_bgd_add_used_dirs(ext2_fs_t *fs, uint32_t inode_num, int delta)
{
    uint32_t ipg = fs->sb.s_inodes_per_group;
    if (!ipg) return -EINVAL;
    uint32_t g = (inode_num - 1) / ipg;
    if (g >= fs->block_group_count) return -EINVAL;
    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, g, &bgd) < 0) return -EIO;
    if (delta > 0) {
        if (bgd.bg_used_dirs_count < 0xFFFFu) bgd.bg_used_dirs_count++;
    } else {
        if (bgd.bg_used_dirs_count) bgd.bg_used_dirs_count--;
    }
    return ext2_write_bgd(fs, g, &bgd) < 0 ? -EIO : 0;
}

/** 把 inode 写成"已删除"状态：全零 + i_dtime（e2fsck 认这个组合）。 */
static int ext2_inode_wipe(ext2_fs_t *fs, uint32_t inode_num)
{
    ext2_inode_t zero;
    memset(&zero, 0, sizeof(zero));
    zero.i_dtime = ext2_now();
    return ext2_write_inode(fs, inode_num, &zero);
}

/* ── Directory create / unlink / rmdir / rename ─────────────── */

int ext2_create_file(ext2_fs_t *fs, uint32_t dir_ino, const char *name,
                     uint8_t file_type)
{
    int err = ext2_writable(fs);
    if (err) return err;
    if (!dir_ino || !name) return -EINVAL;
    uint32_t name_len = (uint32_t)strlen(name);
    if (name_len == 0 || name_len > 255) return -EINVAL;   /* 没有 ENAMETOOLONG：名字长度按 EINVAL 拒绝 */
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -EEXIST;

    ext2_inode_t dir;
    if (ext2_read_inode(fs, dir_ino, &dir) < 0) return -EIO;
    if ((dir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;

    uint32_t existing = 0;
    if (ext2_lookup(fs, &dir, name, &existing) == 0) return -EEXIST;

    uint32_t new_ino = ext2_alloc_inode(fs);
    if (!new_ino) return -ENOSPC;

    ext2_inode_t new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    /* file_type 与 ext2 目录项的 EXT2_FT_* 约定一致：
     *   1 = 普通文件, 2 = 目录, 7 = 符号链接 */
    if (file_type == EXT2_FT_DIR)
        new_inode.i_mode = EXT2_S_IFDIR | 0755u;
    else if (file_type == EXT2_FT_SYMLINK)
        new_inode.i_mode = EXT2_S_IFLNK | 0777u;
    else
        new_inode.i_mode = EXT2_S_IFREG | 0644u;
    new_inode.i_links_count = 1;
    ext2_touch(&new_inode);
    if (ext2_write_inode(fs, new_ino, &new_inode) < 0) {
        (void)ext2_free_inode(fs, new_ino);
        return -EIO;
    }

    int r = ext2_dir_add(fs, dir_ino, &dir, name, name_len, new_ino, file_type);
    if (r < 0) {
        /* 先把 inode 内容清零再还位图：位图空闲但 inode 里还留着 links/mode
         * 会被 e2fsck 判成"未分配 inode 有内容"。 */
        (void)ext2_inode_wipe(fs, new_ino);
        (void)ext2_free_inode(fs, new_ino);
        return r;
    }
    return (int)new_ino;
}

int ext2_unlink(ext2_fs_t *fs, uint32_t dir_ino, const char *name)
{
    int err = ext2_writable(fs);
    if (err) return err;
    if (!name) return -EINVAL;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -EINVAL;

    ext2_inode_t dir;
    if (ext2_read_inode(fs, dir_ino, &dir) < 0) return -EIO;
    if ((dir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;

    uint32_t victim_ino = 0;
    if (ext2_lookup(fs, &dir, name, &victim_ino) < 0) return -ENOENT;

    ext2_inode_t victim;
    if (ext2_read_inode(fs, victim_ino, &victim) < 0) return -EIO;
    if ((victim.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return -EISDIR;

    /* 先摘目录项：这样崩溃时最坏是"没人引用的 inode"（e2fsck 能收进
     * lost+found），而不是"目录项指向已释放 inode"（更严重的损坏）。 */
    int r = ext2_dir_remove(fs, dir_ino, &dir, name, 0);
    if (r < 0) return r;

    if (victim.i_links_count > 1) {
        victim.i_links_count--;
        victim.i_ctime = ext2_now();
        return ext2_write_inode(fs, victim_ino, &victim) < 0 ? -EIO : 0;
    }

    /* fast symlink（目标 ≤60 字节）把路径直接存在 i_block 里、i_blocks == 0，
     * 它没有任何数据块可回收；当成块指针去 free 会误伤别的文件。 */
    int fast_symlink = ((victim.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK) &&
                       victim.i_blocks == 0;
    if (!fast_symlink) (void)ext2_inode_release_all(fs, &victim);
    if (ext2_inode_wipe(fs, victim_ino) < 0) return -EIO;
    return ext2_free_inode(fs, victim_ino) < 0 ? -EIO : 0;
}

int ext2_truncate(ext2_fs_t *fs, uint32_t inode_num, uint32_t new_size)
{
    int err = ext2_writable(fs);
    if (err) return err;
    if (!inode_num) return -EINVAL;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, inode_num, &inode) < 0) return -EIO;
    if (new_size == inode.i_size) return 0;

    uint32_t block_size = fs->block_size ? fs->block_size : 1024;
    uint32_t per512 = block_size / 512;

    /* fast symlink 的 i_block 是路径文本，不是块指针：只允许改 i_size 以外的
     * 语义不做块回收（truncate 符号链接本来也不是合法操作）。 */
    if ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK && inode.i_blocks == 0) {
        inode.i_size = new_size;
        return ext2_write_inode(fs, inode_num, &inode) < 0 ? -EIO : 0;
    }

    if (new_size > inode.i_size) {
        /* 扩大：只改 i_size 造空洞，块在真正写时按需分配 */
        inode.i_size = new_size;
        inode.i_mtime = ext2_now();
        inode.i_ctime = inode.i_mtime;
        return ext2_write_inode(fs, inode_num, &inode) < 0 ? -EIO : 0;
    }

    /* 缩小：extent inode 的树回收还没实现（ext4 布局本来就是只读挂载，
     * 走不到这里；保留原有的"只改 i_size"降级行为）。 */
    if (inode.i_flags & EXT2_EXTENTS_FL) {
        inode.i_size = new_size;
        if (new_size == 0) inode.i_blocks = 0;
        return ext2_write_inode(fs, inode_num, &inode) < 0 ? -EIO : 0;
    }

    uint32_t freed = 0;
    if (new_size == 0) {
        freed += ext2_inode_release_all(fs, &inode);
    } else {
        /* 末块尾部清零：truncate 到 1200 再扩回 3000，1200..3000 必须读成 0。
         * ext2 不会自动擦除块内剩余字节，不显式清零就是旧数据泄漏。 */
        uint32_t tail_off = new_size % block_size;
        if (tail_off) {
            uint32_t last = (new_size - 1) / block_size;
            uint32_t phys_last = ext2_inode_block(fs, &inode, last);
            if (phys_last) {
                if (ext2_read_block(fs, phys_last, g_io_buf) == 0) {
                    memset(g_io_buf + tail_off, 0, block_size - tail_off);
                    (void)ext2_write_block(fs, phys_last, g_io_buf);
                }
            }
        }
        uint32_t keep = (new_size + block_size - 1) / block_size;
        uint32_t old_blocks = (inode.i_size + block_size - 1) / block_size;
        for (uint32_t idx = keep; idx < old_blocks; idx++) {
            uint32_t phys = ext2_inode_block(fs, &inode, idx);
            if (!phys) continue;
            if (ext2_free_block(fs, phys) == 0) freed++;
            if (ext2_inode_set_block(fs, &inode, idx, 0, 0) < 0) return -EIO;
        }
        uint32_t ind = inode.i_block[EXT2_IND_BLOCK];
        freed += ext2_prune_ind(fs, ind, 1, &ind);
        inode.i_block[EXT2_IND_BLOCK] = ind;
        uint32_t dind = inode.i_block[EXT2_DIND_BLOCK];
        freed += ext2_prune_ind(fs, dind, 2, &dind);
        inode.i_block[EXT2_DIND_BLOCK] = dind;
        uint32_t tind = inode.i_block[EXT2_TIND_BLOCK];
        freed += ext2_prune_ind(fs, tind, 3, &tind);
        inode.i_block[EXT2_TIND_BLOCK] = tind;
    }

    uint32_t drop = freed * per512;
    inode.i_blocks = inode.i_blocks > drop ? inode.i_blocks - drop : 0;
    inode.i_size = new_size;
    inode.i_mtime = ext2_now();
    inode.i_ctime = inode.i_mtime;
    return ext2_write_inode(fs, inode_num, &inode) < 0 ? -EIO : 0;
}

int ext2_mkdir(ext2_fs_t *fs, uint32_t parent_ino, const char *name)
{
    int err = ext2_writable(fs);
    if (err) return err;
    if (!parent_ino || !name) return -EINVAL;
    uint32_t name_len = (uint32_t)strlen(name);
    if (name_len == 0 || name_len > 255) return -EINVAL;   /* 没有 ENAMETOOLONG：名字长度按 EINVAL 拒绝 */
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -EEXIST;

    ext2_inode_t parent;
    if (ext2_read_inode(fs, parent_ino, &parent) < 0) return -EIO;
    if ((parent.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;
    uint32_t existing = 0;
    if (ext2_lookup(fs, &parent, name, &existing) == 0) return -EEXIST;

    /* 块和 inode 都先备好再落任何元数据：任一步失败都能完整回滚。 */
    uint32_t data_blk = ext2_alloc_block(fs);
    if (!data_blk) return -ENOSPC;
    uint32_t new_ino = ext2_alloc_inode(fs);
    if (!new_ino) {
        (void)ext2_free_block(fs, data_blk);
        return -ENOSPC;
    }

    memset(g_io_buf, 0, fs->block_size);
    ext2_dir_entry_t *dot = (ext2_dir_entry_t *)(void *)g_io_buf;
    dot->inode = new_ino;
    ext2_dirent_set_name(fs, dot, ".", 1, EXT2_FT_DIR);
    uint32_t dot_real = (uint32_t)(sizeof(ext2_dir_entry_t) + 1u + 3u) & ~3u;
    dot->rec_len = (uint16_t)dot_real;

    ext2_dir_entry_t *dotdot = (ext2_dir_entry_t *)(void *)(g_io_buf + dot_real);
    dotdot->inode = parent_ino;
    ext2_dirent_set_name(fs, dotdot, "..", 2, EXT2_FT_DIR);
    dotdot->rec_len = (uint16_t)(fs->block_size - dot_real);

    if (ext2_write_block(fs, data_blk, g_io_buf) < 0) {
        (void)ext2_free_block(fs, data_blk);
        (void)ext2_free_inode(fs, new_ino);
        return -EIO;
    }

    ext2_inode_t new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.i_mode = EXT2_S_IFDIR | 0755u;
    new_inode.i_links_count = 2;               /* "." + 父目录里的引用 */
    new_inode.i_block[0] = data_blk;
    new_inode.i_size = fs->block_size;         /* 目录尺寸必须块对齐 */
    new_inode.i_blocks = fs->block_size / 512;
    ext2_touch(&new_inode);
    if (ext2_write_inode(fs, new_ino, &new_inode) < 0) {
        (void)ext2_free_block(fs, data_blk);
        (void)ext2_free_inode(fs, new_ino);
        return -EIO;
    }

    int r = ext2_dir_add(fs, parent_ino, &parent, name, name_len,
                         new_ino, EXT2_FT_DIR);
    if (r < 0) {
        (void)ext2_free_block(fs, data_blk);
        (void)ext2_inode_wipe(fs, new_ino);
        (void)ext2_free_inode(fs, new_ino);
        return r;
    }

    /* ".." 让父目录多一个引用；组描述符里的目录计数同步 +1 */
    parent.i_links_count++;
    ext2_touch_dir(fs, parent_ino, &parent);
    (void)ext2_bgd_add_used_dirs(fs, new_ino, +1);
    return (int)new_ino;
}

int ext2_rmdir(ext2_fs_t *fs, uint32_t dir_ino, const char *name)
{
    int err = ext2_writable(fs);
    if (err) return err;
    if (!name) return -EINVAL;
    if (strcmp(name, ".") == 0) return -EINVAL;
    if (strcmp(name, "..") == 0) return -ENOTEMPTY;

    ext2_inode_t dir;
    if (ext2_read_inode(fs, dir_ino, &dir) < 0) return -EIO;
    if ((dir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;

    uint32_t victim_ino = 0;
    if (ext2_lookup(fs, &dir, name, &victim_ino) < 0) return -ENOENT;
    if (victim_ino == EXT2_ROOT_INO) return -EBUSY;

    ext2_inode_t victim;
    if (ext2_read_inode(fs, victim_ino, &victim) < 0) return -EIO;
    if ((victim.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;
    if (ext2_dir_is_empty(fs, &victim) != 1) return -ENOTEMPTY;

    int r = ext2_dir_remove(fs, dir_ino, &dir, name, 0);
    if (r < 0) return r;

    (void)ext2_inode_release_all(fs, &victim);
    if (ext2_inode_wipe(fs, victim_ino) < 0) return -EIO;
    if (ext2_free_inode(fs, victim_ino) < 0) return -EIO;
    (void)ext2_bgd_add_used_dirs(fs, victim_ino, -1);

    if (dir.i_links_count > 1) dir.i_links_count--;
    ext2_touch_dir(fs, dir_ino, &dir);
    return 0;
}

/** 从 start 沿 ".." 向上走，判断 ancestor 是否在其祖先链上（防目录搬进自己）。 */
static int ext2_dir_is_ancestor(ext2_fs_t *fs, uint32_t ancestor,
                                uint32_t start)
{
    uint32_t current = start;
    for (int depth = 0; depth < 64; depth++) {
        if (current == ancestor) return 1;
        if (current == EXT2_ROOT_INO) break;
        ext2_inode_t inode;
        if (ext2_read_inode(fs, current, &inode) < 0) break;
        uint32_t parent = 0;
        if (ext2_lookup(fs, &inode, "..", &parent) < 0 || !parent) break;
        if (parent == current) break;
        current = parent;
    }
    return 0;
}

int ext2_rename(ext2_fs_t *fs, uint32_t old_dir, const char *old_name,
                uint32_t new_dir, const char *new_name, int replace)
{
    int err = ext2_writable(fs);
    if (err) return err;
    if (!old_name || !new_name) return -EINVAL;
    uint32_t old_len = (uint32_t)strlen(old_name);
    uint32_t new_len = (uint32_t)strlen(new_name);
    if (old_len == 0 || old_len > 255 || new_len == 0 || new_len > 255)
        return -EINVAL;   /* 没有 ENAMETOOLONG：名字长度按 EINVAL 拒绝 */
    if (strcmp(old_name, ".") == 0 || strcmp(old_name, "..") == 0 ||
        strcmp(new_name, ".") == 0 || strcmp(new_name, "..") == 0)
        return -EINVAL;

    ext2_inode_t old_parent, new_parent;
    if (ext2_read_inode(fs, old_dir, &old_parent) < 0 ||
        ext2_read_inode(fs, new_dir, &new_parent) < 0)
        return -EIO;
    if ((old_parent.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR ||
        (new_parent.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return -ENOTDIR;

    uint32_t src_ino = 0;
    if (ext2_lookup(fs, &old_parent, old_name, &src_ino) < 0) return -ENOENT;
    if (old_dir == new_dir && strcmp(old_name, new_name) == 0) return 0;

    ext2_inode_t src;
    if (ext2_read_inode(fs, src_ino, &src) < 0) return -EIO;
    int src_is_dir = ((src.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
    if (src_is_dir && ext2_dir_is_ancestor(fs, src_ino, new_dir)) return -EINVAL;

    uint32_t tgt_ino = 0;
    int has_target = ext2_lookup(fs, &new_parent, new_name, &tgt_ino) == 0;
    if (has_target) {
        if (tgt_ino == src_ino) return 0;   /* 同一 inode 的另一个硬链接：无操作 */
        ext2_inode_t tgt;
        if (ext2_read_inode(fs, tgt_ino, &tgt) < 0) return -EIO;
        int tgt_is_dir = ((tgt.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
        if (!src_is_dir && tgt_is_dir) return -EISDIR;
        if (src_is_dir && !tgt_is_dir) return -ENOTDIR;
        if (tgt_is_dir && ext2_dir_is_empty(fs, &tgt) != 1) return -ENOTEMPTY;
        if (!replace) return -EEXIST;

        int r = ext2_dir_remove(fs, new_dir, &new_parent, new_name, 0);
        if (r < 0) return r;
        (void)ext2_inode_release_all(fs, &tgt);
        if (ext2_inode_wipe(fs, tgt_ino) < 0) return -EIO;
        if (ext2_free_inode(fs, tgt_ino) < 0) return -EIO;
        if (tgt_is_dir) {
            (void)ext2_bgd_add_used_dirs(fs, tgt_ino, -1);
            if (new_parent.i_links_count > 1) new_parent.i_links_count--;
        }
    }

    /* 先挂新名字，再摘旧名字：中途失败时源文件仍可访问。 */
    int r = ext2_dir_add(fs, new_dir, &new_parent, new_name, new_len,
                         src_ino, ext2_ftype_from_mode(src.i_mode));
    if (r < 0) return r;
    r = ext2_dir_remove(fs, old_dir, &old_parent, old_name, 0);
    if (r < 0) return r;

    if (src_is_dir) {
        /* 目录换了父目录：目录里的 ".." 与两边父目录的链接计数必须同步，
         * 否则 e2fsck 报 "ref count is N, should be M"。 */
        ext2_inode_t moved;
        if (ext2_read_inode(fs, src_ino, &moved) == 0) {
            (void)ext2_dir_repoint(fs, &moved, "..", new_dir);
            moved.i_mtime = ext2_now();
            moved.i_ctime = moved.i_mtime;
            (void)ext2_write_inode(fs, src_ino, &moved);
        }
        if (old_dir != new_dir) {
            if (old_parent.i_links_count > 1) old_parent.i_links_count--;
            new_parent.i_links_count++;
        }
    }
    ext2_touch_dir(fs, old_dir, &old_parent);
    ext2_touch_dir(fs, new_dir, &new_parent);
    return 0;
}

/* ── Symbolic links ────────────────────────────────────────────────
 *
 * ext2 有两种布局：
 *   fast symlink — 目标 <= 60 字节，直接塞在 inode 的 i_block（15 个 u32）
 *                  里，i_blocks == 0，不占数据块；
 *   slow symlink — 目标写在一个数据块里，i_block[0] 指向它。
 * 两者在磁盘上都必须是 S_IFLNK inode + 目录项 file_type = 7，否则重新挂载
 * 后内核会把链接当成普通文件读（HBOS 之前的实现正是这样：链接在内存里
 * 有效、一重启就变成一段存着路径文本的普通文件）。 */
int ext2_symlink(ext2_fs_t *fs, uint32_t dir_ino, const char *name,
                 const char *target)
{
    int err = ext2_writable(fs);
    if (err) return err;
    if (!name || !name[0] || !target || !target[0]) return -EINVAL;
    uint32_t length = (uint32_t)strlen(target);
    if (length > fs->block_size || length > 4095) return -EINVAL;   /* 没有 ENAMETOOLONG：名字长度按 EINVAL 拒绝 */

    int ino = ext2_create_file(fs, dir_ino, name, EXT2_FT_SYMLINK);
    if (ino < 0) return ino;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, (uint32_t)ino, &inode) < 0) {
        (void)ext2_unlink(fs, dir_ino, name);
        return -EIO;
    }
    inode.i_size = length;
    inode.i_links_count = 1;
    inode.i_blocks = 0;
    if (length <= 60) {
        memset(inode.i_block, 0, sizeof(inode.i_block));
        memcpy((uint8_t *)inode.i_block, target, length);
    } else {
        uint32_t blk = ext2_alloc_block(fs);
        if (!blk) {
            (void)ext2_unlink(fs, dir_ino, name);
            return -ENOSPC;
        }
        memset(g_io_buf, 0, fs->block_size);
        memcpy(g_io_buf, target, length);
        if (ext2_write_block(fs, blk, g_io_buf) < 0) {
            (void)ext2_free_block(fs, blk);
            (void)ext2_unlink(fs, dir_ino, name);
            return -EIO;
        }
        inode.i_block[0] = blk;
        inode.i_blocks = fs->block_size / 512;
    }
    inode.i_mtime = ext2_now();
    inode.i_ctime = inode.i_mtime;
    if (ext2_write_inode(fs, (uint32_t)ino, &inode) < 0) return -EIO;
    return ino;
}

int ext2_readlink(ext2_fs_t *fs, const ext2_inode_t *inode, char *buf,
                  uint32_t capacity)
{
    if (!fs || !inode || !buf || !capacity) return -EINVAL;
    if ((inode->i_mode & EXT2_S_IFMT) != EXT2_S_IFLNK) return -EINVAL;
    uint32_t length = inode->i_size;
    if (length == 0 || length > 4095) return -EINVAL;
    if (length > capacity) length = capacity;

    if (inode->i_blocks == 0 && inode->i_size <= 60) {
        memcpy(buf, (const uint8_t *)inode->i_block, length);
        return (int)length;
    }
    if (!inode->i_block[0]) return -EIO;
    if (fs->block_size > sizeof(g_io_buf)) return -EINVAL;
    if (ext2_read_block(fs, inode->i_block[0], g_io_buf) < 0) return -EIO;
    memcpy(buf, g_io_buf, length);
    return (int)length;
}
