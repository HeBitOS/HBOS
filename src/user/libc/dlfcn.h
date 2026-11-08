/**
 * @file    dlfcn.h
 * @brief   用户态 libc 的运行时装载接口（新 .hax 动态链接 ABI 的 A 部分）
 *
 * 供 .hax 应用在运行时按名字取符号；宿主是 src/user/loader.c 与内核的
 * HAX 装载器，不依赖内核私有结构——本头文件只声明符号，不带任何内部类型。
 *
 * 索引头文件
 *   宏    RTLD_LAZY        惰性绑定（HBOS 实现按 NOW 处理）
 *          RTLD_NOW         立即绑定
 *   类型  （无导出类型）
 *   函数  dlopen           打开 .hax 并返回不透明句柄，失败返回 NULL
 *          dlsym            按符号名取地址，失败返回 NULL 且 dlerror 有值
 *          dlvsym           带版本号取符号，HBOS 下等价于 dlsym
 *          dlclose          释放句柄（计数归零后卸载）
 *          dlerror          取最近一次错误描述，取走后清空
 */
#ifndef HBOS_USER_LIBC_DLFCN_H
#define HBOS_USER_LIBC_DLFCN_H

#define RTLD_LAZY  0x00001
#define RTLD_NOW   0x00002

void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
void *dlvsym(void *handle, const char *symbol, const char *version);
int   dlclose(void *handle);
char *dlerror(void);

#endif
