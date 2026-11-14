/**
 * @file    ext2.h
 * @brief   导出 52 个接口（1 类型 / 27 宏 / 24 函数）
 *
 * @layer   drivers
 * @owner   src/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  ext2_fs_t   结构体 / 类型别名
 *   宏    EXT2_SIGNATURE          常量 / 特性宏
 *         EXT2_INCOMPAT_RECOVER   常量 / 特性宏
 *         EXT2_INCOMPAT_EXTENTS   常量 / 特性宏
 *         EXT2_INCOMPAT_64BIT     常量 / 特性宏
 *         EXT2_INCOMPAT_FLEX_BG   常量 / 特性宏
 *         EXT2_INCOMPAT_FILETYPE  目录项带 file_type 字段
 *         EXT2_INCOMPAT_META_BG   组描述符搬进元块组（本驱动不支持）
 *         EXT2_RO_COMPAT_GDT_CSUM 组描述符带校验和
 *         EXT2_RO_COMPAT_METADATA_CSUM 位图/inode/目录块带校验和
 *         EXT2_INCOMPAT_INLINE_DATA 小文件/目录内联在 inode 里（读不了）
 *         EXT2_INCOMPAT_ENCRYPT   文件名/内容加密（读不了）
 *         EXT2_INCOMPAT_EA_INODE  xattr 值存在独立 inode 里
 *         EXT2_INCOMPAT_DIRDATA   目录数据在独立文件里（实验特性）
 *         EXT2_EXTENTS_FL         常量 / 特性宏
 *         EXT2_SUPERBLOCK_OFFSET  常量 / 特性宏
 *         EXT2_SUPERBLOCK_SIZE    常量 / 特性宏
 *         EXT2_ROOT_INO           常量 / 特性宏
 *         EXT2_S_IFMT             常量 / 特性宏
 *         EXT2_S_IFREG            常量 / 特性宏
 *         EXT2_S_IFDIR            常量 / 特性宏
 *         EXT2_S_IFLNK            常量 / 特性宏
 *         EXT2_NDIR_BLOCKS        常量 / 特性宏
 *         EXT2_IND_BLOCK          EXT2IND块
 *         EXT2_DIND_BLOCK         EXT2DIND块
 *         EXT2_TIND_BLOCK         EXT2TIND块
 *         EXT2_FT_REG_FILE        目录项 file_type：普通文件
 *         EXT2_FT_DIR             目录项 file_type：目录
 *         EXT2_FT_SYMLINK         目录项 file_type：符号链接
 *   函数  ext2_mount          Read
 *         ext2_last_error     最近一次挂载失败原因（静态字符串），成功时…
 *         ext2_read_inode     （待补）
 *         ext2_read_block     （待补）
 *         ext2_iter_dir       一次性遍历目录（按块扫描，不重复从头扫描）
 *         ext2_path_to_inode  （待补）
 *         ext2_write_block    Write
 *         ext2_write_inode    （待补）
 *         ext2_alloc_block    （待补）
 *         ext2_free_block     （待补）
 *         ext2_alloc_inode    （待补）
 *         ext2_free_inode     （待补）
 *         ext2_create_file    （待补）
 *         ext2_unlink         （待补）
 *         ext2_truncate       （待补）
 *         ext2_mkdir          在 parent 下建子目录
 *         ext2_rmdir          删除空目录（非空返 -ENOTEMPTY）
 *         ext2_rename         改名/移动目录项（-EEXIST/-ENOTEMPTY/-EISDIR）
 */
#ifndef HBOS_EXT2_H
#define HBOS_EXT2_H

#include <stdint.h>

#define EXT2_SIGNATURE 0xEF53

/* INCOMPAT 特性：只要置位，本驱动就不能安全挂载。
 * RECOVER=日志需要回放；EXTENTS=inode 的 i_block 是 extent 树而不是
 * 15 个块指针；64BIT/FLEX_BG=超级块/组描述符布局变了。 */
#define EXT2_INCOMPAT_RECOVER   0x0004u
#define EXT2_INCOMPAT_EXTENTS   0x0040u
#define EXT2_INCOMPAT_64BIT     0x0080u
#define EXT2_INCOMPAT_FLEX_BG   0x0200u
#define EXT2_INCOMPAT_FILETYPE  0x0002u  /* 目录项第 7 字节是 file_type */
#define EXT2_INCOMPAT_META_BG   0x0010u  /* 组描述符在元块组里，read_bgd 不适用 */
/* RO_COMPAT：只影响"能不能安全写"，读路径不受影响 */
#define EXT2_RO_COMPAT_GDT_CSUM      0x0010u
#define EXT2_RO_COMPAT_METADATA_CSUM 0x0400u
/* INCOMPAT：改变"数据放在哪"或"目录项怎么编码"，读不了就必须拒绝挂载 */
#define EXT2_INCOMPAT_EA_INODE     0x0400u
#define EXT2_INCOMPAT_DIRDATA      0x1000u
#define EXT2_INCOMPAT_INLINE_DATA  0x8000u
#define EXT2_INCOMPAT_ENCRYPT      0x10000u
#define EXT2_EXTENTS_FL         0x00080000u  /* inode i_flags: i_block 是 extent 树 */

#define EXT2_SUPERBLOCK_OFFSET 1024
#define EXT2_SUPERBLOCK_SIZE   1024

#define EXT2_ROOT_INO 2

#define EXT2_S_IFMT   0xF000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFLNK  0xA000

/* 目录项 file_type（ext2 的 EXT2_FT_*；只有超级块 INCOMPAT_FILETYPE
 * 置位时该字段才有意义，否则该 16 位是 name_len 的高 8 位） */
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_SYMLINK  7

#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK   12
#define EXT2_DIND_BLOCK  13
#define EXT2_TIND_BLOCK  14

typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    uint8_t  s_volume_name[16];
    uint8_t  s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_padding1;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_reserved_char_pad;
    uint16_t s_reserved_word_pad;
} __attribute__((packed)) ext2_superblock_t;

typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_bgd_t;

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed)) ext2_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed)) ext2_dir_entry_t;

typedef struct {
    ext2_superblock_t sb;
    uint32_t block_size;
    uint32_t inode_size;
    uint32_t bgd_count;
    uint32_t block_group_count;
    uint32_t partition_lba;
    uint32_t feature_incompat;   /* 用于判断 extent / 64bit 布局 */
    uint32_t desc_size;          /* 组描述符大小：32 或 64 */
    int      read_only;          /* ext4 布局：只允许读，写入会破坏 extent */
    int      has_filetype;       /* INCOMPAT_FILETYPE：目录项第 7 字节是 file_type */
    int mounted;
} ext2_fs_t;

/* Read */
int ext2_mount(uint32_t partition_lba, ext2_fs_t *fs);
/** 最近一次挂载失败原因（静态字符串），成功时为 "ok"。 */
const char *ext2_last_error(void);
int ext2_read_inode(ext2_fs_t *fs, uint32_t inode_num, ext2_inode_t *inode);
int ext2_read_block(ext2_fs_t *fs, uint32_t block, uint8_t *buf);
int ext2_read_file(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t offset,
                   uint8_t *buf, uint32_t count);
int ext2_readdir(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t index,
                 char *name, uint32_t *type);

/** 目录遍历回调：返回非 0 停止。 */
typedef int (*ext2_dir_cb)(void *ctx, const char *name, uint32_t file_type,
                           uint32_t ino);

/** 一次性遍历目录（按块扫描，不重复从头扫描）。 */
int ext2_iter_dir(ext2_fs_t *fs, ext2_inode_t *dir, ext2_dir_cb cb, void *ctx);
int ext2_lookup(ext2_fs_t *fs, ext2_inode_t *dir_inode,
                const char *name, uint32_t *out_inode);
int ext2_path_to_inode(ext2_fs_t *fs, const char *path, uint32_t *out_inode);

/* Write */
int ext2_write_block(ext2_fs_t *fs, uint32_t block, const uint8_t *buf);
int ext2_write_inode(ext2_fs_t *fs, uint32_t inode_num, const ext2_inode_t *inode);
uint32_t ext2_alloc_block(ext2_fs_t *fs);
int ext2_free_block(ext2_fs_t *fs, uint32_t block);
uint32_t ext2_alloc_inode(ext2_fs_t *fs);
int ext2_free_inode(ext2_fs_t *fs, uint32_t inode_num);
int ext2_write_file(ext2_fs_t *fs, uint32_t inode_num,
                    uint32_t offset, const uint8_t *buf, uint32_t count);
int ext2_create_file(ext2_fs_t *fs, uint32_t dir_ino, const char *name, uint8_t file_type);
/** 创建符号链接：<=60 字节走 inode 内联（fast symlink），更长则占一个数据块。
 *  返回新建 inode 号，失败返回 -1。 */
int ext2_symlink(ext2_fs_t *fs, uint32_t dir_ino, const char *name,
                 const char *target);
/** 读取符号链接目标到 buf（不含 '\0'），返回字节数。 */
int ext2_readlink(ext2_fs_t *fs, const ext2_inode_t *inode, char *buf,
                  uint32_t capacity);
int ext2_unlink(ext2_fs_t *fs, uint32_t dir_ino, const char *name);
int ext2_truncate(ext2_fs_t *fs, uint32_t inode_num, uint32_t new_size);
int ext2_mkdir(ext2_fs_t *fs, uint32_t parent_ino, const char *name);
/** 删除空目录；非空返回 -ENOTEMPTY，目标是普通文件返回 -ENOTDIR。 */
int ext2_rmdir(ext2_fs_t *fs, uint32_t dir_ino, const char *name);
/**
 * @brief 重命名/移动一个目录项。
 *
 * @param fs        已挂载实例
 * @param old_dir   源父目录 inode
 * @param old_name  源名字
 * @param new_dir   目标父目录 inode
 * @param new_name  目标名字
 * @param replace   非 0 时允许覆盖已存在的普通文件/空目录（rename(2) 语义）
 * @return 0 成功；失败返回 -errno（-ENOENT/-EEXIST/-ENOTEMPTY/-EISDIR/-ENOTDIR/-EROFS）
 *
 * 不变量：旧目录项在新目录项落盘之后才被移除，中途失败不会丢名字。
 */
int ext2_rename(ext2_fs_t *fs, uint32_t old_dir, const char *old_name,
                uint32_t new_dir, const char *new_name, int replace);

#endif