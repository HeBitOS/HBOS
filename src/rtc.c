/**
 * @file    rtc.c
 * @brief   CMOS RTC 实时时钟驱动
 *
 * 通过 CMOS I/O 端口 (0x70/0x71) 读取日期和时间。
 * 遵循 BCD 编码惯例，自动检测 BCD 模式并解码。
 * 支持 NMI 禁用位控制。
 */

#include "rtc.h"
#include "string.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define RTC_SECONDS      0x00
#define RTC_MINUTES      0x02
#define RTC_HOURS        0x04
#define RTC_WEEKDAY      0x06
#define RTC_DAY_OF_MONTH 0x07
#define RTC_MONTH         0x08
#define RTC_YEAR          0x09
#define RTC_CENTURY       0x32
#define RTC_STATUS_A      0x0A
#define RTC_STATUS_B      0x0B

static uint8_t rtc_read(uint8_t reg) {
    __asm__ volatile("outb %0, %1" :: "a"(reg), "Nd"(CMOS_ADDR));
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(CMOS_DATA));
    return val;
}

static int rtc_is_updating(void) {
    return rtc_read(RTC_STATUS_A) & 0x80;
}

static uint8_t rtc_bcd_to_bin(uint8_t val) {
    return (val & 0x0F) + ((val / 16) * 10);
}

void rtc_init(void) {
    uint8_t status_b = rtc_read(RTC_STATUS_B);
    (void)status_b;
}

int rtc_read_time(rtc_time_t *time) {
    if (!time) return -1;

    while (rtc_is_updating());

    time->second  = rtc_read(RTC_SECONDS);
    time->minute  = rtc_read(RTC_MINUTES);
    time->hour    = rtc_read(RTC_HOURS);
    time->day     = rtc_read(RTC_DAY_OF_MONTH);
    time->month   = rtc_read(RTC_MONTH);
    time->year    = rtc_read(RTC_YEAR);
    time->century = rtc_read(RTC_CENTURY);
    time->weekday = rtc_read(RTC_WEEKDAY);

    uint8_t status_b = rtc_read(RTC_STATUS_B);

    if (!(status_b & 0x04)) {
        time->second  = rtc_bcd_to_bin(time->second);
        time->minute  = rtc_bcd_to_bin(time->minute);
        time->hour    = rtc_bcd_to_bin(time->hour);
        time->day     = rtc_bcd_to_bin(time->day);
        time->month   = rtc_bcd_to_bin(time->month);
        time->year    = rtc_bcd_to_bin(time->year);
        time->century = rtc_bcd_to_bin(time->century);
    }

    if (time->century == 0) time->century = 20;
    if (time->year < 80) time->year += 2000;
    else time->year += 1900;

    return 0;
}

uint64_t rtc_timestamp(void) {
    rtc_time_t t;
    if (rtc_read_time(&t) < 0) return 0;

    uint64_t days = 0;
    for (int y = 1970; y < (int)t.year; y++) {
        days += 365;
        if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) days++;
    }

    static const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int leap = ((t.year % 4 == 0 && t.year % 100 != 0) || (t.year % 400 == 0));
    for (int m = 1; m < t.month; m++) {
        days += month_days[m - 1];
        if (m == 2 && leap) days++;
    }
    days += t.day - 1;

    return days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
}

void rtc_format_time(char *buf, size_t size) {
    if (!buf || size < 20) return;
    rtc_time_t t;
    if (rtc_read_time(&t) < 0) {
        memset(buf, 0, size);
        return;
    }
    int pos = 0;
    int n;
    char tmp[8];

    n = 0; do { tmp[n++] = '0' + (t.year % 10); t.year /= 10; } while (t.year);
    while (n--) buf[pos++] = tmp[n];
    buf[pos++] = '-';

    n = 0; do { tmp[n++] = '0' + (t.month % 10); t.month /= 10; } while (t.month);
    if (n < 2) tmp[n++] = '0';
    while (n--) buf[pos++] = tmp[n];
    buf[pos++] = '-';

    n = 0; do { tmp[n++] = '0' + (t.day % 10); t.day /= 10; } while (t.day);
    if (n < 2) tmp[n++] = '0';
    while (n--) buf[pos++] = tmp[n];
    buf[pos++] = ' ';

    n = 0; do { tmp[n++] = '0' + (t.hour % 10); t.hour /= 10; } while (t.hour);
    if (n < 2) tmp[n++] = '0';
    while (n--) buf[pos++] = tmp[n];
    buf[pos++] = ':';

    n = 0; do { tmp[n++] = '0' + (t.minute % 10); t.minute /= 10; } while (t.minute);
    if (n < 2) tmp[n++] = '0';
    while (n--) buf[pos++] = tmp[n];
    buf[pos++] = ':';

    n = 0; do { tmp[n++] = '0' + (t.second % 10); t.second /= 10; } while (t.second);
    if (n < 2) tmp[n++] = '0';
    while (n--) buf[pos++] = tmp[n];

    buf[pos] = '\0';
}