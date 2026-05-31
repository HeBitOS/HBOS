#ifndef HBOS_SHELL_H
#define HBOS_SHELL_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// Shell 命令系统接口
// ============================================================

// 命令分组
typedef enum {
    CMD_GROUP_SYSTEM = 0,
    CMD_GROUP_FILE,
    CMD_GROUP_GRAPHICS,
    CMD_GROUP_DEBUG,
    CMD_GROUP_USER,
    CMD_GROUP_COUNT
} cmd_group_t;

// 命令处理器类型
typedef void (*cmd_handler_t)(int argc, char **argv);

// 命令结构
typedef struct {
    const char *name;
    cmd_group_t group;
    const char *description;
    const char *usage;
    cmd_handler_t handler;
} command_t;

// ============================================================
// Shell API
// ============================================================

// 初始化 Shell 子系统
void shell_init(void);

// 注册一条命令
void cmd_register(const command_t *cmd);

// 批量注册命令（以 NULL 结尾的数组）
void cmd_register_multiple(const command_t *cmds[]);

// 执行单条命令
void cmd_execute(const char *line);

// 获取命令列表
const command_t **cmd_get_list(void);

// 获取命令数量
uint32_t cmd_get_count(void);

// 获取命令组名称
const char *cmd_get_group_name(int group_id);

// Shell 主循环（阻塞）
void shell_run(void);

// 输出 Shell 提示符
void shell_print_prompt(void);

// 简易外部命令注册（供应用程序使用）
void cmd_register_external(const char *name, const char *desc,
                           void (*handler)(int argc, char **argv));

// 历史记录接口
const char **cmd_get_history(void);
int           cmd_get_history_count(void);
void          cmd_clear_history(void);

// 键盘 API（供工具模块使用）
int kb_get_key(void);

// 特殊键码定义（kb_get_key 的返回值）
#define KB_KEY_UP      0x100
#define KB_KEY_DOWN    0x101
#define KB_KEY_LEFT    0x102
#define KB_KEY_RIGHT   0x103
#define KB_KEY_PGUP    0x104
#define KB_KEY_PGDWN   0x105
#define KB_KEY_HOME    0x106
#define KB_KEY_END     0x107
#define KB_KEY_INSERT  0x108
#define KB_KEY_DELETE  0x109

// NumLock 状态查询
bool kb_is_numlock(void);

#endif /* HBOS_SHELL_H */
