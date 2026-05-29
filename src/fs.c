#include "block.h"
#include "fs.h"
#include "string.h"

#define HBFS_MAGIC 0x3146534248ULL /* "HBFS1" little-endian-ish marker */
#define HBFS_VERSION 1
#define HBFS_DEFAULT_START_LBA 2048
#define HBFS_TABLE_SECTORS 8
#define HBFS_FILE_SECTORS (RAMFS_MAX_FILE_SIZE / BLOCK_SECTOR_SIZE)
#define HBFS_PARTITION_TYPE 0xEB

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

static int ramfs_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count);
static int ramfs_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count);
static int ramfs_vfs_truncate(vfs_node_t *node);
static int ramfs_vfs_unlink(vfs_node_t *node);

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

static int hbfs_find_mbr_partition(uint32_t *start, uint32_t *sectors) {
    uint8_t mbr[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    if (block_read_sector(0, mbr) < 0) return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return -1;

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

static int hbfs_write_mbr(uint32_t start, uint32_t sectors) {
    uint8_t mbr[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    memset(mbr, 0, sizeof(mbr));
    uint8_t *e = mbr + 446;
    e[0] = 0x80;
    e[1] = 0x01; e[2] = 0x01; e[3] = 0x00;
    e[4] = HBFS_PARTITION_TYPE;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    put_le32(e + 8, start);
    put_le32(e + 12, sectors);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    return block_write_sector(0, mbr);
}

// 文件系统初始化
int fs_init(void) {
    fs_reset_ram();
    (void)fs_mount_disk();
    return 1;
}

int fs_format_disk(void) {
    if (block_init() < 0) return -1;
    uint32_t start = HBFS_DEFAULT_START_LBA;
    uint32_t sectors = block_sector_count() > start ? block_sector_count() - start : 0;
    (void)hbfs_find_mbr_partition(&start, &sectors);
    if (sectors < hbfs_needed_sectors() || block_sector_count() <= start + hbfs_needed_sectors()) return -1;
    hbfs_start_lba = start;
    hbfs_total_sectors = sectors;

    memset(hbfs_table, 0, sizeof(hbfs_table));
    if (hbfs_write_super() < 0) return -1;
    if (hbfs_sync_table() < 0) return -1;
    return fs_mount_disk();
}

int fs_install_disk(void) {
    if (block_init() < 0) return -1;
    uint32_t total = block_sector_count();
    uint32_t start = HBFS_DEFAULT_START_LBA;
    if (total <= start + hbfs_needed_sectors()) return -1;
    uint32_t sectors = total - start;
    if (hbfs_write_mbr(start, sectors) < 0) return -1;
    hbfs_start_lba = start;
    hbfs_total_sectors = sectors;
    return fs_format_disk();
}

int fs_mount_disk(void) {
    if (block_init() < 0) return -1;
    uint32_t start = HBFS_DEFAULT_START_LBA;
    uint32_t sectors = block_sector_count() > start ? block_sector_count() - start : 0;
    (void)hbfs_find_mbr_partition(&start, &sectors);
    if (sectors < hbfs_needed_sectors() || block_sector_count() <= start + hbfs_needed_sectors()) return -1;
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
