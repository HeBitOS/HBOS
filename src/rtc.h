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