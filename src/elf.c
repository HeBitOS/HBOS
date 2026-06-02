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

static int count_strs(char *const ss[]) {
    int n = 0;
    if (ss) while (ss[n]) n++;
    return n;
}

int elf64_load_and_exec(const uint8_t *data, size_t size,
                        char *const argv[], char *const envp[]) {
    if (!data || size < sizeof(elf64_ehdr_t)) return -1;

    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)data;

    if (ehdr->e_ident[EI_MAG0] != ELF_MAG0 ||
        ehdr->e_ident[EI_MAG1] != ELF_MAG1 ||
        ehdr->e_ident[EI_MAG2] != ELF_MAG2 ||
        ehdr->e_ident[EI_MAG3] != ELF_MAG3)
        return -1;

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) return -1;
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)  return -1;
    if (ehdr->e_type != ET_EXEC) return -1;
    if (ehdr->e_machine != EM_X86_64) return -1;
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return -1;

    uint16_t phnum     = ehdr->e_phnum;
    uint16_t phentsize = ehdr->e_phentsize;

    for (uint16_t i = 0; i < phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)
            (data + ehdr->e_phoff + i * phentsize);

        if (ph->p_type != PT_LOAD) continue;

        uint64_t va_start = ph->p_vaddr & PAGE_MASK;
        uint64_t va_end   = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) & PAGE_MASK;

        for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE)
            vmm_alloc_page_at(va, 0x07);

        if (ph->p_filesz > 0)
            memcpy((void *)(uintptr_t)ph->p_vaddr,
                   data + ph->p_offset, ph->p_filesz);

        if (ph->p_memsz > ph->p_filesz)
            memset((void *)(uintptr_t)(ph->p_vaddr + ph->p_filesz),
                   0, ph->p_memsz - ph->p_filesz);
    }

    for (uint64_t va = USER_STACK_BASE; va < USER_STACK_TOP; va += PAGE_SIZE)
        vmm_alloc_page_at(va, 0x07);

    int argc = count_strs(argv);
    int envc = count_strs(envp);

    uint8_t *sp = (uint8_t *)(uintptr_t)USER_STACK_TOP;

    size_t total_strs = 0;
    for (int i = 0; i < argc; i++) total_strs += strlen(argv[i]) + 1;
    for (int i = 0; i < envc; i++) total_strs += strlen(envp[i]) + 1;
    total_strs += 16;

    sp -= total_strs;

    char *arg_strs[32];
    char *env_strs[32];
    uint8_t *p = sp;

    for (int i = 0; i < argc; i++) {
        size_t l = strlen(argv[i]) + 1;
        arg_strs[i] = (char *)p;
        memcpy(p, argv[i], l);
        p += l;
    }
    for (int i = 0; i < envc; i++) {
        size_t l = strlen(envp[i]) + 1;
        env_strs[i] = (char *)p;
        memcpy(p, envp[i], l);
        p += l;
    }

    sp = (uint8_t *)(((uintptr_t)p + 15) & ~15ULL);

    int total_ptrs = 1 + envc + 1 + argc + 1;
    uint64_t *stack = ((uint64_t *)(uintptr_t)sp) - total_ptrs;

    stack[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        stack[1 + i] = (uint64_t)(uintptr_t)arg_strs[i];
    stack[1 + argc] = 0;
    for (int i = 0; i < envc; i++)
        stack[2 + argc + i] = (uint64_t)(uintptr_t)env_strs[i];
    stack[2 + argc + envc] = 0;

    uint64_t user_rsp = (uint64_t)(uintptr_t)stack;

    int new_id = task_create_ring3("elf_app", ehdr->e_entry, user_rsp);
    if (new_id < 0) return -1;

    task_exit();
    return 0;
}