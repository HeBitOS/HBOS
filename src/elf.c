/**
 * @file    elf.c
 * @brief   ELF64 可执行文件加载器
 *
 * 加载流程:
 *   1. 验证 ELF magic + 64-bit + LE + x86_64 + ET_EXEC
 *   2. 遍历 PT_LOAD 程序头，分配虚拟内存页并拷贝段数据
 *   3. 在用户栈上设置 argc/argv/envp/auxv
 *   4. 创建新任务以 ELF entry point 为入口，旧任务 exit
 */

#include "elf.h"
#include "core/task.h"
#include "core/vmm.h"
#include "core/heap.h"
#include "string.h"
#include "user/ldso.h"

#define ELF_MAG0  0x7f
#define ELF_MAG1  'E'
#define ELF_MAG2  'L'
#define ELF_MAG3  'F'

#define USER_STACK_TOP   0x0000700000000000ULL
#define USER_STACK_SIZE  0x0000000000100000ULL
#define USER_STACK_BASE  (USER_STACK_TOP - USER_STACK_SIZE)

#ifndef PAGE_SIZE
#define PAGE_SIZE 0x1000
#endif
#define PAGE_MASK (~(PAGE_SIZE - 1))

#define ELF_ARG_MAX 32
#define ELF_ENV_MAX 32
#define ELF_ARG_BYTES_MAX 65536

static const char *elf_error = "ok";

const char *elf64_last_error(void) {
    return elf_error;
}

static int elf_fail(const char *msg) {
    elf_error = msg;
    return -1;
}

static int count_strs_limited(char *const ss[], int max) {
    int n = 0;
    if (ss) {
        while (ss[n]) {
            if (n >= max) return -1;
            n++;
        }
    }
    return n;
}

typedef struct {
    char *argv[ELF_ARG_MAX + 1];
    char *envp[ELF_ENV_MAX + 1];
    size_t used;
    char bytes[ELF_ARG_BYTES_MAX];
} elf_arg_snapshot_t;

static int elf_readable_address(uintptr_t address, size_t length) {
    if (!address || !length || address > UINTPTR_MAX - (length - 1))
        return 0;
    uintptr_t end = address + length - 1;
    uint64_t start_high = (uint64_t)address >> 48;
    uint64_t end_high = (uint64_t)end >> 48;
    if ((start_high != 0 && start_high != 0xffff) ||
        (end_high != 0 && end_high != 0xffff))
        return 0;
    for (uintptr_t page = address & ~(uintptr_t)(PAGE_SIZE - 1);;
         page += PAGE_SIZE) {
        if (!vmm_get_phys(page)) return 0;
        if (page >= (end & ~(uintptr_t)(PAGE_SIZE - 1))) break;
        if (page > UINTPTR_MAX - PAGE_SIZE) return 0;
    }
    return 1;
}

static int snapshot_string_vector(char *const input[], char **output,
                                  int maximum,
                                  elf_arg_snapshot_t *snapshot) {
    if (!input) {
        output[0] = NULL;
        return 0;
    }
    for (int index = 0; index <= maximum; index++) {
        if (!elf_readable_address((uintptr_t)&input[index],
                                  sizeof(input[index])))
            return -1;
        char *source = input[index];
        if (!source) {
            output[index] = NULL;
            return index;
        }
        if (index == maximum) return -1;
        output[index] = &snapshot->bytes[snapshot->used];
        for (;;) {
            uintptr_t address = (uintptr_t)source;
            if (!elf_readable_address(address, 1) ||
                snapshot->used >= sizeof(snapshot->bytes))
                return -1;
            char value = *source++;
            snapshot->bytes[snapshot->used++] = value;
            if (value == '\0') break;
        }
    }
    return -1;
}

static int snapshot_exec_arguments(char *const argv[], char *const envp[],
                                   elf_arg_snapshot_t *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    if (snapshot_string_vector(argv, snapshot->argv, ELF_ARG_MAX,
                               snapshot) < 0)
        return -1;
    if (snapshot_string_vector(envp, snapshot->envp, ELF_ENV_MAX,
                               snapshot) < 0)
        return -1;
    return 0;
}

static int validate_elf64_headers(const uint8_t *data, size_t size,
                                  const elf64_ehdr_t **out_ehdr) {
    if (!data || size < sizeof(elf64_ehdr_t)) return elf_fail("file too small");

    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)data;

    if (ehdr->e_ident[EI_MAG0] != ELF_MAG0 ||
        ehdr->e_ident[EI_MAG1] != ELF_MAG1 ||
        ehdr->e_ident[EI_MAG2] != ELF_MAG2 ||
        ehdr->e_ident[EI_MAG3] != ELF_MAG3)
        return elf_fail("bad magic");

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) return elf_fail("not ELF64");
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)  return elf_fail("not little endian");
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        return elf_fail("not executable ELF");
    if (ehdr->e_machine != EM_X86_64) return elf_fail("not x86_64");
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return elf_fail("missing program headers");
    if (ehdr->e_phentsize < sizeof(elf64_phdr_t)) return elf_fail("bad phdr size");
    if (ehdr->e_phnum &&
        ehdr->e_phentsize > UINT64_MAX / (uint64_t)ehdr->e_phnum)
        return elf_fail("phdr table overflow");
    uint64_t ph_size = (uint64_t)ehdr->e_phentsize * (uint64_t)ehdr->e_phnum;
    if (ehdr->e_phoff > (uint64_t)size ||
        ph_size > (uint64_t)size - ehdr->e_phoff)
        return elf_fail("phdr table out of file");

    if (out_ehdr) *out_ehdr = ehdr;
    return 0;
}

static void push_user_u64(uintptr_t *sp, uint64_t value) {
    *sp -= sizeof(uint64_t);
    *(volatile uint64_t *)(*sp) = value;
}

/**
 * 找到并复制这个可执行文件自己的 .symtab/.strtab（如果有的话——TCC 默认
 * 不生成，除非显式要求，见 third_party/tinycc/tcc.c 的 do_debug 补丁），
 * 挂到刚创建的任务上。这样之后 dlopen() 一个共享库时，库里对 printf/
 * malloc 等外部符号的引用才能解析回这个程序自己静态链接进来的 libc，
 * 而不是只能在其他 dlopen() 过的共享库里找（见 src/user/ldso.c）。
 *
 * 只看节头，不影响加载/执行本身（只用到程序头）；找不到就什么也不做，
 * 不是错误——没有 .symtab 只是意味着这个程序打开的共享库没法反过来调用
 * 它自己的函数，而不是没法用参数一致的普通静态可执行文件本身。
 */
static void attach_host_symtab(const uint8_t *data, size_t size,
                                const elf64_ehdr_t *ehdr, uint32_t task_id) {
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) return;
    if (ehdr->e_shoff > (uint64_t)size ||
        (uint64_t)ehdr->e_shnum * ehdr->e_shentsize > (uint64_t)size - ehdr->e_shoff)
        return;

    const elf64_shdr_t *shdrs = (const elf64_shdr_t *)(data + ehdr->e_shoff);
    const elf64_shdr_t *symtab_sh = 0;
    const elf64_shdr_t *strtab_sh = 0;

    for (int i = 0; i < (int)ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            symtab_sh = &shdrs[i];
            if (shdrs[i].sh_link < ehdr->e_shnum)
                strtab_sh = &shdrs[shdrs[i].sh_link];
            break;
        }
    }
    if (!symtab_sh || !strtab_sh) return;
    if (symtab_sh->sh_offset > (uint64_t)size ||
        symtab_sh->sh_size > (uint64_t)size - symtab_sh->sh_offset)
        return;
    if (strtab_sh->sh_offset > (uint64_t)size ||
        strtab_sh->sh_size > (uint64_t)size - strtab_sh->sh_offset)
        return;

    void *symtab_copy = kmalloc(symtab_sh->sh_size);
    if (!symtab_copy) return;
    char *strtab_copy = (char *)kmalloc(strtab_sh->sh_size);
    if (!strtab_copy) { kfree(symtab_copy); return; }

    memcpy(symtab_copy, data + symtab_sh->sh_offset, symtab_sh->sh_size);
    memcpy(strtab_copy, data + strtab_sh->sh_offset, strtab_sh->sh_size);

    /* ELF64_Sym 固定 24 字节；sh_entsize 应该总是等于这个值，但防御性地
     * 处理一下万一是 0 的情况。 */
    uint64_t entsize = symtab_sh->sh_entsize ? symtab_sh->sh_entsize : 24;
    uint64_t count = symtab_sh->sh_size / entsize;

    task_set_host_symtab(task_id, symtab_copy, strtab_copy, count);
}

static int elf64_load_and_spawn_snapshot(const uint8_t *data, size_t size,
                                         char *const argv[],
                                         char *const envp[],
                                         const char *task_name) {
    const elf64_ehdr_t *ehdr = NULL;
    if (validate_elf64_headers(data, size, &ehdr) < 0) return -1;
    elf_error = "ok";

    uint64_t load_bias = 0;
    uint64_t image_min = UINT64_MAX;
    int has_interp = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)
            (data + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (ph->p_type == PT_INTERP) has_interp = 1;
        if (ph->p_type == PT_LOAD && ph->p_memsz) {
            uint64_t start = ph->p_vaddr & PAGE_MASK;
            if (start < image_min) image_min = start;
        }
    }
    if (has_interp)
        return elf_fail("PT_INTERP dynamic loader not available");
    if (image_min == UINT64_MAX) return elf_fail("missing load segment");
    if (ehdr->e_type == ET_DYN) {
        const uint64_t pie_base = 0x0000001100000000ULL;
        if (image_min > pie_base) return elf_fail("PIE address overflow");
        load_bias = pie_base - image_min;
    } else if (image_min < 0x100000000ULL) {
        /*
         * The current kernel still owns the low 4 GiB identity map.  A
         * conventional non-PIE Linux ET_EXEC at 0x400000 cannot safely be
         * overlaid until HBOS finishes its high-half kernel migration.
         */
        return elf_fail("low ET_EXEC conflicts with kernel; use PIE");
    }

    uint64_t old_pml4 = vmm_get_pml4();
    uint64_t app_pml4 = vmm_create_address_space();
    if (!app_pml4) return elf_fail("address space create failed");
    /* pml4 临时切换到 app_pml4 期间必须禁止抢占：否则 PIT(100Hz) 触发 →
     * task_schedule 把 pml4 换成别的任务 → 后续 memcpy 写进错误地址空间，
     * 应用的段/栈从未写入 app_pml4，一运行即缺页崩溃。 */
    task_preempt_disable();
    vmm_set_pml4(app_pml4);

    uint16_t phnum     = ehdr->e_phnum;
    uint16_t phentsize = ehdr->e_phentsize;

    for (uint16_t i = 0; i < phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)
            (data + ehdr->e_phoff + i * phentsize);

        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_offset > (uint64_t)size ||
            ph->p_filesz > (uint64_t)size - ph->p_offset ||
            ph->p_memsz < ph->p_filesz ||
            ph->p_vaddr + ph->p_memsz < ph->p_vaddr) {
            vmm_set_pml4(old_pml4);
            task_preempt_enable();
            vmm_destroy_address_space(app_pml4);
            return elf_fail("segment out of file");
        }
        if (ph->p_memsz == 0) continue;

        uint64_t segment_vaddr = load_bias + ph->p_vaddr;
        if (segment_vaddr < ph->p_vaddr) {
            vmm_set_pml4(old_pml4);
            task_preempt_enable();
            vmm_destroy_address_space(app_pml4);
            return elf_fail("segment relocation overflow");
        }
        if (ph->p_memsz > UINT64_MAX - segment_vaddr) {
            vmm_set_pml4(old_pml4);
            task_preempt_enable();
            vmm_destroy_address_space(app_pml4);
            return elf_fail("segment address overflow");
        }
        uint64_t mem_end = segment_vaddr + ph->p_memsz;
        if (mem_end > UINT64_MAX - (PAGE_SIZE - 1)) {
            vmm_set_pml4(old_pml4);
            task_preempt_enable();
            vmm_destroy_address_space(app_pml4);
            return elf_fail("segment address overflow");
        }
        uint64_t va_start = segment_vaddr & PAGE_MASK;
        uint64_t va_end   = (mem_end + PAGE_SIZE - 1) & PAGE_MASK;

        for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
            if (!vmm_alloc_page_at(va, VMM_P | VMM_W | VMM_U)) {
                vmm_set_pml4(old_pml4);
                task_preempt_enable();
                vmm_destroy_address_space(app_pml4);
                return elf_fail("segment map failed");
            }
        }

        if (ph->p_filesz > 0)
            memcpy((void *)(uintptr_t)segment_vaddr,
                   data + ph->p_offset, ph->p_filesz);

        if (ph->p_memsz > ph->p_filesz)
            memset((void *)(uintptr_t)(segment_vaddr + ph->p_filesz),
                   0, ph->p_memsz - ph->p_filesz);
    }

    if (ehdr->e_type == ET_DYN) {
        elf64_rela_t *rela = NULL;
        uint64_t rela_size = 0;
        uint64_t rela_entry = sizeof(elf64_rela_t);
        for (uint16_t i = 0; i < phnum; i++) {
            const elf64_phdr_t *ph = (const elf64_phdr_t *)
                (data + ehdr->e_phoff + i * phentsize);
            if (ph->p_type != PT_DYNAMIC) continue;
            elf64_dyn_t *dynamic =
                (elf64_dyn_t *)(uintptr_t)(load_bias + ph->p_vaddr);
            uint64_t count = ph->p_memsz / sizeof(*dynamic);
            for (uint64_t j = 0; j < count && dynamic[j].d_tag != DT_NULL;
                 j++) {
                if (dynamic[j].d_tag == DT_RELA)
                    rela = (elf64_rela_t *)(uintptr_t)(
                        load_bias + dynamic[j].d_un.d_ptr);
                else if (dynamic[j].d_tag == DT_RELASZ)
                    rela_size = dynamic[j].d_un.d_val;
                else if (dynamic[j].d_tag == DT_RELAENT)
                    rela_entry = dynamic[j].d_un.d_val;
            }
        }
        if (rela && rela_size) {
            if (rela_entry != sizeof(elf64_rela_t)) {
                vmm_set_pml4(old_pml4);
                task_preempt_enable();
                vmm_destroy_address_space(app_pml4);
                return elf_fail("unsupported PIE relocation size");
            }
            uint64_t count = rela_size / rela_entry;
            for (uint64_t i = 0; i < count; i++) {
                uint32_t type = (uint32_t)ELF64_R_TYPE(rela[i].r_info);
                if (type == R_X86_64_NONE) continue;
                if (type != R_X86_64_RELATIVE) {
                    vmm_set_pml4(old_pml4);
                    task_preempt_enable();
                    vmm_destroy_address_space(app_pml4);
                    return elf_fail("unresolved PIE relocation");
                }
                uint64_t location = load_bias + rela[i].r_offset;
                *(uint64_t *)(uintptr_t)location =
                    load_bias + (uint64_t)rela[i].r_addend;
            }
        }
    }

    for (uint64_t va = USER_STACK_BASE; va < USER_STACK_TOP; va += PAGE_SIZE) {
        if (!vmm_alloc_page_at(va, VMM_P | VMM_W | VMM_U)) {
            vmm_set_pml4(old_pml4);
            task_preempt_enable();
            vmm_destroy_address_space(app_pml4);
            return elf_fail("stack map failed");
        }
    }

    int argc = count_strs_limited(argv, ELF_ARG_MAX);
    int envc = count_strs_limited(envp, ELF_ENV_MAX);
    if (argc < 0 || envc < 0) {
        vmm_set_pml4(old_pml4);
        task_preempt_enable();
        vmm_destroy_address_space(app_pml4);
        return elf_fail("too many args");
    }

    size_t total_strs = 0;
    for (int i = 0; i < argc; i++) total_strs += strlen(argv[i]) + 1;
    for (int i = 0; i < envc; i++) total_strs += strlen(envp[i]) + 1;
    const size_t aux_pairs = 16;
    size_t ptr_bytes =
        (size_t)(1 + envc + 1 + argc + 1) * sizeof(uint64_t) +
        aux_pairs * 2 * sizeof(uint64_t);
    if (total_strs + ptr_bytes + 64 > USER_STACK_SIZE) {
        vmm_set_pml4(old_pml4);
        task_preempt_enable();
        vmm_destroy_address_space(app_pml4);
        return elf_fail("args too large");
    }

    uint8_t *sp = (uint8_t *)(uintptr_t)USER_STACK_TOP;

    char *arg_strs[ELF_ARG_MAX];
    char *env_strs[ELF_ENV_MAX];

    for (int i = envc - 1; i >= 0; i--) {
        size_t l = strlen(envp[i]) + 1;
        sp -= l;
        env_strs[i] = (char *)sp;
        memcpy(sp, envp[i], l);
    }
    for (int i = argc - 1; i >= 0; i--) {
        size_t l = strlen(argv[i]) + 1;
        sp -= l;
        arg_strs[i] = (char *)sp;
        memcpy(sp, argv[i], l);
    }

    static const char platform[] = "x86_64";
    sp -= sizeof(platform);
    char *platform_string = (char *)sp;
    memcpy(sp, platform, sizeof(platform));

    sp -= 16;
    uint8_t *random_bytes = sp;
    uint32_t tsc_low, tsc_high;
    __asm__ volatile("rdtsc" : "=a"(tsc_low), "=d"(tsc_high));
    uint64_t random_a = ((uint64_t)tsc_high << 32) | tsc_low;
    uint64_t random_b = random_a ^ (uint64_t)(uintptr_t)data ^
                        ((uint64_t)size << 17);
    memcpy(random_bytes, &random_a, sizeof(random_a));
    memcpy(random_bytes + sizeof(random_a), &random_b, sizeof(random_b));

    sp = (uint8_t *)((uintptr_t)sp & ~15ULL);

    uintptr_t stack_addr = (uintptr_t)sp;
    if (((35 + argc + envc) & 1) != 0)
        push_user_u64(&stack_addr, 0);

    uint64_t phdr_address = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)
            (data + ehdr->e_phoff + i * phentsize);
        if (ph->p_type == PT_PHDR) {
            phdr_address = load_bias + ph->p_vaddr;
            break;
        }
        if (ph->p_type == PT_LOAD &&
            ehdr->e_phoff >= ph->p_offset &&
            ehdr->e_phoff < ph->p_offset + ph->p_filesz)
            phdr_address = load_bias + ph->p_vaddr +
                           (ehdr->e_phoff - ph->p_offset);
    }

    enum {
        AT_NULL = 0,
        AT_PHDR = 3,
        AT_PHENT = 4,
        AT_PHNUM = 5,
        AT_PAGESZ = 6,
        AT_BASE = 7,
        AT_ENTRY = 9,
        AT_UID = 11,
        AT_EUID = 12,
        AT_GID = 13,
        AT_EGID = 14,
        AT_CLKTCK = 17,
        AT_SECURE = 23,
        AT_RANDOM = 25,
        AT_PLATFORM = 15,
        AT_EXECFN = 31
    };
#define PUSH_AUX(type, value) do { \
        push_user_u64(&stack_addr, (uint64_t)(value)); \
        push_user_u64(&stack_addr, (uint64_t)(type)); \
    } while (0)
    PUSH_AUX(AT_NULL, 0);
    PUSH_AUX(AT_EXECFN, argc ? (uintptr_t)arg_strs[0] : 0);
    PUSH_AUX(AT_PLATFORM, (uintptr_t)platform_string);
    PUSH_AUX(AT_RANDOM, (uintptr_t)random_bytes);
    PUSH_AUX(AT_SECURE, 0);
    PUSH_AUX(AT_CLKTCK, PIT_DEFAULT_FREQUENCY_HZ);
    PUSH_AUX(AT_EGID, 0);
    PUSH_AUX(AT_GID, 0);
    PUSH_AUX(AT_EUID, 0);
    PUSH_AUX(AT_UID, 0);
    PUSH_AUX(AT_ENTRY, load_bias + ehdr->e_entry);
    PUSH_AUX(AT_BASE, 0);
    PUSH_AUX(AT_PAGESZ, PAGE_SIZE);
    PUSH_AUX(AT_PHNUM, ehdr->e_phnum);
    PUSH_AUX(AT_PHENT, ehdr->e_phentsize);
    PUSH_AUX(AT_PHDR, phdr_address);
#undef PUSH_AUX

    push_user_u64(&stack_addr, 0);
    for (int i = envc - 1; i >= 0; i--)
        push_user_u64(&stack_addr, (uint64_t)(uintptr_t)env_strs[i]);
    push_user_u64(&stack_addr, 0);
    for (int i = argc - 1; i >= 0; i--)
        push_user_u64(&stack_addr, (uint64_t)(uintptr_t)arg_strs[i]);
    push_user_u64(&stack_addr, (uint64_t)argc);

    uint64_t *stack = (uint64_t *)stack_addr;
    uint64_t user_rsp = (uint64_t)(uintptr_t)stack;
    uint64_t user_argv = (uint64_t)(uintptr_t)&stack[1];

    vmm_set_pml4(old_pml4);
    task_preempt_enable();

    int new_id = task_create_ring3_full(task_name ? task_name : "elf_app",
                                        load_bias + ehdr->e_entry, user_rsp,
                                        (uint64_t)argc, user_argv, app_pml4);
    if (new_id < 0) {
        vmm_destroy_address_space(app_pml4);
        return elf_fail("task create failed");
    }

    attach_host_symtab(data, size, ehdr, (uint32_t)new_id);

    return new_id;
}

int elf64_load_and_spawn(const uint8_t *data, size_t size,
                         char *const argv[], char *const envp[],
                         const char *task_name) {
    /*
     * execve() receives vectors in the old user address space.  The loader
     * switches CR3 while it maps the new image, so reading those pointers
     * afterwards is invalid.  Snapshot them first into a bounded kernel
     * allocation; the internal loader copies this snapshot onto the new user
     * stack before the allocation is released.
     */
    elf_arg_snapshot_t *snapshot =
        (elf_arg_snapshot_t *)kmalloc(sizeof(*snapshot));
    if (!snapshot) return elf_fail("argument snapshot allocation failed");
    if (snapshot_exec_arguments(argv, envp, snapshot) < 0) {
        kfree(snapshot);
        return elf_fail("invalid or oversized arguments");
    }
    int result = elf64_load_and_spawn_snapshot(
        data, size, snapshot->argv, snapshot->envp, task_name);
    kfree(snapshot);
    return result;
}

int elf64_load_and_exec(const uint8_t *data, size_t size,
                        char *const argv[], char *const envp[]) {
    int new_id = elf64_load_and_spawn(data, size, argv, envp, "elf_app");
    if (new_id < 0) return -1;
    task_exit();
    return 0;
}
