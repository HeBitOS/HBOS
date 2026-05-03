#include "fs.h"
#include "ata.h"

// 全局文件系统实例
static filesystem_t fs;

// 字符串操作函数（简化实现）
static int my_strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static void my_strcpy(char *dest, const char *src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = 0;
}

// 文件系统初始化
int fs_init(void) {
    // 初始化文件系统结构
    fs.file_count = 0;
    fs.total_sectors = 0;
    
    // 简单的文件系统实现 - 直接返回成功
    // 在实际系统中，这里应该检测硬盘和文件系统
    fs.total_sectors = 0; // 表示没有实际硬盘支持
    
    return 1; // 初始化成功
}

// 列出文件
void fs_list(void) {
    // 空实现 - 文件系统功能暂不实现
    // 在实际系统中，这里应该显示文件列表
}

// 查找文件
file_t *fs_find_file(const char *name) {
    // 空实现 - 文件系统功能暂不实现
    return NULL;
}

// 读取文件
void fs_read_file(file_t *f) {
    // 空实现 - 文件系统功能暂不实现
}