#include "ext2.h"
#include "block.h"
#include "string.h"

static uint32_t ext2_read_sector(uint32_t partition_lba, uint32_t lba, uint8_t *buf)
{
    return block_read_sector(partition_lba + lba, buf) == 0 ? 1 : 0;
}

int ext2_mount(uint32_t partition_lba, ext2_fs_t *fs)
{
    if (!fs) return -1;
    memset(fs, 0, sizeof(*fs));

    uint32_t sb_lba = EXT2_SUPERBLOCK_OFFSET / BLOCK_SECTOR_SIZE;
    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));

    if (!ext2_read_sector(partition_lba, sb_lba, sector)) return -1;
    memcpy(&fs->sb, sector, sizeof(ext2_superblock_t));

    if (fs->sb.s_magic != EXT2_SIGNATURE) return -1;

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

int ext2_read_block(ext2_fs_t *fs, uint32_t block, uint8_t *buf)
{
    if (!fs || !fs->mounted || !buf) return -1;

    uint32_t sectors_per_block = fs->block_size / BLOCK_SECTOR_SIZE;
    uint32_t start_lba = block * sectors_per_block;

    for (uint32_t i = 0; i < sectors_per_block; i++) {
        if (!ext2_read_sector(fs->partition_lba, start_lba + i,
                              buf + i * BLOCK_SECTOR_SIZE))
            return -1;
    }
    return 0;
}

static int ext2_read_bgd(ext2_fs_t *fs, uint32_t group, ext2_bgd_t *bgd)
{
    if (!fs || !fs->mounted || !bgd) return -1;

    uint32_t bgd_block = fs->sb.s_first_data_block + 1;
    uint32_t bgd_offset = group * sizeof(ext2_bgd_t);
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

static uint32_t ext2_inode_block(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t block_idx)
{
    uint32_t blk_per = fs->block_size / 4;
    uint32_t addr_per_block = fs->block_size / 4;

    if (block_idx < EXT2_NDIR_BLOCKS) {
        return inode->i_block[block_idx];
    }
    block_idx -= EXT2_NDIR_BLOCKS;

    if (block_idx < addr_per_block) {
        uint8_t ind_buf[4096] __attribute__((aligned(2)));
        if (ext2_read_block(fs, inode->i_block[EXT2_IND_BLOCK], ind_buf) < 0)
            return 0;
        return ((uint32_t *)ind_buf)[block_idx];
    }
    block_idx -= addr_per_block;

    uint32_t dind_per = addr_per_block * addr_per_block;
    if (block_idx < dind_per) {
        uint32_t i1 = block_idx / addr_per_block;
        uint32_t i2 = block_idx % addr_per_block;
        uint8_t dind_buf[4096] __attribute__((aligned(2)));
        uint8_t ind_buf[4096] __attribute__((aligned(2)));
        if (ext2_read_block(fs, inode->i_block[EXT2_DIND_BLOCK], dind_buf) < 0)
            return 0;
        if (ext2_read_block(fs, ((uint32_t *)dind_buf)[i1], ind_buf) < 0)
            return 0;
        return ((uint32_t *)ind_buf)[i2];
    }
    block_idx -= dind_per;

    uint32_t tind_per = dind_per * addr_per_block;
    if (block_idx >= tind_per) return 0;

    uint32_t i1 = block_idx / dind_per;
    block_idx %= dind_per;
    uint32_t i2 = block_idx / addr_per_block;
    uint32_t i3 = block_idx % addr_per_block;

    uint8_t tind_buf[4096] __attribute__((aligned(2)));
    uint8_t dind_buf[4096] __attribute__((aligned(2)));
    uint8_t ind_buf[4096] __attribute__((aligned(2)));

    if (ext2_read_block(fs, inode->i_block[EXT2_TIND_BLOCK], tind_buf) < 0)
        return 0;
    if (ext2_read_block(fs, ((uint32_t *)tind_buf)[i1], dind_buf) < 0)
        return 0;
    if (ext2_read_block(fs, ((uint32_t *)dind_buf)[i2], ind_buf) < 0)
        return 0;
    return ((uint32_t *)ind_buf)[i3];

    (void)blk_per;
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