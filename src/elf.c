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
#include "vfs.h"
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
#define ELF_INTERP_PATH_MAX 256
#define ELF_INTERP_HEADERS_MAX (64U * 1024U)
#define ELF_MAIN_HEADERS_MAX (64U * 1024U)
#define ELF_INTERP_BASE 0x0000001200000000ULL

static const char *elf_error = "ok";

const char *elf64_last_error(void) {
    return elf_error;
}

/* execve 的**绝对路径**暂存区。
 *
 * VFS 节点只存 basename，loader 拿不到完整路径；但 /proc/<pid>/exe 必须是
 * 真路径 —— Chromium 用 dirname(readlink("/proc/self/exe")) 找 icudtl.dat
 * 和 *.pak。execve 的调用方（syscall 层）手里正好有那个路径字符串，于是
 * 在调用 loader 之前塞进来，loader 建完新任务后取走。
 *
 * 只在一次 execve 调用内有效，读完立刻清空，避免影响后续 run/spawn。 */
static char elf_exec_path[128];

void elf_set_exec_path(const char *path) {
    elf_exec_path[0] = '\0';
    if (!path || path[0] != '/') return;
    size_t i = 0;
    while (path[i] && i + 1 < sizeof(elf_exec_path)) {
        elf_exec_path[i] = path[i];
        i++;
    }
    elf_exec_path[i] = '\0';
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

typedef struct {
    const uint8_t *memory;
    vfs_node_t *node;
    uint64_t size;
    const uint8_t *headers;
    uint8_t *owned_headers;
    const elf64_ehdr_t *ehdr;
} elf_image_source_t;

static void release_image_source(elf_image_source_t *source) {
    if (!source) return;
    if (source->owned_headers) kfree(source->owned_headers);
    memset(source, 0, sizeof(*source));
}

static int image_source_read(const elf_image_source_t *source,
                             uint64_t offset, void *buffer,
                             uint64_t count) {
    if (!source || (!buffer && count) || offset > source->size ||
        count > source->size - offset)
        return elf_fail("ELF source read out of range");
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
        if (got != (int)chunk) return elf_fail("ELF source read failed");
        done += chunk;
    }
    return 0;
}

static const elf64_phdr_t *image_source_phdr(
    const elf_image_source_t *source, uint16_t index) {
    if (!source || !source->ehdr || index >= source->ehdr->e_phnum)
        return NULL;
    return (const elf64_phdr_t *)(source->headers +
        source->ehdr->e_phoff + index * source->ehdr->e_phentsize);
}

static uint64_t elf_segment_page_flags(uint32_t program_flags) {
    uint64_t flags = VMM_P | VMM_U;
    if (program_flags & PF_W) flags |= VMM_W;
    if (!(program_flags & PF_X)) flags |= VMM_NX;
    return flags;
}

static int protect_image_segments(const elf_image_source_t *source,
                                  uint64_t load_bias) {
    /* Start every load page read-only/non-executable, then union the PT_LOAD
     * permissions for legitimately shared boundary pages. */
    for (uint16_t i = 0; i < source->ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = image_source_phdr(source, i);
        if (!ph || ph->p_type != PT_LOAD || !ph->p_memsz) continue;
        uint64_t start = (load_bias + ph->p_vaddr) & PAGE_MASK;
        uint64_t end = (load_bias + ph->p_vaddr + ph->p_memsz +
                        PAGE_SIZE - 1) & PAGE_MASK;
        for (uint64_t page = start; page < end; page += PAGE_SIZE) {
            if (vmm_protect_page(page, VMM_P | VMM_U | VMM_NX) < 0)
                return elf_fail("segment protection failed");
        }
    }
    for (uint16_t i = 0; i < source->ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = image_source_phdr(source, i);
        if (!ph || ph->p_type != PT_LOAD || !ph->p_memsz) continue;
        uint64_t start = (load_bias + ph->p_vaddr) & PAGE_MASK;
        uint64_t end = (load_bias + ph->p_vaddr + ph->p_memsz +
                        PAGE_SIZE - 1) & PAGE_MASK;
        for (uint64_t page = start; page < end; page += PAGE_SIZE) {
            uint64_t flags = vmm_get_page_flags(page);
            if (ph->p_flags & PF_W) flags |= VMM_W;
            if (ph->p_flags & PF_X) flags &= ~VMM_NX;
            if (vmm_protect_page(page, flags) < 0)
                return elf_fail("segment protection failed");
        }
    }
    return 0;
}

static int prepare_memory_source(const uint8_t *data, size_t size,
                                 elf_image_source_t *source) {
    memset(source, 0, sizeof(*source));
    const elf64_ehdr_t *ehdr = NULL;
    if (validate_elf64_headers(data, size, &ehdr) < 0) return -1;
    source->memory = data;
    source->size = size;
    source->headers = data;
    source->ehdr = ehdr;
    return 0;
}

static int prepare_vfs_source(vfs_node_t *node,
                              elf_image_source_t *source) {
    memset(source, 0, sizeof(*source));
    if (!node || node->type != VFS_NODE_FILE ||
        node->size < sizeof(elf64_ehdr_t))
        return elf_fail("ELF VFS file too small");
    elf64_ehdr_t header;
    if (vfs_read(node, 0, &header, sizeof(header)) != (int)sizeof(header))
        return elf_fail("ELF VFS header read failed");
    if (header.e_phnum == 0 ||
        header.e_phentsize < sizeof(elf64_phdr_t) ||
        header.e_phentsize > UINT64_MAX / (uint64_t)header.e_phnum)
        return elf_fail("ELF VFS program headers invalid");
    uint64_t table_size =
        (uint64_t)header.e_phentsize * (uint64_t)header.e_phnum;
    if (header.e_phoff > UINT64_MAX - table_size)
        return elf_fail("ELF VFS program headers overflow");
    uint64_t headers_size = header.e_phoff + table_size;
    if (headers_size > node->size || headers_size > ELF_MAIN_HEADERS_MAX)
        return elf_fail("ELF VFS headers too large");
    source->owned_headers = (uint8_t *)kmalloc((size_t)headers_size);
    if (!source->owned_headers)
        return elf_fail("ELF VFS header allocation failed");
    source->node = node;
    source->size = node->size;
    source->headers = source->owned_headers;
    if (image_source_read(source, 0, source->owned_headers,
                          headers_size) < 0 ||
        validate_elf64_headers(source->headers, (size_t)headers_size,
                               &source->ehdr) < 0) {
        release_image_source(source);
        return elf_fail("ELF VFS header invalid");
    }
    return 0;
}

typedef struct {
    char path[ELF_INTERP_PATH_MAX];
    vfs_node_t *node;
    uint8_t *headers;
    size_t headers_size;
    const elf64_ehdr_t *ehdr;
    uint64_t load_bias;
    uint64_t entry;
} elf_interpreter_t;

static void release_interpreter(elf_interpreter_t *interpreter) {
    if (!interpreter) return;
    if (interpreter->headers) kfree(interpreter->headers);
    memset(interpreter, 0, sizeof(*interpreter));
}

/*
 * Resolve PT_INTERP before switching CR3.  Only the ELF/program headers are
 * retained in the kernel heap; PT_LOAD bytes are streamed from the VFS into
 * their final user mappings.  This keeps the hot exec path independent of
 * the interpreter's total file size.
 */
static int prepare_interpreter(const elf_image_source_t *main_source,
                               elf_interpreter_t *interpreter) {
    memset(interpreter, 0, sizeof(*interpreter));
    const elf64_ehdr_t *main_ehdr = main_source->ehdr;
    const elf64_phdr_t *interp_ph = NULL;
    for (uint16_t i = 0; i < main_ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = image_source_phdr(main_source, i);
        if (!ph) return elf_fail("invalid main program header");
        if (ph->p_type != PT_INTERP) continue;
        if (interp_ph) return elf_fail("multiple PT_INTERP segments");
        interp_ph = ph;
    }
    if (!interp_ph) return 0;
    if (interp_ph->p_filesz < 2 ||
        interp_ph->p_filesz > sizeof(interpreter->path) ||
        interp_ph->p_offset > main_source->size ||
        interp_ph->p_filesz > main_source->size - interp_ph->p_offset)
        return elf_fail("invalid PT_INTERP path");

    if (image_source_read(main_source, interp_ph->p_offset,
                          interpreter->path, interp_ph->p_filesz) < 0)
        return -1;
    if (interpreter->path[interp_ph->p_filesz - 1] != '\0' ||
        interpreter->path[0] != '/')
        return elf_fail("invalid PT_INTERP path");

    interpreter->node = vfs_lookup(interpreter->path);
    if (!interpreter->node || interpreter->node->type != VFS_NODE_FILE)
        return elf_fail("PT_INTERP file not found");
    if (interpreter->node->size < sizeof(elf64_ehdr_t))
        return elf_fail("PT_INTERP file too small");

    elf64_ehdr_t header;
    if (vfs_read(interpreter->node, 0, &header, sizeof(header)) !=
        (int)sizeof(header))
        return elf_fail("PT_INTERP header read failed");
    if (header.e_phnum == 0 ||
        header.e_phentsize < sizeof(elf64_phdr_t) ||
        header.e_phentsize > UINT64_MAX / (uint64_t)header.e_phnum)
        return elf_fail("PT_INTERP program headers invalid");
    uint64_t table_size =
        (uint64_t)header.e_phentsize * (uint64_t)header.e_phnum;
    if (header.e_phoff > UINT64_MAX - table_size)
        return elf_fail("PT_INTERP program headers overflow");
    uint64_t headers_size = header.e_phoff + table_size;
    if (headers_size > interpreter->node->size ||
        headers_size > ELF_INTERP_HEADERS_MAX)
        return elf_fail("PT_INTERP headers too large");

    interpreter->headers = (uint8_t *)kmalloc((size_t)headers_size);
    if (!interpreter->headers)
        return elf_fail("PT_INTERP header allocation failed");
    interpreter->headers_size = (size_t)headers_size;
    if (vfs_read(interpreter->node, 0, interpreter->headers,
                 (uint32_t)headers_size) != (int)headers_size ||
        validate_elf64_headers(interpreter->headers,
                               interpreter->headers_size,
                               &interpreter->ehdr) < 0) {
        release_interpreter(interpreter);
        return elf_fail("PT_INTERP ELF header invalid");
    }
    if (interpreter->ehdr->e_type != ET_DYN) {
        release_interpreter(interpreter);
        return elf_fail("PT_INTERP is not ET_DYN");
    }

    uint64_t image_min = UINT64_MAX;
    for (uint16_t i = 0; i < interpreter->ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(
            interpreter->headers + interpreter->ehdr->e_phoff +
            i * interpreter->ehdr->e_phentsize);
        if (ph->p_type == PT_INTERP) {
            release_interpreter(interpreter);
            return elf_fail("nested PT_INTERP is unsupported");
        }
        if (ph->p_type == PT_LOAD && ph->p_memsz) {
            uint64_t start = ph->p_vaddr & PAGE_MASK;
            if (start < image_min) image_min = start;
        }
    }
    if (image_min == UINT64_MAX || image_min > ELF_INTERP_BASE) {
        release_interpreter(interpreter);
        return elf_fail("PT_INTERP load address invalid");
    }
    interpreter->load_bias = ELF_INTERP_BASE - image_min;
    if (interpreter->ehdr->e_entry >
        UINT64_MAX - interpreter->load_bias) {
        release_interpreter(interpreter);
        return elf_fail("PT_INTERP entry overflow");
    }
    interpreter->entry = interpreter->load_bias + interpreter->ehdr->e_entry;
    return 1;
}

static int map_interpreter_segments(elf_interpreter_t *interpreter) {
    for (uint16_t i = 0; i < interpreter->ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(
            interpreter->headers + interpreter->ehdr->e_phoff +
            i * interpreter->ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_offset > interpreter->node->size ||
            ph->p_filesz > (uint64_t)interpreter->node->size - ph->p_offset ||
            ph->p_memsz < ph->p_filesz ||
            ph->p_vaddr > UINT64_MAX - ph->p_memsz ||
            ph->p_vaddr > UINT64_MAX - interpreter->load_bias)
            return elf_fail("PT_INTERP segment invalid");

        uint64_t segment_vaddr = interpreter->load_bias + ph->p_vaddr;
        if (segment_vaddr > UINT64_MAX - ph->p_memsz ||
            segment_vaddr + ph->p_memsz > UINT64_MAX - (PAGE_SIZE - 1))
            return elf_fail("PT_INTERP segment overflow");
        uint64_t va_start = segment_vaddr & PAGE_MASK;
        uint64_t va_end = (segment_vaddr + ph->p_memsz + PAGE_SIZE - 1) &
                          PAGE_MASK;
        for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
            /* Adjacent PT_LOAD segments may legitimately share one page. */
            if (!vmm_get_phys(va) &&
                !vmm_alloc_page_at(va, VMM_P | VMM_W | VMM_U | VMM_NX))
                return elf_fail("PT_INTERP segment map failed");
        }

        uint64_t copied = 0;
        while (copied < ph->p_filesz) {
            uint64_t remaining = ph->p_filesz - copied;
            uint32_t chunk = remaining > 64U * 1024U ?
                             64U * 1024U : (uint32_t)remaining;
            int got = vfs_read(interpreter->node,
                               (uint32_t)(ph->p_offset + copied),
                               (void *)(uintptr_t)(segment_vaddr + copied),
                               chunk);
            if (got != (int)chunk)
                return elf_fail("PT_INTERP segment read failed");
            copied += chunk;
        }
        if (ph->p_memsz > ph->p_filesz)
            memset((void *)(uintptr_t)(segment_vaddr + ph->p_filesz), 0,
                   (size_t)(ph->p_memsz - ph->p_filesz));
        /* 把 p_memsz 之后到页边界的那一截也清零。
         *
         * Linux 的 PT_LOAD 是按页 mmap 的，页内 p_memsz 之后的尾部属于
         * 匿名页、必然是 0。HBOS 用 vmm_alloc_page_at() 拿页，拿到的是
         * 上一个使用者留下的内容 —— 于是这块"ELF 没声明、但确实可读"的
         * 内存里是垃圾。
         *
         * 实测后果：ld.so 的 .bss 结束于 0x403C0，而它的启动代码会读
         * 0x40418（超出 p_memsz 88 字节）—— 那块内存页映射了但没清零，
         * 读出来是残留的代码字节，被当成指针用，下一条指令立刻
         * General Protection Fault（err=0，指令本身合法）。 */
        {
            uint64_t seg_end = segment_vaddr + ph->p_memsz;
            uint64_t tail = ((seg_end + PAGE_SIZE - 1) & PAGE_MASK) - seg_end;
            if (tail && tail < PAGE_SIZE)
                memset((void *)(uintptr_t)seg_end, 0, (size_t)tail);
        }
    }
    /* ld.so 自己是 PIE，必须做 R_X86_64_RELATIVE 重定位才能用。
     *
     * 这里以前只映射段、拷字节，一条重定位都不做 —— GOT 里留的还是文件中的
     * 原始值（相对偏移而非绝对地址）。于是 ld.so 的启动代码
     * `mov 0x58(%r15),%rdx` 从 GOT 读出垃圾值，下一条
     * `add %rax,0x8(%rdx)` 用非规范地址访问内存，立刻 General Protection
     * Fault（rip 落在 0x1200_xxxx_xxxx，err=0，指令本身完全合法）。
     *
     * 现象极具迷惑性：主程序能正常跑（它的 RELATIVE 重定位在下面那段做了），
     * 只有 exec 出来的子进程在 ld.so 里崩，而且每次都崩在同一个地址。
     *
     * ld.so 的 RELATIVE 重定位只需要 base+addend，不涉及符号解析，
     * 在内核里做是安全的。 */
    {
        const elf64_dyn_t *dyn = NULL;
        uint64_t rela_va = 0, rela_size = 0, rela_ent = 0;
        for (uint16_t i = 0; i < interpreter->ehdr->e_phnum; i++) {
            const elf64_phdr_t *ph = (const elf64_phdr_t *)(
                interpreter->headers + interpreter->ehdr->e_phoff +
                i * interpreter->ehdr->e_phentsize);
            if (ph->p_type != PT_DYNAMIC) continue;
            dyn = (const elf64_dyn_t *)(uintptr_t)
                  (interpreter->load_bias + ph->p_vaddr);
            break;
        }
        if (dyn) {
            for (uint64_t j = 0; dyn[j].d_tag != DT_NULL; j++) {
                if (dyn[j].d_tag == DT_RELA)
                    rela_va = dyn[j].d_un.d_ptr;
                else if (dyn[j].d_tag == DT_RELASZ)
                    rela_size = dyn[j].d_un.d_val;
                else if (dyn[j].d_tag == DT_RELAENT)
                    rela_ent = dyn[j].d_un.d_val;
            }
        }
        if (rela_va && rela_size && rela_ent == sizeof(elf64_rela_t)) {
            /* DT_RELA 在 PIE 里是"未重定位的虚拟地址"，要加 load_bias。 */
            const elf64_rela_t *rela = (const elf64_rela_t *)(uintptr_t)
                                       (interpreter->load_bias + rela_va);
            uint64_t count = rela_size / rela_ent;
            for (uint64_t i = 0; i < count; i++) {
                uint32_t type = (uint32_t)ELF64_R_TYPE(rela[i].r_info);
                if (type != R_X86_64_RELATIVE) continue;
                uint64_t *location = (uint64_t *)(uintptr_t)
                                     (interpreter->load_bias +
                                      rela[i].r_offset);
                *location = interpreter->load_bias +
                            (uint64_t)rela[i].r_addend;
            }
        }
    }
    for (uint16_t i = 0; i < interpreter->ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(
            interpreter->headers + interpreter->ehdr->e_phoff +
            i * interpreter->ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD || !ph->p_memsz) continue;
        uint64_t start = (interpreter->load_bias + ph->p_vaddr) & PAGE_MASK;
        uint64_t end = (interpreter->load_bias + ph->p_vaddr + ph->p_memsz +
                        PAGE_SIZE - 1) & PAGE_MASK;
        for (uint64_t page = start; page < end; page += PAGE_SIZE) {
            if (vmm_protect_page(page, VMM_P | VMM_U | VMM_NX) < 0)
                return elf_fail("PT_INTERP protection failed");
        }
    }
    for (uint16_t i = 0; i < interpreter->ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(
            interpreter->headers + interpreter->ehdr->e_phoff +
            i * interpreter->ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD || !ph->p_memsz) continue;
        uint64_t start = (interpreter->load_bias + ph->p_vaddr) & PAGE_MASK;
        uint64_t end = (interpreter->load_bias + ph->p_vaddr + ph->p_memsz +
                        PAGE_SIZE - 1) & PAGE_MASK;
        uint64_t segment_flags = elf_segment_page_flags(ph->p_flags);
        for (uint64_t page = start; page < end; page += PAGE_SIZE) {
            uint64_t flags = vmm_get_page_flags(page);
            if (segment_flags & VMM_W) flags |= VMM_W;
            if (!(segment_flags & VMM_NX)) flags &= ~VMM_NX;
            if (vmm_protect_page(page, flags) < 0)
                return elf_fail("PT_INTERP protection failed");
        }
    }
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

static int elf64_load_source_and_spawn_snapshot(
    const elf_image_source_t *source, char *const argv[],
    char *const envp[], const char *task_name) {
    if (!source || !source->ehdr) return elf_fail("missing ELF source");
    const elf64_ehdr_t *ehdr = source->ehdr;
    elf_error = "ok";

    uint64_t load_bias = 0;
    uint64_t image_min = UINT64_MAX;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = image_source_phdr(source, i);
        if (!ph) return elf_fail("invalid main program header");
        if (ph->p_type == PT_LOAD && ph->p_memsz) {
            uint64_t start = ph->p_vaddr & PAGE_MASK;
            if (start < image_min) image_min = start;
        }
    }
    elf_interpreter_t interpreter;
    int interpreter_status = prepare_interpreter(source, &interpreter);
    if (interpreter_status < 0) return -1;
    int has_interp = interpreter_status > 0;
    if (image_min == UINT64_MAX) {
        release_interpreter(&interpreter);
        return elf_fail("missing load segment");
    }
    if (ehdr->e_type == ET_DYN) {
        const uint64_t pie_base = 0x0000001100000000ULL;
        if (image_min > pie_base) {
            release_interpreter(&interpreter);
            return elf_fail("PIE address overflow");
        }
        load_bias = pie_base - image_min;
    } else if (image_min < 0x100000000ULL) {
        /*
         * The current kernel still owns the low 4 GiB identity map.  A
         * conventional non-PIE Linux ET_EXEC at 0x400000 cannot safely be
         * overlaid until HBOS finishes its high-half kernel migration.
         */
        release_interpreter(&interpreter);
        return elf_fail("low ET_EXEC conflicts with kernel; use PIE");
    }

    uint64_t old_pml4 = vmm_get_pml4();
    uint64_t app_pml4 = vmm_create_address_space();
    if (!app_pml4) {
        release_interpreter(&interpreter);
        return elf_fail("address space create failed");
    }
    /* pml4 临时切换到 app_pml4 期间必须禁止抢占：否则 PIT(100Hz) 触发 →
     * task_schedule 把 pml4 换成别的任务 → 后续 memcpy 写进错误地址空间，
     * 应用的段/栈从未写入 app_pml4，一运行即缺页崩溃。 */
    task_preempt_disable();
    vmm_set_pml4(app_pml4);

#define ELF_NEW_IMAGE_FAIL(message) do { \
        vmm_set_pml4(old_pml4); \
        task_preempt_enable(); \
        vmm_destroy_address_space(app_pml4); \
        release_interpreter(&interpreter); \
        return elf_fail(message); \
    } while (0)

    uint16_t phnum     = ehdr->e_phnum;
    for (uint16_t i = 0; i < phnum; i++) {
        const elf64_phdr_t *ph = image_source_phdr(source, i);
        if (!ph) ELF_NEW_IMAGE_FAIL("invalid main program header");

        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_offset > source->size ||
            ph->p_filesz > source->size - ph->p_offset ||
            ph->p_memsz < ph->p_filesz ||
            ph->p_vaddr + ph->p_memsz < ph->p_vaddr) {
            ELF_NEW_IMAGE_FAIL("segment out of file");
        }
        if (ph->p_memsz == 0) continue;

        uint64_t segment_vaddr = load_bias + ph->p_vaddr;
        if (segment_vaddr < ph->p_vaddr) {
            ELF_NEW_IMAGE_FAIL("segment relocation overflow");
        }
        if (ph->p_memsz > UINT64_MAX - segment_vaddr) {
            ELF_NEW_IMAGE_FAIL("segment address overflow");
        }
        uint64_t mem_end = segment_vaddr + ph->p_memsz;
        if (mem_end > UINT64_MAX - (PAGE_SIZE - 1)) {
            ELF_NEW_IMAGE_FAIL("segment address overflow");
        }
        uint64_t va_start = segment_vaddr & PAGE_MASK;
        uint64_t va_end   = (mem_end + PAGE_SIZE - 1) & PAGE_MASK;

        for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
            if (!vmm_get_phys(va) &&
                !vmm_alloc_page_at(
                    va, VMM_P | VMM_W | VMM_U | VMM_NX)) {
                ELF_NEW_IMAGE_FAIL("segment map failed");
            }
        }

        if (ph->p_filesz > 0 &&
            image_source_read(source, ph->p_offset,
                              (void *)(uintptr_t)segment_vaddr,
                              ph->p_filesz) < 0)
            ELF_NEW_IMAGE_FAIL(elf64_last_error());

        if (ph->p_memsz > ph->p_filesz)
            memset((void *)(uintptr_t)(segment_vaddr + ph->p_filesz),
                   0, ph->p_memsz - ph->p_filesz);
        /* p_memsz 之后到页边界的尾部也要清零 —— 见 map_interpreter_segments
         * 里同一处注释：Linux 的 PT_LOAD 按页映射，页内尾部必然是 0；
         * HBOS 复用了未清零的页，尾部会是上一个使用者的残留内容。 */
        {
            uint64_t seg_end = segment_vaddr + ph->p_memsz;
            uint64_t tail = ((seg_end + PAGE_SIZE - 1) & PAGE_MASK) - seg_end;
            if (tail && tail < PAGE_SIZE)
                memset((void *)(uintptr_t)seg_end, 0, (size_t)tail);
        }
    }

    if (ehdr->e_type == ET_DYN && !has_interp) {
        elf64_rela_t *rela = NULL;
        uint64_t rela_size = 0;
        uint64_t rela_entry = sizeof(elf64_rela_t);
        for (uint16_t i = 0; i < phnum; i++) {
            const elf64_phdr_t *ph = image_source_phdr(source, i);
            if (!ph) ELF_NEW_IMAGE_FAIL("invalid main program header");
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
                ELF_NEW_IMAGE_FAIL("unsupported PIE relocation size");
            }
            uint64_t count = rela_size / rela_entry;
            for (uint64_t i = 0; i < count; i++) {
                uint32_t type = (uint32_t)ELF64_R_TYPE(rela[i].r_info);
                if (type == R_X86_64_NONE) continue;
                if (type != R_X86_64_RELATIVE) {
                    ELF_NEW_IMAGE_FAIL("unresolved PIE relocation");
                }
                uint64_t location = load_bias + rela[i].r_offset;
                *(uint64_t *)(uintptr_t)location =
                    load_bias + (uint64_t)rela[i].r_addend;
            }
        }
    }

    if (has_interp && map_interpreter_segments(&interpreter) < 0)
        ELF_NEW_IMAGE_FAIL(elf64_last_error());

    if (protect_image_segments(source, load_bias) < 0)
        ELF_NEW_IMAGE_FAIL(elf64_last_error());

    for (uint64_t va = USER_STACK_BASE; va < USER_STACK_TOP; va += PAGE_SIZE) {
        if (!vmm_alloc_page_at(va, VMM_P | VMM_W | VMM_U | VMM_NX)) {
            ELF_NEW_IMAGE_FAIL("stack map failed");
        }
    }

    int argc = count_strs_limited(argv, ELF_ARG_MAX);
    int envc = count_strs_limited(envp, ELF_ENV_MAX);
    if (argc < 0 || envc < 0) {
        ELF_NEW_IMAGE_FAIL("too many args");
    }

    size_t total_strs = 0;
    for (int i = 0; i < argc; i++) total_strs += strlen(argv[i]) + 1;
    for (int i = 0; i < envc; i++) total_strs += strlen(envp[i]) + 1;
    const size_t aux_pairs = 16;
    size_t ptr_bytes =
        (size_t)(1 + envc + 1 + argc + 1) * sizeof(uint64_t) +
        aux_pairs * 2 * sizeof(uint64_t);
    if (total_strs + ptr_bytes + 64 > USER_STACK_SIZE) {
        ELF_NEW_IMAGE_FAIL("args too large");
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
    uint64_t source_identity = (uint64_t)(uintptr_t)(
        source->memory ? (const void *)source->memory :
                         (const void *)source->node);
    uint64_t random_b = random_a ^ source_identity ^ (source->size << 17);
    memcpy(random_bytes, &random_a, sizeof(random_a));
    memcpy(random_bytes + sizeof(random_a), &random_b, sizeof(random_b));

    sp = (uint8_t *)((uintptr_t)sp & ~15ULL);

    uintptr_t stack_addr = (uintptr_t)sp;
    /* SysV ABI: 进程入口 rsp % 16 == 0（指向 argc）。
     *
     * 静态程序没有 ld.so 接手，_start 直接就是 crt0，所以这里额外把
     * push 次数补成奇数，让 main 里 rsp % 16 == 8；否则 GCC 在 main 里
     * 生成的 movaps 会 #GP（quickjs/tinycc 踩过）。
     *
     * 动态程序**不能**这么补：实测补成奇数会让 glibc 的 ld.so 一进去就
     * #GP（rip 落在 0x1200022xxx 的 loader 代码里，exit 132）。也就是说
     * 入口必须保持 ABI 的 0 mod 16，那 8 字节的差由 ld.so 自己处理。 */
    int want_odd_pushes = !has_interp;
    if (((35 + argc + envc) & 1) != want_odd_pushes)
        push_user_u64(&stack_addr, 0);

    uint64_t phdr_address = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const elf64_phdr_t *ph = image_source_phdr(source, i);
        if (!ph) ELF_NEW_IMAGE_FAIL("invalid main program header");
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
        AT_HWCAP = 16,
        AT_HWCAP2 = 26,
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
    /* CPU 特性位：这两个值**必须**如实反映"内核能提供什么"，不能抄宿主。
     * 以前根本没推 AT_HWCAP/AT_HWCAP2，glibc 会当成"宿主什么都没报告"，
     * 于是它自己挑一套最保守的字符串/内存例程——能跑但慢。更关键的是：
     * AT_HWCAP2 里只要有 HWCAP2_RSEQ 位，glibc 就会积极去调 rseq(2)
     * 并在失败时回退到慢路径；我们从一开始就不置这位，语义是"本内核
     * 不支持 rseq"，进程内所有 dlopen/exec 都不会再走那条分支。
     *
     * 目前只声明 AVX2/SSE2 与 x86-64-v1 基线始终具备的项，其余保持 0：
     * 内核本身编译时禁用了 SSE（-mno-sse），用户态能不能用完全取决于
     * CPUID 直通，这里保守声明不会误伤（glibc 只在 AT_HWCAP 置位时才
     * 用，没置位就用等价的安全实现）。 */
    PUSH_AUX(AT_HWCAP, 0);
    PUSH_AUX(AT_HWCAP2, 0);
    PUSH_AUX(AT_EGID, 0);
    PUSH_AUX(AT_GID, 0);
    PUSH_AUX(AT_EUID, 0);
    PUSH_AUX(AT_UID, 0);
    PUSH_AUX(AT_ENTRY, load_bias + ehdr->e_entry);
    PUSH_AUX(AT_BASE, has_interp ? interpreter.load_bias : 0);
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
                                        has_interp ? interpreter.entry :
                                            load_bias + ehdr->e_entry,
                                        user_rsp,
                                        (uint64_t)argc, user_argv, app_pml4);
    release_interpreter(&interpreter);
    if (new_id < 0) {
        return elf_fail("task create failed");
    }

    if (source->memory)
        attach_host_symtab(source->memory, (size_t)source->size, ehdr,
                           (uint32_t)new_id);
    /* /proc/<pid>/exe 必须是真实路径：Chromium 用它定位 icudtl.dat 与
     * *.pak（dirname(readlink("/proc/self/exe"))）。
     *
     * 优先级：execve 传下来的绝对路径 > VFS 节点的 basename。
     * basename 单独用是**错的** —— 它不以 '/' 开头，readlink 会当成"未知"
     * 并退回 "/<任务名>"，Chromium 于是把 dirname 算成 "/"，去 /icudtl.dat
     * 找资源，必然 ENOENT，然后无限重试。 */
    if (elf_exec_path[0] == '/') {
        task_set_exe_path((uint32_t)new_id, elf_exec_path);
        elf_exec_path[0] = '\0';
    } else if (source->node && source->node->name[0]) {
        task_set_exe_path((uint32_t)new_id, source->node->name);
    }

    return new_id;
#undef ELF_NEW_IMAGE_FAIL
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
    elf_image_source_t source;
    if (prepare_memory_source(data, size, &source) < 0) {
        kfree(snapshot);
        return -1;
    }
    int result = elf64_load_source_and_spawn_snapshot(
        &source, snapshot->argv, snapshot->envp, task_name);
    release_image_source(&source);
    kfree(snapshot);
    return result;
}

int elf64_load_and_exec(const uint8_t *data, size_t size,
                        char *const argv[], char *const envp[]) {
    int new_id = elf64_load_and_spawn(data, size, argv, envp, "elf_app");
    if (new_id < 0) return -1;
    task_exec_inherit(new_id);
    task_exit();
    return 0;
}

int elf64_load_vfs_and_exec(struct vfs_node *node,
                            char *const argv[], char *const envp[]) {
    elf_arg_snapshot_t *snapshot =
        (elf_arg_snapshot_t *)kmalloc(sizeof(*snapshot));
    if (!snapshot) return elf_fail("argument snapshot allocation failed");
    if (snapshot_exec_arguments(argv, envp, snapshot) < 0) {
        kfree(snapshot);
        return elf_fail("invalid or oversized arguments");
    }

    elf_image_source_t source;
    if (prepare_vfs_source((vfs_node_t *)node, &source) < 0) {
        kfree(snapshot);
        return -1;
    }
    int new_id = elf64_load_source_and_spawn_snapshot(
        &source, snapshot->argv, snapshot->envp, "elf_app");
    release_image_source(&source);
    kfree(snapshot);
    if (new_id < 0) return -1;
    /* execve 在 Linux 里是**原地替换**：进程 id 不变、父进程不变、fd 表
     * 里非 FD_CLOEXEC 的文件继续有效、信号处理器重置为默认。
     *
     * HBOS 的实现是"新建一个任务 + 老任务退出"，上面那条 spawn 只做了一半：
     * 新任务拿到全新的空 fd 表、全新的 parent_id（指向马上要死的自己），
     * 于是 fork 出来的子进程 exec 之后**管道 fd 全丢**。Chromium 的 zygote
     * 正是靠 fork 时继承的 socketpair 和浏览器进程通信，fd 一丢，
     * 父进程立刻报 "Cannot communicate with zygote"。
     *
     * 这里把 exec 该继承的东西交给新任务，再让本任务退出。 */
    task_exec_inherit(new_id);
    task_exit();
    return 0;
}

/* Stream an ELF image from a VFS node into a NEW task (non-replacing).
 * Unlike elf64_load_and_spawn() the file is never copied into a kernel/user
 * buffer, so image size is bounded only by the filesystem and address space. */
int elf64_load_vfs_and_spawn(struct vfs_node *node,
                             char *const argv[], char *const envp[],
                             const char *task_name) {
    elf_arg_snapshot_t *snapshot =
        (elf_arg_snapshot_t *)kmalloc(sizeof(*snapshot));
    if (!snapshot) return elf_fail("argument snapshot allocation failed");
    if (snapshot_exec_arguments(argv, envp, snapshot) < 0) {
        kfree(snapshot);
        return elf_fail("invalid or oversized arguments");
    }
    elf_image_source_t source;
    if (prepare_vfs_source((vfs_node_t *)node, &source) < 0) {
        kfree(snapshot);
        return -1;
    }
    int new_id = elf64_load_source_and_spawn_snapshot(
        &source, snapshot->argv, snapshot->envp,
        task_name ? task_name : "elf_app");
    release_image_source(&source);
    kfree(snapshot);
    return new_id;
}
