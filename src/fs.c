/**
 * @file fs.c
 * @brief HBFS 文件系统实现，支持 ramfs（内存文件系统）和 HBFS（磁盘文件系统）两种后端，
 *        包含 MBR/GPT 分区表解析
 */
#include "block.h"
#include "ext2.h"
#include "fs.h"
#include "string.h"

#define HBFS_MAGIC 0x3146534248ULL /**< HBFS 魔数，对应 "HBFS1" 的小端标记 */
#define HBFS_VERSION 1             /**< HBFS 版本号 */
#define HBFS_DEFAULT_START_LBA 2048 /**< HBFS 默认起始 LBA 地址（1MiB 对齐） */
#define HBFS_TABLE_SECTORS 8       /**< 文件表占用的扇区数 */
#define HBFS_FILE_SECTORS (RAMFS_MAX_FILE_SIZE / BLOCK_SECTOR_SIZE) /**< 单文件占用的扇区数 */
#define HBFS_PARTITION_TYPE 0xEB   /**< HBFS 分区类型码（MBR） */
#define GPT_ENTRY_SIZE 128         /**< GPT 分区项大小（字节） */
#define GPT_ENTRY_COUNT 128        /**< GPT 最大分区项数 */
#define GPT_ENTRY_SECTORS 32       /**< GPT 分区项区域占用的扇区数 */

/** 文件系统后端类型枚举 */
typedef enum {
    FS_BACKEND_RAM = 0, /**< 内存文件系统后端 */
    FS_BACKEND_HBFS,    /**< 磁盘文件系统后端 */
    FS_BACKEND_EXT2,    /**< EXT2 文件系统后端 */
} fs_backend_t;

/** HBFS 超级块结构，存储在分区第一个扇区 */
typedef struct {
    uint64_t magic;               /**< 魔数，用于标识 HBFS 文件系统 */
    uint32_t version;             /**< 文件系统版本号 */
    uint32_t start_lba;           /**< 分区起始 LBA 地址 */
    uint32_t max_files;           /**< 最大文件数 */
    uint32_t max_file_size;       /**< 单文件最大容量（字节） */
    uint32_t table_lba;           /**< 文件表起始 LBA 地址 */
    uint32_t table_sectors;       /**< 文件表占用扇区数 */
    uint32_t data_lba;            /**< 数据区起始 LBA 地址 */
    uint32_t file_sectors;        /**< 单文件占用扇区数 */
    uint8_t reserved[BLOCK_SECTOR_SIZE - 40]; /**< 保留填充，对齐到一个扇区 */
} __attribute__((packed)) hbfs_super_t;

/** HBFS 文件表项结构，每个文件对应一个表项 */
typedef struct {
    uint8_t used;                 /**< 是否被占用（0=空闲, 1=已使用） */
    uint8_t type;                 /**< 文件类型（0=文件, 1=目录） */
    uint16_t reserved0;           /**< 保留字段 */
    uint32_t size;                /**< 文件大小（字节） */
    char name[MAX_FILENAME];      /**< 文件名 */
    uint8_t reserved1[24];        /**< 保留字段，对齐到 64 字节 */
} __attribute__((packed)) hbfs_entry_t;

// 全局文件系统实例
static filesystem_t fs;           /**< 全局文件系统实例 */
static fs_backend_t fs_backend = FS_BACKEND_RAM; /**< 当前文件系统后端类型 */
static uint8_t ramfs_storage[MAX_FILES][RAMFS_MAX_FILE_SIZE]; /**< ramfs 模式下的文件数据存储区 */
static hbfs_entry_t hbfs_table[MAX_FILES]; /**< HBFS 文件表 */
static uint32_t hbfs_start_lba = HBFS_DEFAULT_START_LBA; /**< HBFS 分区起始 LBA */
static uint32_t hbfs_total_sectors = 1 + HBFS_TABLE_SECTORS + MAX_FILES * HBFS_FILE_SECTORS; /**< HBFS 分区总扇区数 */
static const char *fs_error = "ok"; /**< 最近一次错误描述 */

static int ramfs_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count);   /**< ramfs VFS 读取操作 */
static int ramfs_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count); /**< ramfs VFS 写入操作 */
static int ramfs_vfs_truncate(vfs_node_t *node); /**< ramfs VFS 截断操作 */
static int ramfs_vfs_unlink(vfs_node_t *node);   /**< ramfs VFS 删除操作 */

static ext2_fs_t ext2_fs; /**< EXT2 文件系统实例 */
static int ext2_try_mount(void);
static int ext2_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count);
static int ext2_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count);
static int ext2_vfs_truncate(vfs_node_t *node);
static int ext2_vfs_unlink(vfs_node_t *node);

/** 设置错误信息并返回失败 */
static int fs_fail(const char *msg) {
    fs_error = msg;
    return -1;
}

/** 获取最近一次错误描述 */
const char *fs_last_error(void) {
    return fs_error;
}

/** ramfs VFS 操作函数表 */
static const vfs_ops_t ramfs_ops = {
    .read = ramfs_vfs_read,
    .write = ramfs_vfs_write,
    .truncate = ramfs_vfs_truncate,
    .unlink = ramfs_vfs_unlink,
};

static const vfs_ops_t ext2_ops = {
    .read = ext2_vfs_read,
    .write = ext2_vfs_write,
    .truncate = ext2_vfs_truncate,
    .unlink = ext2_vfs_unlink,
};

/** 计算 HBFS 文件表的起始 LBA 地址 */
static uint32_t hbfs_table_lba(void) {
    return hbfs_start_lba + 1;
}

/** 计算 HBFS 数据区的起始 LBA 地址 */
static uint32_t hbfs_data_lba(void) {
    return hbfs_start_lba + 1 + HBFS_TABLE_SECTORS;
}

/** 计算 HBFS 所需的总扇区数 */
static uint32_t hbfs_needed_sectors(void) {
    return 1 + HBFS_TABLE_SECTORS + MAX_FILES * HBFS_FILE_SECTORS;
}

/** 获取 HBFS 所需的最小扇区数（对外接口） */
uint32_t fs_min_sectors(void) {
    return hbfs_needed_sectors();
}

/**
 * 规范化文件路径：去除前导 '/'，检查非法字符和长度
 * @param path 原始路径
 * @param out  输出缓冲区，存放规范化后的文件名
 * @return 成功返回 out 指针，失败返回 NULL
 */
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

/** 重置文件系统为 ramfs 模式，初始化所有文件槽位 */
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

/** 填充 HBFS 超级块结构 */
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

/** 从磁盘读取 HBFS 超级块 */
static int hbfs_read_super(hbfs_super_t *sb) {
    return block_read_sector(hbfs_start_lba, (uint8_t *)sb);
}

/** 将超级块写入磁盘 */
static int hbfs_write_super(void) {
    hbfs_super_t sb;
    hbfs_fill_super(&sb);
    return block_write_sector(hbfs_start_lba, (const uint8_t *)&sb);
}

/**
 * 验证超级块内容是否与当前配置一致
 * @return 1 表示有效，0 表示无效
 */
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

/** 计算指定槽位中指定扇区的绝对 LBA 地址 */
static uint32_t hbfs_file_lba(uint32_t slot, uint32_t sector) {
    return hbfs_data_lba() + slot * HBFS_FILE_SECTORS + sector;
}

/** 将内存中的文件表同步写入磁盘 */
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

/** 从磁盘加载文件表到内存 */
static int hbfs_load_table(void) {
    uint8_t *table = (uint8_t *)hbfs_table;
    for (uint32_t s = 0; s < HBFS_TABLE_SECTORS; s++) {
        if (block_read_sector(hbfs_table_lba() + s, table + s * BLOCK_SECTOR_SIZE) < 0)
            return -1;
    }
    return 0;
}

/** 根据 HBFS 文件表重建内存中的文件结构 */
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

/** 更新指定文件的 HBFS 文件表项并同步到磁盘 */
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

/** 从 EXT2 根目录读取文件条目并填充 fs.files[] */
static void ext2_rebuild_files(void) {
    fs.file_count = 0;
    ext2_inode_t root;
    if (ext2_read_inode(&ext2_fs, EXT2_ROOT_INO, &root) < 0) return;
    uint32_t idx = 0;
    char name[64];
    uint32_t type;
    while (idx < MAX_FILES && ext2_readdir(&ext2_fs, &root, idx, name, &type) == 0) {
        if (name[0] == '.' || idx >= MAX_FILES) { idx++; continue; }
        file_t *f = &fs.files[fs.file_count];
        uint32_t ino;
        if (ext2_lookup(&ext2_fs, &root, name, &ino) < 0) { idx++; continue; }
        ext2_inode_t inode;
        if (ext2_read_inode(&ext2_fs, ino, &inode) < 0) { idx++; continue; }
        if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG &&
            (inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) { idx++; continue; }
        uint32_t nlen = 0; while (name[nlen] && nlen < MAX_FILENAME - 1) nlen++;
        memcpy(f->name, name, nlen); f->name[nlen] = '\0';
        f->size = inode.i_size;
        f->capacity = inode.i_size > RAMFS_MAX_FILE_SIZE ? inode.i_size : RAMFS_MAX_FILE_SIZE;
        f->data = NULL;
        f->disk_slot = ino;
        f->used = 1;
        f->type = (type == 1) ? 1 : 0;
        f->node.type = f->type ? VFS_NODE_DIR : VFS_NODE_FILE;
        f->node.size = f->size;
        f->node.capacity = f->capacity;
        f->node.private_data = f;
        f->node.ops = &ext2_ops;
        memcpy(f->node.name, f->name, nlen + 1);
        fs.file_count++;
        idx++;
    }
}

/** EXT2 VFS 读取操作 */
static int ext2_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count) {
    file_t *f = (file_t *)node->private_data;
    if (!f || !f->used || !buf) return 0;
    if (offset >= f->size) return 0;
    uint32_t avail = f->size - offset;
    if (count > avail) count = avail;
    ext2_inode_t inode;
    if (ext2_read_inode(&ext2_fs, f->disk_slot, &inode) < 0) return 0;
    return ext2_read_file(&ext2_fs, &inode, offset, (uint8_t *)buf, count);
}

/** EXT2 VFS 写入操作 */
static int ext2_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count) {
    file_t *f = (file_t *)node->private_data;
    if (!f || !f->used || (!buf && count)) return -1;
    int ret = ext2_write_file(&ext2_fs, f->disk_slot, offset, (const uint8_t *)buf, count);
    if (ret > 0) {
        ext2_inode_t inode;
        if (ext2_read_inode(&ext2_fs, f->disk_slot, &inode) == 0) {
            f->size = inode.i_size;
            f->node.size = inode.i_size;
        }
    }
    return ret;
}

/** EXT2 VFS 截断操作 */
static int ext2_vfs_truncate(vfs_node_t *node) {
    file_t *f = (file_t *)node->private_data;
    if (!f || !f->used) return -1;
    int ret = ext2_truncate(&ext2_fs, f->disk_slot);
    if (ret == 0) { f->size = 0; f->node.size = 0; }
    return ret;
}

/** EXT2 VFS 删除操作 */
static int ext2_vfs_unlink(vfs_node_t *node) {
    file_t *f = (file_t *)node->private_data;
    if (!f || !f->used) return -1;
    int ret = ext2_unlink(&ext2_fs, EXT2_ROOT_INO, f->name);
    if (ret == 0) {
        f->used = 0;
        f->name[0] = '\0';
        f->node.name[0] = '\0';
        f->node.size = 0;
        if (fs.file_count > 0) fs.file_count--;
    }
    return ret;
}

/**
 * 从磁盘读取文件数据
 * @param f      目标文件
 * @param offset 读取偏移量（字节）
 * @param buf    输出缓冲区
 * @param count  请求读取的字节数
 * @return 实际读取的字节数
 */
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

/**
 * 向磁盘写入文件数据
 * @param f      目标文件
 * @param offset 写入偏移量（字节）
 * @param buf    输入数据缓冲区
 * @param count  写入字节数
 * @return 成功返回写入字节数，失败返回 -1
 */
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

/** 以小端序写入 32 位无符号整数 */
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/** 以小端序读取 32 位无符号整数 */
static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/** 以小端序读取 64 位无符号整数 */
static uint64_t get_le64(const uint8_t *p) {
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

/** 比较两个 16 字节 GUID 是否相等 */
static int guid_eq(const uint8_t *a, const uint8_t *b) {
    for (uint32_t i = 0; i < 16; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/** 将 GPT 分区类型 GUID 转换为 MBR 分区类型码 */
static uint8_t gpt_type_to_mbr_type(const uint8_t *guid) {
    static const uint8_t esp_guid[16] = {   /**< EFI 系统分区 GUID */
        0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
        0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
    };
    static const uint8_t hbfs_guid[16] = {  /**< HBFS 分区 GUID */
        0x53, 0x46, 0x42, 0x48, 0x00, 0x00, 0x00, 0x40,
        0x80, 0x00, 0x48, 0x42, 0x4f, 0x53, 0x00, 0x01
    };
    static const uint8_t zero_guid[16] = {0}; /**< 全零 GUID（未使用分区） */

    if (guid_eq(guid, zero_guid)) return 0;
    if (guid_eq(guid, esp_guid)) return 0xEF;
    if (guid_eq(guid, hbfs_guid)) return HBFS_PARTITION_TYPE;
    return 0xEE;
}

/**
 * 读取 GPT 分区项，解析分区信息
 * @param out          输出分区信息数组（最多 4 项）
 * @param hbfs_start   输出 HBFS 分区起始 LBA（可选）
 * @param hbfs_sectors 输出 HBFS 分区扇区数（可选）
 * @return 成功返回 0，失败返回 -1
 */
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

/**
 * 在 MBR 分区表中查找 HBFS 分区
 * @param start   输出分区起始 LBA
 * @param sectors 输出分区扇区数
 * @return 成功返回 0，未找到返回 -1
 */
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

/**
 * 读取磁盘分区表信息（支持 MBR 和 GPT）
 * @param out 输出分区信息数组（最多 4 项）
 * @return 成功返回 0，失败返回 -1
 */
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

/** 填写 MBR 分区表项 */
static void write_mbr_entry(uint8_t *e, uint8_t bootable, uint8_t type,
                            uint32_t start, uint32_t sectors) {
    e[0] = bootable ? 0x80 : 0x00;
    e[1] = 0x01; e[2] = 0x01; e[3] = 0x00;
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    put_le32(e + 8, start);
    put_le32(e + 12, sectors);
}

/** 判断两个 LBA 范围是否重叠 */
static int ranges_overlap(uint32_t a_start, uint32_t a_count,
                          uint32_t b_start, uint32_t b_count) {
    uint64_t a_end = (uint64_t)a_start + a_count;
    uint64_t b_end = (uint64_t)b_start + b_count;
    return a_start < b_end && b_start < a_end;
}

/** 将 HBFS 分区信息写入 MBR 分区表 */
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

/** 将 value 向上对齐到 align 的整数倍 */
static uint32_t align_up(uint32_t value, uint32_t align) {
    return (value + align - 1) & ~(align - 1);
}

/**
 * 自动选择 HBFS 安装范围
 * @param start   输出选定的起始 LBA
 * @param sectors 输出选定的扇区数
 * @return 成功返回 0，失败返回 -1
 */
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

/** 检查 HBFS 分区范围是否有效 */
static int hbfs_range_valid(uint32_t start, uint32_t sectors) {
    uint32_t total = block_sector_count();
    if (start < 2048) return 0;
    if (sectors < hbfs_needed_sectors()) return 0;
    if (start >= total) return 0;
    if (sectors > total - start) return 0;
    return 1;
}

/** 在指定范围内格式化 HBFS 文件系统 */
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
    if (fs_mount_disk() < 0) {
        if (ext2_try_mount() == 0) {
            fs_backend = FS_BACKEND_EXT2;
            ext2_rebuild_files();
            fs.total_sectors = block_sector_count();
        }
    }
    return 1;
}

/**
 * 检查文件系统一致性
 * @param out 输出检查结果
 * @return 无错误返回 0，有错误返回 -1
 */
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

/** 格式化磁盘，使用默认或已有的分区范围 */
int fs_format_disk(void) {
    fs_error = "ok";
    if (block_init() < 0) return fs_fail("未检测到可写硬盘");
    uint32_t start = HBFS_DEFAULT_START_LBA;
    uint32_t sectors = block_sector_count() > start ? block_sector_count() - start : 0;
    (void)hbfs_find_mbr_partition(&start, &sectors);
    return hbfs_format_range(start, sectors);
}

/** 在磁盘上安装 HBFS 文件系统（自动选择分区范围） */
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

/** 在指定 LBA 范围安装 HBFS 文件系统（含写入 MBR 分区表） */
int fs_install_disk_at(uint32_t start, uint32_t sectors) {
    fs_error = "ok";
    if (block_init() < 0) return fs_fail("未检测到可写硬盘");
    if (!hbfs_range_valid(start, sectors)) return fs_fail("HBFS 分区范围无效");
    if (hbfs_write_mbr_partition(start, sectors) < 0) return fs_fail("写入分区表失败");
    return hbfs_format_range(start, sectors);
}

/** 挂载磁盘上的 HBFS 文件系统 */
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

/** 尝试挂载 EXT2 分区（扫描 MBR 中类型 0x83 的分区） */
static int ext2_try_mount(void) {
    uint8_t mbr[512] __attribute__((aligned(2)));
    if (block_read_sector(0, mbr) < 0) return -1;
    for (int i = 0; i < 4; i++) {
        uint8_t *e = mbr + 446 + i * 16;
        uint8_t type = e[4];
        if (type != 0x83) continue;
        uint32_t p_start = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                           ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        if (p_start == 0) continue;
        if (ext2_mount(p_start, &ext2_fs) == 0) return 0;
    }
    return -1;
}

/** 将文件表同步到磁盘（仅 HBFS 模式有效） */
int fs_sync(void) {
    if (fs_backend != FS_BACKEND_HBFS) return 0;
    return hbfs_sync_table();
}

/** 判断当前后端是否为磁盘文件系统 */
int fs_is_disk(void) {
    return fs_backend == FS_BACKEND_HBFS || fs_backend == FS_BACKEND_EXT2;
}

/** 获取当前后端名称字符串 */
const char *fs_backend_name(void) {
    static char name[16];
    if (fs_backend == FS_BACKEND_EXT2) return "ext2";
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

/** 获取 HBFS 分区起始 LBA */
uint32_t fs_disk_start_lba(void) {
    return hbfs_start_lba;
}

/** 获取 HBFS 分区总扇区数 */
uint32_t fs_disk_total_sectors(void) {
    return hbfs_total_sectors;
}

/** 获取文件系统总容量（字节） */
uint32_t fs_capacity_bytes(void) {
    return MAX_FILES * RAMFS_MAX_FILE_SIZE;
}

/** 获取已使用字节数 */
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

/** 创建文件（若同名文件已存在则返回已有文件） */
file_t *fs_create_file(const char *name) {
    char norm[MAX_FILENAME];
    if (!normalize_path(name, norm)) return NULL;
    file_t *existing = fs_find_file(norm);
    if (existing) return existing;

    if (fs_backend == FS_BACKEND_EXT2) {
        int ino = ext2_create_file(&ext2_fs, EXT2_ROOT_INO, norm, 1);
        if (ino < 0) return NULL;
        for (uint32_t i = 0; i < MAX_FILES; i++) {
            file_t *f = &fs.files[i];
            if (f->used) continue;
            strcpy(f->name, norm);
            f->size = 0;
            f->capacity = RAMFS_MAX_FILE_SIZE;
            f->data = NULL;
            f->node.type = VFS_NODE_FILE;
            f->node.size = 0;
            f->node.capacity = RAMFS_MAX_FILE_SIZE;
            f->node.private_data = f;
            f->node.ops = &ext2_ops;
            strcpy(f->node.name, norm);
            f->disk_slot = (uint32_t)ino;
            f->used = 1;
            f->type = 0;
            fs.file_count++;
            return f;
        }
        return NULL;
    }

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

/** 删除指定名称的文件 */
int fs_delete_file(const char *name) {
    file_t *f = fs_find_file(name);
    if (!f) return -1;
    if (fs_backend == FS_BACKEND_EXT2) {
        return ext2_vfs_unlink(&f->node);
    }
    f->used = 0;
    f->name[0] = '\0';
    f->size = 0;
    f->node.name[0] = '\0';
    f->node.size = 0;
    if (fs.file_count > 0) fs.file_count--;
    if (fs_backend == FS_BACKEND_HBFS) return hbfs_update_entry(f);
    return 0;
}

/** 截断文件，将大小清零 */
int fs_truncate_file(file_t *f) {
    if (!f || !f->used) return -1;
    if (fs_backend == FS_BACKEND_EXT2) {
        return ext2_vfs_truncate(&f->node);
    }
    f->size = 0;
    f->node.size = 0;
    if (fs_backend == FS_BACKEND_HBFS) return hbfs_update_entry(f);
    return 0;
}

/** 读取文件数据，根据后端类型分派到 ramfs 或 HBFS 读取 */
uint32_t fs_read_file_data(file_t *f, uint32_t offset, void *buf, uint32_t count) {
    if (fs_backend == FS_BACKEND_HBFS)
        return hbfs_read_data(f, offset, buf, count);
    if (fs_backend == FS_BACKEND_EXT2)
        return (uint32_t)ext2_vfs_read(&f->node, offset, buf, count);
    if (!f || !f->used || !buf) return 0;
    if (offset >= f->size) return 0;
    uint32_t available = f->size - offset;
    if (count > available) count = available;
    memcpy(buf, f->data + offset, count);
    return count;
}

/** 写入文件数据，根据后端类型分派到 ramfs 或 HBFS 写入 */
int fs_write_file_data(file_t *f, uint32_t offset, const void *buf, uint32_t count) {
    if (fs_backend == FS_BACKEND_HBFS)
        return hbfs_write_data(f, offset, buf, count);
    if (fs_backend == FS_BACKEND_EXT2)
        return ext2_vfs_write(&f->node, offset, buf, count);
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

/** 获取已使用的文件数 */
uint32_t fs_get_count(void) {
    return fs.file_count;
}

/** 按索引获取文件（跳过未使用的槽位） */
file_t *fs_get_file(uint32_t index) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < MAX_FILES; i++) {
        if (!fs.files[i].used) continue;
        if (seen == index) return &fs.files[i];
        seen++;
    }
    return NULL;
}

/** 按索引获取 VFS 节点 */
vfs_node_t *fs_get_node(uint32_t index) {
    file_t *f = fs_get_file(index);
    return f ? &f->node : NULL;
}

/** ramfs VFS 读取操作回调 */
static int ramfs_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count) {
    if (!node) return -1;
    return (int)fs_read_file_data((file_t *)node->private_data, offset, buf, count);
}

/** ramfs VFS 写入操作回调 */
static int ramfs_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count) {
    if (!node) return -1;
    int ret = fs_write_file_data((file_t *)node->private_data, offset, buf, count);
    if (ret >= 0) node->size = ((file_t *)node->private_data)->size;
    return ret;
}

/** ramfs VFS 截断操作回调 */
static int ramfs_vfs_truncate(vfs_node_t *node) {
    if (!node) return -1;
    int ret = fs_truncate_file((file_t *)node->private_data);
    if (ret == 0) node->size = 0;
    return ret;
}

/** ramfs VFS 删除操作回调 */
static int ramfs_vfs_unlink(vfs_node_t *node) {
    if (!node) return -1;
    return fs_delete_file(node->name);
}

int fs_mkdir(const char *name) {
    char norm[MAX_FILENAME];
    if (!normalize_path(name, norm)) return -1;
    file_t *existing = fs_find_file(norm);
    if (existing) return -1;

    for (uint32_t i = 0; i < MAX_FILES; i++) {
        file_t *f = &fs.files[i];
        if (f->used) continue;
        strcpy(f->name, norm);
        f->size = 0;
        f->capacity = 0;
        f->data = NULL;
        f->node.type = VFS_NODE_DIR;
        f->node.size = 0;
        f->node.capacity = 0;
        f->node.private_data = f;
        f->node.ops = &ramfs_ops;
        strcpy(f->node.name, norm);
        f->disk_slot = i;
        f->used = 1;
        f->type = 1;
        fs.file_count++;
        if (fs_backend == FS_BACKEND_HBFS && hbfs_update_entry(f) < 0) {
            f->used = 0;
            fs.file_count--;
            return -1;
        }
        return 0;
    }
    return -1;
}

int fs_rmdir(const char *name) {
    file_t *f = fs_find_file(name);
    if (!f || f->type != 1) return -1;

    for (uint32_t i = 0; i < MAX_FILES; i++) {
        if (!fs.files[i].used) continue;
        if (&fs.files[i] == f) continue;
        size_t dlen = strlen(f->name);
        if (strncmp(fs.files[i].name, f->name, dlen) == 0 &&
            fs.files[i].name[dlen] == '/')
            return -1;
    }

    return fs_delete_file(name);
}

static uint32_t fs_readdir_skip;

int fs_opendir(const char *name) {
    char norm[MAX_FILENAME];
    if (!normalize_path(name, norm)) return -1;
    file_t *f = fs_find_file(norm);
    if (!f || f->type != 1) return -1;
    fs_readdir_skip = 0;
    return 0;
}

int fs_readdir(const char *name, char *out_name, uint32_t *out_type) {
    if (!out_name || !out_type) return -1;
    char norm[MAX_FILENAME];
    const char *dir_name = "";
    if (name && name[0] && !(name[0] == '/' && name[1] == '\0')) {
        if (!normalize_path(name, norm)) return -1;
        dir_name = norm;
    }

    size_t dlen = strlen(dir_name);

    for (uint32_t i = fs_readdir_skip; i < MAX_FILES; i++) {
        if (!fs.files[i].used) continue;
        const char *fname = fs.files[i].name;

        if (dlen > 0) {
            if (strncmp(fname, dir_name, dlen) != 0 || fname[dlen] != '/')
                continue;
            fname += dlen + 1;
        }

        if (strchr(fname, '/')) continue;

        strncpy(out_name, fname, VFS_MAX_NAME);
        out_name[VFS_MAX_NAME - 1] = '\0';
        *out_type = fs.files[i].type;
        fs_readdir_skip = i + 1;
        return 0;
    }

    return -1;
}

int fs_closedir(const char *name) {
    (void)name;
    fs_readdir_skip = 0;
    return 0;
}

// 读取文件
void fs_read_file(file_t *f) {
    (void)f;
}
