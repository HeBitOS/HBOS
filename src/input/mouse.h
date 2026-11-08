/**
 * @file    mouse.h
 * @brief   导出 7 个接口（1 类型 / 0 宏 / 6 函数）
 *
 * @layer   input
 * @owner   src/input/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  mouse_event_t  结构体 / 类型别名
 *   函数  mouse_init              （待补）
 *         mouse_shutdown          （待补）
 *         mouse_poll              （待补）
 *         mouse_is_ready          （待补）
 *         mouse_backend_name      （待补）
 *         ps2_mouse_enqueue_byte  （待补）
 */
#ifndef HBOS_MOUSE_H
#define HBOS_MOUSE_H

#include <stdint.h>

typedef struct {
    int dx;
    int dy;
    int dz;
    uint8_t buttons;
} mouse_event_t;

int mouse_init(void);
void mouse_shutdown(void);
int mouse_poll(mouse_event_t *ev);
int mouse_is_ready(void);
const char *mouse_backend_name(void);
void ps2_mouse_enqueue_byte(uint8_t b);

#endif
