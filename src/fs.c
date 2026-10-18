/**
 * @file fs.c
 * @brief HBFS 文件系统实现，支持 ramfs（内存文件系统）和 HBFS（磁盘文件系统）两种后端，
 *        包含 MBR/GPT 分区表解析
 */
#include "block.h"
#include "core/heap.h"
#include "ext2.h"
#include "fat32.h"
#include "fs.h"
#include "partition.h"
#include "exfat.h"
#include "graphics/graphics.h"
#include "string.h"
#include "sys/stat.h"

#define HBFS_MAGIC 0x3146534248ULL /**< HBFS 魔数，对应 "HBFS1" 的小端标记 */
#define HBFS_VERSION 1             /**< HBFS 版本号 */
#define HBFS_DEFAULT_START_LBA 2048 /**< HBFS 默认起始 LBA 地址（1MiB 对齐） */
#define HBFS_TABLE_SECTORS 16      /**< 文件表占用的扇区数（与 MAX_FILES=128 的 64B 表项对应） */
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
    FS_BACKEND_FAT32,   /**< FAT32 文件系统后端 */
    FS_BACKEND_EXFAT,   /**< exFAT 文件系统后端（只读） */
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
typedef char hbfs_table_size_must_match_disk_layout[
    sizeof(hbfs_table) == HBFS_TABLE_SECTORS * BLOCK_SECTOR_SIZE ? 1 : -1];
static uint32_t hbfs_start_lba = HBFS_DEFAULT_START_LBA; /**< HBFS 分区起始 LBA */
static uint32_t hbfs_total_sectors = 1 + HBFS_TABLE_SECTORS + MAX_FILES * HBFS_FILE_SECTORS; /**< HBFS 分区总扇区数 */
static const char *fs_error = "ok"; /**< 最近一次错误描述 */

static int ramfs_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count);   /**< ramfs VFS 读取操作 */
static int ramfs_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count); /**< ramfs VFS 写入操作 */
static int ramfs_vfs_truncate(vfs_node_t *node); /**< ramfs VFS 截断操作 */
static int ramfs_vfs_unlink(vfs_node_t *node);   /**< ramfs VFS 删除操作 */

static ext2_fs_t ext2_fs; /**< EXT2 文件系统实例 */
static int ext2_try_mount(void);

static fat32_fs_t fat32_fs; /**< FAT32 文件系统实例 */
static int fat32_try_mount(void);
static uint32_t fat32_resolve_parent(const char *norm, char *basename_out);
static int ext2_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count);
static int ext2_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count);
static int ext2_vfs_truncate(vfs_node_t *node);
static int ext2_vfs_unlink(vfs_node_t *node);

static int fat32_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count);
static int fat32_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count);
static int fat32_vfs_truncate(vfs_node_t *node);
static int fat32_vfs_unlink(vfs_node_t *node);

/** 设置错误信息并返回失败 */
static int fs_fail(const char *msg) {
    fs_error = msg;
    return -1;
}

/* 分区表解析用的读扇区回调（partition.c 与块设备解耦，见 partition.h）。 */
static int fs_part_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba > 0xFFFFFFFFull) return -1;
    return block_read_sector((uint32_t)lba, (uint8_t *)buf);
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

static const vfs_ops_t fat32_ops = {
    .read = fat32_vfs_read,
    .write = fat32_vfs_write,
    .truncate = fat32_vfs_truncate,
    .unlink = fat32_vfs_unlink,
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
        if (i >= MAX_FILENAME - 1) return NULL;
        out[i] = path[i];
        i++;
    }
    out[i] = '\0';
    return out;
}

/** 从完整路径中提取文件名（最后一个 / 之后的部分） */
static const char *path_basename(const char *path) {
    const char *last = path;
    while (*path) { if (*path == '/') last = path + 1; path++; }
    return last;
}

/** 提取父目录 inode 号（EXT2 后端专用），文件名写入 name_out */
static uint32_t ext2_resolve_parent(const char *full_path, char *name_out) {
    if (!full_path || !name_out) return 0;
    /* 复制路径以便分割 */
    char tmp[256];
    uint32_t len = 0;
    while (full_path[len] && len < 255) { tmp[len] = full_path[len]; len++; }
    tmp[len] = '\0';

    /* 找到最后一个 / */
    int last_slash = -1;
    for (uint32_t i = 0; i < len; i++) if (tmp[i] == '/') last_slash = (int)i;

    if (last_slash <= 0) {
        /* 无 / 或以 / 开头 → 父目录是 root */
        const char *base = path_basename(full_path);
        uint32_t blen = 0; while (base[blen] && blen < MAX_FILENAME - 1) { name_out[blen] = base[blen]; blen++; }
        name_out[blen] = '\0';
        return EXT2_ROOT_INO;
    }

    /* 分割: tmp[0..last_slash-1] = 目录路径, tmp[last_slash+1..] = 文件名 */
    tmp[last_slash] = '\0';
    const char *base = &tmp[last_slash + 1];
    uint32_t blen = 0; while (base[blen] && blen < MAX_FILENAME - 1) { name_out[blen] = base[blen]; blen++; }
    name_out[blen] = '\0';

    uint32_t dir_ino = 0;
    if (ext2_path_to_inode(&ext2_fs, tmp, &dir_ino) < 0) return 0;
    return dir_ino;
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
        f->node.uid = 0;
        f->node.gid = 0;
        f->node.mode = S_IFREG | 0644;
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
    const uint8_t *table = (const uint8_t *)hbfs_table;
    return block_write_sectors(hbfs_table_lba(), table, HBFS_TABLE_SECTORS);
}

/** 从磁盘加载文件表到内存 */
static int hbfs_load_table(void) {
    uint8_t *table = (uint8_t *)hbfs_table;
    return block_read_sectors(hbfs_table_lba(), table, HBFS_TABLE_SECTORS);
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
        f->node.type = f->type == 1 ? VFS_NODE_DIR :
                       (f->type == 2 ? VFS_NODE_SYMLINK : VFS_NODE_FILE);
        f->node.size = e->size;
        f->node.capacity = RAMFS_MAX_FILE_SIZE;
        f->node.private_data = f;
        f->node.ops = &ramfs_ops;
        f->node.uid = 0;
        f->node.gid = 0;
        f->node.mode = (f->type == 1 ? S_IFDIR : S_IFREG) | 0755;

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
        f->node.uid = inode.i_uid;
        f->node.gid = inode.i_gid;
        f->node.mode = inode.i_mode;
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
    char basename[MAX_FILENAME];
    uint32_t parent = ext2_resolve_parent(f->name, basename);
    if (!parent) return -1;
    int ret = ext2_unlink(&ext2_fs, parent, basename);
    if (ret == 0) {
        f->used = 0;
        f->name[0] = '\0';
        f->node.name[0] = '\0';
        f->node.size = 0;
        if (fs.file_count > 0) fs.file_count--;
    }
    return ret;
}

/* ── FAT32 VFS ops ──────────────────────────────────────────── */

/** FAT32 VFS 读取操作 */
static int fat32_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count) {
    file_t *f = (file_t *)node->private_data;
    if (!f || !f->used || !buf) return 0;
    if (offset >= f->size) return 0;
    uint32_t avail = f->size - offset;
    if (count > avail) count = avail;
    return fat32_read_file(&fat32_fs, f->disk_slot, f->size, offset, (uint8_t *)buf, count);
}

/** FAT32 VFS 写入操作 */
static int fat32_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count) {
    file_t *f = (file_t *)node->private_data;
    if (!f || !f->used || (!buf && count)) return -1;
    uint32_t sz = f->size;
    int ret = fat32_write_file(&fat32_fs, f->disk_slot, &sz, offset, (const uint8_t *)buf, count);
    if (ret > 0) {
        f->size = sz; f->node.size = sz;
        /* fat32_write_file() 只更新调用方这份内存变量，从不回写目录项，
         * 不手动同步的话文件大小在重启重新扫描根目录时会读回创建时的
         * 0——之前 writefile+cat 在同一次开机里能用、重启后文件"变空"，
         * 根因就在这。回写失败先不当成整次写入失败（数据已经落盘，只是
         * 大小元数据没跟上，比直接报错整个 write() 更接近真实语义）。 */
        char basename[MAX_FILENAME];
        uint32_t parent = fat32_resolve_parent(f->name, basename);
        if (parent) fat32_set_file_size(&fat32_fs, parent, f->disk_slot, sz);
    }
    return ret;
}

/** FAT32 VFS 截断操作 */
static int fat32_vfs_truncate(vfs_node_t *node) {
    file_t *f = (file_t *)node->private_data;
    if (!f || !f->used) return -1;
    /* FAT32 truncate: write zero-length via write_file at offset 0 with count 0 */
    uint32_t sz = 0;
    fat32_write_file(&fat32_fs, f->disk_slot, &sz, 0, NULL, 0);
    f->size = 0; f->node.size = 0;
    {
        char basename[MAX_FILENAME];
        uint32_t parent = fat32_resolve_parent(f->name, basename);
        if (parent) fat32_set_file_size(&fat32_fs, parent, f->disk_slot, 0);
    }
    return 0;
}

/** FAT32 VFS 删除操作 */
static int fat32_vfs_unlink(vfs_node_t *node) {
    file_t *f = (file_t *)node->private_data;
    if (!f || !f->used) return -1;
    /* 同 fs_delete_file() 的 FAT32 分支踩过的坑：这是 unlink() 实际会走的
     * 那条路径（node->ops->unlink 回调），之前只顾着改 fs_delete_file()
     * 自己内联的 FAT32 分支，漏了这条平行、真正生效的路径——f->name 带
     * 子目录时（"testdir/foo.txt"）直接传给根目录查找必然找不到，导致
     * stat 能看见文件但 unlink/rm 报"not found"。 */
    char basename[MAX_FILENAME];
    uint32_t parent = fat32_resolve_parent(f->name, basename);
    int ret = parent ? fat32_delete_file(&fat32_fs, parent, basename) : -1;
    if (ret == 0) {
        f->used = 0;
        f->name[0] = '\0';
        f->node.name[0] = '\0';
        f->node.size = 0;
        if (fs.file_count > 0) fs.file_count--;
    }
    return ret;
}

/** 递归把 dir_cluster 目录（prefix 为完整路径前缀，根目录传空串）下的
 *  所有条目注册进 fs.files[]。之前只扫根目录：同一次开机里创建的子目录
 *  文件由 fs_create_file 留在内存表里所以能用，重启后重建时子目录内容
 *  没人扫，"testdir/nested.txt" 直接从表里消失——磁盘上数据完好，cat
 *  却报 not found。这里补上递归下钻，对齐 HBFS/ext2 后端"重启后文件还
 *  在"的行为。g_fat32_cluster_buf 的套娃安全性见 fat32.c 顶部注释：
 *  readdir/lookup 每次调用都自己重读目录簇，跨调用不持有缓冲区状态。 */
static void fat32_rebuild_dir(uint32_t dir_cluster, const char *prefix) {
    uint32_t plen = strlen(prefix);
    for (uint32_t idx = 0; fs.file_count < MAX_FILES; idx++) {
        char name[64];
        uint32_t type, size;
        if (fat32_readdir(&fat32_fs, dir_cluster, idx, name, &type, &size) < 0) return;
        if (name[0] == '.' || name[0] == '\0') continue;
        uint32_t cluster, fsize;
        uint8_t attr;
        if (fat32_lookup(&fat32_fs, dir_cluster, name, &cluster, &fsize, &attr) < 0) { continue; }
        if (attr & (FAT32_ATTR_VOLUME_ID | FAT32_ATTR_LFN)) { continue; }
        uint32_t nlen = 0; while (name[nlen] && nlen < MAX_FILENAME - 1) nlen++;
        /* 完整路径（前缀 + 可选 '/' 分隔符 + 名字）必须装得进 MAX_FILENAME-1
         * 个字符再加 '\0'。此前写成 `plen + 1 + nlen >= MAX_FILENAME`，
         * 对**根目录**（plen==0，没有分隔符）恒多算 1 个字符，于是名字正好
         * MAX_FILENAME-1=31 字符的文件被整条 continue 跳过：文件与 LFN 在盘
         * 上都正常（宿主 mdir 可见、fat32_lookup 也能按原名命中），只是不进
         * 扁平文件表，重启后 cat/ls 都找不到，看起来仍像“文件消失”。
         * 分隔符只在 plen>0 时才占一个字符，故按是否真有前缀来算。 */
        if (plen + (plen ? 1u : 0u) + nlen >= MAX_FILENAME) continue;
        char full[MAX_FILENAME];
        memcpy(full, prefix, plen);
        if (plen) full[plen++] = '/';
        memcpy(full + plen, name, nlen);
        full[plen + nlen] = '\0';
        file_t *f = &fs.files[fs.file_count];
        f->size = fsize;
        f->capacity = fsize > RAMFS_MAX_FILE_SIZE ? fsize : RAMFS_MAX_FILE_SIZE;
        f->data = NULL;
        f->disk_slot = cluster;
        f->used = 1;
        f->type = (attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;
        f->node.type = f->type ? VFS_NODE_DIR : VFS_NODE_FILE;
        f->node.size = fsize;
        f->node.capacity = f->capacity;
        f->node.private_data = f;
        f->node.ops = &fat32_ops;
        f->node.uid = 0;
        f->node.gid = 0;
        f->node.mode = (f->type ? S_IFDIR : S_IFREG) | 0755;
        memcpy(f->name, full, plen + nlen + 1);
        memcpy(f->node.name, full, plen + nlen + 1);
        fs.file_count++;
        if (f->type) fat32_rebuild_dir(cluster, full);
    }
}

/** 从 FAT32 根目录读取文件条目并填充 fs.files[] */
static void fat32_rebuild_files(void) {
    fs.file_count = 0;
    fat32_rebuild_dir(fat32_fs.bpb.root_cluster, "");
}

/** 尝试挂载 FAT32 分区（扫描 MBR 中类型 0x0C 的分区） */
static int fat32_try_mount(void) {
    static partition_entry_t parts[PART_MAX_ENTRIES];
    int n = partition_parse(fs_part_read, NULL, parts, PART_MAX_ENTRIES);
    if (n < 0) return -1;

    /* 有分区表：按条目挂（MBR 0x0B/0x0C；GPT 的 MS basic data 也映射到 0x07，
     * 这里只认 FAT32 类型，避免把 NTFS/exFAT 当 FAT32 挂）。 */
    for (int i = 0; i < n; i++) {
        uint8_t type = parts[i].type;
        /* GPT 上 FAT32 通常落在 "MS basic data"（映射成 0x07），ESP 也是
         * FAT；类型码不足以区分，直接让 fat32_mount 用 BPB 去验。 */
        if (!(type == 0x0B || type == 0x0C || type == 0x07 || type == 0xEF))
            continue;
        if (parts[i].start_lba > 0xFFFFFFFFull) continue;
        uint32_t p_start = (uint32_t)parts[i].start_lba;
        if (fat32_mount(p_start, &fat32_fs) == 0) {
            /* diskmgr 的 part/usage 行读 hbfs_* 全局（fs_disk_start_lba 等），
             * 挂上 FAT32 后同步成真实分区范围，别让它们停留在 HBFS 探测时
             * 留下的旧值。 */
            hbfs_start_lba = p_start;
            hbfs_total_sectors = fat32_fs.bpb.total_sectors_32;
            return 0;
        }
    }

    /* 无分区表（superfloppy）：整盘就是 FAT32。 */
    if (n == 0 && fat32_mount(0, &fat32_fs) == 0) {
        hbfs_start_lba = 0;
        hbfs_total_sectors = fat32_fs.bpb.total_sectors_32;
        return 0;
    }
    return -1;
}


/* ── exFAT 后端（只读）──────────────────────────────────────────
 * 只提供 read：写入涉及分配位图与目录项校验和，尚未实现。文件表只展开
 * 根目录，子目录后续再说（先满足"插上 U 盘能看能读"）。 */
static exfat_fs_t exfat_fs;

static int exfat_part_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba > 0xFFFFFFFFull) return -1;
    return block_read_sector((uint32_t)lba, (uint8_t *)buf);
}

static int exfat_part_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    if (lba > 0xFFFFFFFFull) return -1;
    return block_write_sector((uint32_t)lba, (const uint8_t *)buf);
}

/* 把 "dir/file" 拆成父目录簇 + 叶子名。根目录返回 exfat_fs.root_cluster。
 * 父目录必须在文件表里存在且是目录——exFAT 的目录项只存首簇，够了。 */
static int exfat_split(const char *path, char *base, uint32_t *dir_cluster) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        strcpy(base, path);
        *dir_cluster = exfat_fs.root_cluster;
        return 0;
    }
    uint32_t n = (uint32_t)(slash - path);
    if (n == 0 || n >= MAX_FILENAME) return -1;
    char parent[MAX_FILENAME];
    memcpy(parent, path, n);
    parent[n] = 0;
    file_t *p = fs_find_file(parent);
    if (!p || p->type != 1) return -1;
    strcpy(base, slash + 1);
    *dir_cluster = p->disk_slot;
    return 0;
}

static int exfat_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count) {
    file_t *f = (file_t *)node->private_data;
    if (!f || !f->used || !buf) return 0;
    int n = exfat_read_file(&exfat_fs, f->disk_slot, f->size, offset, buf, count);
    return n < 0 ? 0 : n;
}

static int exfat_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count) {
    file_t *f = (file_t *)node->private_data;
    if (!f || !f->used || (!buf && count)) return -1;
    char base[MAX_FILENAME];
    uint32_t dir = 0;
    if (exfat_split(f->name, base, &dir) < 0) return -1;
    if (exfat_write_in_dir(&exfat_fs, dir, base, offset, buf, count) < 0) return -1;
    uint32_t end = offset + count;
    if (end > f->size) {
        f->size = end;
        f->node.size = end;
        f->capacity = end;
    }
    return (int)count;
}
static int exfat_vfs_truncate(vfs_node_t *node) { (void)node; return -1; }
static int exfat_vfs_unlink(vfs_node_t *node) { (void)node; return -1; }

static const vfs_ops_t exfat_ops = {
    .read = exfat_vfs_read,
    .write = exfat_vfs_write,
    .truncate = exfat_vfs_truncate,
    .unlink = exfat_vfs_unlink,
};

/* 递归展开 exFAT 目录：exfat_iter_dir 逐条回调，不在内存里缓存整目录。 */
typedef struct {
    const char *prefix;
    int depth;
} exfat_walk_t;

static void exfat_add_entry(const exfat_dirent_t *e, const char *full, int is_dir) {
    file_t *f = &fs.files[fs.file_count++];
    f->size = e->size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)e->size;
    f->capacity = f->size;
    f->data = NULL;
    f->disk_slot = e->first_cluster;
    f->used = 1;
    f->type = is_dir ? 1 : 0;
    f->node.type = is_dir ? VFS_NODE_DIR : VFS_NODE_FILE;
    f->node.size = f->size;
    f->node.capacity = f->capacity;
    f->node.private_data = f;
    f->node.ops = &exfat_ops;
    f->node.uid = 0;
    f->node.gid = 0;
    f->node.mode = (is_dir ? S_IFDIR | 0555 : S_IFREG | 0444);
    strncpy(f->name, full, MAX_FILENAME - 1);
    f->name[MAX_FILENAME - 1] = 0;
}

static int exfat_emit(void *ctx, const exfat_dirent_t *e) {
    exfat_walk_t *w = (exfat_walk_t *)ctx;
    if (fs.file_count >= MAX_FILES) return 1;
    if (!e->name[0]) return 0;

    uint32_t plen = (uint32_t)strlen(w->prefix);
    uint32_t nlen = (uint32_t)strlen(e->name);
    if (plen + (plen ? 1u : 0u) + nlen >= MAX_FILENAME) return 0;

    char full[MAX_FILENAME];
    uint32_t pos = 0;
    for (uint32_t i = 0; i < plen; i++) full[pos++] = w->prefix[i];
    if (plen) full[pos++] = '/';
    for (uint32_t i = 0; i < nlen; i++) full[pos++] = e->name[i];
    full[pos] = 0;

    int is_dir = (e->attr & 0x10) != 0;
    exfat_add_entry(e, full, is_dir);

    if (is_dir && w->depth < 4) {
        exfat_walk_t child;
        child.prefix = full;
        child.depth = w->depth + 1;
        (void)exfat_iter_dir(&exfat_fs, e->first_cluster, exfat_emit, &child);
    }
    return 0;
}

static void exfat_rebuild_files(void) {
    fs.file_count = 0;
    exfat_walk_t root = { "", 0 };
    (void)exfat_iter_dir(&exfat_fs, exfat_fs.root_cluster, exfat_emit, &root);
}

/* exFAT 与 NTFS 共用 0x07（MS basic data），靠签名区分。 */
static int exfat_try_mount(void) {
    static partition_entry_t parts[PART_MAX_ENTRIES];
    int n = partition_parse(fs_part_read, NULL, parts, PART_MAX_ENTRIES);
    if (n < 0) return -1;

    for (int i = 0; i < n; i++) {
        if (parts[i].type != 0x07) continue;
        if (parts[i].start_lba > 0xFFFFFFFFull) continue;
        if (exfat_mount(&exfat_fs, exfat_part_read, NULL, parts[i].start_lba) == 0) {
            exfat_fs.wr = exfat_part_write;
            hbfs_start_lba = (uint32_t)parts[i].start_lba;
            hbfs_total_sectors = (uint32_t)parts[i].sectors;
            return 0;
        }
    }
    return -1;
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

    /*
     * 批量读：一次 block_read_sectors() 覆盖"中间整扇区段"，只在首尾两个
     * 不完整扇区才走单扇区接口。
     *
     * 旧实现每个扇区都调一次 block_read_sector()：AHCI/USB 后端会把连续的
     * 单扇区读合成一条命令，但每次调用仍要走一遍"LBA 换算 + 后端派发"，
     * 而且**跨后端的合并窗口有限**。当文件大到跨 128 个扇区以上时（本文件
     * 系统单文件上限 256 扇区），逐次调用的固定开销就不可忽略。这与
     * CODE_STYLE §5 "批量化设备命令"的要求一致。
     *
     * 不变式：hbfs 文件在磁盘上是**连续**的（hbfs_file_lba 由 disk_slot 直接
     * 线性推得），所以任意 [start_sector, start_sector+n) 都是连续 LBA 区间，
     * 可以安全合成一条命令——这也是能用批量接口的前提。
     */
    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;
    uint8_t lead[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));

    /* 首扇区不完整：单扇区读后只取需要的部分。 */
    uint32_t sector_off = offset % BLOCK_SECTOR_SIZE;
    if (sector_off != 0) {
        uint32_t first_sector = offset / BLOCK_SECTOR_SIZE;
        uint32_t chunk = BLOCK_SECTOR_SIZE - sector_off;
        if (chunk > count) chunk = count;
        if (block_read_sector(hbfs_file_lba(f->disk_slot, first_sector),
                              lead) < 0) return 0;
        memcpy(out, lead + sector_off, chunk);
        done = chunk;
    }

    /* 中间整扇区段：一次批量命令。 */
    uint32_t mid_sectors = (count - done) / BLOCK_SECTOR_SIZE;
    if (mid_sectors) {
        uint32_t mid_lba = hbfs_file_lba(f->disk_slot,
                                         (offset + done) / BLOCK_SECTOR_SIZE);
        if (block_read_sectors(mid_lba, out + done,
                               mid_sectors) < 0) return done;
        done += mid_sectors * BLOCK_SECTOR_SIZE;
    }

    /* 尾部不足一扇区：单扇区读。 */
    if (done < count) {
        uint32_t tail_sector = (offset + done) / BLOCK_SECTOR_SIZE;
        uint32_t chunk = count - done;
        if (block_read_sector(hbfs_file_lba(f->disk_slot, tail_sector),
                              lead) < 0) return done;
        memcpy(out + done, lead, chunk);
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

    /*
     * 批量写：与 hbfs_read_data() 对称——首尾不完整扇区走"读改写"，
     * 中间整扇区段一次 block_write_sectors() 下发。
     *
     * 注意"读改写"只用于**部分扇区**：整扇区写不需要先读（旧实现虽然对整
     * 扇区跳过了读，但仍是每扇区一条命令），现在整段合成一条。
     */
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t done = 0;
    uint8_t lead[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));

    /* 首扇区不完整：读改写。 */
    uint32_t sector_off = offset % BLOCK_SECTOR_SIZE;
    if (sector_off != 0) {
        uint32_t first_sector = offset / BLOCK_SECTOR_SIZE;
        uint32_t chunk = BLOCK_SECTOR_SIZE - sector_off;
        if (chunk > count) chunk = count;
        if (block_read_sector(hbfs_file_lba(f->disk_slot, first_sector),
                              lead) < 0) {
            /* 该扇区此前从未写过：按全 0 处理，写入后再落盘。 */
            memset(lead, 0, sizeof(lead));
        }
        memcpy(lead + sector_off, in, chunk);
        if (block_write_sector(hbfs_file_lba(f->disk_slot, first_sector),
                               lead) < 0) return -1;
        done = chunk;
    }

    /* 中间整扇区段：一次批量命令，不需要读改写。 */
    uint32_t mid_sectors = (count - done) / BLOCK_SECTOR_SIZE;
    if (mid_sectors) {
        uint32_t mid_lba = hbfs_file_lba(f->disk_slot,
                                         (offset + done) / BLOCK_SECTOR_SIZE);
        if (block_write_sectors(mid_lba, in + done, mid_sectors) < 0) return -1;
        done += mid_sectors * BLOCK_SECTOR_SIZE;
    }

    /* 尾部不足一扇区：读改写。 */
    if (done < count) {
        uint32_t tail_sector = (offset + done) / BLOCK_SECTOR_SIZE;
        uint32_t chunk = count - done;
        if (block_read_sector(hbfs_file_lba(f->disk_slot, tail_sector),
                              lead) < 0) {
            memset(lead, 0, sizeof(lead));
        }
        memcpy(lead, in + done, chunk);
        if (block_write_sector(hbfs_file_lba(f->disk_slot, tail_sector),
                               lead) < 0) return -1;
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
    (void)fs_retry_mount();
    return 1;
}

/* 惰性重挂载：启动后盘才就绪（AHCI 链路晚 up、热插拔、或首次扫描失败）
 * 时，重新探测块设备并挂载，但不重置 ramfs（保留 startup.cfg 等种子文件）。
 * 已有磁盘后端时直接返回，不重复扫描。 */
int fs_retry_mount(void) {
    if (fs_is_disk()) return 0;
    if (block_init() < 0) return -1;
    if (fs_mount_disk() == 0) return 0;
    if (ext2_try_mount() == 0) {
        console_write("fs: ext2 mounted\n", 17);
        fs_backend = FS_BACKEND_EXT2;
        ext2_rebuild_files();
        fs.total_sectors = block_sector_count();
        return 0;
    }
    if (exfat_try_mount() == 0) {
        console_write("fs: exfat mounted\n", 18);
        fs_backend = FS_BACKEND_EXFAT;
        exfat_rebuild_files();
        fs.total_sectors = hbfs_total_sectors;
        return 0;
    }
    if (fat32_try_mount() == 0) {
        console_write("fs: fat32 mounted\n", 18);
        fs_backend = FS_BACKEND_FAT32;
        fat32_rebuild_files();
        fs.total_sectors = block_sector_count();
        return 0;
    }
    return -1;
}

/**
 * @brief 挂载指定分区（1 起）。
 *
 * 为什么限制在 ramfs：切后端要重建 VFS（vfs_init 会清掉 seed 静态文件），
 * 现在没有安全的卸载/重挂路径。这里只允许"还没挂盘时选一个分区"，
 * 避免半路把正在用的文件系统换掉。
 */
int fs_mount_partition(int index) {
    static partition_entry_t parts[PART_MAX_ENTRIES];
    if (fs_is_disk()) return fs_fail("已有磁盘文件系统，换分区请重启");
    if (block_init() < 0) return fs_fail("没有块设备");

    int n = partition_parse(fs_part_read, NULL, parts, PART_MAX_ENTRIES);
    if (n < 0) return fs_fail("分区表读取失败");
    if (n == 0) return fs_fail("没有分区表");
    if (index < 1 || index > n) return fs_fail("分区号超出范围");
    if (parts[index - 1].start_lba > 0xFFFFFFFFull) return fs_fail("分区起始 LBA 超出 32 位");

    uint32_t start = (uint32_t)parts[index - 1].start_lba;
    uint32_t secs = (uint32_t)parts[index - 1].sectors;
    hbfs_start_lba = start;
    hbfs_total_sectors = secs;

    if (exfat_mount(&exfat_fs, exfat_part_read, NULL, start) == 0) {
        exfat_fs.wr = exfat_part_write;
        fs_backend = FS_BACKEND_EXFAT;
        exfat_rebuild_files();
        fs.total_sectors = secs;
        return 0;
    }
    if (fat32_mount(start, &fat32_fs) == 0) {
        fs_backend = FS_BACKEND_FAT32;
        fat32_rebuild_files();
        fs.total_sectors = fat32_fs.bpb.total_sectors_32;
        return 0;
    }
    if (ext2_mount(start, &ext2_fs) == 0) {
        fs_backend = FS_BACKEND_EXT2;
        ext2_rebuild_files();
        fs.total_sectors = secs;
        return 0;
    }

    hbfs_super_t sb;
    if (hbfs_read_super(&sb) == 0 && hbfs_validate_super(&sb) && hbfs_load_table() == 0) {
        fs_backend = FS_BACKEND_HBFS;
        hbfs_rebuild_files_from_table();
        fs.total_sectors = hbfs_total_sectors;
        return 0;
    }
    return fs_fail("该分区没有可识别的文件系统（FAT32/ext2/HBFS）");
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

/* ── FAT32 成为新装默认格式：安装/格式化路径 ──────────────────────
 * 镜像上面 HBFS 的 fs_install_disk[_at]/fs_format_disk 三个入口，
 * 复用同一批分区表读写辅助函数（write_mbr_entry/ranges_overlap/
 * align_up/get_le32/fs_read_partitions），只是分区类型码换成 FAT32
 * 的 0x0C，格式化本体交给 fat32.c 的 fat32_format()。HBFS 的挂载/读取
 * 代码本身不删除——已装 HBFS 的旧盘仍可正常挂载，这里只是把"新装"
 * 的默认目标换成 FAT32，方便真实 Linux/Windows 主机直接挂载识别。 */

/** FAT32 安装所需的最小扇区数（对应 fat32_format 自身的下限；
 *  66624 = 512B/簇时凑足 FAT32 规范 65525 簇下限的最小卷） */
static uint32_t fat32_needed_sectors(void) {
    return 66624;
}

/** 检查 FAT32 分区范围是否有效 */
static int fat32_range_valid(uint32_t start, uint32_t sectors) {
    uint32_t total = block_sector_count();
    if (start < 2048) return 0;
    if (sectors < fat32_needed_sectors()) return 0;
    if (start >= total) return 0;
    if (sectors > total - start) return 0;
    return 1;
}

/** 在 MBR 分区表中查找已存在的 FAT32 分区（类型 0x0B 或 0x0C） */
static int fat32_find_mbr_partition(uint32_t *start, uint32_t *sectors) {
    uint8_t mbr[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    if (block_read_sector(0, mbr) < 0) return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return -1;

    for (uint32_t i = 0; i < 4; i++) {
        uint8_t *e = mbr + 446 + i * 16;
        if (e[4] != 0x0B && e[4] != 0x0C) continue;
        uint32_t lba = get_le32(e + 8);
        uint32_t cnt = get_le32(e + 12);
        if (lba == 0 || cnt < fat32_needed_sectors()) continue;
        *start = lba;
        *sectors = cnt;
        return 0;
    }
    return -1;
}

/** 将 FAT32 分区信息写入 MBR 分区表 */
static int fat32_write_mbr_partition(uint32_t start, uint32_t sectors) {
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
        if (type == 0x0B || type == 0x0C) {
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

    write_mbr_entry(mbr + 446 + (uint32_t)slot * 16, 0, 0x0C, start, sectors);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    return block_write_sector(0, mbr);
}

/** 自动选择 FAT32 安装范围（同 hbfs_choose_install_range 的找空闲区逻辑） */
static int fat32_choose_install_range(uint32_t *start, uint32_t *sectors) {
    uint32_t total = block_sector_count();
    uint32_t min = fat32_needed_sectors();
    if (total == 0) return fs_fail("未检测到可写硬盘");
    if (total <= HBFS_DEFAULT_START_LBA + min) return fs_fail("硬盘空间不足");

    if (fat32_find_mbr_partition(start, sectors) == 0) return 0;

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

/** 挂载磁盘上刚格式化好的 FAT32 分区并切换后端 */
static int fat32_mount_and_switch(uint32_t start) {
    if (fat32_mount(start, &fat32_fs) < 0) return fs_fail("FAT32 挂载失败");
    fs_backend = FS_BACKEND_FAT32;
    /* 同 fat32_try_mount：diskmgr 读的 hbfs_* 全局同步为真实分区范围 */
    hbfs_start_lba = start;
    hbfs_total_sectors = fat32_fs.bpb.total_sectors_32;
    fat32_rebuild_files();
    fs.total_sectors = block_sector_count();
    return 1;
}

/** 在指定范围格式化并挂载 FAT32 */
static int fat32_format_range(uint32_t start, uint32_t sectors) {
    if (!fat32_range_valid(start, sectors)) return fs_fail("FAT32 分区范围无效");
    if (fat32_format(start, sectors, NULL) < 0) return fs_fail("写入 FAT32 文件系统失败");
    return fat32_mount_and_switch(start);
}

/** 格式化磁盘为 FAT32（使用默认或已有的分区范围） */
int fs_format_disk_fat32(void) {
    fs_error = "ok";
    if (block_init() < 0) return fs_fail("未检测到可写硬盘");
    uint32_t start = HBFS_DEFAULT_START_LBA;
    uint32_t sectors = block_sector_count() > start ? block_sector_count() - start : 0;
    (void)fat32_find_mbr_partition(&start, &sectors);
    return fat32_format_range(start, sectors);
}

/** 在磁盘上安装 FAT32 文件系统（自动选择分区范围） */
int fs_install_disk_fat32(void) {
    fs_error = "ok";
    if (block_init() < 0) return fs_fail("未检测到可写硬盘");
    uint32_t start = 0;
    uint32_t sectors = 0;
    if (fat32_find_mbr_partition(&start, &sectors) == 0)
        return fat32_format_range(start, sectors);
    if (fat32_choose_install_range(&start, &sectors) < 0) return -1;
    return fs_install_disk_fat32_at(start, sectors);
}

/** 在指定 LBA 范围安装 FAT32 文件系统（含写入 MBR 分区表） */
int fs_install_disk_fat32_at(uint32_t start, uint32_t sectors) {
    fs_error = "ok";
    if (block_init() < 0) return fs_fail("未检测到可写硬盘");
    if (!fat32_range_valid(start, sectors)) return fs_fail("FAT32 分区范围无效");
    if (fat32_write_mbr_partition(start, sectors) < 0) return fs_fail("写入分区表失败");
    return fat32_format_range(start, sectors);
}

/* ─────────── 可引导安装（BIOS=GRUB） ───────────
 *
 * `install auto` 此前只准备存储（写分区表 + 格式化 FAT32），从不写 MBR 前
 * 446 字节的引导代码，于是磁盘"有分区、不可引导"——弹出 DVD 就进不去系统。
 *
 * 这里补齐引导器安装。构建期已把 GRUB 的 boot.img（MBR 引导代码）与
 * core.img（LBA1 起的核心镜像，内嵌了"找到 FAT 上的 /boot/grub/grub.cfg
 * 并 configfile 过去"的最小配置）经 src/boot/grub_blob.asm incbin 嵌进内核
 * （guest 内既无 ISO9660 也无 ATAPI，拿不到 CD 上的文件，必须嵌）。
 *
 * 写入顺序与 GRUB 的预期布局一致：
 *   LBA 0        : boot.img 的前 446 字节（引导代码）——分区表与 55AA 保留
 *   LBA 1 .. N   : core.img 原始字节
 * 随后把内核镜像与 grub.cfg 放进 FAT 分区，GRUB 才能 chainload 到它。
 */
extern const uint8_t _binary_build_grub_boot_img_start[];
extern const uint8_t _binary_build_grub_boot_img_end[];
extern const uint8_t _binary_build_grub_core_img_start[];
extern const uint8_t _binary_build_grub_core_img_end[];

/* GRUB 引导代码只占 boot.img 的前 446 字节：其后是分区表(446..509)与
 * 55AA 签名(510..511)，这两处必须保留我们自己写的分区表。 */
#define GRUB_BOOT_CODE_LEN 446u

/** 把 GRUB 引导块写入磁盘（MBR 引导代码 + core.img）。不触碰分区表。 */
static int grub_write_boot_blocks(void) {
    const uint8_t *boot = _binary_build_grub_boot_img_start;
    const uint8_t *core = _binary_build_grub_core_img_start;
    uint32_t core_len = (uint32_t)(_binary_build_grub_core_img_end - core);
    if (core_len == 0) return fs_fail("GRUB core.img 未嵌入");

    /* 先把现有 MBR 读进来，只覆盖前 446 字节，保留分区表与签名 */
    uint8_t mbr[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
    if (block_read_sector(0, mbr) < 0) return fs_fail("读取 MBR 失败");
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return fs_fail("分区表无效（请先 install auto）");
    memcpy(mbr, boot, GRUB_BOOT_CODE_LEN);
    if (block_write_sector(0, mbr) < 0) return fs_fail("写入 MBR 引导代码失败");

    /* core.img 从 LBA1 起连续写入。注意不能超出第一个分区起点（通常
     * LBA2048），否则会踩坏 FAT 引导扇区——core.img 约 160KB（313 扇区），
     * 而空隙有 2047 扇区，正常装得下；这里显式校验以防异常布局。 */
    uint32_t core_sectors = (core_len + BLOCK_SECTOR_SIZE - 1) / BLOCK_SECTOR_SIZE;
    uint32_t first_part_lba = 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint8_t *e = mbr + 446 + i * 16;
        uint32_t lba = get_le32(e + 8);
        if (e[4] != 0 && lba > 0 && (first_part_lba == 0 || lba < first_part_lba))
            first_part_lba = lba;
    }
    if (first_part_lba && 1u + core_sectors > first_part_lba)
        return fs_fail("core.img 超出 MBR 后空隙");

    for (uint32_t i = 0; i < core_sectors; i++) {
        uint8_t sec[BLOCK_SECTOR_SIZE] __attribute__((aligned(2)));
        uint32_t off = i * BLOCK_SECTOR_SIZE;
        uint32_t n = core_len - off;
        if (n > BLOCK_SECTOR_SIZE) n = BLOCK_SECTOR_SIZE;
        memset(sec, 0, sizeof(sec));
        memcpy(sec, core + off, n);
        if (block_write_sector(1 + i, sec) < 0) return fs_fail("写入 core.img 失败");
    }
    return 0;
}

/** 构建期嵌入的 GRUB 引导块字节数（供安装命令打印进度）。 */
uint32_t fs_grub_boot_image_size(void) {
    return (uint32_t)(_binary_build_grub_boot_img_end - _binary_build_grub_boot_img_start);
}
uint32_t fs_grub_core_image_size(void) {
    return (uint32_t)(_binary_build_grub_core_img_end - _binary_build_grub_core_img_start);
}

/**
 * 让当前磁盘可独立引导（BIOS/GRUB）。
 *
 * 前置：磁盘已有可用的 FAT32 分区（先跑 install auto）。
 * 步骤：① 写 GRUB 引导块；② 确保 /boot 与 /boot/grub 存在并写出 grub.cfg。
 * 内核镜像 /boot/hbos.bin 由调用方（install 命令）在拿得到自身镜像时写入，
 * 因为只有它知道镜像来源（引导介质上的 module2 或内嵌副本）。
 *
 * @return 0 成功；-1 失败（fs_last_error() 给出原因）
 */
int fs_install_boot_grub(void) {
    fs_error = "ok";
    if (block_init() < 0) return fs_fail("未检测到可写硬盘");
    if (fs_backend != FS_BACKEND_FAT32) return fs_fail("需要先 install auto（FAT32）");

    if (grub_write_boot_blocks() < 0) return -1;

    /* /boot 与 /boot/grub：GRUB 的 prefix 指向 ($root)/boot/grub */
    if (!vfs_lookup("/boot")) {
        if (vfs_mkdir("/boot") < 0 && !vfs_lookup("/boot"))
            return fs_fail("创建 /boot 失败");
    }
    if (!vfs_lookup("/boot/grub")) {
        if (vfs_mkdir("/boot/grub") < 0 && !vfs_lookup("/boot/grub"))
            return fs_fail("创建 /boot/grub 失败");
    }

    /* grub.cfg：菜单与 core.img 里内嵌的最小配置衔接。用 multiboot2 加载
     * 内核，与 ISO 路径（Makefile 的 GRUB 菜单）保持一致。 */
    static const char cfg[] =
        "set timeout=3\n"
        "set default=0\n"
        "insmod all_video\n"
        "insmod gfxterm\n"
        "set gfxmode=1024x768x32,auto\n"
        "menuentry \"HBOS (disk)\" {\n"
        "  multiboot2 /boot/hbos.bin\n"
        "}\n"
        "menuentry \"HBOS (text mode)\" {\n"
        "  terminal_output console\n"
        "  multiboot2 /boot/hbos.bin\n"
        "}\n";
    vfs_node_t *f = vfs_lookup("/boot/grub/grub.cfg");
    if (!f) f = vfs_create("/boot/grub/grub.cfg");
    if (!f) return fs_fail("创建 grub.cfg 失败");
    if (vfs_write(f, 0, cfg, (uint32_t)strlen(cfg)) < 0)
        return fs_fail("写入 grub.cfg 失败");
    return 0;
}

/**
 * 从 multiboot2 的模块标签里找引导器交给我们的内核镜像。
 *
 * 引导配置（Makefile 的 GRUB 菜单）把内核同时作为 module2 传进来，于是
 * guest 内 install 可以把它原样写到目标盘的 /boot/hbos.bin——内核自身在
 * 内存里只有 LOAD 段内容、没有 ELF 头，不能直接当文件写出去，必须用
 * 引导器读盘得到的那份完整镜像。
 *
 * @param out_len 写出镜像长度
 * @return 镜像起始地址（内核恒等映射，可直接访问）；找不到返回 NULL
 */
const uint8_t *fs_boot_module_image(uint32_t *out_len) {
    extern void *g_mbi;
    if (out_len) *out_len = 0;
    if (!g_mbi) return NULL;
    uintptr_t base = (uintptr_t)g_mbi;
    uint32_t total = *(uint32_t *)base;
    for (uintptr_t p = base + 8; p < base + total; ) {
        uint32_t t  = *(uint32_t *)p;
        uint32_t sz = *(uint32_t *)(p + 4);
        if (t == 0) break;
        if (t == 3) {
            uint32_t start = *(uint32_t *)(p + 8);
            uint32_t end   = *(uint32_t *)(p + 12);
            if (end > start) {
                if (out_len) *out_len = end - start;
                return (const uint8_t *)(uintptr_t)start;
            }
        }
        p += sz;
        if (p & 7) p = (p + 7) & ~(uintptr_t)7;
    }
    return NULL;
}

/**
 * 把一段内核镜像写入 /boot/hbos.bin（引导盘上的 payload）。
 * @param data 内核镜像起始地址（须是可直接访问的内核虚拟地址）
 * @param len  镜像长度（字节）
 */
int fs_install_boot_kernel(const uint8_t *data, uint32_t len) {
    fs_error = "ok";
    if (!data || len == 0) return fs_fail("内核镜像无效");
    if (fs_backend != FS_BACKEND_FAT32) return fs_fail("需要先 install auto（FAT32）");
    if (!vfs_lookup("/boot")) {
        if (vfs_mkdir("/boot") < 0 && !vfs_lookup("/boot"))
            return fs_fail("创建 /boot 失败");
    }
    vfs_node_t *f = vfs_lookup("/boot/hbos.bin");
    if (!f) f = vfs_create("/boot/hbos.bin");
    if (!f) return fs_fail("创建 /boot/hbos.bin 失败");
    /* 先截断：旧镜像可能比新镜像长，直接覆盖会留下尾部残留，GRUB 读到的
     * 长度虽由目录项给出，但残留数据仍会占盘且容易误判。 */
    (void)vfs_truncate(f);
    if (vfs_write(f, 0, data, len) < 0) return fs_fail("写入内核镜像失败");
    return 0;
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

/**
 * 把第 index 个分区格式化成 FAT32。自定义安装的第一步：用户先在分区器里
 * 划好区，再按分区号格式化，不用自己算 LBA。
 */
int fs_install_disk_fat32_partition(int index) {
    static partition_entry_t parts[PART_MAX_ENTRIES];
    if (fs_is_disk()) return fs_fail("已有文件系统在挂载，格式化前请重启");
    if (block_init() < 0) return fs_fail("没有块设备");

    int n = partition_parse(fs_part_read, NULL, parts, PART_MAX_ENTRIES);
    if (n < 0) return fs_fail("分区表读取失败");
    if (n == 0) return fs_fail("没有分区表");
    if (index < 1 || index > n) return fs_fail("分区号超出范围");

    uint64_t start = parts[index - 1].start_lba;
    uint64_t secs = parts[index - 1].sectors;
    if (start > 0xFFFFFFFFull || secs > 0xFFFFFFFFull || secs == 0)
        return fs_fail("分区超出 32 位 LBA 范围");
    return fs_install_disk_fat32_at((uint32_t)start, (uint32_t)secs);
}

/** 尝试挂载 EXT2 分区（扫描 MBR 中类型 0x83 的分区） */
static int ext2_try_mount(void) {
    static partition_entry_t parts[PART_MAX_ENTRIES];
    int n = partition_parse(fs_part_read, NULL, parts, PART_MAX_ENTRIES);
    if (n < 0) return -1;

    for (int i = 0; i < n; i++) {
        if (parts[i].type != 0x83) continue;   /* Linux 数据分区（含 GPT 映射） */
        if (parts[i].start_lba > 0xFFFFFFFFull) continue;
        if (ext2_mount((uint32_t)parts[i].start_lba, &ext2_fs) == 0) return 0;
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
    return fs_backend == FS_BACKEND_HBFS || fs_backend == FS_BACKEND_EXT2 ||
           fs_backend == FS_BACKEND_FAT32 || fs_backend == FS_BACKEND_EXFAT;
}

/** 获取当前后端名称字符串 */
const char *fs_backend_name(void) {
    static char name[16];
    if (fs_backend == FS_BACKEND_EXT2) return "ext2";
    if (fs_backend == FS_BACKEND_EXFAT) return "exfat";
    if (fs_backend == FS_BACKEND_FAT32) return "fat32";
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
    /* FAT32 后端的容量是分区实际大小，不是 ramfs 的 128×128KiB 模型上限
     * ——之前 diskmgr 在 63MiB 的 FAT32 分区上照样显示 16MiB。返回值是
     * uint32_t，超过 4GiB 的分区饱和到 0xFFFFFFFF。 */
    if (fs_backend == FS_BACKEND_FAT32) {
        uint64_t bytes = (uint64_t)fat32_fs.bpb.total_sectors_32 * BLOCK_SECTOR_SIZE;
        return bytes > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)bytes;
    }
    if (fs_backend == FS_BACKEND_EXFAT) {
        uint64_t bytes = (uint64_t)hbfs_total_sectors * BLOCK_SECTOR_SIZE;
        return bytes > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)bytes;
    }
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

/**
 * 把一个规范化路径（不含前导 '/'，可能含中间 '/' 表示子目录）解析成
 * FAT32 上真实的父目录簇号 + 叶子短文件名，逐级用 fat32_lookup() 下钻。
 * 镜像 ext2_resolve_parent() 的角色。失败返回 0（0/1 簇号在 FAT 里永远
 * 是保留值，不会是合法数据簇，可安全当"无效"哨兵）。
 *
 * 找到这个真实 bug 的来龙去脉：fs_create_file() 的 FAT32 分支原来直接把
 * 整段 norm（可能带 '/'）当文件名传给 fat32_create_file()，且父目录永远
 * 写死成根簇——对 "hello.txt" 这样的根级文件恰好凑巧能用，但一旦路径带
 * 子目录（比如自检流程的 mkdir+chdir+相对路径操作），会在根目录里创建
 * 一个名字带斜杠、被截断得乱七八糟的短文件名项，最终在重启后的自检里
 * 引发内核 panic——用 HBFS 走同一段自检代码不会崩，因为 HBFS 的整个
 * 通用兜底路径本来就是纯内存 ramfs 语义，从不真的碰磁盘簇。 */
static uint32_t fat32_resolve_parent(const char *norm, char *basename_out) {
    uint32_t cluster = fat32_fs.bpb.root_cluster;
    const char *p = norm;
    char comp[MAX_FILENAME];
    while (1) {
        const char *slash = strchr(p, '/');
        if (!slash) {
            uint32_t len = (uint32_t)strlen(p);
            if (len == 0 || len >= MAX_FILENAME) return 0;
            memcpy(basename_out, p, len + 1);
            return cluster;
        }
        uint32_t clen = (uint32_t)(slash - p);
        if (clen == 0 || clen >= MAX_FILENAME) return 0;
        memcpy(comp, p, clen);
        comp[clen] = '\0';
        uint32_t next_cluster;
        uint8_t attr;
        if (fat32_lookup(&fat32_fs, cluster, comp, &next_cluster, NULL, &attr) < 0) {
            /* 中间目录不存在就顺手建一个（mkdir -p 语义）——ramfs/HBFS 的
             * "目录"本来就只是扁平存储上的假路径前缀，从不要求父目录真的
             * 先存在；FAT32 走真实簇结构，为了不比另外两个后端更难用，
             * 这里补上同等宽松的行为，而不是让 mkdir("/a/b") 在全新的
             * FAT32 盘上莫名其妙失败。 */
            if (fat32_mkdir(&fat32_fs, cluster, comp) < 0) return 0;
            if (fat32_lookup(&fat32_fs, cluster, comp, &next_cluster, NULL, &attr) < 0) return 0;
        }
        if (!(attr & FAT32_ATTR_DIRECTORY)) return 0;
        cluster = next_cluster;
        p = slash + 1;
    }
}

/** 创建文件（若同名文件已存在则返回已有文件） */
file_t *fs_create_file(const char *name) {
    char norm[MAX_FILENAME];
    if (!normalize_path(name, norm)) return NULL;
    file_t *existing = fs_find_file(norm);
    if (existing) return existing;
    if (fs_backend == FS_BACKEND_EXFAT) {
        char base[MAX_FILENAME];
        uint32_t dir = 0;
        if (exfat_split(norm, base, &dir) < 0) {
            (void)fs_fail("exFAT 父目录不存在");
            return NULL;
        }
        if (exfat_create_in_dir(&exfat_fs, dir, base, NULL, 0) < 0) {
            (void)fs_fail("exFAT 创建文件失败");
            return NULL;
        }
        for (uint32_t i = 0; i < MAX_FILES; i++) {
            file_t *f = &fs.files[i];
            if (f->used) continue;
            strcpy(f->name, norm);
            f->size = 0;
            f->capacity = 0;
            f->data = NULL;
            f->disk_slot = 0;
            f->used = 1;
            f->type = 0;
            f->node.type = VFS_NODE_FILE;
            f->node.size = 0;
            f->node.capacity = 0;
            f->node.private_data = f;
            f->node.ops = &exfat_ops;
            f->node.uid = 0;
            f->node.gid = 0;
            f->node.mode = S_IFREG | 0644;
            fs.file_count++;
            return f;
        }
        return NULL;
    }

    if (fs_backend == FS_BACKEND_EXT2) {
        char basename[MAX_FILENAME];
        uint32_t parent = ext2_resolve_parent(norm, basename);
        if (!parent) return NULL;
        int ino = ext2_create_file(&ext2_fs, parent, basename, 1);
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
            f->node.uid = 0;
            f->node.gid = 0;
            f->node.mode = S_IFREG | 0644;
            fs.file_count++;
            return f;
        }
        return NULL;
    }

    if (fs_backend == FS_BACKEND_FAT32) {
        char basename[MAX_FILENAME];
        uint32_t parent = fat32_resolve_parent(norm, basename);
        if (!parent) return NULL;
        uint32_t cluster;
        if (fat32_create_file(&fat32_fs, parent, basename, &cluster) < 0) return NULL;
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
            f->node.ops = &fat32_ops;
            strcpy(f->node.name, norm);
            f->disk_slot = cluster;
            f->used = 1;
            f->type = 0;
            f->node.uid = 0;
            f->node.gid = 0;
            f->node.mode = S_IFREG | 0644;
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
        f->node.uid = 0;
        f->node.gid = 0;
        f->node.mode = S_IFREG | 0644;
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

/** 创建轻量符号链接。链接目标直接存放在文件槽的数据区，不新增 inode
 * 或动态分配；HBFS 会连同 type=2 一次同步。ext2/FAT32 需要各自的真实
 * inode/目录项实现，因此在对应后端明确拒绝，避免伪装成普通文件。 */
file_t *fs_create_symlink(const char *name, const char *target) {
    if (!name || !target || !target[0]) return NULL;
    size_t length = strlen(target);
    if (length >= VFS_MAX_NAME) return NULL;
    if (fs_backend == FS_BACKEND_EXT2 || fs_backend == FS_BACKEND_FAT32)
        return NULL;
    if (fs_find_file(name)) return NULL;

    file_t *link = fs_create_file(name);
    if (!link) return NULL;
    if (length > link->capacity ||
        fs_write_file_data(link, 0, target, (uint32_t)length) < 0) {
        (void)fs_delete_file(name);
        return NULL;
    }
    link->type = 2;
    link->node.type = VFS_NODE_SYMLINK;
    link->node.uid = 0;
    link->node.gid = 0;
    link->node.mode = S_IFLNK | 0777;
    link->size = (uint32_t)length;
    link->node.size = (uint32_t)length;
    if (fs_backend == FS_BACKEND_HBFS && hbfs_update_entry(link) < 0) {
        (void)fs_delete_file(name);
        return NULL;
    }
    return link;
}

/** 删除指定名称的文件 */
int fs_delete_file(const char *name) {
    file_t *f = fs_find_file(name);
    if (!f) return -1;
    if (fs_backend == FS_BACKEND_EXT2) {
        char basename[MAX_FILENAME];
        uint32_t parent = ext2_resolve_parent(f->name, basename);
        if (!parent) return -1;
        int ret = ext2_unlink(&ext2_fs, parent, basename);
        if (ret == 0) {
            f->used = 0;
            f->name[0] = '\0';
            f->node.name[0] = '\0';
            f->node.size = 0;
            if (fs.file_count > 0) fs.file_count--;
        }
        return ret;
    }
    if (fs_backend == FS_BACKEND_FAT32) {
        char basename[MAX_FILENAME];
        uint32_t parent = fat32_resolve_parent(f->name, basename);
        int ret = parent ? fat32_delete_file(&fat32_fs, parent, basename) : -1;
        if (ret == 0) {
            f->used = 0;
            f->name[0] = '\0';
            f->node.name[0] = '\0';
            f->node.size = 0;
            if (fs.file_count > 0) fs.file_count--;
        }
        return ret;
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

/**
 * @brief 复制普通文件（`mv` 在 ext2/FAT32 上也走这里做 copy-delete 回退）。
 *
 * 为什么分块大小是这里唯一重要的性能旋钮
 * ──────────────────────────────────────
 * 底层每一次 read/write 的**固定开销**与传输字节数无关，却相当可观：
 *   - fat32：每次 write 都重解析父目录路径、重扫父目录项、重数一遍 FAT 簇链；
 *   - ext2：每次 read/write 都要重读磁盘 inode（BGD 块 + inode 表块）；
 *   - hbfs：文件长度每增长一次就要同步整张 16 扇区文件表。
 * 原来的 256 字节小块等于把这些固定开销按每 MiB 放大 4096 倍，1 MiB 文件要
 * 走几千次元数据路径。本函数改用 kmalloc 出的大块（64 KiB）搬运，固定开销
 * 摊薄 256 倍；堆不足时退回栈上的小块，复制行为本身不变。
 *
 * 缓冲区走堆而不是栈：任务栈只有 TASK_STACK_SIZE(8 KiB)，放不下大块；也不
 * 用静态缓冲区，因为块设备路径将来可能引入让出点（task_sleep 等），静态
 * 缓冲会在两个任务同时复制时被踩。
 */
#define FS_COPY_BUF_BYTES (64U * 1024U)  /**< 首选搬运块大小（堆分配） */
#define FS_COPY_BUF_MIN   512U           /**< 堆不可用时的栈上回退块大小 */

int fs_copy_file(const char *src_name, const char *dst_name) {
    char src_norm[MAX_FILENAME];
    char dst_norm[MAX_FILENAME];
    if (!normalize_path(src_name, src_norm) || !normalize_path(dst_name, dst_norm))
        return fs_fail("路径无效");
    if (strcmp(src_norm, dst_norm) == 0) return fs_fail("源文件和目标文件相同");
    if (fs_find_file(dst_norm)) return fs_fail("目标已存在");

    file_t *src_file = fs_find_file(src_norm);
    if (!src_file) return fs_fail("源文件不存在");
    if (src_file->type != 0) return fs_fail("暂不支持复制目录");

    file_t *dst_file = fs_create_file(dst_norm);
    if (!dst_file) return fs_fail("创建目标失败");
    if (fs_truncate_file(dst_file) < 0) {
        (void)fs_delete_file(dst_norm);
        return fs_fail("准备目标失败");
    }

    uint8_t stack_buf[FS_COPY_BUF_MIN];
    uint8_t *buf = (uint8_t *)kmalloc(FS_COPY_BUF_BYTES);
    uint32_t buf_bytes = FS_COPY_BUF_BYTES;
    if (!buf) {                       /* 堆紧张时降级，不改变成功/失败语义 */
        buf = stack_buf;
        buf_bytes = (uint32_t)sizeof(stack_buf);
    }

    int result = 0;
    uint32_t off = 0;
    while (off < src_file->size) {
        uint32_t chunk = src_file->size - off;
        if (chunk > buf_bytes) chunk = buf_bytes;
        uint32_t got = fs_read_file_data(src_file, off, buf, chunk);
        if (got != chunk) {
            (void)fs_delete_file(dst_norm);
            result = fs_fail("读取源文件失败");
            break;
        }
        if (fs_write_file_data(dst_file, off, buf, got) < 0) {
            (void)fs_delete_file(dst_norm);
            result = fs_fail("写入目标失败");
            break;
        }
        off += got;
    }
    if (buf != stack_buf) kfree(buf);
    if (result < 0) return result;

    (void)fs_sync();
    fs_error = "ok";
    return 0;
}

static int fs_path_in_tree(const char *path, const char *root) {
    size_t length = strlen(root);
    return strcmp(path, root) == 0 ||
           (strncmp(path, root, length) == 0 && path[length] == '/');
}

/**
 * 重命名文件或目录树。
 *
 * ramfs/HBFS 的目录是扁平文件表中的规范路径前缀，因此先对整棵子树做长度
 * 和冲突预检，再一次性改写所有名称。HBFS 只同步一次文件表；若写盘失败，
 * 内存和表项名称都会回滚。ext2/FAT32 尚无底层 rename primitive，普通文件
 * 保留 copy-delete 回退，目录则明确失败。
 */
int fs_rename_file(const char *old_name, const char *new_name) {
    char old_norm[MAX_FILENAME];
    char new_norm[MAX_FILENAME];
    if (!normalize_path(old_name, old_norm) || !normalize_path(new_name, new_norm))
        return fs_fail("路径无效");
    if (strcmp(old_norm, new_norm) == 0) return 0;
    if (fs_find_file(new_norm)) return fs_fail("目标已存在");

    file_t *f = fs_find_file(old_norm);
    if (!f) return fs_fail("源文件不存在");

    if (fs_backend == FS_BACKEND_EXT2 || fs_backend == FS_BACKEND_FAT32) {
        if (f->type != 0)
            return fs_fail("当前磁盘后端暂不支持重命名目录");
        if (fs_copy_file(old_norm, new_norm) < 0) return -1;
        if (fs_delete_file(old_norm) < 0) return fs_fail("删除源文件失败");
        fs_error = "ok";
        return 0;
    }

    if (f->type == 1) {
        size_t old_length = strlen(old_norm);
        size_t new_length = strlen(new_norm);
        if (fs_path_in_tree(new_norm, old_norm))
            return fs_fail("不能把目录移动到自身内部");

        uint8_t moved[MAX_FILES];
        char previous[MAX_FILES][MAX_FILENAME];
        memset(moved, 0, sizeof(moved));
        for (uint32_t i = 0; i < MAX_FILES; i++) {
            file_t *entry = &fs.files[i];
            if (!entry->used || !fs_path_in_tree(entry->name, old_norm))
                continue;
            const char *suffix = entry->name + old_length;
            if (new_length + strlen(suffix) >= MAX_FILENAME)
                return fs_fail("重命名后的后代路径过长");
            moved[i] = 1;
            strcpy(previous[i], entry->name);
        }

        for (uint32_t i = 0; i < MAX_FILES; i++) {
            if (!moved[i]) continue;
            const char *suffix = previous[i] + old_length;
            char candidate[MAX_FILENAME];
            strcpy(candidate, new_norm);
            strcat(candidate, suffix);
            for (uint32_t j = 0; j < MAX_FILES; j++) {
                if (!fs.files[j].used || moved[j]) continue;
                if (strcmp(fs.files[j].name, candidate) == 0)
                    return fs_fail("重命名后的后代路径冲突");
            }
        }

        for (uint32_t i = 0; i < MAX_FILES; i++) {
            if (!moved[i]) continue;
            const char *suffix = previous[i] + old_length;
            strcpy(fs.files[i].name, new_norm);
            strcat(fs.files[i].name, suffix);
            strcpy(fs.files[i].node.name, fs.files[i].name);
            if (fs_backend == FS_BACKEND_HBFS)
                strcpy(hbfs_table[fs.files[i].disk_slot].name,
                       fs.files[i].name);
        }
        if (fs_backend == FS_BACKEND_HBFS && hbfs_sync_table() < 0) {
            for (uint32_t i = 0; i < MAX_FILES; i++) {
                if (!moved[i]) continue;
                strcpy(fs.files[i].name, previous[i]);
                strcpy(fs.files[i].node.name, previous[i]);
                strcpy(hbfs_table[fs.files[i].disk_slot].name, previous[i]);
            }
            (void)hbfs_sync_table();
            return fs_fail("同步目录重命名失败");
        }
        fs_error = "ok";
        return 0;
    }

    strcpy(f->name, new_norm);
    strcpy(f->node.name, new_norm);
    if (fs_backend == FS_BACKEND_HBFS && hbfs_update_entry(f) < 0)
        return fs_fail("同步文件表失败");
    (void)fs_sync();
    fs_error = "ok";
    return 0;
}

/** 截断文件，将大小清零 */
int fs_truncate_file(file_t *f) {
    if (!f || !f->used) return -1;
    if (fs_backend == FS_BACKEND_EXT2) {
        return ext2_vfs_truncate(&f->node);
    }
    if (fs_backend == FS_BACKEND_FAT32) {
        return fat32_vfs_truncate(&f->node);
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
    if (fs_backend == FS_BACKEND_FAT32)
        return (uint32_t)fat32_vfs_read(&f->node, offset, buf, count);
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
    if (fs_backend == FS_BACKEND_FAT32)
        return fat32_vfs_write(&f->node, offset, buf, count);
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

    if (fs_backend == FS_BACKEND_EXT2) {
        char basename[MAX_FILENAME];
        uint32_t parent = ext2_resolve_parent(norm, basename);
        if (!parent) return -1;
        int ino = ext2_mkdir(&ext2_fs, parent, basename);
        if (ino < 0) return -1;
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
            f->node.ops = &ext2_ops;
            strcpy(f->node.name, norm);
            f->disk_slot = (uint32_t)ino;
            f->used = 1;
            f->type = 1;
            f->node.uid = 0;
            f->node.gid = 0;
            f->node.mode = S_IFDIR | 0755;
            fs.file_count++;
            return 0;
        }
        return -1;
    }

    if (fs_backend == FS_BACKEND_FAT32) {
        char basename[MAX_FILENAME];
        uint32_t parent = fat32_resolve_parent(norm, basename);
        if (!parent) return -1;
        if (fat32_mkdir(&fat32_fs, parent, basename) < 0) return -1;
        uint32_t new_cluster;
        if (fat32_lookup(&fat32_fs, parent, basename, &new_cluster, NULL, NULL) < 0) return -1;
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
            f->node.ops = &fat32_ops;
            strcpy(f->node.name, norm);
            f->disk_slot = new_cluster;
            f->used = 1;
            f->type = 1;
            f->node.uid = 0;
            f->node.gid = 0;
            f->node.mode = S_IFDIR | 0755;
            fs.file_count++;
            return 0;
        }
        return -1;
    }

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
        f->node.uid = 0;
        f->node.gid = 0;
        f->node.mode = S_IFDIR | 0755;
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

    if (fs_backend == FS_BACKEND_FAT32) {
        /* fs_delete_file()/fat32_delete_file() 明确拒绝目录属性（那是给
         * 删普通文件用的保护），rmdir 必须走专门的 fat32_rmdir()，否则
         * 对 FAT32 盘 rmdir 永远失败。 */
        char basename[MAX_FILENAME];
        uint32_t parent = fat32_resolve_parent(f->name, basename);
        if (!parent || fat32_rmdir(&fat32_fs, parent, basename) < 0) return -1;
        f->used = 0;
        f->name[0] = '\0';
        f->node.name[0] = '\0';
        f->node.size = 0;
        if (fs.file_count > 0) fs.file_count--;
        return 0;
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
