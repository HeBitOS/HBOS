/**
 * @file rtc_tz.h
 * @brief Shared "read the hardware RTC and apply a timezone offset" helper.
 *
 * The hardware RTC (CMOS clock) that gui.c and the Clock app both read is set
 * to UTC in this VM environment, but the taskbar clock and Clock app should
 * show local time. RTC_TZ_OFFSET_HOURS is the fixed offset applied to the
 * raw RTC hour; adjust it if the RTC ever gets set to local time directly,
 * or the host timezone changes.
 *
 * Previously gui.c and app_clock.c each had their own copy of the CMOS-read
 * logic; this header centralizes it so a future fix (like this one) doesn't
 * have to be applied twice and risk drifting out of sync again.
 */
#ifndef HBOS_GUI_RTC_TZ_H
#define HBOS_GUI_RTC_TZ_H

#include <stdint.h>

#define RTC_TZ_OFFSET_HOURS 8   /* UTC -> China Standard Time (UTC+8) */

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

static inline int rtc_tz_is_leap(uint32_t y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}
static inline uint8_t rtc_tz_days_in_month(uint32_t y, uint8_t m) {
    static const uint8_t dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && rtc_tz_is_leap(y)) return 29;
    if (m < 1 || m > 12) return 30;
    return dim[m - 1];
}

/**
 * Reads the RTC and applies RTC_TZ_OFFSET_HOURS, rolling the date
 * forward/backward (with correct month/year/leap-year handling) if the
 * offset pushes the hour past midnight in either direction.
 * out_wday: 1=Sunday..7=Saturday, adjusted for any day rollover.
 */
static inline void rtc_tz_read_local(uint8_t *out_h, uint8_t *out_m, uint8_t *out_s,
                                      uint8_t *out_day, uint8_t *out_mon, uint32_t *out_year,
                                      uint8_t *out_wday) {
    uint8_t sb   = rtc_tz_cmos_read(0x0b);
    uint8_t h    = rtc_tz_cmos_read(0x04);
    uint8_t mi   = rtc_tz_cmos_read(0x02);
    uint8_t s    = rtc_tz_cmos_read(0x00);
    uint8_t day  = rtc_tz_cmos_read(0x07);
    uint8_t mon  = rtc_tz_cmos_read(0x08);
    uint8_t yr   = rtc_tz_cmos_read(0x09);
    uint8_t cent = rtc_tz_cmos_read(0x32);
    uint8_t wd   = rtc_tz_cmos_read(0x06);

    if ((sb & 0x04) == 0) {
        h = rtc_tz_bcd_to_bin(h); mi = rtc_tz_bcd_to_bin(mi); s = rtc_tz_bcd_to_bin(s);
        day = rtc_tz_bcd_to_bin(day); mon = rtc_tz_bcd_to_bin(mon);
        yr = rtc_tz_bcd_to_bin(yr); cent = rtc_tz_bcd_to_bin(cent);
        wd = rtc_tz_bcd_to_bin(wd);
    }
    if ((sb & 0x02) == 0) {
        uint8_t pm = h & 0x80; h &= 0x7f;
        if (pm && h < 12) h += 12;
        if (!pm && h == 12) h = 0;
    }
    uint32_t year = (uint32_t)cent * 100u + yr;

    int hour = (int)h + RTC_TZ_OFFSET_HOURS;
    int day_delta = 0;
    while (hour < 0)   { hour += 24; day_delta -= 1; }
    while (hour >= 24) { hour -= 24; day_delta += 1; }

    int wday_i = (int)wd;
    if (day_delta != 0) {
        int d = (int)day + day_delta;
        int mo = (int)mon;
        int32_t yy = (int32_t)year;
        wday_i += day_delta;
        while (wday_i < 1) wday_i += 7;
        while (wday_i > 7) wday_i -= 7;
        while (d < 1) {
            mo -= 1;
            if (mo < 1) { mo = 12; yy -= 1; }
            d += rtc_tz_days_in_month((uint32_t)yy, (uint8_t)mo);
        }
        for (;;) {
            uint8_t dim = rtc_tz_days_in_month((uint32_t)yy, (uint8_t)mo);
            if (d <= dim) break;
            d -= dim;
            mo += 1;
            if (mo > 12) { mo = 1; yy += 1; }
        }
        day = (uint8_t)d; mon = (uint8_t)mo; year = (uint32_t)yy;
    }

    *out_h = (uint8_t)hour; *out_m = mi; *out_s = s;
    *out_day = day; *out_mon = mon; *out_year = year;
    *out_wday = (uint8_t)wday_i;
}

#endif /* HBOS_GUI_RTC_TZ_H */
