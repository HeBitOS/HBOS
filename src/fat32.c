#include "fat32.h"
#include "block.h"
#include "rtc.h"
#include "string.h"

/* 一簇最多 64 扇区 = 32KB（见 fat32_pick_cluster_size 的上限）。这块缓冲
 * 原来是每个函数各自的栈上局部变量；真正的问题是这里不止一层——比如
 * fat32_mkdir() 自己占一份 32KB，接着调用同样有 32KB 局部变量的
 * fat32_write_dir_entry()，两层加起来 64KB，正好顶到 boot.asm 里
 * 64KB 的内核启动栈上限（尤其是从 kmain 深层调用链、加上其余栈帧开销后，
 * 实际可用空间早就不到 64KB）——真机上表现为 install→重启→自检阶段一
 * 碰到带子目录的路径就内核 panic。改成共享的静态（.bss）缓冲区：FAT32
 * 访问在这个单核协作式调度的内核里本来就不会真并发，逐个函数看过用法，
 * 没有谁在自己那份数据还需要的时候被套娃调用者顺手覆盖掉（例如
 * fat32_mkdir 用完自己的内容、已经落盘之后才会调用
 * fat32_write_dir_entry，不存在"两边同时都要读"的情况）。 */
static uint8_t g_fat32_cluster_buf[32768] __attribute__((aligned(2)));

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

/* fatgen103 要求 FAT32 卷至少 65525 个数据簇——不足时 Windows 会把卷当
 * 成非法 FAT32（或按 FAT16 语义处理），fsck.fat 也警告 "less than the
 * required minimum of 65525"。先按 fatgen103 的标准尺寸表选簇，再校验
 * 簇数下限，不达标就逐级缩小（旧静态表在 32~128MB 这段给出 2 扇区/簇，
 * 63MB 卷只落 ~63xxx 簇，正好踩线以下）。 */
static uint32_t fat32_clusters_for(uint32_t total_sectors, uint32_t sectors_per_cluster)
{
    /* 与 fat32_format 里的 FAT 大小公式保持一致 */
    uint32_t tmp2 = ((256 * sectors_per_cluster) + 2) / 2;
    uint32_t sectors_per_fat = (total_sectors - 32 + tmp2 - 1) / tmp2;
    return (total_sectors - 32 - 2 * sectors_per_fat) / sectors_per_cluster;
}

static uint32_t fat32_pick_cluster_size(uint32_t total_sectors)
{
    uint32_t c;
    if (total_sectors < 16384)    c = 1;   /* <8MB   -> 512B/cluster */
    else if (total_sectors < 131072)   c = 2;   /* <64MB  -> 1KB */
    else if (total_sectors < 524288)   c = 4;   /* <256MB -> 2KB */
    else if (total_sectors < 2097152)  c = 8;   /* <1GB   -> 4KB */
    else if (total_sectors < 4194304)  c = 16;  /* <2GB   -> 8KB */
    else if (total_sectors < 8388608)  c = 32;  /* <4GB   -> 16KB */
    else                          c = 64;  /* >=4GB  -> 32KB */

    while (c > 1 && fat32_clusters_for(total_sectors, c) < 65525) c--;
    return c;
}

/**
 * @brief 在 partition_lba 起的分区上写入一个全新的 FAT32 文件系统。
 *
 * 布局：保留区(32扇区: 引导扇区+FSInfo+备份引导扇区+备份FSInfo+填充)、
 * 两份 FAT 表、数据区（根目录固定占 1 簇，簇号 2）。FAT[0]/FAT[1] 写标准
 * 保留值，FAT[2]（根目录）标记为链尾，其余清零。
 */
/* 把 RTC 当前时间打包成 FAT 目录项的 16 位日期/时间。RTC 读不到时退回
 * 2000-01-01——全 0 是非法日期（mtools 显示成 1980-00-00），比合法固定值
 * 更扎眼。FAT 时间戳年份范围 1980~2107，越界截断到端点。 */
static void fat32_now(uint16_t *date_out, uint16_t *time_out)
{
    *date_out = 0x2821; /* 2000-01-01 */
    *time_out = 0;
    rtc_time_t t;
    if (rtc_read_time(&t) < 0) return;
    uint32_t y = t.year;
    if (y < 1980) y = 1980;
    if (y > 2107) y = 2107;
    uint32_t mo = t.month ? t.month : 12;
    uint32_t d = t.day ? t.day : 1;
    *date_out = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
    *time_out = (uint16_t)(((uint32_t)t.hour << 11) |
                           ((uint32_t)t.minute << 5) | (t.second / 2));
}

/* 往 data_start_lba 起的根目录簇第一扇区写入卷标目录项（ATTR_VOLUME_ID），
 * 与引导扇区里的卷标字段保持一致——只写 BPB 不写根目录项时，fsck.fat 会
 * 提示 "Label in boot sector ... but there is no volume label in root
 * directory"，mtools/Windows 也看不到卷标。 */
static int fat32_write_volume_label(const fat32_bpb_t *bpb, uint32_t data_start_lba)
{
    uint8_t label[11];
    memcpy(label, bpb->volume_label, 11);
    int all_space = 1;
    for (uint32_t i = 0; i < 11; i++) {
        if (label[i] != ' ') { all_space = 0; break; }
    }
    if (all_space) return 0;

    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    memset(sector, 0, sizeof(sector));
    memcpy(sector, label, 11);
    sector[11] = FAT32_ATTR_VOLUME_ID;
    uint16_t date, tm;
    fat32_now(&date, &tm);
    memcpy(sector + 14, &date, 2); /* creation_time/date */
    memcpy(sector + 18, &date, 2); /* access_date */
    memcpy(sector + 22, &date, 2); /* write_time */
    memcpy(sector + 24, &date, 2); /* write_date */
    return block_write_sector(data_start_lba, sector);
}

int fat32_format(uint32_t partition_lba, uint32_t total_sectors, const char *volume_label)
{
    if (total_sectors < 66624) return -1; /* 太小，不是安全的 FAT32 尺寸
        （66624 = 512B/簇时凑足 65525 簇下限的最小卷；再小 fsck.fat/Windows
        就会按非法 FAT32 处理，见 fat32_pick_cluster_size 的注释） */

    uint32_t sectors_per_cluster = fat32_pick_cluster_size(total_sectors);
    uint32_t reserved_sectors = 32;
    uint32_t num_fats = 2;

    /* fatgen103 的标准 FAT32 FAT 大小公式 */
    uint32_t tmp1 = total_sectors - reserved_sectors;
    uint32_t tmp2 = ((256 * sectors_per_cluster) + num_fats) / 2;
    uint32_t sectors_per_fat = (tmp1 + tmp2 - 1) / tmp2;

    uint32_t fat_start_lba = partition_lba + reserved_sectors;
    uint32_t data_start_lba = fat_start_lba + num_fats * sectors_per_fat;
    if (data_start_lba + sectors_per_cluster > partition_lba + total_sectors) return -1;
    uint32_t total_clusters = (total_sectors - reserved_sectors - num_fats * sectors_per_fat) /
                              sectors_per_cluster;
    if (total_clusters < 100) return -1;

    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    uint8_t zero[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    memset(zero, 0, sizeof(zero));

    /* 保留区先整体清零，再写入引导扇区/FSInfo/备份 */
    for (uint32_t i = 0; i < reserved_sectors; i++) {
        if (block_write_sector(partition_lba + i, zero) < 0) return -1;
    }

    /* 用一个正常类型的局部变量填字段，再 memcpy 进扇区缓冲——不要通过把
     * uint8_t[] 强转成 fat32_bpb_t* 直接写字段：block_write_sector() 收的
     * 是 const uint8_t*，GCC 在 -O2 下按严格别名规则看不出这些 bpb->
     * 字段写入会被那次调用读到，曾把整段字段赋值当死代码优化掉，只留下
     * 后面单独用 sector[510]/[511] 写的签名字节（这两处走的确实是
     * uint8_t* 访问，编译器认得），结果磁盘上引导扇区除签名外全是 0。 */
    fat32_bpb_t bpb_local;
    memset(&bpb_local, 0, sizeof(bpb_local));
    bpb_local.jmp[0] = 0xEB; bpb_local.jmp[1] = 0x58; bpb_local.jmp[2] = 0x90;
    memcpy(bpb_local.oem, "HBOS4   ", 8);
    bpb_local.bytes_per_sector = BLOCK_SECTOR_SIZE;
    bpb_local.sectors_per_cluster = (uint8_t)sectors_per_cluster;
    bpb_local.reserved_sectors = (uint16_t)reserved_sectors;
    bpb_local.num_fats = (uint8_t)num_fats;
    bpb_local.root_entries = 0;
    bpb_local.total_sectors_16 = 0;
    bpb_local.media_type = 0xF8;
    bpb_local.sectors_per_fat_16 = 0;
    bpb_local.sectors_per_track = 63;
    bpb_local.num_heads = 255;
    bpb_local.hidden_sectors = partition_lba;
    bpb_local.total_sectors_32 = total_sectors;
    bpb_local.sectors_per_fat_32 = sectors_per_fat;
    bpb_local.ext_flags = 0;
    bpb_local.fs_version = 0;
    bpb_local.root_cluster = 2;
    bpb_local.fs_info = 1;
    bpb_local.backup_boot_sector = 6;
    bpb_local.drive_number = 0x80;
    bpb_local.boot_signature = 0x29;
    bpb_local.volume_id = 0x48424F35;
    memset(bpb_local.volume_label, ' ', 11);
    if (volume_label) {
        uint32_t n = 0;
        while (volume_label[n] && n < 11) { bpb_local.volume_label[n] = (uint8_t)volume_label[n]; n++; }
    } else {
        memcpy(bpb_local.volume_label, "HBOS DISK  ", 11);
    }
    memcpy(bpb_local.fs_type, "FAT32   ", 8);

    memset(sector, 0, sizeof(sector));
    memcpy(sector, &bpb_local, sizeof(bpb_local));
    sector[510] = 0x55; sector[511] = 0xAA;
    if (block_write_sector(partition_lba, sector) < 0) return -1;
    if (block_write_sector(partition_lba + bpb_local.backup_boot_sector, sector) < 0) return -1;

    /* FSInfo 扇区（引导扇区 + 其备份各一份）。内核的分配器只扫 FAT、
     * 不维护空闲簇计数（fat32_alloc_cluster/fat32_free_chain 都只改 FAT
     * 表），所以这里写规范认可的"未知" 0xFFFFFFFF，而不是格式化瞬间的
     * 看似准确值——写准了内核一分配 fsck.fat 就报 "Free cluster summary
     * wrong"，反而更糟。0xFFFFFFFF 表示"不确定，使用方自行重算"。 */
    memset(sector, 0, sizeof(sector));
    uint32_t lead_sig = 0x41615252, struct_sig = 0x61417272, trail_sig = 0xAA550000;
    uint32_t free_count = 0xFFFFFFFF, next_free = 0xFFFFFFFF;
    memcpy(sector + 0, &lead_sig, 4);
    memcpy(sector + 484, &struct_sig, 4);
    memcpy(sector + 488, &free_count, 4);
    memcpy(sector + 492, &next_free, 4);
    memcpy(sector + 508, &trail_sig, 4);
    if (block_write_sector(partition_lba + bpb_local.fs_info, sector) < 0) return -1;
    if (block_write_sector(partition_lba + bpb_local.backup_boot_sector + bpb_local.fs_info, sector) < 0) return -1;

    /* 两份 FAT 表：先整体清零 */
    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t fat_lba = fat_start_lba + f * sectors_per_fat;
        for (uint32_t i = 0; i < sectors_per_fat; i++) {
            if (block_write_sector(fat_lba + i, zero) < 0) return -1;
        }
    }
    /* FAT[0]/FAT[1] 标准保留值，FAT[2]（根目录簇）标记为链尾 */
    memset(sector, 0, sizeof(sector));
    uint32_t e0 = 0x0FFFFFF8, e1 = 0x0FFFFFFF, e2 = FAT32_EOC;
    memcpy(sector + 0, &e0, 4);
    memcpy(sector + 4, &e1, 4);
    memcpy(sector + 8, &e2, 4);
    for (uint32_t f = 0; f < num_fats; f++) {
        if (block_write_sector(fat_start_lba + f * sectors_per_fat, sector) < 0) return -1;
    }

    /* 根目录簇（簇号 2）清零 */
    for (uint32_t i = 0; i < sectors_per_cluster; i++) {
        if (block_write_sector(data_start_lba + i, zero) < 0) return -1;
    }

    /* 卷标目录项：与 BPB 的 volume_label 保持一致 */
    if (fat32_write_volume_label(&bpb_local, data_start_lba) < 0) return -1;

    return 0;
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
    uint8_t *cluster_buf = g_fat32_cluster_buf; /* 见文件顶部注释：改用共享静态缓冲区，避免栈溢出 */

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

    uint8_t *cluster_buf = g_fat32_cluster_buf; /* 见文件顶部注释：改用共享静态缓冲区，避免栈溢出 */
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

    uint8_t *cluster_buf = g_fat32_cluster_buf; /* 见文件顶部注释：改用共享静态缓冲区，避免栈溢出 */
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
    uint8_t *cluster_buf = g_fat32_cluster_buf; /* 见文件顶部注释：改用共享静态缓冲区，避免栈溢出 */

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

    uint8_t *cluster_buf = g_fat32_cluster_buf; /* 见文件顶部注释：改用共享静态缓冲区，避免栈溢出 */
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
    uint16_t cdate, ctime;
    fat32_now(&cdate, &ctime);
    entry.creation_time = ctime;
    entry.creation_date = cdate;
    entry.access_date = cdate;
    entry.write_time = ctime;
    entry.write_date = cdate;
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

    uint8_t *cluster_buf = g_fat32_cluster_buf; /* 见文件顶部注释：改用共享静态缓冲区，避免栈溢出 */
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

int fat32_mkdir(fat32_fs_t *fs, uint32_t dir_cluster, const char *name)
{
    if (!fs || !fs->mounted || !name) return -1;

    uint32_t existing;
    if (fat32_lookup(fs, dir_cluster, name, &existing, NULL, NULL) == 0) return -1;

    uint32_t new_clu = fat32_alloc_cluster(fs);
    if (!new_clu) return -1;

    /* 新簇清零后写入 "." ".." 项，让真实系统能把它当正常目录挂载。用局部
     * 结构体变量填字段再 memcpy 进 cluster_buf——不要把 cluster_buf 直接
     * 强转成 fat32_dir_entry_t* 写字段，那样写会在 -O2 严格别名下被当死
     * 代码优化掉（同 fat32_format 里踩过的坑，见那边的注释）。 */
    uint8_t *cluster_buf = g_fat32_cluster_buf; /* 见文件顶部注释：改用共享静态缓冲区，避免栈溢出 */
    memset(cluster_buf, 0, fs->cluster_size);

    fat32_dir_entry_t dot;
    memset(&dot, 0, sizeof(dot));
    memset(dot.name, ' ', 11); dot.name[0] = '.';
    dot.attr = FAT32_ATTR_DIRECTORY;
    dot.first_cluster_low = (uint16_t)(new_clu & 0xFFFF);
    dot.first_cluster_high = (uint16_t)((new_clu >> 16) & 0xFFFF);
    memcpy(cluster_buf, &dot, sizeof(dot));

    fat32_dir_entry_t dotdot;
    memset(&dotdot, 0, sizeof(dotdot));
    memset(dotdot.name, ' ', 11); dotdot.name[0] = '.'; dotdot.name[1] = '.';
    dotdot.attr = FAT32_ATTR_DIRECTORY;
    uint32_t parent = (dir_cluster == fs->bpb.root_cluster) ? 0 : dir_cluster;
    dotdot.first_cluster_low = (uint16_t)(parent & 0xFFFF);
    dotdot.first_cluster_high = (uint16_t)((parent >> 16) & 0xFFFF);
    memcpy(cluster_buf + 32, &dotdot, sizeof(dotdot));

    if (fat32_write_cluster(fs, new_clu, cluster_buf) < 0) {
        fat32_free_chain(fs, new_clu);
        return -1;
    }

    /* 目录短名同 fat32_create_file 的 8.3 拆分方式 */
    char dir_name[11];
    memset(dir_name, ' ', 11);
    uint32_t name_len = (uint32_t)strlen(name);
    const char *dot_pos = NULL;
    for (uint32_t i = 0; i < name_len; i++) {
        if (name[i] == '.') { dot_pos = name + i; break; }
    }
    if (dot_pos) {
        uint32_t base_len = (uint32_t)(dot_pos - name);
        for (uint32_t i = 0; i < base_len && i < 8; i++) dir_name[i] = name[i];
        for (uint32_t i = 0; i < name_len - base_len - 1 && i < 3; i++) dir_name[8 + i] = dot_pos[i + 1];
    } else {
        for (uint32_t i = 0; i < name_len && i < 8; i++) dir_name[i] = name[i];
    }

    fat32_dir_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.name, dir_name, 11);
    entry.attr = FAT32_ATTR_DIRECTORY;
    uint16_t ddate, dtime;
    fat32_now(&ddate, &dtime);
    entry.creation_time = dtime;
    entry.creation_date = ddate;
    entry.access_date = ddate;
    entry.write_time = dtime;
    entry.write_date = ddate;
    entry.first_cluster_low = (uint16_t)(new_clu & 0xFFFF);
    entry.first_cluster_high = (uint16_t)((new_clu >> 16) & 0xFFFF);
    entry.file_size = 0;

    if (fat32_write_dir_entry(fs, dir_cluster, &entry) < 0) {
        fat32_free_chain(fs, new_clu);
        return -1;
    }
    return 0;
}

/* raw 是否是短目录项里的 "." 或 ".." 保留项（11 字节短名，空格补齐） */
static int fat32_is_dot_entry(const uint8_t *raw)
{
    if (raw[0] != '.') return 0;
    if (raw[1] == '.') {
        for (int i = 2; i < 11; i++) if (raw[i] != ' ') return 0;
        return 1;
    }
    for (int i = 1; i < 11; i++) if (raw[i] != ' ') return 0;
    return 1;
}

/* 目录是否只剩 "." ".." 两个保留项（没有其它 LFN/短名条目） */
static int fat32_dir_is_empty(fat32_fs_t *fs, uint32_t dir_cluster)
{
    uint8_t *cluster_buf = g_fat32_cluster_buf;
    uint32_t c = dir_cluster;
    while (c >= 2) {
        if (fat32_read_cluster(fs, c, cluster_buf) < 0) return 0;
        for (uint32_t off = 0; off < fs->cluster_size; off += 32) {
            uint8_t *raw = cluster_buf + off;
            if (raw[0] == 0x00) return 1;   /* 后面全是未用区，到此为止 */
            if (raw[0] == 0xE5) continue;   /* 已删除 */
            if (raw[11] == FAT32_ATTR_LFN) continue;
            if (raw[11] & FAT32_ATTR_VOLUME_ID) continue;
            if (!fat32_is_dot_entry(raw)) return 0;  /* 有非 "."/".." 的真条目 */
        }
        int next = fat32_next_cluster(fs, c);
        if (next < 0) return 1;
        c = (uint32_t)next;
    }
    return 1;
}

int fat32_rmdir(fat32_fs_t *fs, uint32_t dir_cluster, const char *name)
{
    if (!fs || !fs->mounted || !name) return -1;

    uint32_t cluster;
    uint8_t attr;
    if (fat32_lookup(fs, dir_cluster, name, &cluster, NULL, &attr) < 0) return -1;
    if (!(attr & FAT32_ATTR_DIRECTORY)) return -1;
    if (!fat32_dir_is_empty(fs, cluster)) return -1;

    /* 从父目录里把这一项标成已删除——同 fat32_delete_file() 的扫描/标记
     * 手法，只是不再拒绝 DIRECTORY 属性（那是给"删文件"用的限制）。 */
    uint8_t *cluster_buf = g_fat32_cluster_buf;
    uint32_t dir_clus = dir_cluster;
    while (dir_clus >= 2) {
        if (fat32_read_cluster(fs, dir_clus, cluster_buf) < 0) return -1;
        for (uint32_t off = 0; off < fs->cluster_size; off += 32) {
            fat32_dir_entry_t *entry = (fat32_dir_entry_t *)(cluster_buf + off);
            uint32_t ent_cluster = entry->first_cluster_low |
                                   ((uint32_t)entry->first_cluster_high << 16);
            if (ent_cluster == cluster && entry->name[0] != 0xE5) {
                cluster_buf[off] = 0xE5;
                if (fat32_write_cluster(fs, dir_clus, cluster_buf) < 0) return -1;
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

int fat32_set_file_size(fat32_fs_t *fs, uint32_t dir_cluster, uint32_t file_cluster, uint32_t new_size)
{
    if (!fs || !fs->mounted) return -1;
    uint8_t *cluster_buf = g_fat32_cluster_buf;
    uint32_t c = dir_cluster;
    while (c >= 2) {
        if (fat32_read_cluster(fs, c, cluster_buf) < 0) return -1;
        for (uint32_t off = 0; off < fs->cluster_size; off += 32) {
            fat32_dir_entry_t *entry = (fat32_dir_entry_t *)(cluster_buf + off);
            if (entry->name[0] == 0x00) break;
            if (entry->name[0] == 0xE5) continue;
            if (entry->attr == FAT32_ATTR_LFN) continue;
            uint32_t ent_cluster = entry->first_cluster_low |
                                   ((uint32_t)entry->first_cluster_high << 16);
            if (ent_cluster == file_cluster) {
                /* 只改 file_size/write_time/write_date 这几个字节，走
                 * memcpy 写进 uint8_t 缓冲区，不通过强转出来的结构体指针
                 * 赋值——见 fat32_format() 里那次严格别名死代码优化的
                 * 教训，同一个坑不重复踩。file_size 在 fat32_dir_entry_t
                 * 里的偏移量固定是 28（name[11]+attr+nt_reserved+
                 * creation_tenth+creation_time+creation_date+access_date+
                 * first_cluster_high+write_time+write_date+
                 * first_cluster_low = 11+1+1+1+2+2+2+2+2+2+2 = 28），
                 * write_time/write_date 在 22/24。顺手把修改时间刷成
                 * 当前值，文件"改过但时间戳没动"在宿主机上看着很怪。 */
                uint32_t sz = new_size;
                memcpy(cluster_buf + off + 28, &sz, 4);
                uint16_t mdate, mtime;
                fat32_now(&mdate, &mtime);
                memcpy(cluster_buf + off + 22, &mtime, 2);
                memcpy(cluster_buf + off + 24, &mdate, 2);
                return fat32_write_cluster(fs, c, cluster_buf);
            }
        }
        int next = fat32_next_cluster(fs, c);
        if (next < 0) return -1;
        c = (uint32_t)next;
    }
    return -1;
}