#ifndef HBOS_APP_REGISTRY_H
#define HBOS_APP_REGISTRY_H

#include "desktop_entry.h"

/* ══════════════════════════════════════════════════════════════════
 * 应用注册表：开始菜单与桌面图标**只从 .desktop 读**，不再盲扫 /bin
 *
 * 两个作用域，和 Linux 的 FHS 对齐：
 *   用户级  ~/.local/share/applications      （普通用户可写，无需 root）
 *   系统级  /usr/share/applications          （需要管理员）
 * 同一个 id 在两处都存在时，用户级优先（和 XDG_DATA_DIRS 的查找顺序一致）。
 * ══════════════════════════════════════════════════════════════════ */

#define HPT_USER_NAME  "linpinf"
#define HPT_USER_HOME  "/home/linpinf"
#define HPT_USER_LOCAL HPT_USER_HOME "/.local"

#define APPTAB_MAX 32
#define APPTAB_PATH_MAX 112

typedef struct {
    int used;
    de_entry_t de;
    char path[APPTAB_PATH_MAX];   /**< .desktop 实际路径，卸载/重注册用 */
} app_reg_t;

/** 建好两个 registry 目录（可重复调用）。 */
void appreg_init(void);

/** 重新扫描两个目录，用户级覆盖系统级。菜单打开/安装完成后调用。 */
void appreg_rescan(void);

int appreg_count(void);
const app_reg_t *appreg_at(int i);
int appreg_find_id(const char *id);          /**< 未找到返回 -1 */
int appreg_find_by_pkg(const char *pkg, int start);  /**< 从 start 找下一个 */

/** 把条目写进 registry 并落盘（覆盖同 id 条目）。返回 0 成功。 */
int appreg_register(de_entry_t *de, int scope);

/** 删除某个包注册的所有条目，返回删除数量。 */
int appreg_unregister_pkg(const char *pkg);

/** 删除单条（按 id）。 */
int appreg_unregister_id(const char *id);

/** 每次 rescan 递增的代号，UI 用它判断要不要重建布局缓存。 */
uint32_t appreg_generation(void);

/** 路径查询（安装器用） */
const char *appreg_scope_name(int scope);      /**< "user" / "system" */
const char *appreg_apps_dir(int scope);
const char *appreg_bin_dir(int scope);
const char *appreg_prefix(int scope);          /**< 安装根：/opt/<pkg> 的父目录 */
const char *appreg_state_dir(int scope);       /**< status / info 所在目录 */

/* 由 scope 决定的实际路径，写成形如 <prefix>/<pkg> 的绝对路径。 */
void appreg_pkg_root(int scope, const char *pkg, char *out, uint32_t cap);
void appreg_bin_path(int scope, const char *name, char *out, uint32_t cap);

#endif
