#include "ext2.h"
#include "block.h"
#include "string.h"

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

static int ext2_write_sector(uint32_t partition_lba, uint32_t lba, const uint8_t *buf)
{
    return block_write_sector(partition_lba + lba, buf) == 0 ? 0 : -1;
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
     * inode 上写等于破坏文件系统。 */
    fs->read_only = (incompat & (EXT2_INCOMPAT_EXTENTS | EXT2_INCOMPAT_64BIT)) != 0;

    fs->block_size = 1024U << fs->sb.s_log_block_size;
    fs->inode_size = fs->sb.s_inode_size ? fs->sb.s_inode_size : 128;
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

/* ── ext4 extent 树 ──────────────────────────────────────────────
 * ext4 把 inode 的 i_block[15]（60 字节）当成 extent 树的根：header 12 字节，
 * 后面是 extent 或 index 项，每项 12 字节。索引节点指向的块重复同样的结构。
 * 这里只做"逻辑块 → 物理块"的映射，读路径不用改。 */
#define EXT4_EXT_MAGIC 0xF30Au

typedef struct __attribute__((packed)) {
    uint16_t magic, entries, max, depth;
    uint32_t generation;
} ext4_ext_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} ext4_ext_t;

typedef struct __attribute__((packed)) {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t unused;
} ext4_ext_idx_t;

static uint32_t ext4_extent_bmap(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t logical) {
    uint8_t node[4096];
    if (fs->block_size > sizeof(node)) return 0;
    memcpy(node, inode->i_block, 60);

    for (int level = 0; level < 6; level++) {
        const ext4_ext_hdr_t *h = (const ext4_ext_hdr_t *)node;
        if (h->magic != EXT4_EXT_MAGIC) return 0;

        uint16_t entries = h->entries;
        if (h->depth == 0) {
            const ext4_ext_t *e = (const ext4_ext_t *)(node + sizeof(*h));
            for (uint16_t i = 0; i < entries; i++) {
                uint32_t len = e[i].ee_len;
                if (len > 0x8000) len -= 0x8000;      /* unwritten extent */
                if (!len) continue;
                if (logical >= e[i].ee_block && logical < e[i].ee_block + len) {
                    uint64_t phys = ((uint64_t)e[i].ee_start_hi << 32) | e[i].ee_start_lo;
                    phys += (uint64_t)(logical - e[i].ee_block);
                    return phys > 0xFFFFFFFFull ? 0 : (uint32_t)phys;
                }
            }
            return 0;   /* 空洞 */
        }

        const ext4_ext_idx_t *ix = (const ext4_ext_idx_t *)(node + sizeof(*h));
        int pick = -1;
        for (uint16_t i = 0; i < entries; i++)
            if (ix[i].ei_block <= logical) pick = (int)i;
        if (pick < 0) return 0;

        uint64_t leaf = ((uint64_t)ix[pick].ei_leaf_hi << 32) | ix[pick].ei_leaf_lo;
        if (leaf > 0xFFFFFFFFull) return 0;
        if (ext2_read_block(fs, (uint32_t)leaf, node) < 0) return 0;
    }
    return 0;
}

static uint32_t ext2_inode_block(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t block_idx)
{
    /* ext4：i_block 是 extent 树，不能按 15 个块指针解释 */
    if (inode->i_flags & EXT2_EXTENTS_FL)
        return ext4_extent_bmap(fs, inode, block_idx);

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
        if (!phys_block) return (int)done;

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
    uint32_t offset = 0;
    uint32_t entry_idx = 0;

    while (offset < inode->i_size) {
        uint32_t block_idx = offset / fs->block_size;
        uint32_t block_off = offset % fs->block_size;

        uint32_t phys_block = ext2_inode_block(fs, inode, block_idx);
        if (!phys_block) return -1;

        if (ext2_read_block(fs, phys_block, block_buf) < 0) return -1;

        while (block_off < fs->block_size && offset < inode->i_size) {
            ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(block_buf + block_off);
            if (entry->rec_len == 0) return -1;

            if (entry->inode != 0 && entry->name_len > 0) {
                if (entry_idx == index) {
                    uint32_t nlen = entry->name_len;
                    if (nlen > 31) nlen = 31;
                    memcpy(name, entry->name, nlen);
                    name[nlen] = '\0';
                    *type = (entry->file_type == 2) ? 1 : 0;
                    return 0;
                }
                entry_idx++;
            }
            offset += entry->rec_len;
            block_off += entry->rec_len;
        }
    }
    return -1;
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

            if (entry->inode != 0 && entry->name_len == name_len &&
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
    uint8_t sector[512] __attribute__((aligned(2)));
    uint32_t sb_lba = EXT2_SUPERBLOCK_OFFSET / BLOCK_SECTOR_SIZE;
    uint32_t copy_sz = sizeof(ext2_superblock_t) < 512 ? sizeof(ext2_superblock_t) : 512;
    memcpy(sector, &fs->sb, copy_sz);
    return ext2_write_sector(fs->partition_lba, sb_lba, sector);
}

int ext2_write_inode(ext2_fs_t *fs, uint32_t inode_num, const ext2_inode_t *inode)
{
    if (!fs || !fs->mounted || !inode || inode_num == 0) return -1;
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
    if (!fs || !fs->mounted || fs->sb.s_free_blocks_count == 0) return 0;
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
        bgd.bg_free_blocks_count--;
        ext2_write_bgd(fs, g, &bgd);
        fs->sb.s_free_blocks_count--;
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
    if (!fs || !fs->mounted || !block) return -1;
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
    if (!fs || !fs->mounted || fs->sb.s_free_inodes_count == 0) return 0;
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
        bgd.bg_free_inodes_count--;
        ext2_write_bgd(fs, g, &bgd);
        fs->sb.s_free_inodes_count--;
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
    if (!fs || !fs->mounted || inode_num == 0) return -1;
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
    bgd.bg_free_inodes_count++;
    ext2_write_bgd(fs, g, &bgd);
    fs->sb.s_free_inodes_count++;
    return ext2_write_superblock(fs);
}

/* ── Inode block map write ──────────────────────────────────── */

static int ext2_inode_set_block(ext2_fs_t *fs, ext2_inode_t *inode,
                                 uint32_t inode_num, uint32_t block_idx, uint32_t phys)
{
    uint32_t apb = fs->block_size / 4;
    if (block_idx < EXT2_NDIR_BLOCKS) {
        inode->i_block[block_idx] = phys;
        return ext2_write_inode(fs, inode_num, inode);
    }
    block_idx -= EXT2_NDIR_BLOCKS;
    if (block_idx < apb) {
        uint8_t buf[4096] __attribute__((aligned(2)));
        if (!inode->i_block[EXT2_IND_BLOCK]) {
            uint32_t nb = ext2_alloc_block(fs);
            if (!nb) return -1;
            memset(buf, 0, fs->block_size);
            ext2_write_block(fs, nb, buf);
            inode->i_block[EXT2_IND_BLOCK] = nb;
            ext2_write_inode(fs, inode_num, inode);
        }
        if (ext2_read_block(fs, inode->i_block[EXT2_IND_BLOCK], buf) < 0) return -1;
        ((uint32_t *)buf)[block_idx] = phys;
        return ext2_write_block(fs, inode->i_block[EXT2_IND_BLOCK], buf);
    }
    return -1;
}

/* ── File write ─────────────────────────────────────────────── */

int ext2_write_file(ext2_fs_t *fs, uint32_t inode_num,
                    uint32_t offset, const uint8_t *buf, uint32_t count)
{
    if (!fs || !fs->mounted || !buf || !count) return -1;
    ext2_inode_t inode;
    if (ext2_read_inode(fs, inode_num, &inode) < 0) return -1;

    uint32_t done = 0;
    uint8_t block_buf[4096] __attribute__((aligned(2)));

    while (done < count) {
        uint32_t abs = offset + done;
        uint32_t bidx = abs / fs->block_size;
        uint32_t boff = abs % fs->block_size;
        uint32_t chunk = fs->block_size - boff;
        if (chunk > count - done) chunk = count - done;

        uint32_t phys = ext2_inode_block(fs, &inode, bidx);
        if (!phys) {
            phys = ext2_alloc_block(fs);
            if (!phys) break;
            memset(block_buf, 0, fs->block_size);
            if (ext2_inode_set_block(fs, &inode, inode_num, bidx, phys) < 0) break;
            inode.i_blocks += fs->block_size / 512;
        } else {
            if (ext2_read_block(fs, phys, block_buf) < 0) break;
        }
        memcpy(block_buf + boff, buf + done, chunk);
        if (ext2_write_block(fs, phys, block_buf) < 0) break;
        done += chunk;
    }
    if (offset + done > inode.i_size) inode.i_size = offset + done;
    ext2_write_inode(fs, inode_num, &inode);
    return (int)done;
}

/* ── Directory create / unlink ──────────────────────────────── */

int ext2_create_file(ext2_fs_t *fs, uint32_t dir_ino, const char *name, uint8_t file_type)
{
    if (!fs || !fs->mounted || !name || !dir_ino) return -1;
    uint32_t name_len = (uint32_t)strlen(name);
    if (name_len == 0 || name_len > 255) return -1;

    uint32_t new_ino = ext2_alloc_inode(fs);
    if (!new_ino) return -1;

    ext2_inode_t new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.i_mode = (file_type == 2) ? (EXT2_S_IFDIR | 0755u) : (EXT2_S_IFREG | 0644u);
    new_inode.i_links_count = 1;
    if (ext2_write_inode(fs, new_ino, &new_inode) < 0) { ext2_free_inode(fs, new_ino); return -1; }

    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, dir_ino, &dir_inode) < 0) return -1;

    uint32_t needed = ((uint32_t)(sizeof(ext2_dir_entry_t) + name_len) + 3u) & ~3u;
    uint8_t block_buf[4096] __attribute__((aligned(2)));
    uint32_t offset = 0;

    while (offset < dir_inode.i_size) {
        uint32_t bidx = offset / fs->block_size;
        uint32_t phys = ext2_inode_block(fs, &dir_inode, bidx);
        if (!phys) break;
        if (ext2_read_block(fs, phys, block_buf) < 0) break;
        uint32_t boff = 0;
        while (boff < fs->block_size) {
            ext2_dir_entry_t *e = (ext2_dir_entry_t *)(block_buf + boff);
            if (e->rec_len == 0) break;
            uint32_t real_len = ((uint32_t)(sizeof(ext2_dir_entry_t) + e->name_len) + 3u) & ~3u;
            uint32_t slack = e->rec_len - real_len;
            if (e->inode == 0) { slack = e->rec_len; real_len = 0; }
            if (slack >= needed) {
                if (real_len) { e->rec_len = (uint16_t)real_len; boff += real_len; }
                ext2_dir_entry_t *ne = (ext2_dir_entry_t *)(block_buf + boff);
                ne->inode = new_ino;
                ne->rec_len = (uint16_t)(e->inode ? slack : e->rec_len);
                ne->name_len = (uint8_t)name_len;
                ne->file_type = file_type;
                memcpy(ne->name, name, name_len);
                ext2_write_block(fs, phys, block_buf);
                return (int)new_ino;
            }
            boff += e->rec_len;
        }
        offset += fs->block_size;
    }

    uint32_t new_blk = ext2_alloc_block(fs);
    if (!new_blk) return -1;
    memset(block_buf, 0, fs->block_size);
    ext2_dir_entry_t *e = (ext2_dir_entry_t *)block_buf;
    e->inode = new_ino;
    e->rec_len = (uint16_t)fs->block_size;
    e->name_len = (uint8_t)name_len;
    e->file_type = file_type;
    memcpy(e->name, name, name_len);
    ext2_write_block(fs, new_blk, block_buf);
    uint32_t new_bidx = dir_inode.i_size / fs->block_size;
    ext2_inode_set_block(fs, &dir_inode, dir_ino, new_bidx, new_blk);
    dir_inode.i_size += fs->block_size;
    dir_inode.i_blocks += fs->block_size / 512;
    ext2_write_inode(fs, dir_ino, &dir_inode);
    return (int)new_ino;
}

int ext2_unlink(ext2_fs_t *fs, uint32_t dir_ino, const char *name)
{
    if (!fs || !fs->mounted || !name) return -1;
    uint32_t name_len = (uint32_t)strlen(name);
    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, dir_ino, &dir_inode) < 0) return -1;

    uint8_t block_buf[4096] __attribute__((aligned(2)));
    uint32_t offset = 0;
    while (offset < dir_inode.i_size) {
        uint32_t bidx = offset / fs->block_size;
        uint32_t phys = ext2_inode_block(fs, &dir_inode, bidx);
        if (!phys) { offset += fs->block_size; continue; }
        if (ext2_read_block(fs, phys, block_buf) < 0) break;
        uint32_t boff = 0;
        while (boff < fs->block_size) {
            ext2_dir_entry_t *e = (ext2_dir_entry_t *)(block_buf + boff);
            if (e->rec_len == 0) break;
            if (e->inode && e->name_len == name_len &&
                memcmp(e->name, name, name_len) == 0) {
                uint32_t victim_ino = e->inode;
                e->inode = 0;
                ext2_write_block(fs, phys, block_buf);
                ext2_inode_t vic;
                if (ext2_read_inode(fs, victim_ino, &vic) == 0) {
                    for (int k = 0; k < EXT2_NDIR_BLOCKS; k++)
                        if (vic.i_block[k]) ext2_free_block(fs, vic.i_block[k]);
                    vic.i_links_count = 0;
                    vic.i_dtime = 1;
                    ext2_write_inode(fs, victim_ino, &vic);
                }
                ext2_free_inode(fs, victim_ino);
                return 0;
            }
            boff += e->rec_len;
        }
        offset += fs->block_size;
    }
    return -1;
}

int ext2_truncate(ext2_fs_t *fs, uint32_t inode_num)
{
    if (!fs || !fs->mounted || !inode_num) return -1;
    ext2_inode_t inode;
    if (ext2_read_inode(fs, inode_num, &inode) < 0) return -1;
    for (int i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        if (inode.i_block[i]) { ext2_free_block(fs, inode.i_block[i]); inode.i_block[i] = 0; }
    }
    inode.i_size = 0;
    inode.i_blocks = 0;
    return ext2_write_inode(fs, inode_num, &inode);
}

/* ── Directory creation (mkdir) ─────────────────────────────── */

int ext2_mkdir(ext2_fs_t *fs, uint32_t parent_ino, const char *name)
{
    if (!fs || !fs->mounted || !name || !parent_ino) return -1;
    uint32_t name_len = (uint32_t)strlen(name);
    if (name_len == 0 || name_len > 255) return -1;

    /* Create the new directory inode */
    uint32_t new_ino = ext2_create_file(fs, parent_ino, name, 2);
    if (!new_ino) return -1;

    /* Allocate a block for the new directory's . and .. entries */
    uint32_t new_blk = ext2_alloc_block(fs);
    if (!new_blk) return -1;

    uint8_t block_buf[4096] __attribute__((aligned(2)));
    memset(block_buf, 0, fs->block_size);

    /* "." entry — points to self */
    ext2_dir_entry_t *dot = (ext2_dir_entry_t *)block_buf;
    dot->inode = new_ino;
    dot->name_len = 1;
    dot->file_type = 2; /* directory */
    dot->name[0] = '.';
    uint32_t dot_real = (sizeof(ext2_dir_entry_t) + 1 + 3u) & ~3u;

    /* ".." entry — points to parent */
    ext2_dir_entry_t *dotdot = (ext2_dir_entry_t *)(block_buf + dot_real);
    dotdot->inode = parent_ino;
    dotdot->name_len = 2;
    dotdot->file_type = 2;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->rec_len = (uint16_t)(fs->block_size - dot_real);

    dot->rec_len = (uint16_t)dot_real;

    if (ext2_write_block(fs, new_blk, block_buf) < 0) return -1;

    /* Update the new inode to point to this block */
    ext2_inode_t new_inode;
    if (ext2_read_inode(fs, new_ino, &new_inode) < 0) return -1;
    new_inode.i_block[0] = new_blk;
    new_inode.i_size = fs->block_size;
    new_inode.i_blocks = fs->block_size / 512;
    new_inode.i_links_count = 2; /* . + parent link */
    ext2_write_inode(fs, new_ino, &new_inode);

    /* Increment parent's link count (for "..") */
    ext2_inode_t parent_inode;
    if (ext2_read_inode(fs, parent_ino, &parent_inode) == 0) {
        parent_inode.i_links_count++;
        ext2_write_inode(fs, parent_ino, &parent_inode);
    }

    return (int)new_ino;
}
