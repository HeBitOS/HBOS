/**
 * @file    elf.h
 * @brief   ELF64 可执行文件加载器 — 头文件
 *
 * 对标 Linux fs/binfmt_elf.c，实现 ELF64 可执行文件的
 * 解析、段加载和栈初始化。
 * 当前仅支持静态链接的 ET_EXEC 类型 x86_64 ELF。
 */

#ifndef HBOS_ELF_H
#define HBOS_ELF_H

#include <stdint.h>
#include <stddef.h>

struct vfs_node;

/** ELF ident 索引 */
#define EI_MAG0        0
#define EI_MAG1        1
#define EI_MAG2        2
#define EI_MAG3        3
#define EI_CLASS       4
#define EI_DATA        5
#define EI_VERSION     6
#define EI_OSABI       7
#define EI_ABIVERSION  8
#define EI_NIDENT      16

/** ELF class */
#define ELFCLASS32     1
#define ELFCLASS64     2

/** ELF data encoding */
#define ELFDATA2LSB    1
#define ELFDATA2MSB    2

/** ELF 类型 */
#define ET_NONE        0
#define ET_REL         1
#define ET_EXEC        2
#define ET_DYN         3
#define ET_CORE        4

/** 目标机器架构 */
#define EM_X86_64      62

/** 程序头类型 */
#define PT_NULL        0
#define PT_LOAD        1
#define PT_DYNAMIC     2
#define PT_INTERP      3
#define PT_NOTE        4
#define PT_PHDR        6
#define PT_TLS         7

/** 段标志 */
#define PF_X           1
#ifndef PF_W
#define PF_W           2
#endif
#define PF_R           4

/**
 * ELF64 文件头 (64 bytes)
 * 位于文件偏移 0 处
 */
typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

/**
 * ELF64 程序头 (56 bytes)
 * 每个段对应一个 Phdr，由 e_phoff 指向
 */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

/** ELF64 节头 (64 bytes) — 用于查找 .symtab/.strtab（见下面 host-symbol
 *  相关字段），加载/执行本身只看程序头，不需要节头。 */
typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} __attribute__((packed)) elf64_shdr_t;

#define SHT_SYMTAB 2
#define SHT_STRTAB 3

/**
 * 加载 ELF64 可执行文件并派生为新任务
 *
 * @param data       ELF 文件数据的指针（文件必须已读入内存）
 * @param size       文件大小
 * @param argv       参数向量（NULL 结尾）
 * @param envp       环境向量（NULL 结尾）
 * @param task_name  任务名称，NULL 时使用 elf_app
 * @return 成功返回新任务 ID，失败返回 -1
 */
int elf64_load_and_spawn(const uint8_t *data, size_t size,
                         char *const argv[], char *const envp[],
                         const char *task_name);

/**
 * 加载 ELF64 可执行文件并替换当前任务
 *
 * @param data   ELF 文件数据的指针（文件必须已读入内存）
 * @param size   文件大小
 * @param argv   参数向量（NULL 结尾）
 * @param envp   环境向量（NULL 结尾）
 * @return 成功时不返回（任务被替换），失败返回 -1
 */
int elf64_load_and_exec(const uint8_t *data, size_t size,
                        char *const argv[], char *const envp[]);

/** Stream an ELF image directly from a VFS node and replace this task. */
int elf64_load_vfs_and_exec(struct vfs_node *node,
                            char *const argv[], char *const envp[]);

const char *elf64_last_error(void);

#endif
