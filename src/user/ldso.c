#include "user/ldso.h"
#include "core/heap.h"
#include "core/vmm.h"
#include "core/task.h"
#include "smp.h"
#include "string.h"
#include "vfs.h"

#define LDSO_HEADERS_MAX (64U * 1024U)
#define LDSO_MAX_NEEDED 16U
#define LDSO_PATH_MAX   64U
#define LDSO_GRAPH_MAX  64U
#define LDSO_TLS_MAX_SIZE (1024U * 1024U)
#define LDSO_VERSION_RECORD_MAX 256U
#define LDSO_IFUNC_MAX 256U

typedef struct {
    const uint8_t *memory;
    vfs_node_t *node;
    uint64_t size;
    const uint8_t *headers;
    uint8_t *owned_headers;
    const elf64_ehdr_t *ehdr;
} ldso_source_t;

typedef struct ldso_ifunc_target {
    struct ldso_ifunc_target *next;
    uint64_t *location;
    int64_t addend;
} ldso_ifunc_target_t;

typedef struct ldso_ifunc_record {
    struct ldso_ifunc_record *next;
    ldso_ifunc_target_t *targets;
    uint64_t resolver;
    uint64_t result;
    uint64_t symbol_index;
    bool resolved;
} ldso_ifunc_record_t;

/* 已加载的共享库链表 */
typedef struct loaded_lib {
    struct loaded_lib *next;
    const void        *owner_mm;
    void              *base;
    uint64_t           vaddr_base;
    const char        *soname;
    const char        *strtab;
    elf64_sym_t       *symtab;
    uint64_t           symtab_entries;
    uint64_t           strtab_size;
    uint32_t          *gnu_hash;
    uint16_t          *versym;
    elf64_verdef_t    *verdef;
    elf64_verneed_t   *verneed;
    uint32_t           verdef_count;
    uint32_t           verneed_count;
    ldso_ifunc_record_t *ifuncs;
    uint32_t           ifunc_count;
    elf64_rela_t      *jmprel;
    uint64_t           jmprel_size;
    void              *pltgot;
    uint64_t           num_pages;
    uint32_t           refs;
    uint32_t           dependency_count;
    bool               loading;
    bool               initializing;
    bool               initialized;
    bool               finalized;
    uint64_t           init_function;
    uint64_t           fini_function;
    uint64_t          *init_array;
    uint64_t          *fini_array;
    uint32_t           init_array_count;
    uint32_t           fini_array_count;
    uint32_t           tls_module_id;
    uint64_t           tls_template;
    uint64_t           tls_filesz;
    uint64_t           tls_memsz;
    uint64_t           tls_align;
    char               path[LDSO_PATH_MAX];
    struct loaded_lib *dependencies[LDSO_MAX_NEEDED];
} loaded_lib_t;

static loaded_lib_t *g_loaded_libs;
static spinlock_t g_ldso_metadata_lock;
static uint32_t g_next_tls_module_id = 1;
static uint64_t g_next_ldso_base = 0x0000300000000000ULL;
static uint64_t g_next_tls_instance_base = 0x0000380000000000ULL;

static void ldso_metadata_lock(void) {
    /* A raw spin lock must not be held by a task that can be preempted on the
     * same CPU: the successor would spin forever waiting for a task that can
     * no longer run.  Loader metadata critical sections are deliberately
     * short and never include VFS I/O, page allocation, or user callbacks. */
    task_preempt_disable();
    spinlock_acquire(&g_ldso_metadata_lock);
}

static void ldso_metadata_unlock(void) {
    spinlock_release(&g_ldso_metadata_lock);
    task_preempt_enable();
}

static uint32_t ldso_reserve_tls_module_id(void) {
    for (;;) {
        uint32_t observed = __atomic_load_n(&g_next_tls_module_id,
                                             __ATOMIC_RELAXED);
        if (!observed || observed == UINT32_MAX) return 0;
        uint32_t expected = observed;
        if (__atomic_compare_exchange_n(&g_next_tls_module_id, &expected,
                                        observed + 1, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return observed;
    }
}

static loaded_lib_t *ldso_find_path(const void *owner_mm,
                                    const char *path) {
    if (!owner_mm || !path || !path[0]) return NULL;
    loaded_lib_t *found = NULL;
    ldso_metadata_lock();
    for (loaded_lib_t *lib = g_loaded_libs; lib; lib = lib->next) {
        if (lib->owner_mm == owner_mm && lib->path[0] &&
            strcmp(lib->path, path) == 0) {
            found = lib;
            break;
        }
    }
    ldso_metadata_unlock();
    return found;
}

static void ldso_unlink(loaded_lib_t *lib) {
    ldso_metadata_lock();
    loaded_lib_t **link = &g_loaded_libs;
    while (*link) {
        if (*link == lib) {
            *link = lib->next;
            break;
        }
        link = &(*link)->next;
    }
    ldso_metadata_unlock();
}

static void ldso_free_ifuncs(loaded_lib_t *lib) {
    if (!lib) return;
    ldso_ifunc_record_t *record = lib->ifuncs;
    while (record) {
        ldso_ifunc_record_t *next_record = record->next;
        ldso_ifunc_target_t *target = record->targets;
        while (target) {
            ldso_ifunc_target_t *next_target = target->next;
            kfree(target);
            target = next_target;
        }
        kfree(record);
        record = next_record;
    }
    lib->ifuncs = NULL;
    lib->ifunc_count = 0;
}

static void ldso_release_reference(loaded_lib_t *lib) {
    if (!lib || !lib->refs) return;
    if (--lib->refs) return;

    ldso_unlink(lib);
    for (uint32_t i = 0; i < lib->dependency_count; i++)
        ldso_release_reference(lib->dependencies[i]);
    for (uint64_t page = 0; page < lib->num_pages; page++)
        vmm_release_page((uint64_t)(uintptr_t)lib->base + page * PAGE_SIZE);
    ldso_free_ifuncs(lib);
    kfree(lib);
}

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

static int ldso_source_read(const ldso_source_t *source, uint64_t offset,
                            void *buffer, uint64_t count) {
    if (!source || (!buffer && count) || offset > source->size ||
        count > source->size - offset)
        return -1;
    if (source->memory) {
        if (count) memcpy(buffer, source->memory + offset, (size_t)count);
        return 0;
    }
    uint64_t done = 0;
    while (done < count) {
        uint64_t remaining = count - done;
        uint32_t chunk = remaining > 64U * 1024U ?
                         64U * 1024U : (uint32_t)remaining;
        int got = vfs_read(source->node, (uint32_t)(offset + done),
                           (uint8_t *)buffer + done, chunk);
        if (got != (int)chunk) return -1;
        done += chunk;
    }
    return 0;
}

static int ldso_validate_headers(ldso_source_t *source) {
    if (!source || !source->headers ||
        elf64_check((const elf64_ehdr_t *)source->headers,
                    (size_t)source->size) != 0)
        return -1;
    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)source->headers;
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0 ||
        ehdr->e_phentsize < sizeof(elf64_phdr_t) ||
        ehdr->e_phentsize > UINT64_MAX / (uint64_t)ehdr->e_phnum)
        return -1;
    uint64_t table_size =
        (uint64_t)ehdr->e_phentsize * (uint64_t)ehdr->e_phnum;
    if (ehdr->e_phoff > source->size ||
        table_size > source->size - ehdr->e_phoff)
        return -1;
    source->ehdr = ehdr;
    return 0;
}

static int ldso_prepare_memory_source(const uint8_t *data, size_t size,
                                      ldso_source_t *source) {
    memset(source, 0, sizeof(*source));
    source->memory = data;
    source->headers = data;
    source->size = size;
    return ldso_validate_headers(source);
}

static int ldso_prepare_vfs_source(vfs_node_t *node,
                                   ldso_source_t *source) {
    memset(source, 0, sizeof(*source));
    if (!node || node->type != VFS_NODE_FILE ||
        node->size < sizeof(elf64_ehdr_t))
        return -1;
    elf64_ehdr_t header;
    if (vfs_read(node, 0, &header, sizeof(header)) != (int)sizeof(header) ||
        header.e_phnum == 0 ||
        header.e_phentsize < sizeof(elf64_phdr_t) ||
        header.e_phentsize > UINT64_MAX / (uint64_t)header.e_phnum)
        return -1;
    uint64_t table_size =
        (uint64_t)header.e_phentsize * (uint64_t)header.e_phnum;
    if (header.e_phoff > UINT64_MAX - table_size) return -1;
    uint64_t headers_size = header.e_phoff + table_size;
    if (headers_size > node->size || headers_size > LDSO_HEADERS_MAX)
        return -1;
    source->owned_headers = (uint8_t *)kmalloc((size_t)headers_size);
    if (!source->owned_headers) return -1;
    source->node = node;
    source->size = node->size;
    source->headers = source->owned_headers;
    if (ldso_source_read(source, 0, source->owned_headers,
                         headers_size) != 0 ||
        ldso_validate_headers(source) != 0) {
        kfree(source->owned_headers);
        memset(source, 0, sizeof(*source));
        return -1;
    }
    return 0;
}

static void ldso_release_source(ldso_source_t *source) {
    if (!source) return;
    if (source->owned_headers) kfree(source->owned_headers);
    memset(source, 0, sizeof(*source));
}

static const elf64_phdr_t *ldso_source_phdr(const ldso_source_t *source,
                                            uint16_t index) {
    if (!source || !source->ehdr || index >= source->ehdr->e_phnum)
        return NULL;
    return (const elf64_phdr_t *)(source->headers +
        source->ehdr->e_phoff + index * source->ehdr->e_phentsize);
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
static int ldso_object_contains(const loaded_lib_t *lib, uint64_t address,
                                uint64_t size) {
    if (!lib) return 0;
    uint64_t start = (uint64_t)(uintptr_t)lib->base;
    uint64_t span = lib->num_pages * PAGE_SIZE;
    return address >= start && address <= start + span &&
           size <= start + span - address;
}

static const char *ldso_version_string(const loaded_lib_t *lib,
                                       uint32_t offset) {
    if (!lib || !lib->strtab || offset >= lib->strtab_size) return NULL;
    size_t available = (size_t)(lib->strtab_size - offset);
    const char *value = lib->strtab + offset;
    return strnlen(value, available) < available ? value : NULL;
}

static int ldso_validate_versions(const loaded_lib_t *lib) {
    if (!lib) return -1;
    if (lib->versym &&
        (lib->symtab_entries > UINT64_MAX / sizeof(uint16_t) ||
         !ldso_object_contains(lib, (uint64_t)(uintptr_t)lib->versym,
                               lib->symtab_entries * sizeof(uint16_t))))
        return -1;
    if (lib->verdef_count > LDSO_VERSION_RECORD_MAX ||
        lib->verneed_count > LDSO_VERSION_RECORD_MAX ||
        (lib->verdef_count && !lib->verdef) ||
        (lib->verneed_count && !lib->verneed))
        return -1;

    elf64_verdef_t *definition = lib->verdef;
    for (uint32_t i = 0; i < lib->verdef_count; i++) {
        uint64_t address = (uint64_t)(uintptr_t)definition;
        if (!ldso_object_contains(lib, address, sizeof(*definition)) ||
            definition->vd_version != 1 || !definition->vd_cnt ||
            !definition->vd_aux ||
            address > UINT64_MAX - definition->vd_aux)
            return -1;
        elf64_verdaux_t *aux = (elf64_verdaux_t *)(uintptr_t)(
            address + definition->vd_aux);
        for (uint16_t j = 0; j < definition->vd_cnt; j++) {
            uint64_t aux_address = (uint64_t)(uintptr_t)aux;
            if (!ldso_object_contains(lib, aux_address, sizeof(*aux)) ||
                !ldso_version_string(lib, aux->vda_name))
                return -1;
            if (j + 1 < definition->vd_cnt) {
                if (!aux->vda_next ||
                    aux_address > UINT64_MAX - aux->vda_next)
                    return -1;
                aux = (elf64_verdaux_t *)(uintptr_t)(aux_address +
                                                       aux->vda_next);
            }
        }
        if (i + 1 < lib->verdef_count) {
            if (!definition->vd_next ||
                address > UINT64_MAX - definition->vd_next)
                return -1;
            definition = (elf64_verdef_t *)(uintptr_t)(address +
                                                        definition->vd_next);
        }
    }

    elf64_verneed_t *need = lib->verneed;
    for (uint32_t i = 0; i < lib->verneed_count; i++) {
        uint64_t address = (uint64_t)(uintptr_t)need;
        if (!ldso_object_contains(lib, address, sizeof(*need)) ||
            need->vn_version != 1 || !need->vn_cnt || !need->vn_aux ||
            !ldso_version_string(lib, need->vn_file) ||
            address > UINT64_MAX - need->vn_aux)
            return -1;
        elf64_vernaux_t *aux = (elf64_vernaux_t *)(uintptr_t)(
            address + need->vn_aux);
        for (uint16_t j = 0; j < need->vn_cnt; j++) {
            uint64_t aux_address = (uint64_t)(uintptr_t)aux;
            if (!ldso_object_contains(lib, aux_address, sizeof(*aux)) ||
                !ldso_version_string(lib, aux->vna_name))
                return -1;
            if (j + 1 < need->vn_cnt) {
                if (!aux->vna_next ||
                    aux_address > UINT64_MAX - aux->vna_next)
                    return -1;
                aux = (elf64_vernaux_t *)(uintptr_t)(aux_address +
                                                       aux->vna_next);
            }
        }
        if (i + 1 < lib->verneed_count) {
            if (!need->vn_next || address > UINT64_MAX - need->vn_next)
                return -1;
            need = (elf64_verneed_t *)(uintptr_t)(address + need->vn_next);
        }
    }
    return 0;
}

static const char *ldso_definition_version(const loaded_lib_t *lib,
                                           uint16_t version_index) {
    uint16_t wanted = version_index & VER_NDX_MASK;
    if (wanted <= VER_NDX_GLOBAL) return NULL;
    elf64_verdef_t *definition = lib->verdef;
    for (uint32_t i = 0; i < lib->verdef_count; i++) {
        uint64_t address = (uint64_t)(uintptr_t)definition;
        if ((definition->vd_ndx & VER_NDX_MASK) == wanted) {
            elf64_verdaux_t *aux = (elf64_verdaux_t *)(uintptr_t)(
                address + definition->vd_aux);
            return ldso_version_string(lib, aux->vda_name);
        }
        if (!definition->vd_next) break;
        definition = (elf64_verdef_t *)(uintptr_t)(address +
                                                    definition->vd_next);
    }
    return NULL;
}

static const char *ldso_required_version(const loaded_lib_t *lib,
                                         uint64_t symbol_index) {
    if (!lib || !lib->versym || symbol_index >= lib->symtab_entries)
        return NULL;
    uint16_t wanted = lib->versym[symbol_index] & VER_NDX_MASK;
    if (wanted <= VER_NDX_GLOBAL) return NULL;
    elf64_verneed_t *need = lib->verneed;
    for (uint32_t i = 0; i < lib->verneed_count; i++) {
        uint64_t address = (uint64_t)(uintptr_t)need;
        elf64_vernaux_t *aux = (elf64_vernaux_t *)(uintptr_t)(
            address + need->vn_aux);
        for (uint16_t j = 0; j < need->vn_cnt; j++) {
            if ((aux->vna_other & VER_NDX_MASK) == wanted)
                return ldso_version_string(lib, aux->vna_name);
            if (!aux->vna_next) break;
            aux = (elf64_vernaux_t *)(uintptr_t)(
                (uint64_t)(uintptr_t)aux + aux->vna_next);
        }
        if (!need->vn_next) break;
        need = (elf64_verneed_t *)(uintptr_t)(address + need->vn_next);
    }
    return NULL;
}

static int ldso_requested_version(const loaded_lib_t *requester,
                                  uint64_t symbol_index,
                                  const char **version) {
    if (!requester || !version || symbol_index >= requester->symtab_entries)
        return -1;
    *version = NULL;
    if (!requester->versym) return 0;
    uint16_t index = requester->versym[symbol_index] & VER_NDX_MASK;
    if (index <= VER_NDX_GLOBAL) return 0;
    elf64_sym_t *symbol = &requester->symtab[symbol_index];
    *version = symbol->st_shndx != 0 ?
        ldso_definition_version(requester, index) :
        ldso_required_version(requester, symbol_index);
    return *version ? 0 : -1;
}

static int ldso_symbol_version_matches(const loaded_lib_t *provider,
                                       uint64_t symbol_index,
                                       const char *required_version) {
    uint16_t version = provider->versym ? provider->versym[symbol_index] :
                                         VER_NDX_GLOBAL;
    uint16_t index = version & VER_NDX_MASK;
    if (required_version) {
        const char *provided = ldso_definition_version(provider, index);
        return provided && strcmp(provided, required_version) == 0;
    }
    return index <= VER_NDX_GLOBAL || !(version & VER_NDX_HIDDEN);
}

static elf64_sym_t *ldso_lookup_export(loaded_lib_t *provider,
                                      const char *name,
                                      const char *required_version) {
    if (!provider || !name || !provider->symtab || !provider->strtab)
        return NULL;
    elf64_sym_t *hashed = gnu_hash_lookup(provider, name);
    if (hashed) {
        uint64_t index = (uint64_t)(hashed - provider->symtab);
        const char *hashed_name = index < provider->symtab_entries ?
            ldso_version_string(provider, hashed->st_name) : NULL;
        if (hashed_name && strcmp(hashed_name, name) == 0 &&
            hashed->st_shndx != 0 &&
            (ELF64_ST_BIND(hashed->st_info) == STB_GLOBAL ||
             ELF64_ST_BIND(hashed->st_info) == STB_WEAK) &&
            ldso_symbol_version_matches(provider, index, required_version))
            return hashed;
    }
    elf64_sym_t *weak = NULL;
    for (uint64_t i = 0; i < provider->symtab_entries; i++) {
        elf64_sym_t *symbol = &provider->symtab[i];
        const char *symbol_name =
            ldso_version_string(provider, symbol->st_name);
        if (symbol->st_shndx == 0 ||
            !symbol_name || strcmp(symbol_name, name) != 0 ||
            (ELF64_ST_BIND(symbol->st_info) != STB_GLOBAL &&
             ELF64_ST_BIND(symbol->st_info) != STB_WEAK) ||
            !ldso_symbol_version_matches(provider, i, required_version))
            continue;
        if (ELF64_ST_BIND(symbol->st_info) == STB_GLOBAL) return symbol;
        if (!weak) weak = symbol;
    }
    return weak;
}

static loaded_lib_t *ldso_resolve_object_symbol(loaded_lib_t *requester,
                                                 uint64_t symbol_index,
                                                 elf64_sym_t **resolved,
                                                 const char **version) {
    if (!requester || !resolved || !version || !requester->symtab ||
        symbol_index >= requester->symtab_entries ||
        ldso_requested_version(requester, symbol_index, version) < 0)
        return NULL;
    elf64_sym_t *symbol = &requester->symtab[symbol_index];
    if (symbol->st_shndx != 0 &&
        ELF64_ST_BIND(symbol->st_info) == STB_LOCAL) {
        *resolved = symbol;
        return requester;
    }
    const char *name = ldso_version_string(requester, symbol->st_name);
    if (!name) return NULL;
    loaded_lib_t *provider_result = NULL;
    elf64_sym_t *symbol_result = NULL;
    ldso_metadata_lock();
    for (loaded_lib_t *provider = g_loaded_libs; provider;
         provider = provider->next) {
        if (provider->owner_mm != requester->owner_mm) continue;
        elf64_sym_t *candidate =
            ldso_lookup_export(provider, name, *version);
        if (candidate) {
            provider_result = provider;
            symbol_result = candidate;
            break;
        }
    }
    ldso_metadata_unlock();
    if (provider_result) {
        *resolved = symbol_result;
        return provider_result;
    }
    if (symbol->st_shndx != 0) {
        *resolved = symbol;
        return requester;
    }
    return NULL;
}

static void *ldso_resolve_host_symbol(const char *name) {
    const task_t *task = task_current();
    if (!name || !task || !task->host_symtab || !task->host_strtab ||
        !task->host_symtab_count)
        return NULL;
    elf64_sym_t *symbol = symtab_lookup(
        (elf64_sym_t *)task->host_symtab, task->host_symtab_count,
        (const char *)task->host_strtab, name);
    if (!symbol || symbol->st_shndx == 0 ||
        (ELF64_ST_BIND(symbol->st_info) != STB_GLOBAL &&
         ELF64_ST_BIND(symbol->st_info) != STB_WEAK))
        return NULL;
    return (void *)(uintptr_t)symbol->st_value;
}

static uint64_t *ldso_relocation_target(loaded_lib_t *lib,
                                        uint64_t offset) {
    if (!lib || offset > UINT64_MAX - lib->vaddr_base)
        return NULL;
    uint64_t target = lib->vaddr_base + offset;
    if (!ldso_object_contains(lib, target, sizeof(uint64_t))) return NULL;
    return (uint64_t *)(uintptr_t)target;
}

static int ldso_ifunc_value(uint64_t result, int64_t addend,
                            uint64_t *value) {
    if (!value) return -1;
    if (addend < 0) {
        uint64_t magnitude = (uint64_t)(-(addend + 1)) + 1;
        if (magnitude > result) return -1;
        *value = result - magnitude;
    } else {
        if ((uint64_t)addend > UINT64_MAX - result) return -1;
        *value = result + (uint64_t)addend;
    }
    return 0;
}

static ldso_ifunc_record_t *ldso_find_ifunc(loaded_lib_t *provider,
                                             uint64_t symbol_index,
                                             uint64_t resolver) {
    for (ldso_ifunc_record_t *record = provider ? provider->ifuncs : NULL;
         record; record = record->next) {
        if (record->symbol_index == symbol_index &&
            (symbol_index != UINT64_MAX || record->resolver == resolver))
            return record;
    }
    return NULL;
}

static ldso_ifunc_record_t *ldso_register_ifunc(
    loaded_lib_t *provider, uint64_t symbol_index, uint64_t resolver,
    uint64_t *location, int64_t addend) {
    if (!provider || !resolver ||
        !ldso_object_contains(provider, resolver, 1))
        return NULL;
    ldso_ifunc_record_t *record =
        ldso_find_ifunc(provider, symbol_index, resolver);
    if (!record) {
        if (provider->ifunc_count >= LDSO_IFUNC_MAX) return NULL;
        record = (ldso_ifunc_record_t *)kmalloc(sizeof(*record));
        if (!record) return NULL;
        memset(record, 0, sizeof(*record));
        record->resolver = resolver;
        record->symbol_index = symbol_index;
        record->next = provider->ifuncs;
        provider->ifuncs = record;
        provider->ifunc_count++;
    }
    if (!location) return record;
    if (record->resolved) {
        uint64_t value = 0;
        if (ldso_ifunc_value(record->result, addend, &value) < 0)
            return NULL;
        *location = value;
        return record;
    }
    ldso_ifunc_target_t *target =
        (ldso_ifunc_target_t *)kmalloc(sizeof(*target));
    if (!target) return NULL;
    target->location = location;
    target->addend = addend;
    target->next = record->targets;
    record->targets = target;
    return record;
}

static int ldso_register_exported_ifuncs(loaded_lib_t *lib) {
    if (!lib || !lib->symtab) return 0;
    for (uint64_t i = 0; i < lib->symtab_entries; i++) {
        elf64_sym_t *symbol = &lib->symtab[i];
        if (symbol->st_shndx == 0 ||
            ELF64_ST_TYPE(symbol->st_info) != STT_GNU_IFUNC ||
            (ELF64_ST_BIND(symbol->st_info) != STB_GLOBAL &&
             ELF64_ST_BIND(symbol->st_info) != STB_WEAK))
            continue;
        if (symbol->st_value > UINT64_MAX - lib->vaddr_base ||
            !ldso_register_ifunc(lib, i,
                                 lib->vaddr_base + symbol->st_value,
                                 NULL, 0))
            return -1;
    }
    return 0;
}

static int do_rela_relative(loaded_lib_t *lib, elf64_rela_t *rela,
                            uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        if (ELF64_R_TYPE(rela[i].r_info) == R_X86_64_RELATIVE) {
            uint64_t *loc = ldso_relocation_target(lib, rela[i].r_offset);
            if (!loc) return -1;
            *loc = lib->vaddr_base + rela[i].r_addend;
        }
    }
    return 0;
}

static int do_rela_symbols(loaded_lib_t *lib, elf64_rela_t *rela,
                           uint64_t count, uint32_t wanted_type) {
    for (uint64_t i = 0; i < count; i++) {
        if (ELF64_R_TYPE(rela[i].r_info) != wanted_type) continue;

        uint64_t sym_idx = ELF64_R_SYM(rela[i].r_info);
        if (!lib->symtab || !lib->strtab ||
            sym_idx >= lib->symtab_entries)
            return -1;
        elf64_sym_t *sym = &lib->symtab[sym_idx];
        const char *sym_name = ldso_version_string(lib, sym->st_name);
        if (!sym_name) return -1;
        uint64_t *loc = ldso_relocation_target(lib, rela[i].r_offset);
        if (!loc) return -1;

        const char *required_version = NULL;
        if (ldso_requested_version(lib, sym_idx, &required_version) < 0)
            return -1;
        elf64_sym_t *resolved = NULL;
        loaded_lib_t *provider = ldso_resolve_object_symbol(
            lib, sym_idx, &resolved, &required_version);
        if (provider && resolved &&
            ELF64_ST_TYPE(resolved->st_info) == STT_GNU_IFUNC) {
            uint64_t provider_index =
                (uint64_t)(resolved - provider->symtab);
            int64_t addend = wanted_type == R_X86_64_64 ?
                             rela[i].r_addend : 0;
            if (!ldso_register_ifunc(
                    provider, provider_index,
                    provider->vaddr_base + resolved->st_value,
                    loc, addend))
                return -1;
            continue;
        }
        void *addr = provider && resolved ?
            (void *)(uintptr_t)(provider->vaddr_base + resolved->st_value) :
            NULL;
        if (!addr && !required_version)
            addr = ldso_resolve_host_symbol(sym_name);
        if (!addr && ELF64_ST_BIND(sym->st_info) != STB_WEAK)
            return -1;
        uint64_t value = (uint64_t)(uintptr_t)addr;
        if (wanted_type == R_X86_64_64)
            value += (uint64_t)rela[i].r_addend;
        *loc = value;
    }
    return 0;
}

static int do_rela_irelative(loaded_lib_t *lib, elf64_rela_t *rela,
                             uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        if (ELF64_R_TYPE(rela[i].r_info) != R_X86_64_IRELATIVE) continue;
        if (rela[i].r_addend < 0 ||
            (uint64_t)rela[i].r_addend > UINT64_MAX - lib->vaddr_base)
            return -1;
        uint64_t resolver = lib->vaddr_base + (uint64_t)rela[i].r_addend;
        uint64_t *target =
            ldso_relocation_target(lib, rela[i].r_offset);
        if (!target || !ldso_register_ifunc(
                lib, UINT64_MAX, resolver, target, 0))
            return -1;
    }
    return 0;
}

static int do_rela_tls(loaded_lib_t *lib, elf64_rela_t *rela,
                       uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint32_t type = ELF64_R_TYPE(rela[i].r_info);
        if (type != R_X86_64_DTPMOD64 && type != R_X86_64_DTPOFF64)
            continue;
        uint64_t index = ELF64_R_SYM(rela[i].r_info);
        if (!lib->symtab || index >= lib->symtab_entries) return -1;
        uint64_t *target =
            ldso_relocation_target(lib, rela[i].r_offset);
        if (!target) return -1;
        /* GNU ld emits a symbol-index-zero DTPMOD64 relocation for a
         * translation-unit-local TLS variable.  In that form the module is
         * unambiguously the object containing the relocation. */
        if (index == 0) {
            if (!lib->tls_module_id) return -1;
            if (type == R_X86_64_DTPMOD64) {
                *target = lib->tls_module_id;
            } else {
                if (rela[i].r_addend < 0) return -1;
                *target = (uint64_t)rela[i].r_addend;
            }
            continue;
        }
        elf64_sym_t *symbol = &lib->symtab[index];
        elf64_sym_t *resolved = NULL;
        const char *required_version = NULL;
        loaded_lib_t *owner = ldso_resolve_object_symbol(
            lib, index, &resolved, &required_version);
        if (!owner || !owner->tls_module_id || !resolved ||
            ELF64_ST_TYPE(resolved->st_info) != STT_TLS) {
            if (ELF64_ST_BIND(symbol->st_info) == STB_WEAK) {
                *target = 0;
                continue;
            }
            return -1;
        }
        if (type == R_X86_64_DTPMOD64) {
            *target = owner->tls_module_id;
        } else {
            uint64_t value = resolved->st_value;
            if (rela[i].r_addend < 0) {
                uint64_t magnitude =
                    (uint64_t)(-(rela[i].r_addend + 1)) + 1;
                if (magnitude > value) return -1;
                value -= magnitude;
            } else {
                if ((uint64_t)rela[i].r_addend > UINT64_MAX - value)
                    return -1;
                value += (uint64_t)rela[i].r_addend;
            }
            *target = value;
        }
    }
    return 0;
}

static loaded_lib_t *ldso_load_vfs_named(vfs_node_t *node,
                                          const char *path);

static vfs_node_t *ldso_find_needed_node(const char *parent_path,
                                         const char *needed,
                                         char resolved[LDSO_PATH_MAX]) {
    if (!needed || !needed[0]) return NULL;
    size_t needed_length = strlen(needed);
    if (needed_length + 1 > LDSO_PATH_MAX) return NULL;

    if (needed[0] == '/') {
        strcpy(resolved, needed);
        vfs_node_t *node = vfs_lookup(resolved);
        return node && node->type == VFS_NODE_FILE ? node : NULL;
    }

    if (parent_path && parent_path[0]) {
        size_t slash = 0;
        for (size_t i = 0; parent_path[i]; i++)
            if (parent_path[i] == '/') slash = i + 1;
        if (slash && slash + needed_length + 1 <= LDSO_PATH_MAX) {
            memcpy(resolved, parent_path, slash);
            memcpy(resolved + slash, needed, needed_length + 1);
            vfs_node_t *node = vfs_lookup(resolved);
            if (node && node->type == VFS_NODE_FILE) return node;
        }
    }

    static const char *prefixes[] = { "/lib/", "/" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t prefix_length = strlen(prefixes[i]);
        if (prefix_length + needed_length + 1 > LDSO_PATH_MAX) continue;
        memcpy(resolved, prefixes[i], prefix_length);
        memcpy(resolved + prefix_length, needed, needed_length + 1);
        vfs_node_t *node = vfs_lookup(resolved);
        if (node && node->type == VFS_NODE_FILE) return node;
    }
    resolved[0] = '\0';
    return NULL;
}

static loaded_lib_t *ldso_load_source(const ldso_source_t *source,
                                       const char *path) {
    if (!source || !source->ehdr || source->ehdr->e_type != ET_DYN)
        return NULL;
    const task_t *owner = task_current();
    if (!owner || !owner->mm) return NULL;
    const elf64_ehdr_t *ehdr = source->ehdr;
    loaded_lib_t *lib = NULL;
    int lib_linked = 0;

    /* 第一步：验证 PT_LOAD 并计算总内存需求。 */
    uint64_t vaddr_min = (uint64_t)-1;
    uint64_t vaddr_max = 0;
    int has_load = 0;
    uint64_t tls_vaddr = 0;
    uint64_t tls_offset = 0;
    uint64_t tls_filesz = 0;
    uint64_t tls_memsz = 0;
    uint64_t tls_align = 1;
    int has_tls = 0;

    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = ldso_source_phdr(source, (uint16_t)i);
        if (!ph) return NULL;
        if (ph->p_type == PT_TLS) {
            if (has_tls || ph->p_memsz < ph->p_filesz ||
                ph->p_memsz > LDSO_TLS_MAX_SIZE ||
                ph->p_offset > source->size ||
                ph->p_filesz > source->size - ph->p_offset ||
                ph->p_vaddr > UINT64_MAX - ph->p_memsz ||
                (ph->p_align && (ph->p_align & (ph->p_align - 1))))
                return NULL;
            has_tls = 1;
            tls_vaddr = ph->p_vaddr;
            tls_offset = ph->p_offset;
            tls_filesz = ph->p_filesz;
            tls_memsz = ph->p_memsz;
            tls_align = ph->p_align ? ph->p_align : 1;
        }
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz < ph->p_filesz ||
            ph->p_offset > source->size ||
            ph->p_filesz > source->size - ph->p_offset ||
            ph->p_vaddr > UINT64_MAX - ph->p_memsz)
            return NULL;
        has_load = 1;
        if (ph->p_vaddr < vaddr_min) vaddr_min = ph->p_vaddr;
        uint64_t end = ph->p_vaddr + ph->p_memsz;
        if (end > vaddr_max) vaddr_max = end;
    }
    if (!has_load || vaddr_max <= vaddr_min) return NULL;
    if (has_tls && (tls_vaddr < vaddr_min || tls_vaddr > vaddr_max ||
                    tls_memsz > vaddr_max - tls_vaddr))
        return NULL;
    if (has_tls && tls_memsz) {
        int covered = 0;
        for (int i = 0; i < (int)ehdr->e_phnum; i++) {
            const elf64_phdr_t *load =
                ldso_source_phdr(source, (uint16_t)i);
            if (!load || load->p_type != PT_LOAD ||
                tls_vaddr < load->p_vaddr ||
                tls_memsz > load->p_memsz ||
                tls_vaddr - load->p_vaddr > load->p_memsz - tls_memsz)
                continue;
            uint64_t delta = tls_vaddr - load->p_vaddr;
            if (tls_filesz &&
                (delta > load->p_filesz ||
                 tls_filesz > load->p_filesz - delta ||
                 load->p_offset > UINT64_MAX - delta ||
                 load->p_offset + delta != tls_offset))
                continue;
            covered = 1;
            break;
        }
        if (!covered) return NULL;
    }

    uint64_t image_size = vaddr_max - vaddr_min;
    if (image_size > UINT64_MAX - (PAGE_SIZE - 1)) return NULL;
    uint64_t total_size = (image_size + PAGE_SIZE - 1) &
                          ~(uint64_t)(PAGE_SIZE - 1);
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
    uint64_t real_base = 0;
    for (;;) {
        uint64_t observed = __atomic_load_n(&g_next_ldso_base,
                                             __ATOMIC_RELAXED);
        if (observed >= 0x0000800000000000ULL - PAGE_SIZE ||
            total_size > 0x0000800000000000ULL - PAGE_SIZE - observed)
            return NULL;
        uint64_t next = observed + total_size + PAGE_SIZE;
        uint64_t expected = observed;
        if (__atomic_compare_exchange_n(&g_next_ldso_base, &expected, next,
                                        false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
            real_base = observed;
            break;
        }
    }

    for (uint64_t p = 0; p < num_pages; p++) {
        uint64_t va = real_base + p * PAGE_SIZE;
        if (!vmm_alloc_page_at(va, VMM_P | VMM_W | VMM_U | VMM_NX)) {
            goto fail;
        }
    }
    uint64_t base = real_base;
    uint64_t vaddr_base = real_base - vaddr_min; /* 换算用的加法偏移量 */

    /* 第二步：复制段数据（目的地址要换算，不能用文件自带的 p_vaddr） */
    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = ldso_source_phdr(source, (uint16_t)i);
        if (!ph || ph->p_type != PT_LOAD) continue;
        uint64_t dst_va = vaddr_base + ph->p_vaddr;

        if (ph->p_filesz > 0 &&
            ldso_source_read(source, ph->p_offset,
                             (void *)(uintptr_t)dst_va,
                             ph->p_filesz) != 0)
            goto fail;
        if (ph->p_memsz > ph->p_filesz)
            memset((void *)(uintptr_t)(dst_va + ph->p_filesz), 0,
                   (size_t)(ph->p_memsz - ph->p_filesz));
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
    uint16_t             *versym     = 0;
    elf64_verdef_t       *verdef     = 0;
    elf64_verneed_t      *verneed    = 0;
    uint64_t              verdef_count = 0;
    uint64_t              verneed_count = 0;
    uint64_t              strtab_size= 0;
    uint64_t              soname_offset = UINT64_MAX;
    uint64_t              init_address = 0;
    uint64_t              fini_address = 0;
    uint64_t              init_array_address = 0;
    uint64_t              fini_array_address = 0;
    uint64_t              init_array_size = 0;
    uint64_t              fini_array_size = 0;
    uint64_t              needed_offsets[LDSO_MAX_NEEDED];
    uint32_t              needed_count = 0;

    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = ldso_source_phdr(source, (uint16_t)i);
        if (!ph || ph->p_type != PT_DYNAMIC) continue;
        if (ph->p_vaddr < vaddr_min ||
            ph->p_vaddr > vaddr_max ||
            ph->p_memsz > vaddr_max - ph->p_vaddr)
            goto fail;

        const elf64_dyn_t *dyn = (const elf64_dyn_t *)(uintptr_t)(
            vaddr_base + ph->p_vaddr);
        uint64_t dyn_count = ph->p_memsz / sizeof(elf64_dyn_t);

        for (uint64_t j = 0; j < dyn_count; j++) {
            switch (dyn[j].d_tag) {
            case DT_STRTAB:   strtab  = (const char *)(vaddr_base + dyn[j].d_un.d_ptr); break;
            case DT_STRSZ:    strtab_size = dyn[j].d_un.d_val; break;
            case DT_SYMTAB:   symtab  = (elf64_sym_t *)(vaddr_base + dyn[j].d_un.d_ptr); break;
            case DT_RELA:     rela    = (elf64_rela_t *)(vaddr_base + dyn[j].d_un.d_ptr); break;
            case DT_RELASZ:   rela_size = dyn[j].d_un.d_val; break;
            case DT_RELAENT:  rela_ent  = dyn[j].d_un.d_val; break;
            case DT_JMPREL:   jmprel  = (elf64_rela_t *)(vaddr_base + dyn[j].d_un.d_ptr); break;
            case DT_PLTRELSZ: jmprel_size = dyn[j].d_un.d_val; break;
            case DT_PLTGOT:   pltgot  = (void *)(vaddr_base + dyn[j].d_un.d_ptr); break;
            case DT_GNU_HASH: gnu_hash = (uint32_t *)(vaddr_base + dyn[j].d_un.d_ptr); break;
            case DT_VERSYM:   versym = (uint16_t *)(vaddr_base + dyn[j].d_un.d_ptr); break;
            case DT_VERDEF:   verdef = (elf64_verdef_t *)(vaddr_base + dyn[j].d_un.d_ptr); break;
            case DT_VERDEFNUM: verdef_count = dyn[j].d_un.d_val; break;
            case DT_VERNEED:  verneed = (elf64_verneed_t *)(vaddr_base + dyn[j].d_un.d_ptr); break;
            case DT_VERNEEDNUM: verneed_count = dyn[j].d_un.d_val; break;
            case DT_SONAME:   soname_offset = dyn[j].d_un.d_val; break;
            case DT_INIT:     init_address = dyn[j].d_un.d_ptr; break;
            case DT_FINI:     fini_address = dyn[j].d_un.d_ptr; break;
            case DT_INIT_ARRAY: init_array_address = dyn[j].d_un.d_ptr; break;
            case DT_FINI_ARRAY: fini_array_address = dyn[j].d_un.d_ptr; break;
            case DT_INIT_ARRAYSZ: init_array_size = dyn[j].d_un.d_val; break;
            case DT_FINI_ARRAYSZ: fini_array_size = dyn[j].d_un.d_val; break;
            case DT_NEEDED:
                if (needed_count >= LDSO_MAX_NEEDED) goto fail;
                needed_offsets[needed_count++] = dyn[j].d_un.d_val;
                break;
            default: break;
            }
        }
        break;
    }

    /* 第四步：分配并填充 loaded_lib_t */
    lib = (loaded_lib_t *)kmalloc(sizeof(loaded_lib_t));
    if (!lib) goto fail;
    memset(lib, 0, sizeof(*lib));
    lib->owner_mm   = owner->mm;
    lib->base       = (void *)base;
    lib->vaddr_base = vaddr_base;
    lib->strtab     = strtab;
    lib->strtab_size= strtab_size;
    lib->symtab     = symtab;
    lib->jmprel     = jmprel;
    lib->jmprel_size= jmprel_size;
    lib->pltgot     = pltgot;
    lib->gnu_hash   = gnu_hash;
    lib->versym     = versym;
    lib->verdef     = verdef;
    lib->verneed    = verneed;
    if (verdef_count > UINT32_MAX || verneed_count > UINT32_MAX) goto fail;
    lib->verdef_count = (uint32_t)verdef_count;
    lib->verneed_count = (uint32_t)verneed_count;
    lib->num_pages  = num_pages;
    lib->refs       = 1;
    lib->loading    = true;
    if ((init_array_size % sizeof(uint64_t)) != 0 ||
        (fini_array_size % sizeof(uint64_t)) != 0 ||
        init_array_size / sizeof(uint64_t) > UINT32_MAX ||
        fini_array_size / sizeof(uint64_t) > UINT32_MAX)
        goto fail;
    lib->init_function = init_address ? vaddr_base + init_address : 0;
    lib->fini_function = fini_address ? vaddr_base + fini_address : 0;
    lib->init_array = init_array_address ?
        (uint64_t *)(uintptr_t)(vaddr_base + init_array_address) : NULL;
    lib->fini_array = fini_array_address ?
        (uint64_t *)(uintptr_t)(vaddr_base + fini_array_address) : NULL;
    lib->init_array_count = (uint32_t)(init_array_size / sizeof(uint64_t));
    lib->fini_array_count = (uint32_t)(fini_array_size / sizeof(uint64_t));
    if (has_tls && tls_memsz) {
        lib->tls_module_id = ldso_reserve_tls_module_id();
        if (lib->tls_module_id == 0) goto fail;
        lib->tls_template = vaddr_base + tls_vaddr;
        lib->tls_filesz = tls_filesz;
        lib->tls_memsz = tls_memsz;
        lib->tls_align = tls_align;
        if (!ldso_object_contains(lib, lib->tls_template, tls_memsz))
            goto fail;
    }
    if ((lib->init_function &&
         !ldso_object_contains(lib, lib->init_function, 1)) ||
        (lib->fini_function &&
         !ldso_object_contains(lib, lib->fini_function, 1)) ||
        (init_array_size &&
         (!lib->init_array || !ldso_object_contains(
             lib, (uint64_t)(uintptr_t)lib->init_array,
             init_array_size))) ||
        (fini_array_size &&
         (!lib->fini_array || !ldso_object_contains(
             lib, (uint64_t)(uintptr_t)lib->fini_array,
             fini_array_size))))
        goto fail;
    if (path && path[0]) {
        size_t path_length = strnlen(path, LDSO_PATH_MAX);
        if (path_length >= LDSO_PATH_MAX) goto fail;
        memcpy(lib->path, path, path_length + 1);
    }
    if (soname_offset != UINT64_MAX) {
        if (!strtab || soname_offset >= strtab_size ||
            strnlen(strtab + soname_offset,
                    (size_t)(strtab_size - soname_offset)) >=
                strtab_size - soname_offset)
            goto fail;
        lib->soname = strtab + soname_offset;
    }

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
    if ((strtab_size && (!strtab || !ldso_object_contains(
             lib, (uint64_t)(uintptr_t)strtab, strtab_size))) ||
        (lib->symtab_entries && (!symtab ||
             lib->symtab_entries > UINT64_MAX / sizeof(elf64_sym_t) ||
             !ldso_object_contains(lib, (uint64_t)(uintptr_t)symtab,
                                   lib->symtab_entries * sizeof(elf64_sym_t)))) ||
        ldso_validate_versions(lib) < 0 ||
        ldso_register_exported_ifuncs(lib) < 0)
        goto fail;

    /* Publish the object before loading dependencies so their relocations may
     * resolve symbols supplied by the parent. A dependency cycle is rejected
     * explicitly by ldso_load_vfs_named instead of leaking a reference loop. */
    ldso_metadata_lock();
    lib->next = g_loaded_libs;
    g_loaded_libs = lib;
    ldso_metadata_unlock();
    lib_linked = 1;

    for (uint32_t i = 0; i < needed_count; i++) {
        uint64_t offset = needed_offsets[i];
        if (!strtab || offset >= strtab_size) goto fail;
        size_t available = (size_t)(strtab_size - offset);
        const char *needed = strtab + offset;
        if (strnlen(needed, available) >= available) goto fail;
        char resolved[LDSO_PATH_MAX];
        vfs_node_t *dependency_node =
            ldso_find_needed_node(path, needed, resolved);
        if (!dependency_node) goto fail;
        loaded_lib_t *dependency =
            ldso_load_vfs_named(dependency_node, resolved);
        if (!dependency) goto fail;
        lib->dependencies[lib->dependency_count++] = dependency;
    }

    /* 第五步：执行重定位 */
    if (rela && rela_size > 0 && rela_ent > 0) {
        if (rela_ent != sizeof(elf64_rela_t) ||
            rela_size % rela_ent != 0)
            goto fail;
        uint64_t count = rela_size / rela_ent;
        if (do_rela_relative(lib, rela, count) < 0 ||
            do_rela_irelative(lib, rela, count) < 0 ||
            do_rela_tls(lib, rela, count) < 0 ||
            do_rela_symbols(lib, rela, count, R_X86_64_GLOB_DAT) < 0 ||
            do_rela_symbols(lib, rela, count, R_X86_64_64) < 0)
            goto fail;
    }

    if (jmprel && jmprel_size > 0) {
        if (jmprel_size % sizeof(elf64_rela_t) != 0) goto fail;
        uint64_t count = jmprel_size / sizeof(elf64_rela_t);
        if (do_rela_irelative(lib, jmprel, count) < 0 ||
            do_rela_symbols(lib, jmprel, count,
                            R_X86_64_JUMP_SLOT) < 0)
            goto fail;
    }

    /* Relocations are complete: remove temporary write permission and union
     * PT_LOAD flags for shared boundary pages.  Gaps remain read-only + NX. */
    for (uint64_t page = 0; page < num_pages; page++) {
        if (vmm_protect_page(real_base + page * PAGE_SIZE,
                             VMM_P | VMM_U | VMM_NX) < 0) {
            goto fail;
        }
    }
    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = ldso_source_phdr(source, (uint16_t)i);
        if (!ph || ph->p_type != PT_LOAD || !ph->p_memsz) continue;
        uint64_t start = (vaddr_base + ph->p_vaddr) &
                         ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t end = (vaddr_base + ph->p_vaddr + ph->p_memsz +
                        PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        for (uint64_t page = start; page < end; page += PAGE_SIZE) {
            uint64_t flags = vmm_get_page_flags(page);
            if (ph->p_flags & PF_W) flags |= VMM_W;
            if (ph->p_flags & PF_X) flags &= ~VMM_NX;
            if (vmm_protect_page(page, flags) < 0) {
                goto fail;
            }
        }
    }

    lib->loading = false;
    return lib;

fail:
    /* 简化清理：释放已分配页面（用真实映射地址 real_base，不是换算用的
     * vaddr_base 偏移量——这里失败时 vaddr_base 可能还没来得及赋值）。 */
    if (lib_linked) ldso_unlink(lib);
    if (lib) {
        for (uint32_t i = 0; i < lib->dependency_count; i++)
            ldso_release_reference(lib->dependencies[i]);
        ldso_free_ifuncs(lib);
        kfree(lib);
    }
    for (uint64_t p = 0; p < num_pages; p++) {
        uint64_t va = real_base + p * PAGE_SIZE;
        vmm_release_page(va);
    }
    return 0;
}

void *ldso_load(const uint8_t *data, size_t size) {
    ldso_source_t source;
    if (ldso_prepare_memory_source(data, size, &source) != 0) return NULL;
    void *handle = ldso_load_source(&source, NULL);
    ldso_release_source(&source);
    return handle;
}

static loaded_lib_t *ldso_load_vfs_named(vfs_node_t *node,
                                          const char *path) {
    const task_t *owner = task_current();
    if (!owner || !owner->mm || !node || !path || !path[0]) return NULL;
    loaded_lib_t *existing = ldso_find_path(owner->mm, path);
    if (existing) {
        if (existing->loading || existing->refs == UINT32_MAX) return NULL;
        existing->refs++;
        return existing;
    }
    ldso_source_t source;
    if (ldso_prepare_vfs_source(node, &source) != 0)
        return NULL;
    loaded_lib_t *handle = ldso_load_source(&source, path);
    ldso_release_source(&source);
    return handle;
}

void *ldso_load_vfs_path(struct vfs_node *node, const char *path) {
    return ldso_load_vfs_named((vfs_node_t *)node, path);
}

void *ldso_load_vfs(struct vfs_node *node) {
    vfs_node_t *vfs_node = (vfs_node_t *)node;
    return vfs_node ? ldso_load_vfs_named(vfs_node, vfs_node->name) : NULL;
}

/* 验证句柄确实指向当前已加载库链表中的一项，防止调用方传入野指针 */
static int handle_is_valid(void *handle) {
    const task_t *owner = task_current();
    const void *owner_mm = owner ? (const void *)owner->mm : NULL;
    int valid = 0;
    ldso_metadata_lock();
    for (loaded_lib_t *l = g_loaded_libs; l; l = l->next) {
        if ((void *)l == handle && l->owner_mm == owner_mm) {
            valid = 1;
            break;
        }
    }
    ldso_metadata_unlock();
    return valid;
}

typedef struct {
    loaded_lib_t *items[LDSO_GRAPH_MAX];
    uint32_t count;
} ldso_object_list_t;

static int ldso_list_contains(const ldso_object_list_t *list,
                              const loaded_lib_t *lib) {
    for (uint32_t i = 0; i < list->count; i++)
        if (list->items[i] == lib) return 1;
    return 0;
}

static int ldso_collect_init_order(loaded_lib_t *lib,
                                   ldso_object_list_t *order) {
    if (!lib || lib->initialized || ldso_list_contains(order, lib)) return 0;
    for (uint32_t i = 0; i < lib->dependency_count; i++)
        if (ldso_collect_init_order(lib->dependencies[i], order) < 0)
            return -1;
    if (order->count >= LDSO_GRAPH_MAX) return -1;
    order->items[order->count++] = lib;
    return 0;
}

static ldso_ifunc_record_t *ldso_ifunc_at(void *handle, uint64_t ordinal) {
    ldso_object_list_t order;
    memset(&order, 0, sizeof(order));
    if (ldso_collect_init_order((loaded_lib_t *)handle, &order) < 0)
        return NULL;
    uint64_t seen = 0;
    for (uint32_t i = 0; i < order.count; i++) {
        for (ldso_ifunc_record_t *record = order.items[i]->ifuncs;
             record; record = record->next) {
            if (seen++ == ordinal) return record;
        }
    }
    return NULL;
}

uintptr_t ldso_ifunc_next(void *handle, uint64_t ordinal) {
    if (!handle || !handle_is_valid(handle)) return UINT64_MAX;
    ldso_ifunc_record_t *record = ldso_ifunc_at(handle, ordinal);
    if (!record) return 0;
    return record->resolved ? 1 : (uintptr_t)record->resolver;
}

int ldso_ifunc_apply(void *handle, uint64_t ordinal, uintptr_t result) {
    if (!handle || !handle_is_valid(handle) || !result ||
        result >= 0x0000800000000000ULL || !vmm_get_phys(result))
        return -1;
    uint64_t flags = vmm_get_page_flags(result & ~(uint64_t)(PAGE_SIZE - 1));
    if (!(flags & VMM_U) || (flags & VMM_NX)) return -1;
    ldso_ifunc_record_t *record = ldso_ifunc_at(handle, ordinal);
    if (!record) return -1;
    if (record->resolved) return record->result == result ? 0 : -1;
    for (ldso_ifunc_target_t *target = record->targets; target;
         target = target->next) {
        uint64_t ignored = 0;
        if (!target->location ||
            !vmm_get_phys((uint64_t)(uintptr_t)target->location) ||
            ldso_ifunc_value(result, target->addend, &ignored) < 0)
            return -1;
    }
    ldso_ifunc_target_t *target = record->targets;
    while (target) {
        ldso_ifunc_target_t *next = target->next;
        uint64_t value = 0;
        (void)ldso_ifunc_value(result, target->addend, &value);
        *target->location = value;
        kfree(target);
        target = next;
    }
    record->targets = NULL;
    record->result = result;
    record->resolved = true;
    return 0;
}

static uint64_t ldso_object_init_entry(loaded_lib_t *lib, uint64_t ordinal,
                                       uint64_t *seen) {
    if (lib->init_function) {
        if ((*seen)++ == ordinal) return lib->init_function;
    }
    for (uint32_t i = 0; i < lib->init_array_count; i++) {
        uint64_t entry = lib->init_array[i];
        if (!entry || entry == UINT64_MAX) continue;
        if (!ldso_object_contains(lib, entry, 1)) return UINT64_MAX;
        if ((*seen)++ == ordinal) return entry;
    }
    return 0;
}

uintptr_t ldso_init_next(void *handle, uint64_t ordinal) {
    if (!handle || !handle_is_valid(handle)) return UINT64_MAX;
    loaded_lib_t *root = (loaded_lib_t *)handle;
    if (ordinal == 0 && root->initializing) return 0;
    ldso_object_list_t order;
    memset(&order, 0, sizeof(order));
    if (ldso_collect_init_order(root, &order) < 0)
        return UINT64_MAX;
    if (ordinal == 0)
        for (uint32_t i = 0; i < order.count; i++)
            order.items[i]->initializing = true;
    uint64_t seen = 0;
    for (uint32_t i = 0; i < order.count; i++) {
        uint64_t entry = ldso_object_init_entry(order.items[i], ordinal, &seen);
        if (entry == UINT64_MAX) return UINT64_MAX;
        if (entry) return (uintptr_t)entry;
    }
    for (uint32_t i = 0; i < order.count; i++) {
        order.items[i]->initialized = true;
        order.items[i]->initializing = false;
    }
    return 0;
}

static int ldso_collect_reachable(loaded_lib_t *lib,
                                  ldso_object_list_t *list) {
    if (!lib || ldso_list_contains(list, lib)) return 0;
    if (list->count >= LDSO_GRAPH_MAX) return -1;
    list->items[list->count++] = lib;
    for (uint32_t i = 0; i < lib->dependency_count; i++)
        if (ldso_collect_reachable(lib->dependencies[i], list) < 0) return -1;
    return 0;
}

static int ldso_will_unload(const ldso_object_list_t *reachable,
                            const bool unload[LDSO_GRAPH_MAX],
                            const loaded_lib_t *candidate) {
    uint32_t released_refs = 0;
    for (uint32_t i = 0; i < reachable->count; i++) {
        if (!unload[i]) continue;
        loaded_lib_t *parent = reachable->items[i];
        for (uint32_t j = 0; j < parent->dependency_count; j++)
            if (parent->dependencies[j] == candidate) released_refs++;
    }
    return released_refs >= candidate->refs;
}

static int ldso_collect_fini_order(loaded_lib_t *lib,
                                   const ldso_object_list_t *reachable,
                                   const bool unload[LDSO_GRAPH_MAX],
                                   ldso_object_list_t *order) {
    if (!lib || ldso_list_contains(order, lib)) return 0;
    uint32_t index = 0;
    while (index < reachable->count && reachable->items[index] != lib) index++;
    if (index == reachable->count || !unload[index]) return 0;
    if (order->count >= LDSO_GRAPH_MAX) return -1;
    order->items[order->count++] = lib;
    for (uint32_t i = lib->dependency_count; i > 0; i--)
        if (ldso_collect_fini_order(lib->dependencies[i - 1], reachable,
                                    unload, order) < 0)
            return -1;
    return 0;
}

static uint64_t ldso_object_fini_entry(loaded_lib_t *lib, uint64_t ordinal,
                                       uint64_t *seen) {
    for (uint32_t i = lib->fini_array_count; i > 0; i--) {
        uint64_t entry = lib->fini_array[i - 1];
        if (!entry || entry == UINT64_MAX) continue;
        if (!ldso_object_contains(lib, entry, 1)) return UINT64_MAX;
        if ((*seen)++ == ordinal) return entry;
    }
    if (lib->fini_function) {
        if ((*seen)++ == ordinal) return lib->fini_function;
    }
    return 0;
}

uintptr_t ldso_fini_next(void *handle, uint64_t ordinal) {
    if (!handle || !handle_is_valid(handle)) return UINT64_MAX;
    loaded_lib_t *root = (loaded_lib_t *)handle;
    if (root->refs != 1 || !root->initialized || root->finalized) return 0;
    ldso_object_list_t reachable;
    memset(&reachable, 0, sizeof(reachable));
    if (ldso_collect_reachable(root, &reachable) < 0) return UINT64_MAX;
    bool unload[LDSO_GRAPH_MAX];
    memset(unload, 0, sizeof(unload));
    unload[0] = true;
    for (uint32_t pass = 0; pass < reachable.count; pass++) {
        int changed = 0;
        for (uint32_t i = 1; i < reachable.count; i++) {
            if (!unload[i] &&
                ldso_will_unload(&reachable, unload, reachable.items[i])) {
                unload[i] = true;
                changed = 1;
            }
        }
        if (!changed) break;
    }
    ldso_object_list_t order;
    memset(&order, 0, sizeof(order));
    if (ldso_collect_fini_order(root, &reachable, unload, &order) < 0)
        return UINT64_MAX;
    uint64_t seen = 0;
    for (uint32_t i = 0; i < order.count; i++) {
        loaded_lib_t *lib = order.items[i];
        if (!lib->initialized || lib->finalized) continue;
        uint64_t entry = ldso_object_fini_entry(lib, ordinal, &seen);
        if (entry == UINT64_MAX) return UINT64_MAX;
        if (entry) return (uintptr_t)entry;
    }
    for (uint32_t i = 0; i < order.count; i++)
        order.items[i]->finalized = true;
    return 0;
}

void *ldso_dlsym_version(void *handle, const char *name,
                         const char *version) {
    if (!handle || !name || !handle_is_valid(handle)) return 0;
    if (version && !version[0]) return 0;
    loaded_lib_t *lib = (loaded_lib_t *)handle;
    if (!lib->strtab || !lib->symtab || lib->symtab_entries == 0) return 0;

    elf64_sym_t *sym = ldso_lookup_export(lib, name, version);
    if (!sym) return 0;
    if (ELF64_ST_TYPE(sym->st_info) == STT_GNU_IFUNC) {
        uint64_t index = (uint64_t)(sym - lib->symtab);
        ldso_ifunc_record_t *record = ldso_find_ifunc(
            lib, index, lib->vaddr_base + sym->st_value);
        return record && record->resolved ?
            (void *)(uintptr_t)record->result : NULL;
    }
    if (ELF64_ST_TYPE(sym->st_info) == STT_TLS) {
        if (!lib->tls_module_id) return 0;
        return (void *)ldso_tls_get_addr(lib->tls_module_id, sym->st_value);
    }
    return (void *)(uintptr_t)(lib->vaddr_base + sym->st_value);
}

void *ldso_dlsym(void *handle, const char *name) {
    return ldso_dlsym_version(handle, name, NULL);
}

int ldso_close(void *handle) {
    if (!handle || !handle_is_valid(handle)) return -1;
    ldso_release_reference((loaded_lib_t *)handle);
    return 0;
}

void *ldso_resolve(const char *name) {
    if (!name) return 0;
    const task_t *owner = task_current();
    const void *owner_mm = owner ? (const void *)owner->mm : NULL;

    /* 遍历所有已加载库，查找导出符号 */
    void *result = NULL;
    ldso_metadata_lock();
    loaded_lib_t *lib = g_loaded_libs;
    while (lib) {
        if (lib->owner_mm == owner_mm && lib->strtab && lib->symtab &&
            lib->symtab_entries > 0) {
            elf64_sym_t *sym = ldso_lookup_export(lib, name, NULL);
            if (sym && ELF64_ST_TYPE(sym->st_info) != STT_TLS) {
                result =
                    (void *)(uintptr_t)(lib->vaddr_base + sym->st_value);
                break;
            }
        }
        lib = lib->next;
    }
    ldso_metadata_unlock();
    if (result) return result;

    /* 最后一步：在发起调用的这个程序自己的 .symtab 里找（让共享库能反过来
     * 调用它自己静态链接进来的 printf/malloc 等——见 task.h 里
     * host_symtab 字段的注释和 src/elf.c 的 attach_host_symtab）。这里
     * st_value 就是最终绝对地址，不用像上面共享库那样再加 lib->base：
     * 宿主程序是静态可执行文件，其 .symtab 里记录的本来就是链接时定好的
     * 真实地址，不是文件相对偏移量。 */
    return ldso_resolve_host_symbol(name);
}

static loaded_lib_t *ldso_find_tls_module(const void *owner_mm,
                                           uint32_t module_id) {
    if (!owner_mm || !module_id) return NULL;
    loaded_lib_t *found = NULL;
    ldso_metadata_lock();
    for (loaded_lib_t *lib = g_loaded_libs; lib; lib = lib->next) {
        if (lib->owner_mm == owner_mm &&
            lib->tls_module_id == module_id) {
            found = lib;
            break;
        }
    }
    ldso_metadata_unlock();
    return found;
}

static int ldso_tls_reserve_address(uint64_t span, uint64_t alignment,
                                    uint64_t *result) {
    if (!span || !alignment || !result) return -1;
    uint64_t mask = alignment - 1;
    for (;;) {
        uint64_t observed = __atomic_load_n(&g_next_tls_instance_base,
                                             __ATOMIC_RELAXED);
        if (observed > UINT64_MAX - mask) return -1;
        uint64_t address = (observed + mask) & ~mask;
        if (address > 0x0000800000000000ULL - PAGE_SIZE ||
            span > 0x0000800000000000ULL - PAGE_SIZE - address)
            return -1;
        uint64_t next = address + span + PAGE_SIZE;
        uint64_t expected = observed;
        if (__atomic_compare_exchange_n(&g_next_tls_instance_base, &expected,
                                        next, false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
            *result = address;
            return 0;
        }
    }
}

uintptr_t ldso_tls_get_addr(uint32_t module_id, uint64_t offset) {
    task_t *task = task_current();
    if (!task || !task->mm) return 0;
    loaded_lib_t *lib = ldso_find_tls_module(task->mm, module_id);
    if (!lib || !lib->tls_memsz || offset >= lib->tls_memsz) return 0;

    task_dynamic_tls_t *empty = NULL;
    for (uint32_t i = 0; i < TASK_DYNAMIC_TLS_MAX; i++) {
        task_dynamic_tls_t *slot = &task->dynamic_tls[i];
        if (slot->module_id == module_id) {
            if (!slot->address || offset > UINT64_MAX - slot->address)
                return 0;
            return (uintptr_t)(slot->address + offset);
        }
        if (!slot->module_id && !empty) empty = slot;
    }
    if (!empty) return 0;

    uint64_t alignment = lib->tls_align;
    if (alignment < PAGE_SIZE) alignment = PAGE_SIZE;
    if (lib->tls_memsz > UINT64_MAX - (PAGE_SIZE - 1)) return 0;
    uint64_t span = (lib->tls_memsz + PAGE_SIZE - 1) &
                    ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t address = 0;
    if (ldso_tls_reserve_address(span, alignment, &address) < 0) return 0;

    uint32_t pages = (uint32_t)(span / PAGE_SIZE);
    uint32_t allocated = 0;
    for (; allocated < pages; allocated++) {
        if (!vmm_alloc_page_at(address + (uint64_t)allocated * PAGE_SIZE,
                               VMM_P | VMM_W | VMM_U | VMM_NX))
            break;
    }
    if (allocated != pages) {
        while (allocated > 0) {
            allocated--;
            vmm_release_page(address + (uint64_t)allocated * PAGE_SIZE);
        }
        return 0;
    }
    memset((void *)(uintptr_t)address, 0, (size_t)span);
    if (lib->tls_filesz)
        memcpy((void *)(uintptr_t)address,
               (const void *)(uintptr_t)lib->tls_template,
               (size_t)lib->tls_filesz);
    empty->address = address;
    empty->page_count = pages;
    __atomic_store_n(&empty->module_id, module_id, __ATOMIC_RELEASE);
    return (uintptr_t)(address + offset);
}

void ldso_release_thread_tls(struct task *opaque) {
    task_t *task = (task_t *)opaque;
    task_t *current = task_current();
    if (!task || !current || !task->mm || task->mm != current->mm) return;
    for (uint32_t i = 0; i < TASK_DYNAMIC_TLS_MAX; i++) {
        task_dynamic_tls_t *slot = &task->dynamic_tls[i];
        if (!slot->module_id) continue;
        for (uint32_t page = 0; page < slot->page_count; page++)
            vmm_release_page(slot->address + (uint64_t)page * PAGE_SIZE);
        memset(slot, 0, sizeof(*slot));
    }
}

void ldso_release_address_space(const void *owner_mm) {
    if (!owner_mm) return;
    loaded_lib_t *victims = NULL;
    ldso_metadata_lock();
    loaded_lib_t **link = &g_loaded_libs;
    while (*link) {
        loaded_lib_t *lib = *link;
        if (lib->owner_mm != owner_mm) {
            link = &lib->next;
            continue;
        }
        *link = lib->next;
        lib->next = victims;
        victims = lib;
    }
    ldso_metadata_unlock();
    while (victims) {
        loaded_lib_t *lib = victims;
        victims = lib->next;
        /* The owning address space is destroyed immediately after this
         * metadata pass.  Its VMM_OWNED PTEs reclaim the frames even when
         * the task being killed is not the currently active address space. */
        ldso_free_ifuncs(lib);
        kfree(lib);
    }
}
