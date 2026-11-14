/**
 * @file    locale.h
 * @brief   LC_ALL/LC_CTYPE/LANG 语义与最小 setlocale 实现
 *
 * HBOS 没有 locale 数据（LC_COLLATE 的排序表、LC_TIME 的月份名、LC_NUMERIC
 * 的小数点表都不存在），所以这里只回答两个问题：
 *   1) 当前 locale 名字是什么（让 glibc/Qt/KDE 的 setlocale(LC_ALL,"") 有
 *      确定的回答，而不是返回 NULL 让调用方退化）；
 *   2) 当前 locale 是不是 UTF-8（决定宽字符路径走不走）。
 * 任何 LC_* 名字只要编码是 UTF-8 就接受（如 zh_CN.UTF-8），语义等价于
 * C.UTF-8：字节级排序、C 数值格式、UTF-8 文本。这是**契约**，写在
 * docs/wiki/I18N-PLAN.md，别在这里偷偷改成"假装有翻译"。
 *
 * 环境变量优先级按 POSIX：LC_ALL > LC_<CATEGORY> > LANG > "C"。
 *
 * @layer   lib
 * @owner   src/i18n/
 * @stable  internal
 *
 * 依赖    <stdint.h>；不依赖 libc 的 setlocale/getenv
 * 被依赖  src/i18n/locale.c 的使用方（引导期/linux_compat 的后续接入）
 *
 * 索引头文件
 *   类型  i18n_getenv_fn_t  环境变量读取回调（宿主/内核各自注入）
 *   宏    LC_CTYPE / LC_NUMERIC / LC_TIME / LC_COLLATE / LC_MONETARY /
 *         LC_MESSAGES / LC_ALL  与 glibc 数值一致的分类号
 *         I18N_LOCALE_NAME_MAX   locale 名字缓冲上限
 *   函数  setlocale           兼容 POSIX 的最小实现
 *         i18n_locale_init    注入 getenv 并按环境初始化
 *         i18n_locale_reset   全部复位为 "C"
 *         i18n_locale_name    查询分类名字
 *         i18n_locale_is_utf8 分类是否 UTF-8 编码
 */
#ifndef HBOS_I18N_LOCALE_H
#define HBOS_I18N_LOCALE_H

#define LC_CTYPE     0
#define LC_NUMERIC   1
#define LC_TIME      2
#define LC_COLLATE   3
#define LC_MONETARY  4
#define LC_MESSAGES  5
#define LC_ALL       6

#define I18N_LOCALE_NAME_MAX 32

typedef const char *(*i18n_getenv_fn_t)(const char *name);

/**
 * @brief 最小 setlocale。
 * @param category LC_* 之一
 * @param locale   NULL=查询；""=按环境变量初始化；其他=尝试设置
 * @return 成功返回当前名字（静态存储，勿释放）；失败返回 NULL 且不改变状态
 *
 * 支持的名字：C / POSIX / 任意以 .UTF-8 或 .utf8 结尾的名字 / UTF-8。
 * 不支持的名字（如 en_US.ISO-8859-1）返回 NULL。
 * 不变量：非线程安全；约定在引导期或单线程初始化阶段调用。
 */
char *setlocale(int category, const char *locale);

/**
 * @brief 注入环境读取函数并执行 setlocale(LC_ALL, "")。
 * @return 0。getenv_fn 为 NULL 表示没有环境变量，全部保持 "C"；
 *         环境里有不支持的 locale 时也退回 "C"，不留下半套状态。
 */
int i18n_locale_init(i18n_getenv_fn_t getenv_fn);

/** 所有分类复位为 "C"（引导期/测试用）。 */
void i18n_locale_reset(void);

/** 查询分类当前名字；category 非法返回 NULL。 */
const char *i18n_locale_name(int category);

/** 分类当前编码是否 UTF-8；LC_ALL 要求全部为 UTF-8 才算 1。 */
int i18n_locale_is_utf8(int category);

#endif /* HBOS_I18N_LOCALE_H */
