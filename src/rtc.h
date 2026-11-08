/**
 * @file    rtc.h
 * @brief   导出 5 个接口（1 类型 / 0 宏 / 4 函数）
 *
 * @layer   drivers
 * @owner   src/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  rtc_time_t  结构体 / 类型别名
 *   函数  rtc_init         （待补）
 *         rtc_read_time    （待补）
 *         rtc_timestamp    （待补）
 *         rtc_format_time  （待补）
 */
#ifndef HBOS_RTC_H
#define HBOS_RTC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
    uint8_t century;
    uint8_t weekday;
} rtc_time_t;

void rtc_init(void);
int rtc_read_time(rtc_time_t *time);
uint64_t rtc_timestamp(void);
void rtc_format_time(char *buf, size_t size);

#endif