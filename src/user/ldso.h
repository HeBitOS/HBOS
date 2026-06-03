#ifndef HBOS_LDSO_H
#define HBOS_LDSO_H

#include <stdint.h>
#include <stddef.h>

/* ELF dynamic section entry types */
#define DT_NULL            0
#define DT_NEEDED          1
#define DT_PLTRELSZ        2
#define DT_PLTGOT          3
#define DT_HASH            4
#define DT_STRTAB          5
#define DT_SYMTAB          6
#define DT_RELA            7
#define DT_RELASZ          8
#define DT_RELAENT         9
#define DT_STRSZ           10
#define DT_SYMENT          11
#define DT_INIT            12
#define DT_FINI            13
#define DT_SONAME          14
#define DT_RPATH           15
#define DT_SYMBOLIC        16
#define DT_REL             17
#define DT_RELSZ           18
#define DT_RELENT          19
#define DT_PLTREL          20
#define DT_DEBUG           21
#define DT_TEXTREL         22
#define DT_JMPREL          23
#define DT_BIND_NOW        24
#define DT_INIT_ARRAY      25
#define DT_FINI_ARRAY      26
#define DT_INIT_ARRAYSZ    27
#define DT_FINI_ARRAYSZ    28
#define DT_RUNPATH         29
#define DT_FLAGS           30
#define DT_GNU_HASH        0x6ffffef5
#define DT_RELACOUNT       0x6ffffff9
#define DT_FLAGS_1         0x6ffffffb

/* ELF relocation types (x86_64) */
#define R_X86_64_NONE      0
#define R_X86_64_64        1
#define R_X86_64_PC32      2
#define R_X86_64_GOT32     3
#define R_X86_64_PLT32     4
#define R_X86_64_COPY      5
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8
#define R_X86_64_IRELATIVE 37

/* ELF symbol binding */
#define STB_LOCAL     0
#define STB_GLOBAL    1
#define STB_WEAK      2

/* ELF symbol type */
#define STT_NOTYPE    0
#define STT_OBJECT    1
#define STT_FUNC      2
#define STT_SECTION   3
#define STT_FILE      4

/* ELF dynamic section entry */
typedef struct {
    int64_t d_tag;
    union {
        uint64_t d_val;
        uint64_t d_ptr;
    } d_un;
} elf64_dyn_t;

/* ELF symbol table entry */
typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} elf64_sym_t;

/* ELF relocation entry with addend */
typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} elf64_rela_t;

#define ELF64_R_SYM(i)    ((i) >> 32)
#define ELF64_R_TYPE(i)   ((i) & 0xffffffff)
#define ELF64_ST_BIND(i)  ((i) >> 4)
#define ELF64_ST_TYPE(i)  ((i) & 0xf)

/* ELF header types */
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
#define ET_DYN         3

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

#define PT_LOAD    1
#define PT_DYNAMIC 2

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

#define PF_R 4
#define PF_W 2
#define PF_X 1

/**
 * 加载 ELF 共享对象 (.so) 并返回基地址
 * @param data   ELF 文件数据
 * @param size   文件大小
 * @return 成功返回基地址，失败返回 NULL
 */
void *ldso_load(const uint8_t *data, size_t size);

/**
 * 在已加载的共享库中解析符号
 * @param name   符号名称
 * @return 符号地址，未找到返回 NULL
 */
void *ldso_resolve(const char *name);

#endif