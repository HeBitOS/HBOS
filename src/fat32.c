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

/**
 * @brief 读一个扇区（薄封装，仅用于给返回 0/1 的老式调用点保留原语义）。
 *
 * 批量路径（整簇读写、整段目录扫描）一律直接用 block_read_sectors /
 * block_write_sectors，不要再退回逐扇区命令。
 */
static int fat32_read_sector(fat32_fs_t *fs, uint32_t lba, uint8_t *buf)
{
    (void)fs;
    return block_read_sector(lba, buf) == 0 ? 1 : 0;
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
    /* 分配游标从最低可用簇起步，保持"分配最低空闲簇"的既有语义 */
    fs->alloc_cursor = 2;
    fs->mounted = 1;
    return 0;
}

/**
 * @brief 读入整簇数据（簇内所有扇区）。
 *
 * 一次 `block_read_sectors` 取代原来的"逐扇区 512 字节命令"循环：扇区在卷上
 * 连续，块层能把 AHCI 合成单条 DMA 命令、把 USB 合成单条 SCSI 命令，命令数从
 * sectors_per_cluster（最大 64）降到 1。语义不变——读的是同一段 LBA 区间，
 * 失败仍然返回 -1，由调用者按"部分读"处理。
 */
int fat32_read_cluster(fat32_fs_t *fs, uint32_t cluster, uint8_t *buf)
{
    if (!fs || !fs->mounted || !buf) return -1;
    if (cluster < 2 || cluster >= fs->total_clusters + 2) return -1;

    uint32_t first_sector = fs->data_start_lba +
                            (cluster - 2) * fs->bpb.sectors_per_cluster;

    return block_read_sectors(first_sector, buf, fs->bpb.sectors_per_cluster) == 0
               ? 0 : -1;
}

/**
 * @brief 写出整簇数据（簇内所有扇区）。
 *
 * 与 fat32_read_cluster 对称：合并成一条块层命令。写失败时不再尝试后续扇区，
 * 与旧实现"遇错即返回 -1"的行为一致（簇内是否部分写入由块层决定，文件系统
 * 事后依靠 fs_sync/重挂载校验）。
 */
static int fat32_write_cluster(fat32_fs_t *fs, uint32_t cluster, const uint8_t *buf)
{
    if (!fs || !fs->mounted || !buf) return -1;
    if (cluster < 2 || cluster >= fs->total_clusters + 2) return -1;

    uint32_t first_sector = fs->data_start_lba +
                            (cluster - 2) * fs->bpb.sectors_per_cluster;

    return block_write_sectors(first_sector, buf, fs->bpb.sectors_per_cluster) == 0
               ? 0 : -1;
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

/**
 * @brief 分配一个空闲簇：从簇 2 起顺序找第一个 FAT 项为 0 的簇，并预置为 EOC。
 *
 * 性能要点：一个 FAT 项占 4 字节，一个扇区装得下 128 项。旧实现每检查一个
 * 簇就重读一次所在扇区（127/128 的读是纯浪费），在接近写满的卷上要发出
 * 数十万条块命令；这里改成按扇区推进——一次读入、内存里查 128 项，扇区读
 * 少 128 倍，找到的仍是同一个「最小编号空闲簇」，语义完全不变。
 *
 * 4 字节 FAT 项恒不跨扇区边界（512 % 4 == 0），故可安全地按 memcpy 取值。
 *
 * @param fs 已挂载实例
 * @return 新簇号；0 表示卷已满（0/1 为保留簇号，永不返回）
 */
static uint32_t fat32_alloc_cluster(fat32_fs_t *fs)
{
    if (!fs || !fs->mounted) return 0;

    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    const uint32_t per_sector   = BLOCK_SECTOR_SIZE / 4;   /* 每扇区 128 个 FAT 项 */
    const uint32_t last_cluster = fs->total_clusters + 2;  /* 开区间上界 */

    /* 从上次分配位置继续（见 fat32_fs_t.alloc_cursor 的注释）：旧实现每次都
     * 从簇 2 重扫，分配 n 个簇是 O(n²/128) 次 FAT 扇区读——写一个 32 MB 的
     * 内核要读上百万扇区，慢到不可用。游标让顺序分配时每个 FAT 扇区只读一次。
     * "游标之前不会留下更低空闲簇" 由释放路径下调游标保证。 */
    uint32_t start = fs->alloc_cursor;
    if (start < 2 || start >= last_cluster) start = 2;

    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t from = (pass == 0) ? start : 2;
        uint32_t to   = (pass == 0) ? last_cluster : start;
        if (pass == 1 && start == 2) break;   /* 第一圈已覆盖全部，无需回扫 */

        for (uint32_t c = from; c < to; ) {
            /* FAT 项号 == 簇号：簇 c 位于第 c/128 个 FAT 扇区的第 c%128 项。 */
            uint32_t sec_idx = c / per_sector;
            uint32_t k       = c % per_sector;
            if (block_read_sector(fs->fat_start_lba + sec_idx, sector) < 0) {
                c = (sec_idx + 1) * per_sector;            /* 读失败：跳过该扇区 */
                continue;
            }
            for (; k < per_sector; k++) {
                uint32_t clu = sec_idx * per_sector + k;
                if (clu >= to) break;
                uint32_t val;
                memcpy(&val, sector + k * 4u, 4);
                if ((val & 0x0FFFFFFF) == 0) {
                    fat32_set_cluster_entry(fs, clu, FAT32_EOC);
                    fs->alloc_cursor = clu + 1;            /* 下次从这里继续 */
                    return clu;
                }
            }
            c = (sec_idx + 1) * per_sector;
        }
    }
    return 0;
}

static void fat32_free_chain(fat32_fs_t *fs, uint32_t cluster)
{
    /* 释放后把分配游标下调到被释放的最低簇，维持"游标之前无更低空闲簇"
     * 这一不变式——否则 fat32_alloc_cluster 会跳过这些回收的空闲簇，
     * 磁盘看起来写满（见 fat32_fs_t.alloc_cursor 的注释）。 */
    uint32_t lowest = cluster;
    while (cluster >= 2 && cluster < FAT32_EOC) {
        int next = fat32_next_cluster(fs, cluster);
        fat32_set_cluster_entry(fs, cluster, 0);
        if (cluster < lowest) lowest = cluster;
        if (next < 0) break;
        cluster = (uint32_t)next;
    }
    if (lowest >= 2 && (fs->alloc_cursor == 0 || lowest < fs->alloc_cursor))
        fs->alloc_cursor = lowest;
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

    /* 单游标沿簇链前进（与 fat32_write_file 同样的线性化改法）：offset 单调
     * 递增 ⇒ cluster_num 单调不减，游标只需从头到尾走一遍簇链。旧写法每个
     * 块边界都从 first_cluster 重新走链，读一个大文件是二次方的 FAT 读。 */
    uint32_t cur_cluster = first_cluster;
    uint32_t cur_idx     = 0;                    /* cur_cluster 的簇序号 */

    while (done < count && first_cluster >= 2) {
        uint32_t abs = offset + done;
        uint32_t cluster_num = abs / fs->cluster_size;
        uint32_t cluster_off = abs % fs->cluster_size;
        uint32_t chunk = fs->cluster_size - cluster_off;
        if (chunk > count - done) chunk = count - done;

        while (cur_idx < cluster_num) {
            int next = fat32_next_cluster(fs, cur_cluster);
            if (next < 0) return (int)done;
            cur_cluster = (uint32_t)next;
            cur_idx++;
        }

        if (fat32_read_cluster(fs, cur_cluster, cluster_buf) < 0)
            return (int)done;
        memcpy(buf + done, cluster_buf + cluster_off, chunk);
        done += chunk;
    }
    return (int)done;
}

/* ─────────────────── 长文件名（LFN）支持 ───────────────────
 *
 * 背景（真缺陷）：写路径原来只会把调用者给的名字截断成 8+3 短名直接落盘，
 * 从不写 LFN 目录项。`writefile persist_test.txt ...` 于是落成 `persist_ txt`，
 * 重启后按原名 `cat persist_test.txt` 报 not found——内容一直都在盘上，只是
 * 名字被静默截断了，看起来像"文件消失了"。读路径（fat32_readdir）本来就有
 * LFN 解析，但从没有写过 LFN，所以永远读不回原名。
 *
 * 这里补齐写侧规范 LFN 条目序列 + 目录内唯一的 8.3 短名生成，并把读侧
 * （readdir 名字输出 + lookup 匹配）对齐成同一套规则：
 *   - 原名本身就是合法 8.3 大写名 → 不写 LFN，行为与旧版完全一致；
 *   - 否则写 N 条 attr=0x0F 的 LFN 项（校验和基于最终短名，>13 字符多槽，
 *     磁盘顺序为最高序号项带 0x40 标志在前、序号递减、最后紧贴短名项）；
 *   - 短名按 Windows/mtools 惯例取"基名截断 + ~N"，并在目录内查重。
 * 规范依据 fatgen103（Microsoft FAT 规范）的 Long Directory Entries 一节。
 */

#define FAT32_LFN_MAX_SLOTS 20                              /* 255 字符 / 13 字符每槽 */
#define FAT32_LFN_BUF       (FAT32_LFN_MAX_SLOTS * 13 + 1)   /* 展开后的名字缓冲 */

/* LFN 条目里 13 个 UTF-16 字符在 32 字节条目内的字节偏移 */
static const uint8_t g_lfn_char_off[13] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
};

/* 8.3 短名里合法的非字母数字字符（fatgen103 "Valid characters"） */
static int fat32_short_char_ok(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    switch (c) {
    case '_': case '$': case '~': case '!': case '#': case '%': case '&':
    case '-': case '{': case '}': case '(': case ')': case '@': case '\'':
    case '`': case '^':
        return 1;
    default:
        return 0;
    }
}

/* 按 FAT 的 8.3 语义拆名：最后一个 '.' 之后是扩展名；前导点（".foo"）不算
 * 分隔符。'-' 与 ".." 之类的名字拆出空基名，调用方据此判定不是合法 8.3。 */
static void fat32_split_83(const char *name, uint32_t *base_len,
                           const char **ext, uint32_t *ext_len)
{
    uint32_t len = (uint32_t)strlen(name);
    const char *dot = NULL;
    for (uint32_t i = 0; i < len; i++)
        if (name[i] == '.') dot = name + i;      /* 取最后一个点 */
    if (dot && dot != name && dot != name + 1) {
        *base_len = (uint32_t)(dot - name);
        *ext = dot + 1;
        *ext_len = len - *base_len - 1;
    } else if (dot && dot == name + 1) {         /* "a.b" 正常；".x" 走 else */
        *base_len = 1;
        *ext = dot + 1;
        *ext_len = len - 2;
    } else {
        *base_len = len;
        *ext = name + len;
        *ext_len = 0;
    }
}

/**
 * @brief 把 name 规整成"纯截断"候选短名（大写化 + 非法字符换成 '_'）。
 *
 * @param out            11 字节短名输出（空格补齐）
 * @param fits_83        返回是否"装得下 8.3"（基名 1..8 且扩展名 0..3）。
 *                       装得下才允许读侧用短名比较，装不下就只能靠 LFN 匹配，
 *                       否则 "persist_test.txt" 会按截断后的 "PERSIST_" 误命中
 *                       同前缀的别的文件。
 * @return 1 表示原名本身就是合法 8.3 大写名（写侧无需 LFN），0 表示需要 LFN
 */
static int fat32_plain_short_name(const char *name, uint8_t out[11], int *fits_83)
{
    uint32_t base_len, ext_len;
    const char *ext;
    fat32_split_83(name, &base_len, &ext, &ext_len);

    int exact = 1;
    memset(out, ' ', 11);
    for (uint32_t i = 0; i < base_len; i++) {
        uint8_t c = (uint8_t)name[i];
        if (c >= 'a' && c <= 'z') { c -= 32; exact = 0; }
        else if (!fat32_short_char_ok(c)) { c = '_'; exact = 0; }
        if (i < 8) out[i] = c;
        else exact = 0;                          /* 基名超长，原名装不进 8.3 */
    }
    for (uint32_t i = 0; i < ext_len; i++) {
        uint8_t c = (uint8_t)ext[i];
        if (c >= 'a' && c <= 'z') { c -= 32; exact = 0; }
        else if (!fat32_short_char_ok(c)) { c = '_'; exact = 0; }
        if (i < 3) out[8 + i] = c;
        else exact = 0;
    }
    if (base_len == 0 || base_len > 8 || ext_len > 3) exact = 0;
    if (fits_83) *fits_83 = (base_len >= 1 && base_len <= 8 && ext_len <= 3);
    return exact;
}

/* 短名是否已被目录里别的条目占用。LFN 条目（attr=0x0F）与已删除项跳过：
 * LFN 条目里带的那份短名副本不是真实条目，不能算占用。读失败按"已占用"
 * 处理——宁可换一个数字尾，也不要冒覆盖别人文件的风险。 */
static int fat32_short_name_taken(fat32_fs_t *fs, uint32_t dir_cluster,
                                  const uint8_t short_name[11])
{
    uint8_t *cluster_buf = g_fat32_cluster_buf; /* 见文件顶部注释：共享静态缓冲区 */
    uint32_t cluster = dir_cluster;

    while (cluster >= 2) {
        if (fat32_read_cluster(fs, cluster, cluster_buf) < 0) return 1;
        for (uint32_t off = 0; off < fs->cluster_size; off += 32) {
            uint8_t *raw = cluster_buf + off;
            if (raw[0] == 0x00) return 0;        /* 后面全是未用槽 */
            if (raw[0] == 0xE5) continue;        /* 已删除 */
            if (raw[11] == FAT32_ATTR_LFN) continue;
            if (raw[11] & FAT32_ATTR_VOLUME_ID) continue;
            if (memcmp(raw, short_name, 11) == 0) return 1;
        }
        int next = fat32_next_cluster(fs, cluster);
        if (next < 0) break;
        cluster = (uint32_t)next;
    }
    return 0;
}

/* 短名（11 字节，空格补齐）还原成 "NAME.EXT" 形式，用于判断"原名与短名是否
 * 完全一致"——一致就不写 LFN，保持 8.3 老行为不变。 */
static int fat32_short_matches_name(const uint8_t short_name[11], const char *name)
{
    char buf[13];
    uint32_t n = 0;
    for (uint32_t i = 0; i < 8 && short_name[i] != ' '; i++) buf[n++] = (char)short_name[i];
    if (short_name[8] != ' ') {
        buf[n++] = '.';
        for (uint32_t i = 8; i < 11 && short_name[i] != ' '; i++) buf[n++] = (char)short_name[i];
    }
    buf[n] = '\0';
    return strcmp(buf, name) == 0;
}

/**
 * @brief 生成目录内唯一的 8.3 短名（需要时同时写 LFN 保存原名）。
 *
 * 规则对齐 Windows/mtools：
 *   - 原名本身就是合法 8.3（基名 1..8、扩展名 0..3，且只含短名字符集）
 *     且目录内没被占用 → 直接用规整后的短名，**不写 LFN**。大小写差异
 *     不算"长名"（"persist.txt" 用掉约 250 字节 LFN 纯属浪费，且会让
 *     `mdir` 多显示一列），大小写不敏感查找本来就能命中；
 *   - 其余情况（基名 >8 / 扩展名 >3 / 含非法字符）→ 用"基名截断 + ~N"
 *     （N 从 1 递增，最多 6 位）在目录内查重，避免两个长名截断到同一短名
 *     时互相覆盖（persist_test.txt 与 persist_other.txt 都截成 "persist_"
 *     就是这个场景）。
 *
 * 非法字符改成 '_' 的同时必定写 LFN：折算后的短名（"a b.txt"→"A_B.TXT"）
 * 与原名不等，没有 LFN 就永远查不回来。
 */
static int fat32_make_short_name(fat32_fs_t *fs, uint32_t dir_cluster,
                                 const char *name, uint8_t out[11])
{
    int fits_83 = 0;
    fat32_plain_short_name(name, out, &fits_83);
    if (fits_83 && !fat32_short_name_taken(fs, dir_cluster, out))
        return 0;                                /* 纯 8.3：不需要 LFN */

    /* 需要 LFN：先取出规整后的基名主干与扩展名 */
    char stem[8];
    uint32_t stem_len = 0;
    for (uint32_t i = 0; i < 8 && out[i] != ' '; i++) stem[stem_len++] = (char)out[i];
    if (stem_len == 0) { stem[0] = '_'; stem_len = 1; }   /* ".bashrc" 之类 */

    uint8_t ext[3];
    uint32_t ext_len = 0;
    for (uint32_t i = 0; i < 3 && out[8 + i] != ' '; i++) ext[ext_len++] = (char)out[8 + i];

    for (uint32_t n = 1; n <= 999999; n++) {
        char num[8];
        uint32_t nd = 0;
        for (uint32_t t = n; t; t /= 10) num[nd++] = (char)('0' + (t % 10));
        for (uint32_t i = 0; i < nd / 2; i++) {  /* 反转成正常十进制 */
            char t = num[i]; num[i] = num[nd - 1 - i]; num[nd - 1 - i] = t;
        }

        uint32_t budget = 8 - 1 - nd;             /* "~N" 占 1+nd 字节 */
        uint32_t use = stem_len < budget ? stem_len : budget;
        uint8_t cand[11];
        memset(cand, ' ', 11);
        memcpy(cand, stem, use);
        cand[use] = '~';
        memcpy(cand + use + 1, num, nd);
        memcpy(cand + 8, ext, ext_len);

        if (!fat32_short_name_taken(fs, dir_cluster, cand)) {
            memcpy(out, cand, 11);
            return 0;
        }
    }
    return -1;                                   /* 同目录里 999999 个同名截断冲突 */
}

/* LFN 校验和：对 11 字节短名做 rotate-right + add（fatgen103 的算法，读侧
 * readdir/lookup 用同一函数复核，两边必须一致，否则长名会被判为"不可信"）。 */
static uint8_t fat32_lfn_checksum(const uint8_t short_name[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    return sum;
}

/**
 * @brief 解析一条 LFN 条目里的 13 个 UTF-16 字符。
 *
 * @param raw        32 字节 LFN 条目
 * @param pos        这条槽对应的原名字符偏移（(seq-1)*13）
 * @param out        展开缓冲，需能容纳 pos+13 字节
 * @param ascii_ok   非 ASCII（UTF-16 码位 >127）时被置 0，读侧据此退回短名
 * @return 这条槽里名字实际占用的字符数（0..13；遇 0x0000/0xFFFF 结束，
 *         整槽被名字填满返回 13）
 */
static uint32_t fat32_lfn_slot_chars(const uint8_t *raw, uint32_t pos, char *out,
                                     int *ascii_ok)
{
    for (uint32_t i = 0; i < 13; i++) {
        uint16_t ch = (uint16_t)raw[g_lfn_char_off[i]] |
                      ((uint16_t)raw[g_lfn_char_off[i] + 1] << 8);
        if (ch == 0x0000 || ch == 0xFFFF) return i;   /* 名字到此为止 */
        if (ch >= 128) {
            if (ascii_ok) *ascii_ok = 0;
            out[pos + i] = '?';
        } else {
            out[pos + i] = (char)ch;
        }
    }
    return 13;
}

/**
 * @brief 生成 LFN 条目序列（不含随后的短名项）。
 *
 * 磁盘顺序按规范：最高序号项先落盘且带 0x40（last）标志，序号递减，最后
 * 一条（seq=1）紧贴在短名项前面，解析方扫描时才能按 seq 归位。
 *
 * @param entries 至少 FAT32_LFN_MAX_SLOTS*32 字节
 * @return 条目数（0 表示不该写——调用方只在原名装不进 8.3 时调用）
 */
static uint32_t fat32_build_lfn_entries(const char *long_name,
                                        const uint8_t short_name[11],
                                        uint8_t *entries)
{
    uint32_t len = (uint32_t)strlen(long_name);
    if (len > 255) len = 255;
    uint32_t slots = (len + 12) / 13;
    if (slots == 0) slots = 1;
    if (slots > FAT32_LFN_MAX_SLOTS) slots = FAT32_LFN_MAX_SLOTS;

    uint8_t checksum = fat32_lfn_checksum(short_name);

    for (uint32_t s = 0; s < slots; s++) {
        uint8_t *e = entries + s * 32;
        memset(e, 0, 32);
        uint32_t seq = slots - s;
        uint32_t base = (seq - 1) * 13;
        e[0] = (uint8_t)seq;
        if (seq == slots) e[0] |= 0x40;          /* 序列里序号最高的这条先落盘 */
        e[11] = FAT32_ATTR_LFN;
        e[12] = 0;                               /* LFN 条目此字节固定 0（type） */
        e[13] = checksum;
        e[26] = 0; e[27] = 0;                    /* 簇号字段固定 0 */
        for (uint32_t i = 0; i < 13; i++) {
            uint32_t idx = base + i;
            uint16_t ch;
            if (idx < len) ch = (uint8_t)long_name[idx];
            else if (idx == len) ch = 0x0000;     /* 名字结束符 */
            else ch = 0xFFFF;                     /* 补位 */
            e[g_lfn_char_off[i]] = (uint8_t)(ch & 0xFF);
            e[g_lfn_char_off[i] + 1] = (uint8_t)(ch >> 8);
        }
    }
    return slots;
}

/**
 * @brief 把 LFN 序列 + 短名项一共 lfn_slots+1 条连续写进目录。
 *
 * 两条硬约束：
 *   1. LFN 条目必须物理紧贴在它描述的短名项前面（解析方靠顺序归位），
 *      所以整条序列要么落在同一簇里的连续空闲槽上，要么跨簇连续写；
 *   2. 写完之后，序列末尾之前**不能留下 0x00 未用槽**——0x00 是目录结束
 *      标记，解析方（内核 readdir/lookup、宿主 fsck.fat/mtools）扫到它就会
 *      掉头，后面的条目全部"消失"。目录从一簇扩到两簇时最容易踩这个坑：
 *      先把旧簇尾巴空着、只往新簇写，旧簇里那个 0x00 会让新簇的所有条目
 *      看不见（本实现在补长名支持的过程中实际踩到过：第二个目录簇里的
 *      文件写成功但读不到，fsck.fat 报 "Duplicate directory entry"）。
 *      mtools 的做法是"旧簇写满再开新簇"，这里沿用同一语义：只能从簇尾
 *      那一段连续空闲槽开始连续写，必要时把序列跨簇铺下去。
 */
static int fat32_write_dir_entries(fat32_fs_t *fs, uint32_t dir_cluster,
                                   const uint8_t *lfn_entries, uint32_t lfn_slots,
                                   const fat32_dir_entry_t *entry)
{
    if (!fs || !fs->mounted || !entry) return -1;

    uint8_t *cluster_buf = g_fat32_cluster_buf; /* 见文件顶部注释：共享静态缓冲区 */
    uint32_t need = lfn_slots + 1;
    uint32_t slots_per_cluster = fs->cluster_size / 32;
    if (slots_per_cluster == 0 || need > slots_per_cluster) return -1;

    /* 第 1 遍：优先复用某一簇内部的连续空闲槽（删除留下的洞也在此列），
     * 不动簇链。 */
    uint32_t cluster = dir_cluster;
    while (cluster >= 2) {
        if (fat32_read_cluster(fs, cluster, cluster_buf) < 0) return -1;

        uint32_t run = 0;
        for (uint32_t idx = 0; idx < slots_per_cluster; idx++) {
            uint8_t first = cluster_buf[idx * 32];
            if (first == 0x00 || first == 0xE5) {
                run++;
                if (run == need) {
                    uint32_t start = idx + 1 - need;
                    for (uint32_t i = 0; i < lfn_slots; i++)
                        memcpy(cluster_buf + (start + i) * 32, lfn_entries + i * 32, 32);
                    memcpy(cluster_buf + idx * 32, entry, 32);
                    return fat32_write_cluster(fs, cluster, cluster_buf);
                }
            } else {
                run = 0;
            }
        }

        int next = fat32_next_cluster(fs, cluster);
        if (next < 0) break;
        cluster = (uint32_t)next;
    }

    /* 第 2 遍：链尾簇的"空闲尾巴"（从最后一个已占用槽之后到簇尾）起点。
     * 从这个位置往后连续铺，必要时扩簇，保证中途不留 0x00。 */
    uint32_t last = dir_cluster;
    uint32_t tail_idx = 0;
    while (last >= 2) {
        if (fat32_read_cluster(fs, last, cluster_buf) < 0) return -1;
        uint32_t p = slots_per_cluster;
        while (p > 0) {
            uint8_t f = cluster_buf[(p - 1) * 32];
            if (f == 0x00 || f == 0xE5) p--;
            else break;
        }
        int next = fat32_next_cluster(fs, last);
        if (next < 0) { tail_idx = p; break; }
        last = (uint32_t)next;
    }
    if (last < 2) return -1;

    uint32_t placed = 0;
    uint32_t idx = tail_idx;
    uint32_t cur = last;
    int fresh = 0;                     /* 当前簇是否刚分配（整簇全 0） */
    while (placed < need) {
        if (fresh) {
            memset(cluster_buf, 0, fs->cluster_size);
        } else if (fat32_read_cluster(fs, cur, cluster_buf) < 0) {
            return -1;
        }

        while (idx < slots_per_cluster && placed < need) {
            const uint8_t *src = (placed < lfn_slots)
                                     ? lfn_entries + placed * 32
                                     : (const uint8_t *)entry;
            memcpy(cluster_buf + idx * 32, src, 32);
            idx++;
            placed++;
        }

        if (fat32_write_cluster(fs, cur, cluster_buf) < 0) return -1;
        if (placed == need) break;

        uint32_t new_clu = fat32_alloc_cluster(fs);
        if (!new_clu) return -1;
        fat32_set_cluster_entry(fs, cur, new_clu);
        cur = new_clu;
        idx = 0;
        fresh = 1;
    }
    return 0;
}

int fat32_readdir(fat32_fs_t *fs, uint32_t dir_cluster, uint32_t index,
                  char *name, uint32_t *type, uint32_t *size)
{
    if (!fs || !fs->mounted || !name || !type || !size) return -1;

    uint8_t *cluster_buf = g_fat32_cluster_buf; /* 见文件顶部注释：改用共享静态缓冲区，避免栈溢出 */
    uint32_t cluster = dir_cluster;
    uint32_t entry_idx = 0;
    int lfn = 0;                  /* 上一个条目是不是 LFN（即当前短名项有长名） */
    int lfn_ascii = 1;            /* LFN 里是否出现非 ASCII 码位（出现则退回短名） */
    uint8_t lfn_checksum = 0;     /* 当前 LFN 序列声明的校验和（短名项的校验和） */
    char lfn_buf[FAT32_LFN_BUF];
    uint32_t lfn_len = 0;         /* 展开后的长名字符数（去掉 0x0000 结束符） */

    while (cluster >= 2) {
        if (fat32_read_cluster(fs, cluster, cluster_buf) < 0) return -1;

        for (uint32_t off = 0; off < fs->cluster_size; off += 32) {
            uint8_t *raw = cluster_buf + off;
            if (raw[0] == 0x00) return -1;

            if (raw[0] == 0xE5) { lfn = 0; continue; }
            if (raw[11] == FAT32_ATTR_LFN) {
                /* 同一条短名项前面的多槽 LFN 会连续出现；旧的逐槽写法在每槽里
                 * 独立 break，第 13 个字符之后的 0x0000 会静默丢掉后续槽的
                 * 内容（"名字到 13 字符就断"）。这里改成逐字符展开整个 13 槽，
                 * 由 0x40 标志的那条决定名字总长。 */
                uint8_t seq = raw[0] & 0x3F;
                if (seq == 0 || seq > FAT32_LFN_MAX_SLOTS) { lfn = 0; continue; }
                uint32_t pos = (seq - 1) * 13;
                if (raw[0] & 0x40) lfn_ascii = 1;  /* 新序列从最高槽开始重建状态 */
                uint32_t chars = fat32_lfn_slot_chars(raw, pos, lfn_buf, &lfn_ascii);
                if (raw[0] & 0x40) {
                    lfn_len = pos + chars;       /* 最高序号槽给出精确长度 */
                    lfn_checksum = raw[13];      /* 以及校验和（对短名项的） */
                    lfn = 1;                     /* 但校验和要等短名项到了才验 */
                }
                continue;
            }

            fat32_dir_entry_t *entry = (fat32_dir_entry_t *)raw;
            if (entry->attr & FAT32_ATTR_VOLUME_ID) continue;
            if (entry->name[0] == '.') continue;

            /* LFN 只有在"校验和匹配 + 同一序列 + 全是 ASCII"时才可信：
             * 校验和基于它自己的 8.3 短名，写侧/读侧用同一个算法，能挡住
             * 被误认的槽（例如目录里残留的旧 LFN 条目）。 */
            int lfn_ok = lfn && lfn_ascii && lfn_len > 0 &&
                         fat32_lfn_checksum(entry->name) == lfn_checksum;
            if (entry_idx == index) {
                if (lfn_ok) {
                    uint32_t nlen = lfn_len;
                    if (nlen > 63) nlen = 63;
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

/* 取出短名项指向的簇号/大小/属性。走 memcpy 而不是把 uint8_t* 强转成
 * fat32_dir_entry_t*：见文件顶部关于 -O2 严格别名死代码优化的注释。 */
static void fat32_entry_out(const uint8_t *raw, uint32_t *out_cluster,
                            uint32_t *out_size, uint8_t *out_attr)
{
    fat32_dir_entry_t e;
    memcpy(&e, raw, sizeof(e));
    *out_cluster = (uint32_t)e.first_cluster_low |
                   ((uint32_t)e.first_cluster_high << 16);
    if (out_size) *out_size = e.file_size;
    if (out_attr) *out_attr = e.attr;
}

/**
 * @brief 在目录里按名字定位条目（大小写不敏感）。
 *
 * 与写路径对称：写侧落盘时，名字装得进 8.3 的按短名匹配（老行为，含
 * "persist.txt" 这类原名大小写不同的情况），装不进 8.3 的（基名 >8 / 扩展名
 * >3 / 含非法字符）必须靠 LFN 原名匹配——绝不能用截断后的短名去比，否则
 * "persist_test.txt" 会命中同前缀的 "persist_other.txt"。两种情况都排在
 * "条目自己的长名"上比较，且 LFN 需要校验和与短名项一致才可信。
 */
int fat32_lookup(fat32_fs_t *fs, uint32_t dir_cluster, const char *name,
                 uint32_t *out_cluster, uint32_t *out_size, uint8_t *out_attr)
{
    if (!fs || !fs->mounted || !name || !out_cluster) return -1;

    uint8_t *cluster_buf = g_fat32_cluster_buf; /* 见文件顶部注释：改用共享静态缓冲区，避免栈溢出 */
    uint32_t cluster = dir_cluster;

    /* 目标名：装得进 8.3 就规整成短名（大写化/非法字符换 '_'），否则只保留
     * 原名走 LFN 比较。 */
    int want_fits_83 = 0;
    uint8_t short_name[11];
    fat32_plain_short_name(name, short_name, &want_fits_83);

    uint32_t name_len = (uint32_t)strlen(name);
    int lfn = 0;
    int lfn_ascii = 1;
    uint8_t lfn_checksum = 0;
    char lfn_buf[FAT32_LFN_BUF];
    uint32_t lfn_len = 0;

    while (cluster >= 2) {
        if (fat32_read_cluster(fs, cluster, cluster_buf) < 0) return -1;

        for (uint32_t off = 0; off < fs->cluster_size; off += 32) {
            uint8_t *raw = cluster_buf + off;
            if (raw[0] == 0x00) return -1;
            if (raw[0] == 0xE5) { lfn = 0; continue; }
            if (raw[11] == FAT32_ATTR_LFN) {
                /* 同 fat32_readdir：整条 13 槽展开，长度由带 0x40 的那条给出，
                 * 不能逐槽 break（会把 >13 字符的名字在第 13 个字符处截断）。 */
                uint8_t seq = raw[0] & 0x3F;
                if (seq == 0 || seq > FAT32_LFN_MAX_SLOTS) { lfn = 0; continue; }
                uint32_t pos = (seq - 1) * 13;
                if (raw[0] & 0x40) lfn_ascii = 1;  /* 新序列从最高槽开始重建状态 */
                uint32_t chars = fat32_lfn_slot_chars(raw, pos, lfn_buf, &lfn_ascii);
                if (raw[0] & 0x40) {
                    lfn_len = pos + chars;
                    lfn_checksum = raw[13];
                    lfn = 1;
                }
                continue;
            }

            uint8_t attr = raw[11];
            if (attr & FAT32_ATTR_VOLUME_ID) continue;
            if (raw[0] == '.') continue;            /* "." / ".." */

            /* 短名匹配：LFN 条目、以及宏为 '.' 的项已经在上面被跳过，且只有
             * 目标名装得进 8.3 时才允许——避免长名被截断后误命中。 */
            if (want_fits_83 && memcmp(raw, short_name, 11) == 0) {
                fat32_entry_out(raw, out_cluster, out_size, out_attr);
                return 0;
            }

            /* LFN 原名匹配（大小写不敏感，与短名路径的语义一致） */
            int lfn_ok = lfn && lfn_ascii &&
                         fat32_lfn_checksum(raw) == lfn_checksum;
            if (lfn_ok && lfn_len == name_len) {
                uint32_t i = 0;
                for (; i < name_len; i++) {
                    char a = lfn_buf[i], b = name[i];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) break;
                }
                if (i == name_len) {
                    fat32_entry_out(raw, out_cluster, out_size, out_attr);
                    return 0;
                }
            }
            lfn = 0;
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

    /* 逐簇前进的游标：cluster_num 在外层循环里单调递增，因此游标可以就地
     * 跟着走。旧写法每处理一个簇都从 first_cluster 重新走一遍整条链，写
     * n 个簇要做 1+2+…+n 次 FAT 读（1 MiB / 4 KiB 簇 = 256 簇即 32896 次）；
     * 改成单游标后总 FAT 读与簇数成正比，写大文件的耗时从二次方降为线性。 */
    uint32_t cur_cluster = first_cluster;
    uint32_t cur_idx     = 0;                    /* cur_cluster 对应的簇序号 */

    while (done < count) {
        uint32_t abs = offset + done;
        uint32_t cluster_num = abs / fs->cluster_size;
        uint32_t cluster_off = abs % fs->cluster_size;
        uint32_t chunk = fs->cluster_size - cluster_off;
        if (chunk > count - done) chunk = count - done;

        while (cur_idx < cluster_num) {
            int next = fat32_next_cluster(fs, cur_cluster);
            if (next < 0) return -1;
            cur_cluster = (uint32_t)next;
            cur_idx++;
        }

        if (chunk < fs->cluster_size) {
            /* 部分簇写：先读回整簇，改中间一段再整簇写回（读-改-写） */
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

    /* 短名：装得进 8.3 且目录内没占用就用原名（老行为不变）；否则按
     * "截断 + ~N" 生成一个目录内唯一的短名，原名交给下面的 LFN 序列保存。 */
    uint8_t dir_name[11];
    if (fat32_make_short_name(fs, dir_cluster, name, dir_name) < 0) {
        fat32_free_chain(fs, new_clu);
        return -1;
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

    /* 原名 != 短名 ⇒ 原名装不进 8.3，必须在短名项**前面**写 LFN 序列把原名
     * 连同存盘：不写的话重启后按原名查不到，文件就像"消失"了（本缺陷的根因）。 */
    uint8_t lfn_entries[FAT32_LFN_MAX_SLOTS * 32];
    uint32_t lfn_slots = 0;
    if (!fat32_short_matches_name(dir_name, name))
        lfn_slots = fat32_build_lfn_entries(name, dir_name, lfn_entries);

    if (fat32_write_dir_entries(fs, dir_cluster, lfn_entries, lfn_slots, &entry) < 0) {
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
                /* 紧贴在这个短名项前面的连续 LFN 槽一并标删：留着的话它们会被
                 * 后续解析当成下一个条目（或下一个条目的）长名前缀。文件系统对
                 * 孤儿 LFN 的通行做法也是清除（get_long_entry_slots 同款）。 */
                uint32_t offp = off;
                while (offp >= 32 && cluster_buf[offp - 32] != 0x00 &&
                       cluster_buf[offp - 32] != 0xE5 &&
                       cluster_buf[offp - 32 + 11] == FAT32_ATTR_LFN) {
                    offp -= 32;
                    cluster_buf[offp] = 0xE5;
                }
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

    /* 目录短名同 fat32_create_file 的 8.3 拆分方式（唯一化 + 需要时写 LFN） */
    uint8_t dir_name[11];
    if (fat32_make_short_name(fs, dir_cluster, name, dir_name) < 0) {
        fat32_free_chain(fs, new_clu);
        return -1;
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

    /* 目录名装不进 8.3 时同样写 LFN 序列（否则重启后按原名 cd 不进去） */
    uint8_t lfn_entries[FAT32_LFN_MAX_SLOTS * 32];
    uint32_t lfn_slots = 0;
    if (!fat32_short_matches_name(dir_name, name))
        lfn_slots = fat32_build_lfn_entries(name, dir_name, lfn_entries);

    if (fat32_write_dir_entries(fs, dir_cluster, lfn_entries, lfn_slots, &entry) < 0) {
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
                /* 紧贴在这个短名项前面的连续 LFN 槽必须一并标删：留着就成了
                 * "孤儿 LFN"——fsck.fat 会报 `Orphaned long file name part`
                 * 并自动删除，长名目录被 rmdir 后磁盘就不再是规范合规状态。
                 * 与 fat32_delete_file() 的处理保持一致（那边已经这么做了）。 */
                uint32_t offp = off;
                while (offp >= 32 && cluster_buf[offp - 32] != 0x00 &&
                       cluster_buf[offp - 32] != 0xE5 &&
                       cluster_buf[offp - 32 + 11] == FAT32_ATTR_LFN) {
                    offp -= 32;
                    cluster_buf[offp] = 0xE5;
                }
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