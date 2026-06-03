#include "user/ldso.h"
#include "core/heap.h"
#include "core/vmm.h"
#include "core/pmm.h"
#include "string.h"

/* 已加载的共享库链表 */
typedef struct loaded_lib {
    struct loaded_lib *next;
    void              *base;
    uint64_t           vaddr_base;
    const char        *soname;
    const char        *strtab;
    elf64_sym_t       *symtab;
    uint64_t           symtab_entries;
    elf64_rela_t      *jmprel;
    uint64_t           jmprel_size;
    void              *pltgot;
} loaded_lib_t;

static loaded_lib_t *g_loaded_libs;

/* 验证 ELF 头 */
static int elf64_check(const elf64_ehdr_t *ehdr, size_t size) {
    if (size < sizeof(elf64_ehdr_t)) return -1;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F')
        return -1;
    if (ehdr->e_ident[EI_CLASS] != 2) return -1;       /* ELFCLASS64 */
    if (ehdr->e_ident[EI_DATA]  != 1) return -1;       /* ELFDATA2LSB */
    if (ehdr->e_machine != 62) return -1;               /* EM_X86_64 */
    return 0;
}

/* 在字符串表中查找符号 */
static elf64_sym_t *symtab_lookup(elf64_sym_t *symtab, uint64_t nentries,
                                   const char *strtab, const char *name) {
    for (uint64_t i = 0; i < nentries; i++) {
        const char *sym_name = strtab + symtab[i].st_name;
        if (strcmp(sym_name, name) == 0) return &symtab[i];
    }
    return 0;
}

/* 对已加载库执行 R_X86_64_RELATIVE 重定位 */
static void do_rela_relative(loaded_lib_t *lib, elf64_rela_t *rela,
                              uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        if (ELF64_R_TYPE(rela[i].r_info) == R_X86_64_RELATIVE) {
            uint64_t *loc = (uint64_t *)((uint8_t *)lib->base +
                                          rela[i].r_offset);
            *loc = lib->vaddr_base + rela[i].r_addend;
        }
    }
}

/* 对已加载库执行 R_X86_64_GLOB_DAT 重定位 */
static void do_rela_glob_dat(loaded_lib_t *lib, elf64_rela_t *rela,
                              uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        if (ELF64_R_TYPE(rela[i].r_info) != R_X86_64_GLOB_DAT) continue;

        uint64_t sym_idx = ELF64_R_SYM(rela[i].r_info);
        elf64_sym_t *sym = &lib->symtab[sym_idx];
        const char *sym_name = lib->strtab + sym->st_name;
        uint64_t *loc = (uint64_t *)((uint8_t *)lib->base +
                                      rela[i].r_offset);

        void *addr = ldso_resolve(sym_name);
        if (!addr) {
            /* 未解析 — 在自身中查找 */
            if (sym->st_value != 0) {
                addr = (uint8_t *)lib->base + sym->st_value;
            }
        }
        if (addr) *loc = (uint64_t)addr;
    }
}

/* 对已加载库执行 R_X86_64_JUMP_SLOT 重定位 */
static void do_rela_jump_slot(loaded_lib_t *lib, elf64_rela_t *rela,
                               uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        if (ELF64_R_TYPE(rela[i].r_info) != R_X86_64_JUMP_SLOT) continue;

        uint64_t sym_idx = ELF64_R_SYM(rela[i].r_info);
        elf64_sym_t *sym = &lib->symtab[sym_idx];
        const char *sym_name = lib->strtab + sym->st_name;
        uint64_t *loc = (uint64_t *)((uint8_t *)lib->base +
                                      rela[i].r_offset);

        void *addr = ldso_resolve(sym_name);
        if (!addr) {
            if (sym->st_value != 0) {
                addr = (uint8_t *)lib->base + sym->st_value;
            }
        }
        if (addr) *loc = (uint64_t)addr;
    }
}

void *ldso_load(const uint8_t *data, size_t size) {
    if (!data || size < sizeof(elf64_ehdr_t)) return 0;

    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)data;
    if (elf64_check(ehdr, size) != 0) return 0;
    if (ehdr->e_type != ET_DYN) return 0;

    const elf64_phdr_t *phdrs = (const elf64_phdr_t *)(data + ehdr->e_phoff);

    /* 第一步：计算总内存需求 */
    uint64_t vaddr_min = (uint64_t)-1;
    uint64_t vaddr_max = 0;
    int has_load = 0;

    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        has_load = 1;
        if (phdrs[i].p_vaddr < vaddr_min) vaddr_min = phdrs[i].p_vaddr;
        uint64_t end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (end > vaddr_max) vaddr_max = end;
    }
    if (!has_load) return 0;

    uint64_t total_size = (vaddr_max - vaddr_min + 0xFFF) & ~0xFFFULL;
    uint64_t num_pages = total_size / PAGE_SIZE;

    /* 分配虚拟地址空间 */
    uint64_t vaddr_base = vaddr_min;
    uint64_t base = 0;

    for (uint64_t p = 0; p < num_pages; p++) {
        uint64_t va = vaddr_base + p * PAGE_SIZE;
        uint64_t phys = pmm_alloc_page();
        if (!phys) goto fail;
        if (!vmm_alloc_page_at(va, VMM_P | VMM_W | VMM_U)) {
            pmm_free_page(phys);
            goto fail;
        }
        if (p == 0) base = va;
    }

    /* 第二步：复制段数据 */
    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uint64_t src_off = phdrs[i].p_offset;
        uint64_t dst_va  = phdrs[i].p_vaddr;
        uint64_t filesz  = phdrs[i].p_filesz;
        uint64_t memsz   = phdrs[i].p_memsz;

        if (filesz > 0) {
            memcpy((void *)dst_va, data + src_off, filesz);
        }
        if (memsz > filesz) {
            memset((void *)(dst_va + filesz), 0, memsz - filesz);
        }
    }

    /* 第三步：解析 .dynamic 段 */
    const char           *strtab     = 0;
    elf64_sym_t          *symtab     = 0;
    elf64_rela_t         *rela       = 0;
    uint64_t              rela_size  = 0;
    uint64_t              rela_ent   = 0;
    elf64_rela_t         *jmprel     = 0;
    uint64_t              jmprel_size= 0;
    void                 *pltgot     = 0;

    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_DYNAMIC) continue;

        const elf64_dyn_t *dyn = (const elf64_dyn_t *)(data + phdrs[i].p_offset);
        uint64_t dyn_count = phdrs[i].p_filesz / sizeof(elf64_dyn_t);

        for (uint64_t j = 0; j < dyn_count; j++) {
            switch (dyn[j].d_tag) {
            case DT_STRTAB:   strtab  = (const char *)(vaddr_base + dyn[j].d_un.d_ptr); break;
            case DT_SYMTAB:   symtab  = (elf64_sym_t *)(vaddr_base + dyn[j].d_un.d_ptr); break;
            case DT_RELA:     rela    = (elf64_rela_t *)(vaddr_base + dyn[j].d_un.d_ptr); break;
            case DT_RELASZ:   rela_size = dyn[j].d_un.d_val; break;
            case DT_RELAENT:  rela_ent  = dyn[j].d_un.d_val; break;
            case DT_JMPREL:   jmprel  = (elf64_rela_t *)(vaddr_base + dyn[j].d_un.d_ptr); break;
            case DT_PLTRELSZ: jmprel_size = dyn[j].d_un.d_val; break;
            case DT_PLTGOT:   pltgot  = (void *)(vaddr_base + dyn[j].d_un.d_ptr); break;
            default: break;
            }
        }
        break;
    }

    /* 第四步：分配并填充 loaded_lib_t */
    loaded_lib_t *lib = (loaded_lib_t *)kmalloc(sizeof(loaded_lib_t));
    if (!lib) goto fail;
    memset(lib, 0, sizeof(*lib));
    lib->base       = (void *)base;
    lib->vaddr_base = vaddr_base;
    lib->strtab     = strtab;
    lib->symtab     = symtab;
    lib->jmprel     = jmprel;
    lib->jmprel_size= jmprel_size;
    lib->pltgot     = pltgot;

    /* 计算符号表条目数 */
    if (strtab && symtab) {
        uint64_t sym_off = (uint64_t)((uint8_t *)symtab - (uint8_t *)vaddr_base);
        uint64_t str_off = (uint64_t)((const uint8_t *)strtab - (const uint8_t *)vaddr_base);
        if (str_off > sym_off) {
            lib->symtab_entries = (str_off - sym_off) / sizeof(elf64_sym_t);
        }
    }

    /* 第五步：执行重定位 */
    if (rela && rela_size > 0 && rela_ent > 0) {
        uint64_t count = rela_size / rela_ent;
        do_rela_relative(lib, rela, count);
        do_rela_glob_dat(lib, rela, count);
    }

    if (jmprel && jmprel_size > 0) {
        uint64_t count = jmprel_size / sizeof(elf64_rela_t);
        do_rela_jump_slot(lib, jmprel, count);
    }

    /* 第六步：加入链表 */
    lib->next = g_loaded_libs;
    g_loaded_libs = lib;

    return (void *)base;

fail:
    /* 简化清理：释放已分配页面 */
    for (uint64_t p = 0; p < num_pages; p++) {
        uint64_t va = vaddr_base + p * PAGE_SIZE;
        uint64_t phys = vmm_get_phys(va);
        if (phys) {
            pmm_free_page(phys);
            vmm_unmap_page(va);
        }
    }
    return 0;
}

void *ldso_resolve(const char *name) {
    if (!name) return 0;

    /* 遍历所有已加载库，查找导出符号 */
    loaded_lib_t *lib = g_loaded_libs;
    while (lib) {
        if (lib->strtab && lib->symtab && lib->symtab_entries > 0) {
            elf64_sym_t *sym = symtab_lookup(lib->symtab,
                                              lib->symtab_entries,
                                              lib->strtab, name);
            if (sym && sym->st_value != 0 &&
                ELF64_ST_BIND(sym->st_info) == STB_GLOBAL) {
                return (uint8_t *)lib->base + sym->st_value;
            }
        }
        lib = lib->next;
    }
    return 0;
}