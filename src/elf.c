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
#include "string.h"

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
    if (ehdr->e_type != ET_EXEC) return elf_fail("not ET_EXEC");
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

int elf64_load_and_spawn(const uint8_t *data, size_t size,
                         char *const argv[], char *const envp[],
                         const char *task_name) {
    const elf64_ehdr_t *ehdr = NULL;
    if (validate_elf64_headers(data, size, &ehdr) < 0) return -1;
    elf_error = "ok";

    uint64_t old_pml4 = vmm_get_pml4();
    uint64_t app_pml4 = vmm_create_address_space();
    if (!app_pml4) return elf_fail("address space create failed");
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
            vmm_destroy_address_space(app_pml4);
            return elf_fail("segment out of file");
        }
        if (ph->p_memsz == 0) continue;

        uint64_t mem_end = ph->p_vaddr + ph->p_memsz;
        if (mem_end > UINT64_MAX - (PAGE_SIZE - 1)) {
            vmm_set_pml4(old_pml4);
            vmm_destroy_address_space(app_pml4);
            return elf_fail("segment address overflow");
        }
        uint64_t va_start = ph->p_vaddr & PAGE_MASK;
        uint64_t va_end   = (mem_end + PAGE_SIZE - 1) & PAGE_MASK;

        for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
            if (!vmm_alloc_page_at(va, VMM_P | VMM_W | VMM_U)) {
                vmm_set_pml4(old_pml4);
                vmm_destroy_address_space(app_pml4);
                return elf_fail("segment map failed");
            }
        }

        if (ph->p_filesz > 0)
            memcpy((void *)(uintptr_t)ph->p_vaddr,
                   data + ph->p_offset, ph->p_filesz);

        if (ph->p_memsz > ph->p_filesz)
            memset((void *)(uintptr_t)(ph->p_vaddr + ph->p_filesz),
                   0, ph->p_memsz - ph->p_filesz);
    }

    for (uint64_t va = USER_STACK_BASE; va < USER_STACK_TOP; va += PAGE_SIZE) {
        if (!vmm_alloc_page_at(va, VMM_P | VMM_W | VMM_U)) {
            vmm_set_pml4(old_pml4);
            vmm_destroy_address_space(app_pml4);
            return elf_fail("stack map failed");
        }
    }

    int argc = count_strs_limited(argv, ELF_ARG_MAX);
    int envc = count_strs_limited(envp, ELF_ENV_MAX);
    if (argc < 0 || envc < 0) {
        vmm_set_pml4(old_pml4);
        vmm_destroy_address_space(app_pml4);
        return elf_fail("too many args");
    }

    size_t total_strs = 0;
    for (int i = 0; i < argc; i++) total_strs += strlen(argv[i]) + 1;
    for (int i = 0; i < envc; i++) total_strs += strlen(envp[i]) + 1;
    size_t ptr_bytes = (size_t)(1 + envc + 1 + argc + 1) * sizeof(uint64_t);
    if (total_strs + ptr_bytes + 16 > USER_STACK_SIZE) {
        vmm_set_pml4(old_pml4);
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

    sp = (uint8_t *)((uintptr_t)sp & ~15ULL);

    uintptr_t stack_addr = (uintptr_t)sp;
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

    int new_id = task_create_ring3_full(task_name ? task_name : "elf_app",
                                        ehdr->e_entry, user_rsp,
                                        (uint64_t)argc, user_argv, app_pml4);
    if (new_id < 0) {
        vmm_destroy_address_space(app_pml4);
        return elf_fail("task create failed");
    }

    return new_id;
}

int elf64_load_and_exec(const uint8_t *data, size_t size,
                        char *const argv[], char *const envp[]) {
    int new_id = elf64_load_and_spawn(data, size, argv, envp, "elf_app");
    if (new_id < 0) return -1;
    task_exit();
    return 0;
}
