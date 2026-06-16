# FS — 文件系统 (`src/`)

> VFS (虚拟文件系统) + ext2 只读 + fat32 只读 + devfs + ramfs + 块设备层

## 文件清单

| 文件 | 行数 | 职责 |
|------|------|------|
| `vfs.c` + `vfs.h` | 250 | VFS 层: 文件描述符表, open/read/write/close/seek/stat |
| `fs.c` + `fs.h` | 1524 | 文件系统操作: ramfs 实现, 路径解析, 文件/目录操作 |
| `ext2.c` + `ext2.h` | — | ext2 只读驱动 |
| `fat32.c` + `fat32.h` | — | FAT32 只读驱动 |
| `devfs.c` + `devfs.h` | — | 设备文件系统 (/dev/) |
| `block.c` + `block.h` | — | 块设备抽象层 (ATA 后端) |
| `ata.c` + `ata.h` | — | ATA PIO 磁盘驱动 |
| `ahci.c` + `ahci.h` | — | AHCI SATA 框架 (未完成) |
| `elf.c` + `elf.h` | — | ELF 加载器 (用户程序) |

## VFS API

```c
// 初始化
void vfs_init(void);                    // 挂载 ramfs 为根, 注册 devfs, 创建 stdin/stdout/stderr

// 文件操作 (POSIX 风格)
int  vfs_open(const char *path, int flags);        // O_RDONLY=0, O_WRONLY=1, O_RDWR=2, O_CREAT=0100, O_TRUNC=01000
int  vfs_read(int fd, void *buf, int len);
int  vfs_write(int fd, const void *buf, int len);
int  vfs_close(int fd);
int  vfs_seek(int fd, int offset, int whence);     // SEEK_SET=0, SEEK_CUR=1, SEEK_END=2
int  vfs_tell(int fd);

// 文件元数据
int  vfs_stat(const char *path, struct stat *st);
int  vfs_unlink(const char *path);
int  vfs_rename(const char *old, const char *new);
int  vfs_mkdir(const char *path);

// 目录
int  vfs_opendir(const char *path);
int  vfs_readdir(int fd, char *name, int maxlen);
int  vfs_closedir(int fd);

// 设备
int  vfs_mknod(const char *path, int major, int minor);
```

## 内部架构

```
VFS Layer (vfs.c)
  ├── 文件描述符表 (per-process, 全局数组)
  ├── 路径解析: 遍历挂载点 → 调用对应 fs 操作
  └── fd 管理: stdin(0), stdout(1), stderr(2)

File System Backends (fs.c)
  ├── ramfs: 内存文件系统 (默认根文件系统)
  │   ├── 文件节点: file_t 结构 (名称, 类型, 内容, 大小)
  │   ├── 目录: 子节点链表
  │   └── 操作: create, read, write, truncate, unlink, rename, mkdir, readdir
  ├── ext2: 只读, 解析 superblock + inode + directory entry
  ├── fat32: 只读, 解析 BPB + FAT + 目录项
  └── devfs: /dev/ 下设备节点 (mknod 创建)

Block Layer (block.c, ata.c, ahci.c)
  ├── 块设备抽象 (block_read_sectors, block_write_sectors)
  ├── ATA PIO 模式 (ata_read, ata_write)
  └── AHCI 框架 (未完成)
```

## ramfs 内部结构

```c
typedef struct file {
    char name[256];
    uint32_t type;           // FILE_TYPE_REGULAR, FILE_TYPE_DIRECTORY
    uint32_t size;           // 文件大小
    uint8_t *data;           // 文件内容 (kmalloc)
    struct file *parent;
    struct file *children;   // 子文件/目录链表
    struct file *next;       // 兄弟链表
    uint32_t uid, gid;
    uint32_t mode;
} file_t;

// 操作:
file_t *file_create(file_t *parent, const char *name, uint32_t type);
file_t *file_lookup(file_t *parent, const char *name);
int     file_read(file_t *f, void *buf, int offset, int len);
int     file_write(file_t *f, const void *buf, int offset, int len);
int     file_truncate(file_t *f, int size);
```

## 状态

| 后端 | 读 | 写 | 挂载 |
|------|-----|-----|------|
| ramfs | ✅ | ✅ | 默认根 |
| ext2 | ✅ (只读) | ❌ | 需要手动挂载 |
| fat32 | ✅ (只读) | ❌ | 需要手动挂载 |
| devfs | ✅ | ✅ | /dev |
