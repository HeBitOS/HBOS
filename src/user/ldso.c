#include "user/ldso.h"
#include "core/heap.h"
#include "core/vmm.h"
#include "core/pmm.h"
#include "core/task.h"
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
    uint32_t          *gnu_hash;
    elf64_rela_t      *jmprel;
    uint64_t           jmprel_size;
    void              *pltgot;
    uint64_t           num_pages;
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

static uint32_t gnu_symbol_hash(const char *name) {
    uint32_t hash = 5381;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        hash = hash * 33U + *p;
    return hash;
}

static elf64_sym_t *gnu_hash_lookup(const loaded_lib_t *lib,
                                     const char *name) {
    if (!lib || !lib->gnu_hash || !lib->symtab || !lib->strtab)
        return NULL;
    const uint32_t *header = lib->gnu_hash;
    uint32_t bucket_count = header[0];
    uint32_t symbol_offset = header[1];
    uint32_t bloom_size = header[2];
    uint32_t bloom_shift = header[3];
    if (!bucket_count || !bloom_size) return NULL;

    const uint64_t *bloom = (const uint64_t *)(header + 4);
    const uint32_t *buckets =
        (const uint32_t *)(bloom + bloom_size);
    const uint32_t *chains = buckets + bucket_count;
    uint32_t hash = gnu_symbol_hash(name);
    uint64_t word = bloom[(hash / 64U) % bloom_size];
    uint64_t mask = (1ULL << (hash % 64U)) |
                    (1ULL << ((hash >> bloom_shift) % 64U));
    if ((word & mask) != mask) return NULL;

    uint32_t index = buckets[hash % bucket_count];
    if (index < symbol_offset) return NULL;
    for (uint32_t scanned = 0; scanned < 1048576U; scanned++, index++) {
        uint32_t chain_hash = chains[index - symbol_offset];
        if ((chain_hash | 1U) == (hash | 1U)) {
            elf64_sym_t *symbol = &lib->symtab[index];
            if (strcmp(lib->strtab + symbol->st_name, name) == 0)
                return symbol;
        }
        if (chain_hash & 1U) break;
    }
    return NULL;
}

static uint64_t gnu_hash_symbol_count(const uint32_t *header) {
    if (!header || !header[0] || !header[2]) return 0;
    uint32_t bucket_count = header[0];
    uint32_t symbol_offset = header[1];
    uint32_t bloom_size = header[2];
    const uint64_t *bloom = (const uint64_t *)(header + 4);
    const uint32_t *buckets =
        (const uint32_t *)(bloom + bloom_size);
    const uint32_t *chains = buckets + bucket_count;
    uint64_t count = symbol_offset;
    for (uint32_t i = 0; i < bucket_count; i++) {
        uint32_t index = buckets[i];
        if (index < symbol_offset) continue;
        for (uint32_t scanned = 0; scanned < 1048576U; scanned++, index++) {
            uint32_t chain_hash = chains[index - symbol_offset];
            if ((uint64_t)index + 1 > count) count = (uint64_t)index + 1;
            if (chain_hash & 1U) break;
        }
    }
    return count;
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

    /* 分配虚拟地址空间 — 真实共享库的 PT_LOAD vaddr 几乎总是从 0 开始
     * （由真正的动态链接器在加载时选一个实际地址、再整体加上偏移量），
     * 不能直接照抄文件自带的 vaddr 当成真实加载地址去映射/写入——那样会
     * 尝试在虚拟地址 0（NULL 页）附近映射，基本必然失败。这里用一个简单
     * 的 bump 分配器每次选一段全新、足够大、明显不会跟内核/任务栈/堆/
     * TCC 编译产物（0x1000000000）/MMIO（0x100000000）等现有约定冲突的
     * 虚拟地址区间作为本次加载的真实基址，随后所有对 PT_LOAD/.dynamic
     * 里原始 vaddr 的引用都要换算成"真实基址 + (原始 vaddr - 文件最小
     * vaddr)"，而不能直接沿用原始 vaddr。 */
    static uint64_t g_next_ldso_base = 0x0000300000000000ULL;
    uint64_t real_base = g_next_ldso_base;
    g_next_ldso_base += total_size + PAGE_SIZE; /* 留一页保护间隙 */

    for (uint64_t p = 0; p < num_pages; p++) {
        uint64_t va = real_base + p * PAGE_SIZE;
        uint64_t phys = pmm_alloc_page();
        if (!phys) goto fail;
        if (!vmm_alloc_page_at(va, VMM_P | VMM_W | VMM_U)) {
            pmm_free_page(phys);
            goto fail;
        }
    }
    uint64_t base = real_base;
    uint64_t vaddr_base = real_base - vaddr_min; /* 换算用的加法偏移量 */

    /* 第二步：复制段数据（目的地址要换算，不能用文件自带的 p_vaddr） */
    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uint64_t src_off = phdrs[i].p_offset;
        uint64_t dst_va  = vaddr_base + phdrs[i].p_vaddr;
        uint64_t filesz  = phdrs[i].p_filesz;
        uint64_t memsz   = phdrs[i].p_memsz;

        if (filesz > 0) {
            memcpy((void *)dst_va, data + src_off, filesz);
        }
        if (memsz > filesz) {
            memset((void *)(dst_va + filesz), 0, memsz - filesz);
        }
    }

    /* 第三步：解析 .dynamic 段（同样要换算） */
    const char           *strtab     = 0;
    elf64_sym_t          *symtab     = 0;
    elf64_rela_t         *rela       = 0;
    uint64_t              rela_size  = 0;
    uint64_t              rela_ent   = 0;
    elf64_rela_t         *jmprel     = 0;
    uint64_t              jmprel_size= 0;
    void                 *pltgot     = 0;
    uint32_t             *gnu_hash   = 0;

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
            case DT_GNU_HASH: gnu_hash = (uint32_t *)(vaddr_base + dyn[j].d_un.d_ptr); break;
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
    lib->gnu_hash   = gnu_hash;

    /* 计算符号表条目数 */
    if (gnu_hash) {
        lib->symtab_entries = gnu_hash_symbol_count(gnu_hash);
    } else if (strtab && symtab) {
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
    lib->num_pages = num_pages;

    return (void *)lib;

fail:
    /* 简化清理：释放已分配页面（用真实映射地址 real_base，不是换算用的
     * vaddr_base 偏移量——这里失败时 vaddr_base 可能还没来得及赋值）。 */
    for (uint64_t p = 0; p < num_pages; p++) {
        uint64_t va = real_base + p * PAGE_SIZE;
        uint64_t phys = vmm_get_phys(va);
        if (phys) {
            pmm_free_page(phys);
            vmm_unmap_page(va);
        }
    }
    return 0;
}

/* 验证句柄确实指向当前已加载库链表中的一项，防止调用方传入野指针 */
static int handle_is_valid(void *handle) {
    for (loaded_lib_t *l = g_loaded_libs; l; l = l->next) {
        if ((void *)l == handle) return 1;
    }
    return 0;
}

void *ldso_dlsym(void *handle, const char *name) {
    if (!handle || !name || !handle_is_valid(handle)) return 0;
    loaded_lib_t *lib = (loaded_lib_t *)handle;
    if (!lib->strtab || !lib->symtab || lib->symtab_entries == 0) return 0;

    elf64_sym_t *sym = gnu_hash_lookup(lib, name);
    if (!sym)
        sym = symtab_lookup(lib->symtab, lib->symtab_entries,
                            lib->strtab, name);
    if (!sym || sym->st_value == 0) return 0;
    if (ELF64_ST_BIND(sym->st_info) != STB_GLOBAL &&
        ELF64_ST_BIND(sym->st_info) != STB_WEAK) return 0;
    return (void *)(uintptr_t)(lib->vaddr_base + sym->st_value);
}

int ldso_close(void *handle) {
    if (!handle || !handle_is_valid(handle)) return -1;
    loaded_lib_t *lib = (loaded_lib_t *)handle;

    for (uint64_t p = 0; p < lib->num_pages; p++) {
        uint64_t va = (uint64_t)(uintptr_t)lib->base + p * PAGE_SIZE;
        uint64_t phys = vmm_get_phys(va);
        if (phys) {
            pmm_free_page(phys);
            vmm_unmap_page(va);
        }
    }

    loaded_lib_t **pp = &g_loaded_libs;
    while (*pp) {
        if (*pp == lib) { *pp = lib->next; break; }
        pp = &(*pp)->next;
    }
    kfree(lib);
    return 0;
}

void *ldso_resolve(const char *name) {
    if (!name) return 0;

    /* 遍历所有已加载库，查找导出符号 */
    loaded_lib_t *lib = g_loaded_libs;
    while (lib) {
        if (lib->strtab && lib->symtab && lib->symtab_entries > 0) {
            elf64_sym_t *sym = gnu_hash_lookup(lib, name);
            if (!sym)
                sym = symtab_lookup(lib->symtab, lib->symtab_entries,
                                    lib->strtab, name);
            if (sym && sym->st_value != 0 &&
                ELF64_ST_BIND(sym->st_info) == STB_GLOBAL) {
                return (void *)(uintptr_t)(lib->vaddr_base + sym->st_value);
            }
        }
        lib = lib->next;
    }

    /* 最后一步：在发起调用的这个程序自己的 .symtab 里找（让共享库能反过来
     * 调用它自己静态链接进来的 printf/malloc 等——见 task.h 里
     * host_symtab 字段的注释和 src/elf.c 的 attach_host_symtab）。这里
     * st_value 就是最终绝对地址，不用像上面共享库那样再加 lib->base：
     * 宿主程序是静态可执行文件，其 .symtab 里记录的本来就是链接时定好的
     * 真实地址，不是文件相对偏移量。 */
    const task_t *t = task_current();
    if (t && t->host_symtab && t->host_strtab && t->host_symtab_count > 0) {
        elf64_sym_t *sym = symtab_lookup((elf64_sym_t *)t->host_symtab,
                                          t->host_symtab_count,
                                          (const char *)t->host_strtab, name);
        if (sym && sym->st_value != 0 &&
            (ELF64_ST_BIND(sym->st_info) == STB_GLOBAL ||
             ELF64_ST_BIND(sym->st_info) == STB_WEAK)) {
            return (void *)sym->st_value;
        }
    }

    return 0;
}
