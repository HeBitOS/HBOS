#ifndef HBOS_LDSO_H
#define HBOS_LDSO_H

#include <stdint.h>
#include <stddef.h>
/* elf64_ehdr_t / elf64_phdr_t / EI_ / PT_LOAD / PT_DYNAMIC / ET_DYN /
 * PF_R|W|X are already defined there -- reused here instead of a second,
 * conflicting copy (this header used to duplicate all of them, which broke
 * as soon as a single translation unit needed both headers, e.g. syscall.c
 * wiring up dlopen()). */
#include "../elf.h"

struct vfs_node;
struct task;

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
#define DT_VERSYM          0x6ffffff0
#define DT_RELACOUNT       0x6ffffff9
#define DT_VERDEF          0x6ffffffc
#define DT_VERDEFNUM       0x6ffffffd
#define DT_VERNEED         0x6ffffffe
#define DT_VERNEEDNUM      0x6fffffff
#define DT_FLAGS_1         0x6ffffffb

#define VER_NDX_LOCAL      0
#define VER_NDX_GLOBAL     1
#define VER_NDX_HIDDEN     0x8000
#define VER_NDX_MASK       0x7fff

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
#define R_X86_64_DTPMOD64  16
#define R_X86_64_DTPOFF64  17
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
#define STT_TLS       6
#define STT_GNU_IFUNC 10

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

typedef struct {
    uint16_t vd_version;
    uint16_t vd_flags;
    uint16_t vd_ndx;
    uint16_t vd_cnt;
    uint32_t vd_hash;
    uint32_t vd_aux;
    uint32_t vd_next;
} elf64_verdef_t;

typedef struct {
    uint32_t vda_name;
    uint32_t vda_next;
} elf64_verdaux_t;

typedef struct {
    uint16_t vn_version;
    uint16_t vn_cnt;
    uint32_t vn_file;
    uint32_t vn_aux;
    uint32_t vn_next;
} elf64_verneed_t;

typedef struct {
    uint32_t vna_hash;
    uint16_t vna_flags;
    uint16_t vna_other;
    uint32_t vna_name;
    uint32_t vna_next;
} elf64_vernaux_t;

#define ELF64_R_SYM(i)    ((i) >> 32)
#define ELF64_R_TYPE(i)   ((i) & 0xffffffff)
#define ELF64_ST_BIND(i)  ((i) >> 4)
#define ELF64_ST_TYPE(i)  ((i) & 0xf)

/**
 * 加载 ELF 共享对象 (.so)
 * @param data   ELF 文件数据
 * @param size   文件大小
 * @return 成功返回不透明句柄（供 ldso_dlsym/ldso_close 使用，dlopen()
 *         的返回值），失败返回 NULL。调用方不应解引用这个指针。
 */
void *ldso_load(const uint8_t *data, size_t size);

/** Stream an ET_DYN object directly from an existing VFS node. */
void *ldso_load_vfs(struct vfs_node *node);

/** Stream an ET_DYN object using a canonical path for DT_NEEDED lookup and
 * per-address-space object de-duplication. */
void *ldso_load_vfs_path(struct vfs_node *node, const char *path);

/** Enumerate constructor/destructor entry points. The kernel validates and
 * orders them; user libc invokes the returned address in ring3. */
uintptr_t ldso_init_next(void *handle, uint64_t ordinal);
uintptr_t ldso_fini_next(void *handle, uint64_t ordinal);

/** Enumerate/apply GNU IFUNC resolvers. Resolvers are always called in ring3. */
uintptr_t ldso_ifunc_next(void *handle, uint64_t ordinal);
int ldso_ifunc_apply(void *handle, uint64_t ordinal, uintptr_t result);

/** Resolve one General Dynamic TLS address for the calling thread. */
uintptr_t ldso_tls_get_addr(uint32_t module_id, uint64_t offset);

/** Release lazily allocated TLS blocks owned by a terminating thread. */
void ldso_release_thread_tls(struct task *task);

/**
 * 在所有已加载的共享库中解析符号（用于库间相互引用的重定位）
 * @param name   符号名称
 * @return 符号地址，未找到返回 NULL
 */
void *ldso_resolve(const char *name);

/**
 * 在指定句柄对应的共享库中查找符号（dlsym() 语义：只在这一个库里找，
 * 不像 ldso_resolve 那样搜索全部已加载库）
 * @param handle ldso_load() 返回的句柄
 * @param name   符号名称
 * @return 符号地址，未找到或句柄无效返回 NULL
 */
void *ldso_dlsym(void *handle, const char *name);

/** GNU dlvsym semantics: resolve one explicitly named symbol version. */
void *ldso_dlsym_version(void *handle, const char *name,
                         const char *version);

/**
 * 卸载共享库：释放它占用的物理页并从已加载链表中移除。
 * 简化实现——不检查是否还有其他代码持有指向其符号的指针，调用方需自行
 * 保证卸载时机安全（和 dlclose() 在真实 Linux 上的一般用法一致）。
 * @param handle ldso_load() 返回的句柄
 * @return 0 成功，-1 句柄无效
 */
int ldso_close(void *handle);

/** Drop every loaded-object mapping and metadata owned by one task mm. */
void ldso_release_address_space(const void *owner_mm);

#endif
