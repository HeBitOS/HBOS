# Shell — 交互式命令行 (`src/shell/`)

> 1066 行, REPL 命令行解释器, 命令注册/查找/执行, 历史记录, 参数解析

## 文件清单

| 文件 | 职责 |
|------|------|
| `shell.c` | 命令注册表, REPL 循环, 命令解析, 执行, 历史, 管道 |
| `shell.h` | command_t 结构, 命令组枚举, 注册/查找 API |

## 命令注册

```c
// shell.h
#define CMD_GROUP_COUNT 5
typedef enum {
    CMD_GROUP_SYSTEM   = 0,  // 系统命令: drivers, reboot, poweroff, ...
    CMD_GROUP_FILE     = 1,  // 文件命令: ls, cat, cp, rm, mkdir, ...
    CMD_GROUP_GRAPHICS = 2,  // 图形命令: gui
    CMD_GROUP_DEBUG    = 3,  // 调试命令: selftest, mmap, ...
    CMD_GROUP_USER     = 4,  // 用户命令: app, ...
} cmd_group_t;

typedef struct {
    const char *name;           // 命令名 (如 "drivers")
    const char *desc;           // 一行描述
    cmd_group_t group;          // 所属组
    int (*fn)(int argc, char *argv[]);  // 执行函数, 返回 0=成功
} command_t;

// 注册/查找
void cmd_register(const command_t *cmd);                 // 注册一条命令 (最多 128)
void cmd_register_multiple(const command_t *cmds[]);     // 批量注册
const command_t *cmd_find(const char *name);             // 按名称查找
const command_t **cmd_get_list(void);                    // 获取完整列表
uint32_t cmd_get_count(void);
```

## REPL 循环 (简化)

```c
// shell.c → shell_run()
while (1) {
    // 1. 读键盘 (kb_poll_key, 阻塞等按键)
    // 2. 特殊键: Enter→执行, Backspace→删除, Tab→补全, ↑↓→历史
    // 3. 到达行尾 (Enter) → 解析参数 → 执行
    // 4. 超过最大命令长度 → 截断

    // 命令执行:
    const command_t *cmd = cmd_find(name);  // 按 name 查找
    if (cmd) {
        cmd->fn(argc, argv);                // 调执行函数
    } else {
        // 尝试作为用户程序: vfs_open → 检查 ELF → app_run
    }
}
```

## Shell 内置功能

| 功能 | 说明 |
|------|------|
| 命令提示符 | `HBOS>` 前缀 |
| 历史记录 | ↑↓ 键翻阅 (历史缓冲区) |
| Tab 补全 | 按 Tab 自动补全命令名 |
| 参数解析 | 空格分割, 引号支持 |
| 用户程序 | 如命令名不在注册表中, 尝试作为 ELF 运行 |
| 管道 | 基础支持 |

## 如何新增命令

```c
// 1. 在 tools/ 或 src/ 下写:
#include "../shell/shell.h"
static int my_cmd(int argc, char *argv[]) {
    console_print("Hello from my_cmd\n");
    return 0;
}

// 2. 在某 tool_*_init() 里注册:
static command_t cmd_my = {
    .name = "mycmd",
    .desc = "My custom command",
    .group = CMD_GROUP_SYSTEM,
    .fn = my_cmd,
};
cmd_register(&cmd_my);

// 3. 确保 tool_init_all() 调用你的 tool_*_init()
```

## 与用户空间程序的关系

```c
// shell_run() 中如果 cmd_find 返回 NULL:
// → 调用 app_run(name, argc, argv)
//    → 从 VFS 加载文件 (如 /bin/ls)
//    → 检查 ELF header
//    → 创建 ring3 任务
//    → app_runtime 初始化
//    → ldso 动态链接
//    → 执行 entry point
```

## 键盘输入处理

键盘在 `src/graphics/graphics.c` 的 `console_*` 函数中处理：
- `kb_poll_key()`: 从 ring buffer 出队一个按键 (阻塞)
- 特殊键: 方向键, Home, End, Delete, PgUp, PgDn
- 修饰键: Shift, Ctrl 处理
