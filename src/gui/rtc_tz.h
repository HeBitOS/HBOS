/**
 * @file rtc_tz.h
 * @brief Shared "read the hardware RTC and apply a timezone offset" helper.
 *
 * Real hardware and QEMU keep the CMOS RTC in UTC, but VMware commonly
 * mirrors the *host's local time* into CMOS instead — there is no reliable
 * way to tell which convention a given BIOS uses from inside the guest, so
 * instead of hardcoding "CMOS is UTC, add 8 hours" this exposes the offset
 * as a runtime setting (see app_settings.c) that defaults to 8 (China
 * Standard Time, matching the previous hardcoded behavior) but can be
 * changed per-VM. net_ntp_sync() (src/net.c) can additionally correct for
 * an inaccurate/drifted CMOS clock by storing a seconds-level delta here;
 * the timezone offset is still needed on top of that to pick the right
 * *local* wall-clock hour.
 *
 * Previously gui.c and app_clock.c each had their own copy of the CMOS-read
 * logic; this header centralizes it so a future fix doesn't have to be
 * applied twice and risk drifting out of sync again.
 */
#ifndef HBOS_GUI_RTC_TZ_H
#define HBOS_GUI_RTC_TZ_H

#include <stdint.h>

/** 用户在设置里选择的时区偏移（小时），默认 8 = UTC+8。 */
extern int g_rtc_tz_offset_hours;
/** net_ntp_sync() 成功后写入的修正量（秒） = NTP 服务器时间 - 当时 CMOS 读数；
 *  未同步过为 0（完全信任 CMOS）。 */
extern long long g_rtc_ntp_correction_sec;

static inline void rtc_tz_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t rtc_tz_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline uint8_t rtc_tz_cmos_read(uint8_t reg) {
    rtc_tz_outb(0x70, reg);
    return rtc_tz_inb(0x71);
}
static inline uint8_t rtc_tz_bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0f) + ((v >> 4) * 10));
}

/* Howard Hinnant 的 days_from_civil / civil_from_days 算法：民用日期 (y,m,d)
 * 与"自 1970-01-01 起的天数"之间的换算，公历下恒正确（含闰年）。 */
static inline int64_t rtc_tz_days_from_civil(int64_t y, int m, int d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                   /* [0, 399] */
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;   /* [0, 365] */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

static inline void rtc_tz_civil_from_days(int64_t z, int64_t *out_y, int *out_m, int *out_d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;                                        /* [0, 146096] */
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;   /* [0, 399] */
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                  /* [0, 365] */
    int64_t mp = (5 * doy + 2) / 153;                                       /* [0, 11] */
    int64_t d = doy - (153 * mp + 2) / 5 + 1;                               /* [1, 31] */
    int64_t m = mp + (mp < 10 ? 3 : -9);                                    /* [1, 12] */
    *out_y = y + (m <= 2);
    *out_m = (int)m;
    *out_d = (int)d;
}

/**
 * 读取 CMOS 原始时间字段，换算为自 1970-01-01 00:00:00 起的秒数——不做任何时区
 * 或 NTP 修正，只是把 CMOS 里写的数值本身当作一个时间戳。gui.c/app_clock.c 不
 * 需要关心这个；net_ntp_sync() 用它来算出应写入 g_rtc_ntp_correction_sec 的量。
 */
static inline int64_t rtc_tz_cmos_epoch_now(void) {
    uint8_t sb   = rtc_tz_cmos_read(0x0b);
    uint8_t h    = rtc_tz_cmos_read(0x04);
    uint8_t mi   = rtc_tz_cmos_read(0x02);
    uint8_t s    = rtc_tz_cmos_read(0x00);
    uint8_t day  = rtc_tz_cmos_read(0x07);
    uint8_t mon  = rtc_tz_cmos_read(0x08);
    uint8_t yr   = rtc_tz_cmos_read(0x09);
    uint8_t cent = rtc_tz_cmos_read(0x32);

    if ((sb & 0x04) == 0) {
        h = rtc_tz_bcd_to_bin(h); mi = rtc_tz_bcd_to_bin(mi); s = rtc_tz_bcd_to_bin(s);
        day = rtc_tz_bcd_to_bin(day); mon = rtc_tz_bcd_to_bin(mon);
        yr = rtc_tz_bcd_to_bin(yr); cent = rtc_tz_bcd_to_bin(cent);
    }
    if ((sb & 0x02) == 0) {
        uint8_t pm = h & 0x80; h &= 0x7f;
        if (pm && h < 12) h += 12;
        if (!pm && h == 12) h = 0;
    }
    int64_t year = (int64_t)cent * 100 + yr;
    int64_t days = rtc_tz_days_from_civil(year, mon, day);
    return days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + s;
}

/**
 * 读取 RTC 并换算为"本地时间"：CMOS 原始时间戳 + 上次 NTP 同步修正量
 * (g_rtc_ntp_correction_sec) + 用户设置的时区偏移 (g_rtc_tz_offset_hours)，
 * 再换算回年月日时分秒 + 星期。
 *
 * out_wday: 1=Sunday..7=Saturday。
 */
static inline void rtc_tz_read_local(uint8_t *out_h, uint8_t *out_m, uint8_t *out_s,
                                      uint8_t *out_day, uint8_t *out_mon, uint32_t *out_year,
                                      uint8_t *out_wday) {
    int64_t epoch = rtc_tz_cmos_epoch_now() + g_rtc_ntp_correction_sec
                    + (int64_t)g_rtc_tz_offset_hours * 3600;

    int64_t days = epoch >= 0 ? epoch / 86400 : (epoch - 86399) / 86400;
    int64_t sod  = epoch - days * 86400;   /* [0, 86399] */

    int64_t y; int m, d;
    rtc_tz_civil_from_days(days, &y, &m, &d);

    *out_h = (uint8_t)(sod / 3600);
    *out_m = (uint8_t)((sod / 60) % 60);
    *out_s = (uint8_t)(sod % 60);
    *out_day = (uint8_t)d;
    *out_mon = (uint8_t)m;
    *out_year = (uint32_t)y;
    /* days=0 (1970-01-01) 是星期四；Sunday=1..Saturday=7 */
    int64_t wd = ((days % 7) + 7 + 4) % 7 + 1;
    *out_wday = (uint8_t)wd;
}

#endif /* HBOS_GUI_RTC_TZ_H */
