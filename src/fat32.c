#include "fat32.h"
#include "block.h"
#include "string.h"

static int fat32_read_sector(fat32_fs_t *fs, uint32_t lba, uint8_t *buf)
{
    (void)fs;
    return block_read_sector(lba, buf) == 0 ? 1 : 0;
}

static int fat32_write_sector(fat32_fs_t *fs, uint32_t lba, const uint8_t *buf)
{
    (void)fs;
    return block_write_sector(lba, buf) == 0 ? 1 : 0;
}

int fat32_mount(uint32_t partition_lba, fat32_fs_t *fs)
{
    if (!fs) return -1;
    memset(fs, 0, sizeof(*fs));

    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    if (block_read_sector(partition_lba, sector) < 0) return -1;

    memcpy(&fs->bpb, sector, sizeof(fat32_bpb_t));

    if (fs->bpb.signature != 0xAA55) return -1;
    if (fs->bpb.bytes_per_sector != BLOCK_SECTOR_SIZE) return -1;
    if (fs->bpb.sectors_per_fat_32 == 0) return -1;

    fs->partition_lba = partition_lba;
    fs->fat_start_lba = partition_lba + fs->bpb.reserved_sectors;
    fs->cluster_size = fs->bpb.sectors_per_cluster * BLOCK_SECTOR_SIZE;
    fs->data_start_lba = fs->fat_start_lba + fs->bpb.num_fats * fs->bpb.sectors_per_fat_32;
    fs->total_clusters = (fs->bpb.total_sectors_32 - fs->bpb.reserved_sectors -
                          fs->bpb.num_fats * fs->bpb.sectors_per_fat_32) /
                          fs->bpb.sectors_per_cluster;
    fs->mounted = 1;
    return 0;
}

int fat32_read_cluster(fat32_fs_t *fs, uint32_t cluster, uint8_t *buf)
{
    if (!fs || !fs->mounted || !buf) return -1;
    if (cluster < 2 || cluster >= fs->total_clusters + 2) return -1;

    uint32_t first_sector = fs->data_start_lba +
                            (cluster - 2) * fs->bpb.sectors_per_cluster;

    for (uint32_t i = 0; i < fs->bpb.sectors_per_cluster; i++) {
        if (!fat32_read_sector(fs, first_sector + i, buf + i * BLOCK_SECTOR_SIZE))
            return -1;
    }
    return 0;
}

static int fat32_write_cluster(fat32_fs_t *fs, uint32_t cluster, const uint8_t *buf)
{
    if (!fs || !fs->mounted || !buf) return -1;
    if (cluster < 2 || cluster >= fs->total_clusters + 2) return -1;

    uint32_t first_sector = fs->data_start_lba +
                            (cluster - 2) * fs->bpb.sectors_per_cluster;

    for (uint32_t i = 0; i < fs->bpb.sectors_per_cluster; i++) {
        if (!fat32_write_sector(fs, first_sector + i, buf + i * BLOCK_SECTOR_SIZE))
            return -1;
    }
    return 0;
}

int fat32_next_cluster(fat32_fs_t *fs, uint32_t cluster)
{
    if (!fs || !fs->mounted) return -1;
    if (cluster < 2) return -1;

    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fs->fat_start_lba + fat_offset / BLOCK_SECTOR_SIZE;
    uint32_t ent_offset = fat_offset % BLOCK_SECTOR_SIZE;

    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    if (!fat32_read_sector(fs, fat_sector, sector)) return -1;

    uint32_t next;
    memcpy(&next, sector + ent_offset, 4);
    next &= 0x0FFFFFFF;

    if (next >= FAT32_EOC) return -1;
    if (next == FAT32_BAD) return -1;
    return (int)next;
}

static int fat32_set_cluster_entry(fat32_fs_t *fs, uint32_t cluster, uint32_t value)
{
    if (!fs || !fs->mounted) return -1;

    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fs->fat_start_lba + fat_offset / BLOCK_SECTOR_SIZE;
    uint32_t ent_offset = fat_offset % BLOCK_SECTOR_SIZE;

    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    if (block_read_sector(fat_sector, sector) < 0) return -1;

    uint32_t cur;
    memcpy(&cur, sector + ent_offset, 4);
    value = (value & 0x0FFFFFFF) | (cur & 0xF0000000);
    memcpy(sector + ent_offset, &value, 4);

    if (block_write_sector(fat_sector, sector) < 0) return -1;

    if (fs->bpb.num_fats > 1) {
        uint32_t fat2_sector = fs->fat_start_lba + fs->bpb.sectors_per_fat_32 + fat_sector - fs->fat_start_lba;
        return block_write_sector(fat2_sector, sector) == 0 ? 0 : -1;
    }
    return 0;
}

static uint32_t fat32_alloc_cluster(fat32_fs_t *fs)
{
    if (!fs || !fs->mounted) return 0;

    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));

    for (uint32_t c = 2; c < fs->total_clusters + 2; c++) {
        uint32_t fat_offset = c * 4;
        uint32_t fat_sector = fs->fat_start_lba + fat_offset / BLOCK_SECTOR_SIZE;
        uint32_t ent_offset = fat_offset % BLOCK_SECTOR_SIZE;

        if (block_read_sector(fat_sector, sector) < 0) continue;

        uint32_t val;
        memcpy(&val, sector + ent_offset, 4);
        if ((val & 0x0FFFFFFF) == 0) {
            fat32_set_cluster_entry(fs, c, FAT32_EOC);
            return c;
        }
    }
    return 0;
}

static void fat32_free_chain(fat32_fs_t *fs, uint32_t cluster)
{
    while (cluster >= 2 && cluster < FAT32_EOC) {
        int next = fat32_next_cluster(fs, cluster);
        fat32_set_cluster_entry(fs, cluster, 0);
        if (next < 0) break;
        cluster = (uint32_t)next;
    }
}

int fat32_read_file(fat32_fs_t *fs, uint32_t first_cluster, uint32_t file_size,
                    uint32_t offset, uint8_t *buf, uint32_t count)
{
    if (!fs || !fs->mounted || !buf) return -1;
    if (offset >= file_size) return 0;

    uint32_t remaining = file_size - offset;
    if (count > remaining) count = remaining;

    uint32_t done = 0;
    uint8_t cluster_buf[32768] __attribute__((aligned(2)));

    while (done < count && first_cluster >= 2) {
        uint32_t abs = offset + done;
        uint32_t cluster_num = abs / fs->cluster_size;
        uint32_t cluster_off = abs % fs->cluster_size;
        uint32_t chunk = fs->cluster_size - cluster_off;
        if (chunk > count - done) chunk = count - done;

        uint32_t cur_cluster = first_cluster;
        for (uint32_t i = 0; i < cluster_num; i++) {
            int next = fat32_next_cluster(fs, cur_cluster);
            if (next < 0) return (int)done;
            cur_cluster = (uint32_t)next;
        }

        if (fat32_read_cluster(fs, cur_cluster, cluster_buf) < 0)
            return (int)done;
        memcpy(buf + done, cluster_buf + cluster_off, chunk);
        done += chunk;
    }
    return (int)done;
}

int fat32_readdir(fat32_fs_t *fs, uint32_t dir_cluster, uint32_t index,
                  char *name, uint32_t *type, uint32_t *size)
{
    if (!fs || !fs->mounted || !name || !type || !size) return -1;

    uint8_t cluster_buf[32768] __attribute__((aligned(2)));
    uint32_t cluster = dir_cluster;
    uint32_t entry_idx = 0;
    int lfn = 0;
    char lfn_buf[256];
    uint32_t lfn_pos = 0;

    while (cluster >= 2) {
        if (fat32_read_cluster(fs, cluster, cluster_buf) < 0) return -1;

        for (uint32_t off = 0; off < fs->cluster_size; off += 32) {
            uint8_t *raw = cluster_buf + off;
            if (raw[0] == 0x00) return -1;

            if (raw[0] == 0xE5) { lfn = 0; continue; }
            if (raw[11] == FAT32_ATTR_LFN) {
                lfn = 1;
                uint8_t seq = raw[0] & 0x3F;
                uint32_t pos = (seq - 1) * 13;
                for (int i = 0; i < 5 && pos + i < 255; i++) {
                    uint16_t ch = ((uint16_t)raw[1 + 2 * i]) |
                                  ((uint16_t)raw[1 + 2 * i + 1] << 8);
                    if (ch == 0 || ch == 0xFFFF) break;
                    if (ch < 128) lfn_buf[pos + i] = (char)ch;
                }
                for (int i = 0; i < 6 && pos + 5 + i < 255; i++) {
                    uint16_t ch = ((uint16_t)raw[14 + 2 * i]) |
                                  ((uint16_t)raw[14 + 2 * i + 1] << 8);
                    if (ch == 0 || ch == 0xFFFF) break;
                    if (ch < 128) lfn_buf[pos + 5 + i] = (char)ch;
                }
                for (int i = 0; i < 2 && pos + 11 + i < 255; i++) {
                    uint16_t ch = ((uint16_t)raw[28 + 2 * i]) |
                                  ((uint16_t)raw[28 + 2 * i + 1] << 8);
                    if (ch == 0 || ch == 0xFFFF) break;
                    if (ch < 128) lfn_buf[pos + 11 + i] = (char)ch;
                }
                if (raw[0] & 0x40) {
                    lfn_pos = pos + 13;
                }
                continue;
            }

            fat32_dir_entry_t *entry = (fat32_dir_entry_t *)raw;
            if (entry->attr & FAT32_ATTR_VOLUME_ID) continue;
            if (entry->name[0] == '.') continue;

            if (entry_idx == index) {
                if (lfn && lfn_pos > 0) {
                    uint32_t nlen = lfn_pos < 64 ? lfn_pos : 63;
                    memcpy(name, lfn_buf, nlen);
                    name[nlen] = '\0';
                } else {
                    uint32_t i = 0;
                    for (; i < 8 && entry->name[i] != ' '; i++)
                        name[i] = entry->name[i];
                    if (entry->name[8] != ' ') {
                        name[i++] = '.';
                        for (uint32_t j = 8; j < 11 && entry->name[j] != ' '; j++)
                            name[i++] = entry->name[j];
                    }
                    name[i] = '\0';
                }
                *type = (entry->attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;
                *size = entry->file_size;
                return 0;
            }
            entry_idx++;
            lfn = 0;
        }

        int next = fat32_next_cluster(fs, cluster);
        if (next < 0) return -1;
        cluster = (uint32_t)next;
    }
    (void)lfn;
    return -1;
}

int fat32_lookup(fat32_fs_t *fs, uint32_t dir_cluster, const char *name,
                 uint32_t *out_cluster, uint32_t *out_size, uint8_t *out_attr)
{
    if (!fs || !fs->mounted || !name || !out_cluster) return -1;

    uint8_t cluster_buf[32768] __attribute__((aligned(2)));
    uint32_t cluster = dir_cluster;

    char short_name[12];
    uint32_t name_len = (uint32_t)strlen(name);
    memset(short_name, ' ', 11);
    short_name[11] = '\0';

    const char *dot = NULL;
    for (uint32_t i = 0; i < name_len; i++) {
        if (name[i] == '.') { dot = name + i; break; }
    }
    if (dot) {
        uint32_t base_len = (uint32_t)(dot - name);
        for (uint32_t i = 0; i < base_len && i < 8; i++)
            short_name[i] = name[i];
        for (uint32_t i = 0; i < name_len - base_len - 1 && i < 3; i++)
            short_name[8 + i] = dot[i + 1];
    } else {
        for (uint32_t i = 0; i < name_len && i < 8; i++)
            short_name[i] = name[i];
    }

    while (cluster >= 2) {
        if (fat32_read_cluster(fs, cluster, cluster_buf) < 0) return -1;

        for (uint32_t off = 0; off < fs->cluster_size; off += 32) {
            fat32_dir_entry_t *entry = (fat32_dir_entry_t *)(cluster_buf + off);
            if (entry->name[0] == 0x00) return -1;
            if (entry->name[0] == 0xE5) continue;
            if (entry->attr == FAT32_ATTR_LFN) continue;
            if (entry->attr & FAT32_ATTR_VOLUME_ID) continue;

            if (memcmp(entry->name, short_name, 11) == 0) {
                *out_cluster = entry->first_cluster_low |
                               ((uint32_t)entry->first_cluster_high << 16);
                if (out_size) *out_size = entry->file_size;
                if (out_attr) *out_attr = entry->attr;
                return 0;
            }
        }

        int next = fat32_next_cluster(fs, cluster);
        if (next < 0) return -1;
        cluster = (uint32_t)next;
    }
    return -1;
}

int fat32_write_file(fat32_fs_t *fs, uint32_t first_cluster, uint32_t *file_size,
                     uint32_t offset, const uint8_t *buf, uint32_t count)
{
    if (!fs || !fs->mounted || !buf || !file_size) return -1;

    uint32_t needed_bytes = offset + count;
    uint32_t needed_clusters = (needed_bytes + fs->cluster_size - 1) / fs->cluster_size;

    uint32_t current = first_cluster;
    uint32_t chain_count = 1;

    while (1) {
        int next = fat32_next_cluster(fs, current);
        if (next < 0) break;
        current = (uint32_t)next;
        chain_count++;
    }

    while (chain_count < needed_clusters) {
        uint32_t new_clu = fat32_alloc_cluster(fs);
        if (!new_clu) return -1;
        fat32_set_cluster_entry(fs, current, new_clu);
        current = new_clu;
        chain_count++;
    }

    uint32_t done = 0;
    uint8_t cluster_buf[32768] __attribute__((aligned(2)));

    while (done < count) {
        uint32_t abs = offset + done;
        uint32_t cluster_num = abs / fs->cluster_size;
        uint32_t cluster_off = abs % fs->cluster_size;
        uint32_t chunk = fs->cluster_size - cluster_off;
        if (chunk > count - done) chunk = count - done;

        uint32_t cur_cluster = first_cluster;
        for (uint32_t i = 0; i < cluster_num; i++) {
            int next = fat32_next_cluster(fs, cur_cluster);
            if (next < 0) return -1;
            cur_cluster = (uint32_t)next;
        }

        if (chunk < fs->cluster_size) {
            if (fat32_read_cluster(fs, cur_cluster, cluster_buf) < 0)
                return -1;
        }
        memcpy(cluster_buf + cluster_off, buf + done, chunk);
        if (fat32_write_cluster(fs, cur_cluster, cluster_buf) < 0)
            return -1;
        done += chunk;
    }

    if (needed_bytes > *file_size)
        *file_size = needed_bytes;

    return (int)done;
}

static int fat32_write_dir_entry(fat32_fs_t *fs, uint32_t dir_cluster,
                                  fat32_dir_entry_t *entry)
{
    if (!fs || !fs->mounted || !entry) return -1;

    uint8_t cluster_buf[32768] __attribute__((aligned(2)));
    uint32_t cluster = dir_cluster;

    while (cluster >= 2) {
        if (fat32_read_cluster(fs, cluster, cluster_buf) < 0) return -1;

        for (uint32_t off = 0; off < fs->cluster_size; off += 32) {
            uint8_t first = cluster_buf[off];
            if (first == 0x00 || first == 0xE5) {
                memcpy(cluster_buf + off, entry, 32);
                if (fat32_write_cluster(fs, cluster, cluster_buf) < 0)
                    return -1;
                return 0;
            }
        }

        int next = fat32_next_cluster(fs, cluster);
        if (next < 0) {
            uint32_t new_clu = fat32_alloc_cluster(fs);
            if (!new_clu) return -1;
            fat32_set_cluster_entry(fs, cluster, new_clu);
            memset(cluster_buf, 0, fs->cluster_size);
            memcpy(cluster_buf, entry, 32);
            if (fat32_write_cluster(fs, new_clu, cluster_buf) < 0)
                return -1;
            return 0;
        }
        cluster = (uint32_t)next;
    }
    return -1;
}

int fat32_create_file(fat32_fs_t *fs, uint32_t dir_cluster, const char *name,
                      uint32_t *out_cluster)
{
    if (!fs || !fs->mounted || !name || !out_cluster) return -1;

    uint32_t existing;
    if (fat32_lookup(fs, dir_cluster, name, &existing, NULL, NULL) == 0) {
        *out_cluster = existing;
        return 0;
    }

    uint32_t new_clu = fat32_alloc_cluster(fs);
    if (!new_clu) return -1;

    char dir_name[11];
    memset(dir_name, ' ', 11);
    uint32_t name_len = (uint32_t)strlen(name);
    const char *dot = NULL;
    for (uint32_t i = 0; i < name_len; i++) {
        if (name[i] == '.') { dot = name + i; break; }
    }
    if (dot) {
        uint32_t base_len = (uint32_t)(dot - name);
        for (uint32_t i = 0; i < base_len && i < 8; i++)
            dir_name[i] = name[i];
        for (uint32_t i = 0; i < name_len - base_len - 1 && i < 3; i++)
            dir_name[8 + i] = dot[i + 1];
    } else {
        for (uint32_t i = 0; i < name_len && i < 8; i++)
            dir_name[i] = name[i];
    }

    fat32_dir_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.name, dir_name, 11);
    entry.attr = 0x20;
    entry.first_cluster_low = (uint16_t)(new_clu & 0xFFFF);
    entry.first_cluster_high = (uint16_t)((new_clu >> 16) & 0xFFFF);
    entry.file_size = 0;

    if (fat32_write_dir_entry(fs, dir_cluster, &entry) < 0) {
        fat32_free_chain(fs, new_clu);
        return -1;
    }

    *out_cluster = new_clu;
    return 0;
}

int fat32_delete_file(fat32_fs_t *fs, uint32_t dir_cluster, const char *name)
{
    if (!fs || !fs->mounted || !name) return -1;

    uint32_t cluster;
    uint8_t attr;
    if (fat32_lookup(fs, dir_cluster, name, &cluster, NULL, &attr) < 0)
        return -1;
    if (attr & FAT32_ATTR_DIRECTORY) return -1;

    uint8_t cluster_buf[32768] __attribute__((aligned(2)));
    uint32_t dir_clus = dir_cluster;

    while (dir_clus >= 2) {
        if (fat32_read_cluster(fs, dir_clus, cluster_buf) < 0) return -1;

        for (uint32_t off = 0; off < fs->cluster_size; off += 32) {
            fat32_dir_entry_t *entry = (fat32_dir_entry_t *)(cluster_buf + off);
            uint32_t ent_cluster = entry->first_cluster_low |
                                   ((uint32_t)entry->first_cluster_high << 16);
            if (ent_cluster == cluster && entry->name[0] != 0xE5) {
                cluster_buf[off] = 0xE5;
                if (fat32_write_cluster(fs, dir_clus, cluster_buf) < 0)
                    return -1;
                fat32_free_chain(fs, cluster);
                return 0;
            }
        }

        int next = fat32_next_cluster(fs, dir_clus);
        if (next < 0) return -1;
        dir_clus = (uint32_t)next;
    }
    return -1;
}