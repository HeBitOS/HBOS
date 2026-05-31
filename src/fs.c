#include "block.h"
#include "fs.h"
#include "string.h"

#define HBFS_MAGIC 0x3146534248ULL /* "HBFS1" little-endian-ish marker */
#define HBFS_VERSION 1
#define HBFS_DEFAULT_START_LBA 2048
#define HBFS_TABLE_SECTORS 8
#define HBFS_FILE_SECTORS (RAMFS_MAX_FILE_SIZE / BLOCK_SECTOR_SIZE)
#define HBFS_PARTITION_TYPE 0xEB
#define GPT_ENTRY_SIZE 128
#define GPT_ENTRY_COUNT 128
#define GPT_ENTRY_SECTORS 32

typedef enum {
    FS_BACKEND_RAM = 0,
    FS_BACKEND_HBFS,
} fs_backend_t;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t start_lba;
    uint32_t max_files;
    uint32_t max_file_size;
    uint32_t table_lba;
    uint32_t table_sectors;
    uint32_t data_lba;
    uint32_t file_sectors;
    uint8_t reserved[BLOCK_SECTOR_SIZE - 40];
} __attribute__((packed)) hbfs_super_t;

typedef struct {
    uint8_t used;
    uint8_t type;
    uint16_t reserved0;
    uint32_t size;
    char name[MAX_FILENAME];
    uint8_t reserved1[24];
} __attribute__((packed)) hbfs_entry_t;

// 全局文件系统实例
static filesystem_t fs;
static fs_backend_t fs_backend = FS_BACKEND_RAM;
static uint8_t ramfs_storage[MAX_FILES][RAMFS_MAX_FILE_SIZE];
static hbfs_entry_t hbfs_table[MAX_FILES];
static uint32_t hbfs_start_lba = HBFS_DEFAULT_START_LBA;
static uint32_t hbfs_total_sectors = 1 + HBFS_TABLE_SECTORS + MAX_FILES * HBFS_FILE_SECTORS;
static const char *fs_error = "ok";

static int ramfs_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count);
static int ramfs_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count);
static int ramfs_vfs_truncate(vfs_node_t *node);
static int ramfs_vfs_unlink(vfs_node_t *node);

static int fs_fail(const char *msg) {
    fs_error = msg;
    return -1;
}

const char *fs_last_error(void) {
    return fs_error;
}

static const vfs_ops_t ramfs_ops = {
    .read = ramfs_vfs_read,
    .write = ramfs_vfs_write,
    .truncate = ramfs_vfs_truncate,
    .unlink = ramfs_vfs_unlink,
};

static uint32_t hbfs_table_lba(void) {
    return hbfs_start_lba + 1;
}

static uint32_t hbfs_data_lba(void) {
    return hbfs_start_lba + 1 + HBFS_TABLE_SECTORS;
}

static uint32_t hbfs_needed_sectors(void) {
    return 1 + HBFS_TABLE_SECTORS + MAX_FILES * HBFS_FILE_SECTORS;
}

uint32_t fs_min_sectors(void) {
    return hbfs_needed_sectors();
}

static const char *normalize_path(const char *path, char *out) {
    if (!path) return NULL;
    while (*path == '/') path++;
    if (*path == '\0') return NULL;

    uint32_t i = 0;
    while (path[i]) {
        if (path[i] == '/') return NULL;
        if (i >= MAX_FILENAME - 1) return NULL;
        out[i] = path[i];
        i++;
    }
    out[i] = '\0';
    return out;
}

static void fs_reset_ram(void) {
    fs.file_count = 0;
    fs.total_sectors = 0;
    fs_backend = FS_BACKEND_RAM;
    hbfs_start_lba = HBFS_DEFAULT_START_LBA;
    hbfs_total_sectors = hbfs_needed_sectors();
    memset(hbfs_table, 0, sizeof(hbfs_table));

    for (uint32_t i = 0; i < MAX_FILES; i++) {
        file_t *f = &fs.files[i];
        f->name[0] = '\0';
        f->size = 0;
        f->capacity = RAMFS_MAX_FILE_SIZE;
        f->data = ramfs_storage[i];
        f->node.name[0] = '\0';
        f->node.type = VFS_NODE_FILE;
        f->node.size = 0;
        f->node.capacity = RAMFS_MAX_FILE_SIZE;
        f->node.private_data = f;
        f->node.ops = &ramfs_ops;
        f->disk_slot = i;
        f->used = 0;
        f->type = 0;
    }
}

static void hbfs_fill_super(hbfs_super_t *sb) {
    memset(sb, 0, sizeof(*sb));
    sb->magic = HBFS_MAGIC;
    sb->version = HBFS_VERSION;
    sb->start_lba = hbfs_start_lba;
    sb->max_files = MAX_FILES;
    sb->max_file_size = RAMFS_MAX_FILE_SIZE;
    sb->table_lba = hbfs_table_lba();
    sb->table_sectors = HBFS_TABLE_SECTORS;
    sb->data_lba = hbfs_data_lba();
    sb->file_sectors = HBFS_FILE_SECTORS;
}

static int hbfs_read_super(hbfs_super_t *sb) {
    return block_read_sector(hbfs_start_lba, (uint8_t *)sb);
}

static int hbfs_write_super(void) {
    hbfs_super_t sb;
    hbfs_fill_super(&sb);
    return block_write_sector(hbfs_start_lba, (const uint8_t *)&sb);
}

static int hbfs_validate_super(const hbfs_super_t *sb) {
    return sb->magic == HBFS_MAGIC &&
           sb->version == HBFS_VERSION &&
           sb->max_files == MAX_FILES &&
           sb->max_file_size == RAMFS_MAX_FILE_SIZE &&
           sb->start_lba == hbfs_start_lba &&
           sb->table_lba == hbfs_table_lba() &&
           sb->data_lba == hbfs_data_lba() &&
           sb->file_sectors == HBFS_FILE_SECTORS;
}

static uint32_t hbfs_file_lba(uint32_t slot, uint32_t sector) {
    return hbfs_data_lba() + slot * HBFS_FILE_SECTORS + sector;
}

static int hbfs_sync_table(void) {
    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    const uint8_t *table = (const uint8_t *)hbfs_table;
    for (uint32_t s = 0; s < HBFS_TABLE_SECTORS; s++) {
        memcpy(sector, table + s * BLOCK_SECTOR_SIZE, BLOCK_SECTOR_SIZE);
        if (block_write_sector(hbfs_table_lba() + s, sector) < 0)
            return -1;
    }
    return 0;
}

static int hbfs_load_table(void) {
    uint8_t *table = (uint8_t *)hbfs_table;
    for (uint32_t s = 0; s < HBFS_TABLE_SECTORS; s++) {
        if (block_read_sector(hbfs_table_lba() + s, table + s * BLOCK_SECTOR_SIZE) < 0)
            return -1;
    }
    return 0;
}

static void hbfs_rebuild_files_from_table(void) {
    fs.file_count = 0;
    for (uint32_t i = 0; i < MAX_FILES; i++) {
        file_t *f = &fs.files[i];
        hbfs_entry_t *e = &hbfs_table[i];
        f->used = e->used ? 1 : 0;
        f->type = e->type;
        f->size = e->size;
        f->capacity = RAMFS_MAX_FILE_SIZE;
        f->data = NULL;
        f->disk_slot = i;
        f->node.type = VFS_NODE_FILE;
        f->node.size = e->size;
        f->node.capacity = RAMFS_MAX_FILE_SIZE;
        f->node.private_data = f;
        f->node.ops = &ramfs_ops;

        if (f->used) {
            strcpy(f->name, e->name);
            strcpy(f->node.name, e->name);
            fs.file_count++;
        } else {
            f->name[0] = '\0';
            f->node.name[0] = '\0';
            f->node.size = 0;
        }
    }
}

static int hbfs_update_entry(file_t *f) {
    if (!f || f->disk_slot >= MAX_FILES) return -1;
    hbfs_entry_t *e = &hbfs_table[f->disk_slot];
    memset(e, 0, sizeof(*e));
    if (f->used) {
        e->used = 1;
        e->type = f->type;
        e->size = f->size;
        strcpy(e->name, f->name);
    }
    return hbfs_sync_table();
}

static uint32_t hbfs_read_data(file_t *f, uint32_t offset, void *buf, uint32_t count) {
    if (!f || !f->used || !buf) return 0;
    if (offset >= f->size) return 0;
    uint32_t available = f->size - offset;
    if (count > available) count = available;

    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t abs = offset + done;
        uint32_t sector_index = abs / BLOCK_SECTOR_SIZE;
        uint32_t sector_off = abs % BLOCK_SECTOR_SIZE;
        uint32_t chunk = BLOCK_SECTOR_SIZE - sector_off;
        if (chunk > count - done) chunk = count - done;
        if (block_read_sector(hbfs_file_lba(f->disk_slot, sector_index), sector) < 0)
            return done;
        memcpy(out + done, sector + sector_off, chunk);
        done += chunk;
    }
    return done;
}

static int hbfs_write_data(file_t *f, uint32_t offset, const void *buf, uint32_t count) {
    if (!f || !f->used || (!buf && count)) return -1;
    if (offset > f->capacity || count > f->capacity - offset) return -1;

    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t abs = offset + done;
        uint32_t sector_index = abs / BLOCK_SECTOR_SIZE;
        uint32_t sector_off = abs % BLOCK_SECTOR_SIZE;
        uint32_t chunk = BLOCK_SECTOR_SIZE - sector_off;
        if (chunk > count - done) chunk = count - done;

        if (sector_off != 0 || chunk != BLOCK_SECTOR_SIZE) {
            if (block_read_sector(hbfs_file_lba(f->disk_slot, sector_index), sector) < 0)
                memset(sector, 0, sizeof(sector));
        } else {
            memset(sector, 0, sizeof(sector));
        }
        memcpy(sector + sector_off, in + done, chunk);
        if (block_write_sector(hbfs_file_lba(f->disk_slot, sector_index), sector) < 0)
            return -1;
        done += chunk;
    }

    uint32_t end = offset + count;
    if (end > f->size) {
        f->size = end;
        f->node.size = end;
        if (hbfs_update_entry(f) < 0) return -1;
    }
    return (int)count;
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p) {
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static int guid_eq(const uint8_t *a, const uint8_t *b) {
    for (uint32_t i = 0; i < 16; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static uint8_t gpt_type_to_mbr_type(const uint8_t *guid) {
    static const uint8_t esp_guid[16] = {
        0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
        0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
    };
    static const uint8_t hbfs_guid[16] = {
        0x53, 0x46, 0x42, 0x48, 0x00, 0x00, 0x00, 0x40,
        0x80, 0x00, 0x48, 0x42, 0x4f, 0x53, 0x00, 0x01
    };
    static const uint8_t zero_guid[16] = {0};

    if (guid_eq(guid, zero_guid)) return 0;
    if (guid_eq(guid, esp_guid)) return 0xEF;
    if (guid_eq(guid, hbfs_guid)) return HBFS_PARTITION_TYPE;
    return 0xEE;
}

static int read_gpt_entries(fs_partition_info_t out[4], uint32_t *hbfs_start, uint32_t *hbfs_sectors) {
    uint8_t sector[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    if (block_read_sector(1, sector) < 0) return -1;
    if (sector[0] != 'E' || sector[1] != 'F' || sector[2] != 'I' || sector[3] != ' ' ||
        sector[4] != 'P' || sector[5] != 'A' || sector[6] != 'R' || sector[7] != 'T') {
        return -1;
    }

    uint64_t entries_lba = get_le64(sector + 72);
    uint32_t entry_count = get_le32(sector + 80);
    uint32_t entry_size = get_le32(sector + 84);
    if (entries_lba == 0 || entry_size < GPT_ENTRY_SIZE) return -1;
    if (entry_count > GPT_ENTRY_COUNT) entry_count = GPT_ENTRY_COUNT;

    uint32_t shown = 0;
    uint8_t entries[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    for (uint32_t i = 0; i < entry_count; i++) {
        uint32_t sector_index = (i * entry_size) / BLOCK_SECTOR_SIZE;
        uint32_t sector_off = (i * entry_size) % BLOCK_SECTOR_SIZE;
        if (sector_off + GPT_ENTRY_SIZE > BLOCK_SECTOR_SIZE) return -1;
        if (block_read_sector((uint32_t)entries_lba + sector_index, entries) < 0) return -1;

        uint8_t type = gpt_type_to_mbr_type(entries + sector_off);
        if (type == 0) continue;

        uint64_t first = get_le64(entries + sector_off + 32);
        uint64_t last = get_le64(entries + sector_off + 40);
        if (first == 0 || last < first || first > 0xFFFFFFFFULL) continue;
        uint64_t count64 = last - first + 1;
        if (count64 > 0xFFFFFFFFULL) count64 = 0xFFFFFFFFULL;

        if (shown < 4 && out) {
            out[shown].present = 1;
            out[shown].bootable = (type == 0xEF);
            out[shown].type = type;
            out[shown].start_lba = (uint32_t)first;
            out[shown].sectors = (uint32_t)count64;
            shown++;
        }

        if (type == HBFS_PARTITION_TYPE && count64 >= hbfs_needed_sectors()) {
            if (hbfs_start) *hbfs_start = (uint32_t)first;
            if (hbfs_sectors) *hbfs_sectors = (uint32_t)count64;
        }
    }
    return shown > 0 ? 0 : -1;
}

static int hbfs_find_mbr_partition(uint32_t *start, uint32_t *sectors) {
    uint8_t mbr[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    if (block_read_sector(0, mbr) < 0) return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return -1;

    if ((mbr + 446)[4] == 0xEE && read_gpt_entries(NULL, start, sectors) == 0)
        return 0;

    for (uint32_t i = 0; i < 4; i++) {
        uint8_t *e = mbr + 446 + i * 16;
        if (e[4] != HBFS_PARTITION_TYPE) continue;
        uint32_t lba = get_le32(e + 8);
        uint32_t cnt = get_le32(e + 12);
        if (lba == 0 || cnt < hbfs_needed_sectors()) continue;
        *start = lba;
        *sectors = cnt;
        return 0;
    }
    return -1;
}

int fs_read_partitions(fs_partition_info_t out[4]) {
    if (!out) return -1;
    for (uint32_t i = 0; i < 4; i++) {
        out[i].present = 0;
        out[i].bootable = 0;
        out[i].type = 0;
        out[i].start_lba = 0;
        out[i].sectors = 0;
    }

    if (block_init() < 0) return -1;

    uint8_t mbr[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    if (block_read_sector(0, mbr) < 0) return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;

    if ((mbr + 446)[4] == 0xEE && read_gpt_entries(out, NULL, NULL) == 0)
        return 0;

    for (uint32_t i = 0; i < 4; i++) {
        uint8_t *e = mbr + 446 + i * 16;
        uint32_t lba = get_le32(e + 8);
        uint32_t cnt = get_le32(e + 12);
        if (e[4] == 0 || cnt == 0) continue;
        out[i].present = 1;
        out[i].bootable = (e[0] == 0x80);
        out[i].type = e[4];
        out[i].start_lba = lba;
        out[i].sectors = cnt;
    }
    return 0;
}

static void write_mbr_entry(uint8_t *e, uint8_t bootable, uint8_t type,
                            uint32_t start, uint32_t sectors) {
    e[0] = bootable ? 0x80 : 0x00;
    e[1] = 0x01; e[2] = 0x01; e[3] = 0x00;
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    put_le32(e + 8, start);
    put_le32(e + 12, sectors);
}

static int ranges_overlap(uint32_t a_start, uint32_t a_count,
                          uint32_t b_start, uint32_t b_count) {
    uint64_t a_end = (uint64_t)a_start + a_count;
    uint64_t b_end = (uint64_t)b_start + b_count;
    return a_start < b_end && b_start < a_end;
}

static int hbfs_write_mbr_partition(uint32_t start, uint32_t sectors) {
    uint8_t mbr[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    if (block_read_sector(0, mbr) < 0 || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        memset(mbr, 0, sizeof(mbr));
    }

    int slot = -1;
    int empty = -1;
    for (uint32_t i = 0; i < 4; i++) {
        uint8_t *e = mbr + 446 + i * 16;
        uint8_t type = e[4];
        uint32_t lba = get_le32(e + 8);
        uint32_t cnt = get_le32(e + 12);
        if (type == HBFS_PARTITION_TYPE) {
            slot = (int)i;
            break;
        }
        if ((type == 0 || cnt == 0) && empty < 0) {
            empty = (int)i;
            continue;
        }
        if (cnt != 0 && ranges_overlap(start, sectors, lba, cnt))
            return -1;
    }
    if (slot < 0) slot = empty;
    if (slot < 0) return -1;

    write_mbr_entry(mbr + 446 + (uint32_t)slot * 16, 0, HBFS_PARTITION_TYPE, start, sectors);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    return block_write_sector(0, mbr);
}

static uint32_t align_up(uint32_t value, uint32_t align) {
    return (value + align - 1) & ~(align - 1);
}

static int hbfs_choose_install_range(uint32_t *start, uint32_t *sectors) {
    uint32_t total = block_sector_count();
    uint32_t min = hbfs_needed_sectors();
    if (total == 0) return fs_fail("未检测到可写硬盘");
    if (total <= HBFS_DEFAULT_START_LBA + min) return fs_fail("硬盘空间不足");

    if (hbfs_find_mbr_partition(start, sectors) == 0) return 0;

    fs_partition_info_t parts[4];
    uint32_t candidate = HBFS_DEFAULT_START_LBA;
    if (fs_read_partitions(parts) == 0) {
        for (uint32_t pass = 0; pass < 4; pass++) {
            uint32_t best_start = 0xFFFFFFFFU;
            uint32_t best_end = 0;
            for (uint32_t i = 0; i < 4; i++) {
                if (!parts[i].present) continue;
                uint32_t end = parts[i].start_lba + parts[i].sectors;
                if (parts[i].start_lba >= candidate && parts[i].start_lba < best_start) {
                    best_start = parts[i].start_lba;
                    best_end = end;
                }
            }
            if (best_start == 0xFFFFFFFFU) break;
            if (candidate + min <= best_start) break;
            candidate = align_up(best_end, 2048);
        }
    }

    if (candidate + min > total) return fs_fail("没有可用分区空间");
    *start = candidate;
    *sectors = total - candidate;
    return 0;
}

static int hbfs_range_valid(uint32_t start, uint32_t sectors) {
    uint32_t total = block_sector_count();
    if (start < 2048) return 0;
    if (sectors < hbfs_needed_sectors()) return 0;
    if (start >= total) return 0;
    if (sectors > total - start) return 0;
    return 1;
}

static int hbfs_format_range(uint32_t start, uint32_t sectors) {
    if (!hbfs_range_valid(start, sectors)) return fs_fail("HBFS 分区范围无效");
    hbfs_start_lba = start;
    hbfs_total_sectors = sectors;

    memset(hbfs_table, 0, sizeof(hbfs_table));
    if (hbfs_write_super() < 0) return fs_fail("写入超级块失败");
    if (hbfs_sync_table() < 0) return fs_fail("写入文件表失败");
    return fs_mount_disk();
}

// 文件系统初始化
int fs_init(void) {
    fs_reset_ram();
    (void)fs_mount_disk();
    return 1;
}

int fs_check(fs_check_result_t *out) {
    fs_check_result_t r;
    memset(&r, 0, sizeof(r));
    r.capacity_bytes = fs_capacity_bytes();
    r.file_count = fs.file_count;
    r.first_error = "ok";

    uint32_t seen_names = 0;
    for (uint32_t i = 0; i < MAX_FILES; i++) {
        file_t *f = &fs.files[i];
        if (!f->used) continue;
        r.files_seen++;
        r.used_bytes += f->size;

        if (!f->name[0]) {
            if (r.errors++ == 0) r.first_error = "empty file name";
        }
        if (f->size > f->capacity || f->capacity != RAMFS_MAX_FILE_SIZE) {
            if (r.errors++ == 0) r.first_error = "invalid file size";
        }
        if (f->node.private_data != f || f->node.ops != &ramfs_ops) {
            if (r.errors++ == 0) r.first_error = "invalid vfs node";
        }
        if (f->node.size != f->size) {
            if (r.errors++ == 0) r.first_error = "vfs size mismatch";
        }
        for (uint32_t j = i + 1; j < MAX_FILES; j++) {
            if (fs.files[j].used && strcmp(f->name, fs.files[j].name) == 0) {
                if (r.errors++ == 0) r.first_error = "duplicate file name";
            }
        }
        seen_names++;
    }

    if (seen_names != fs.file_count) {
        if (r.errors++ == 0) r.first_error = "file count mismatch";
    }

    if (fs_backend == FS_BACKEND_HBFS) {
        hbfs_super_t sb;
        if (hbfs_read_super(&sb) < 0 || !hbfs_validate_super(&sb)) {
            if (r.errors++ == 0) r.first_error = "invalid hbfs superblock";
        }
        for (uint32_t i = 0; i < MAX_FILES; i++) {
            if (fs.files[i].used != (hbfs_table[i].used ? 1 : 0)) {
                if (r.errors++ == 0) r.first_error = "hbfs table mismatch";
            }
            if (fs.files[i].used && hbfs_table[i].size != fs.files[i].size) {
                if (r.errors++ == 0) r.first_error = "hbfs size mismatch";
            }
        }
    }

    if (out) *out = r;
    return r.errors == 0 ? 0 : -1;
}

int fs_format_disk(void) {
    fs_error = "ok";
    if (block_init() < 0) return fs_fail("未检测到可写硬盘");
    uint32_t start = HBFS_DEFAULT_START_LBA;
    uint32_t sectors = block_sector_count() > start ? block_sector_count() - start : 0;
    (void)hbfs_find_mbr_partition(&start, &sectors);
    return hbfs_format_range(start, sectors);
}

int fs_install_disk(void) {
    fs_error = "ok";
    if (block_init() < 0) return fs_fail("未检测到可写硬盘");
    uint32_t start = 0;
    uint32_t sectors = 0;
    if (hbfs_find_mbr_partition(&start, &sectors) == 0)
        return hbfs_format_range(start, sectors);
    if (hbfs_choose_install_range(&start, &sectors) < 0) return -1;
    return fs_install_disk_at(start, sectors);
}

int fs_install_disk_at(uint32_t start, uint32_t sectors) {
    fs_error = "ok";
    if (block_init() < 0) return fs_fail("未检测到可写硬盘");
    if (!hbfs_range_valid(start, sectors)) return fs_fail("HBFS 分区范围无效");
    if (hbfs_write_mbr_partition(start, sectors) < 0) return fs_fail("写入分区表失败");
    return hbfs_format_range(start, sectors);
}

int fs_mount_disk(void) {
    if (block_init() < 0) return -1;
    uint32_t start = HBFS_DEFAULT_START_LBA;
    uint32_t sectors = block_sector_count() > start ? block_sector_count() - start : 0;
    (void)hbfs_find_mbr_partition(&start, &sectors);
    if (!hbfs_range_valid(start, sectors)) return -1;
    hbfs_start_lba = start;
    hbfs_total_sectors = sectors;

    hbfs_super_t sb;
    if (hbfs_read_super(&sb) < 0) return -1;
    if (!hbfs_validate_super(&sb)) return -1;
    if (hbfs_load_table() < 0) return -1;

    fs_backend = FS_BACKEND_HBFS;
    hbfs_rebuild_files_from_table();
    fs.total_sectors = hbfs_total_sectors;
    return 0;
}

int fs_sync(void) {
    if (fs_backend != FS_BACKEND_HBFS) return 0;
    return hbfs_sync_table();
}

int fs_is_disk(void) {
    return fs_backend == FS_BACKEND_HBFS;
}

const char *fs_backend_name(void) {
    static char name[16];
    if (fs_backend != FS_BACKEND_HBFS) return "ramfs";
    const char *blk = block_backend_name();
    name[0] = 'h'; name[1] = 'b'; name[2] = 'f'; name[3] = 's'; name[4] = '/';
    uint32_t i = 0;
    while (blk[i] && i < sizeof(name) - 6) {
        name[5 + i] = blk[i];
        i++;
    }
    name[5 + i] = '\0';
    return name;
}

uint32_t fs_disk_start_lba(void) {
    return hbfs_start_lba;
}

uint32_t fs_disk_total_sectors(void) {
    return hbfs_total_sectors;
}

uint32_t fs_capacity_bytes(void) {
    return MAX_FILES * RAMFS_MAX_FILE_SIZE;
}

uint32_t fs_used_bytes(void) {
    uint32_t used = 0;
    for (uint32_t i = 0; i < MAX_FILES; i++) {
        if (fs.files[i].used) used += fs.files[i].size;
    }
    return used;
}

// 列出文件
void fs_list(void) {
    // Shell commands use fs_get_count/fs_get_file to render lists.
}

// 查找文件
file_t *fs_find_file(const char *name) {
    char norm[MAX_FILENAME];
    if (!normalize_path(name, norm)) return NULL;
    for (uint32_t i = 0; i < MAX_FILES; i++) {
        if (fs.files[i].used && strcmp(fs.files[i].name, norm) == 0)
            return &fs.files[i];
    }
    return NULL;
}

file_t *fs_create_file(const char *name) {
    char norm[MAX_FILENAME];
    if (!normalize_path(name, norm)) return NULL;
    file_t *existing = fs_find_file(norm);
    if (existing) return existing;

    for (uint32_t i = 0; i < MAX_FILES; i++) {
        file_t *f = &fs.files[i];
        if (f->used) continue;
        strcpy(f->name, norm);
        f->size = 0;
        f->capacity = RAMFS_MAX_FILE_SIZE;
        f->data = (fs_backend == FS_BACKEND_HBFS) ? NULL : ramfs_storage[i];
        f->node.type = VFS_NODE_FILE;
        f->node.size = 0;
        f->node.capacity = RAMFS_MAX_FILE_SIZE;
        f->node.private_data = f;
        f->node.ops = &ramfs_ops;
        strcpy(f->node.name, norm);
        f->disk_slot = i;
        f->used = 1;
        f->type = 0;
        fs.file_count++;
        if (fs_backend == FS_BACKEND_HBFS && hbfs_update_entry(f) < 0) {
            f->used = 0;
            fs.file_count--;
            return NULL;
        }
        return f;
    }
    return NULL;
}

int fs_delete_file(const char *name) {
    file_t *f = fs_find_file(name);
    if (!f) return -1;
    f->used = 0;
    f->name[0] = '\0';
    f->size = 0;
    f->node.name[0] = '\0';
    f->node.size = 0;
    if (fs.file_count > 0) fs.file_count--;
    if (fs_backend == FS_BACKEND_HBFS) return hbfs_update_entry(f);
    return 0;
}

int fs_truncate_file(file_t *f) {
    if (!f || !f->used) return -1;
    f->size = 0;
    f->node.size = 0;
    if (fs_backend == FS_BACKEND_HBFS) return hbfs_update_entry(f);
    return 0;
}

uint32_t fs_read_file_data(file_t *f, uint32_t offset, void *buf, uint32_t count) {
    if (fs_backend == FS_BACKEND_HBFS)
        return hbfs_read_data(f, offset, buf, count);
    if (!f || !f->used || !buf) return 0;
    if (offset >= f->size) return 0;
    uint32_t available = f->size - offset;
    if (count > available) count = available;
    memcpy(buf, f->data + offset, count);
    return count;
}

int fs_write_file_data(file_t *f, uint32_t offset, const void *buf, uint32_t count) {
    if (fs_backend == FS_BACKEND_HBFS)
        return hbfs_write_data(f, offset, buf, count);
    if (!f || !f->used || (!buf && count)) return -1;
    if (offset > f->capacity || count > f->capacity - offset) return -1;
    memcpy(f->data + offset, buf, count);
    uint32_t end = offset + count;
    if (end > f->size) {
        f->size = end;
        f->node.size = end;
    }
    return (int)count;
}

uint32_t fs_get_count(void) {
    return fs.file_count;
}

file_t *fs_get_file(uint32_t index) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < MAX_FILES; i++) {
        if (!fs.files[i].used) continue;
        if (seen == index) return &fs.files[i];
        seen++;
    }
    return NULL;
}

vfs_node_t *fs_get_node(uint32_t index) {
    file_t *f = fs_get_file(index);
    return f ? &f->node : NULL;
}

static int ramfs_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count) {
    if (!node) return -1;
    return (int)fs_read_file_data((file_t *)node->private_data, offset, buf, count);
}

static int ramfs_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count) {
    if (!node) return -1;
    int ret = fs_write_file_data((file_t *)node->private_data, offset, buf, count);
    if (ret >= 0) node->size = ((file_t *)node->private_data)->size;
    return ret;
}

static int ramfs_vfs_truncate(vfs_node_t *node) {
    if (!node) return -1;
    int ret = fs_truncate_file((file_t *)node->private_data);
    if (ret == 0) node->size = 0;
    return ret;
}

static int ramfs_vfs_unlink(vfs_node_t *node) {
    if (!node) return -1;
    return fs_delete_file(node->name);
}

// 读取文件
void fs_read_file(file_t *f) {
    (void)f;
}
