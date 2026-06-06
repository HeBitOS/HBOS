#include "ext2.h"
#include "block.h"
#include "string.h"

static uint32_t ext2_read_sector(uint32_t partition_lba, uint32_t lba, uint8_t *buf)
{
    return block_read_sector(partition_lba + lba, buf) == 0 ? 1 : 0;
}

static int ext2_write_sector(uint32_t partition_lba, uint32_t lba, const uint8_t *buf)
{
    return block_write_sector(partition_lba + lba, buf) == 0 ? 0 : -1;
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
/* ── Write helpers ──────────────────────────────────────────── */

int ext2_write_block(ext2_fs_t *fs, uint32_t block, const uint8_t *buf)
{
    if (!fs || !fs->mounted || !buf || !block) return -1;
    uint32_t spb = fs->block_size / BLOCK_SECTOR_SIZE;
    uint32_t start = block * spb;
    for (uint32_t i = 0; i < spb; i++)
        if (ext2_write_sector(fs->partition_lba, start + i, buf + i * BLOCK_SECTOR_SIZE) < 0)
            return -1;
    return 0;
}

static int ext2_write_bgd(ext2_fs_t *fs, uint32_t group, const ext2_bgd_t *bgd)
{
    uint32_t bgd_block = fs->sb.s_first_data_block + 1;
    uint32_t bgd_offset = group * sizeof(ext2_bgd_t);
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

uint32_t ext2_alloc_block(ext2_fs_t *fs)
{
    if (!fs || !fs->mounted || fs->sb.s_free_blocks_count == 0) return 0;
    uint8_t bitmap[4096] __attribute__((aligned(2)));
    for (uint32_t g = 0; g < fs->block_group_count; g++) {
        ext2_bgd_t bgd;
        if (ext2_read_bgd(fs, g, &bgd) < 0) continue;
        if (bgd.bg_free_blocks_count == 0) continue;
        if (ext2_read_block(fs, bgd.bg_block_bitmap, bitmap) < 0) continue;
        uint32_t bpg = fs->sb.s_blocks_per_group;
        for (uint32_t i = 0; i < bpg; i++) {
            if (!(bitmap[i / 8] & (1u << (i % 8)))) {
                bitmap[i / 8] |= (1u << (i % 8));
                if (ext2_write_block(fs, bgd.bg_block_bitmap, bitmap) < 0) return 0;
                bgd.bg_free_blocks_count--;
                ext2_write_bgd(fs, g, &bgd);
                fs->sb.s_free_blocks_count--;
                ext2_write_superblock(fs);
                return g * bpg + i + fs->sb.s_first_data_block;
            }
        }
    }
    return 0;
}

int ext2_free_block(ext2_fs_t *fs, uint32_t block)
{
    if (!fs || !fs->mounted || !block) return -1;
    uint32_t bpg = fs->sb.s_blocks_per_group;
    uint32_t adj = block - fs->sb.s_first_data_block;
    uint32_t g = adj / bpg, i = adj % bpg;
    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, g, &bgd) < 0) return -1;
    uint8_t bitmap[4096] __attribute__((aligned(2)));
    if (ext2_read_block(fs, bgd.bg_block_bitmap, bitmap) < 0) return -1;
    bitmap[i / 8] &= ~(1u << (i % 8));
    if (ext2_write_block(fs, bgd.bg_block_bitmap, bitmap) < 0) return -1;
    bgd.bg_free_blocks_count++;
    ext2_write_bgd(fs, g, &bgd);
    fs->sb.s_free_blocks_count++;
    return ext2_write_superblock(fs);
}

uint32_t ext2_alloc_inode(ext2_fs_t *fs)
{
    if (!fs || !fs->mounted || fs->sb.s_free_inodes_count == 0) return 0;
    uint8_t bitmap[4096] __attribute__((aligned(2)));
    for (uint32_t g = 0; g < fs->block_group_count; g++) {
        ext2_bgd_t bgd;
        if (ext2_read_bgd(fs, g, &bgd) < 0) continue;
        if (bgd.bg_free_inodes_count == 0) continue;
        if (ext2_read_block(fs, bgd.bg_inode_bitmap, bitmap) < 0) continue;
        uint32_t ipg = fs->sb.s_inodes_per_group;
        for (uint32_t i = 0; i < ipg; i++) {
            if (!(bitmap[i / 8] & (1u << (i % 8)))) {
                bitmap[i / 8] |= (1u << (i % 8));
                if (ext2_write_block(fs, bgd.bg_inode_bitmap, bitmap) < 0) return 0;
                bgd.bg_free_inodes_count--;
                ext2_write_bgd(fs, g, &bgd);
                fs->sb.s_free_inodes_count--;
                ext2_write_superblock(fs);
                return g * ipg + i + 1;
            }
        }
    }
    return 0;
}

int ext2_free_inode(ext2_fs_t *fs, uint32_t inode_num)
{
    if (!fs || !fs->mounted || inode_num == 0) return -1;
    uint32_t ipg = fs->sb.s_inodes_per_group;
    uint32_t g = (inode_num - 1) / ipg, i = (inode_num - 1) % ipg;
    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, g, &bgd) < 0) return -1;
    uint8_t bitmap[4096] __attribute__((aligned(2)));
    if (ext2_read_block(fs, bgd.bg_inode_bitmap, bitmap) < 0) return -1;
    bitmap[i / 8] &= ~(1u << (i % 8));
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
