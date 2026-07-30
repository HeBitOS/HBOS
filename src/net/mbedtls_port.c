#include "net/mbedtls_port.h"

#include "core/heap.h"
#include "rtc_tz.h"
#include "string.h"

#include <mbedtls/platform_time.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>
#include <psa/crypto_extra.h>
#include <psa/crypto_values.h>
#include <time.h>
#include <stdarg.h>

uint64_t pit_get_ticks(void);
uint32_t pit_get_frequency_hz(void) __attribute__((weak));

void *hbos_mbedtls_calloc(size_t count, size_t size) {
    return kcalloc(count, size);
}

void hbos_mbedtls_free(void *pointer) {
    kfree(pointer);
}

static void format_character(char *buffer, size_t size, size_t *position,
                             char value) {
    if (size && *position + 1U < size) buffer[*position] = value;
    (*position)++;
}

static void format_unsigned(char *buffer, size_t size, size_t *position,
                            unsigned long long value, unsigned base,
                            int width, char padding, int uppercase) {
    char digits[32];
    int count = 0;
    const char *alphabet = uppercase
        ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        digits[count++] = alphabet[value % base];
        value /= base;
    } while (value && count < (int)sizeof(digits));
    while (count < width) digits[count++] = padding;
    while (count) format_character(buffer, size, position, digits[--count]);
}

static int hbos_mbedtls_vsnprintf(char *buffer, size_t size,
                                  const char *format, va_list arguments) {
    size_t position = 0;
    while (format && *format) {
        if (*format != '%') {
            format_character(buffer, size, &position, *format++);
            continue;
        }
        format++;
        int left = 0;
        char padding = ' ';
        if (*format == '-') {
            left = 1;
            format++;
        }
        if (*format == '0') {
            padding = '0';
            format++;
        }
        int width = 0;
        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (*format - '0');
            format++;
        }
        int length = 0;
        if (*format == 'z') {
            length = 3;
            format++;
        } else if (*format == 'l') {
            length = 1;
            format++;
            if (*format == 'l') {
                length = 2;
                format++;
            }
        }

        if (*format == 's') {
            const char *text = va_arg(arguments, const char *);
            if (!text) text = "(null)";
            size_t text_length = strlen(text);
            if (!left)
                while (width-- > (int)text_length)
                    format_character(buffer, size, &position, ' ');
            while (*text)
                format_character(buffer, size, &position, *text++);
            if (left)
                while (width-- > (int)text_length)
                    format_character(buffer, size, &position, ' ');
        } else if (*format == 'c') {
            format_character(buffer, size, &position,
                             (char)va_arg(arguments, int));
        } else if (*format == 'd' || *format == 'i') {
            long long signed_value =
                length == 2 ? va_arg(arguments, long long) :
                length == 1 ? va_arg(arguments, long) :
                              va_arg(arguments, int);
            unsigned long long value;
            if (signed_value < 0) {
                format_character(buffer, size, &position, '-');
                value = (unsigned long long)(-(signed_value + 1)) + 1U;
                if (width > 0) width--;
            } else {
                value = (unsigned long long)signed_value;
            }
            format_unsigned(buffer, size, &position, value, 10,
                            width, padding, 0);
        } else if (*format == 'u' || *format == 'x' || *format == 'X') {
            unsigned long long value =
                length == 2 ? va_arg(arguments, unsigned long long) :
                length == 1 ? va_arg(arguments, unsigned long) :
                length == 3 ? (unsigned long long)va_arg(arguments, size_t) :
                              va_arg(arguments, unsigned int);
            format_unsigned(buffer, size, &position, value,
                            *format == 'u' ? 10U : 16U,
                            width, padding, *format == 'X');
        } else if (*format == 'p') {
            uintptr_t value = (uintptr_t)va_arg(arguments, void *);
            format_character(buffer, size, &position, '0');
            format_character(buffer, size, &position, 'x');
            format_unsigned(buffer, size, &position, value, 16,
                            (int)(sizeof(void *) * 2U), '0', 0);
        } else if (*format == '%') {
            format_character(buffer, size, &position, '%');
        } else {
            format_character(buffer, size, &position, '%');
            if (*format) format_character(buffer, size, &position, *format);
        }
        if (*format) format++;
    }
    if (size) buffer[position < size ? position : size - 1U] = '\0';
    return position > 0x7FFFFFFFU ? -1 : (int)position;
}

int hbos_mbedtls_snprintf(char *buffer, size_t size,
                          const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result =
        hbos_mbedtls_vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return result;
}

unsigned __int128 __udivti3(unsigned __int128 numerator,
                           unsigned __int128 denominator) {
    if (!denominator) return 0;
    unsigned __int128 quotient = 0;
    unsigned __int128 remainder = 0;
    for (int bit = 127; bit >= 0; bit--) {
        int carry = (int)(remainder >> 127);
        remainder = (remainder << 1) | ((numerator >> bit) & 1U);
        if (carry || remainder >= denominator) {
            remainder -= denominator;
            quotient |= (unsigned __int128)1U << bit;
        }
    }
    return quotient;
}

unsigned __int128 __umodti3(unsigned __int128 numerator,
                           unsigned __int128 denominator) {
    if (!denominator) return 0;
    unsigned __int128 remainder = 0;
    for (int bit = 127; bit >= 0; bit--) {
        int carry = (int)(remainder >> 127);
        remainder = (remainder << 1) | ((numerator >> bit) & 1U);
        if (carry || remainder >= denominator) remainder -= denominator;
    }
    return remainder;
}

int64_t hbos_mbedtls_time(int64_t *result) {
    int64_t now = rtc_tz_cmos_epoch_now() + g_rtc_ntp_correction_sec;
    if (result) *result = now;
    return now;
}

mbedtls_ms_time_t mbedtls_ms_time(void) {
    uint32_t frequency =
        pit_get_frequency_hz ? pit_get_frequency_hz() : 100U;
    if (!frequency) frequency = 100U;
    uint64_t ticks = pit_get_ticks();
    return (mbedtls_ms_time_t)((ticks * 1000ULL) / frequency);
}

void mbedtls_platform_zeroize(void *buffer, size_t length) {
    volatile uint8_t *bytes = buffer;
    while (length--) *bytes++ = 0;
}

static void civil_from_days(int64_t days, int *year, int *month, int *day) {
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yoe + (int)(era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    int m = (int)mp + (mp < 10 ? 3 : -9);
    *year = y + (m <= 2);
    *month = m;
    *day = (int)d;
}

struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *input,
                                     struct tm *output) {
    if (!input || !output) return NULL;
    int64_t seconds = *input;
    int64_t days =
        seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400;
    int64_t day_seconds = seconds - days * 86400;
    int year, month, day;
    civil_from_days(days, &year, &month, &day);

    output->tm_sec = (int)(day_seconds % 60);
    output->tm_min = (int)((day_seconds / 60) % 60);
    output->tm_hour = (int)(day_seconds / 3600);
    output->tm_mday = day;
    output->tm_mon = month - 1;
    output->tm_year = year - 1900;
    output->tm_wday = (int)(((days + 4) % 7 + 7) % 7);
    output->tm_yday = 0;
    output->tm_isdst = 0;
    return output;
}

static int cpu_has_rdrand(void) {
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    (void)ebx;
    (void)edx;
    return (ecx & (1U << 30)) != 0;
}

static int rdrand64(uint64_t *value) {
    unsigned char ok;
    for (int attempt = 0; attempt < 16; attempt++) {
        __asm__ volatile("rdrand %0; setc %1"
                         : "=r"(*value), "=qm"(ok));
        if (ok) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

psa_status_t mbedtls_psa_external_get_random(
    mbedtls_psa_external_random_context_t *context,
    uint8_t *output, size_t output_size, size_t *output_length) {
    (void)context;
    if (!output || !output_length || !cpu_has_rdrand())
        return PSA_ERROR_INSUFFICIENT_ENTROPY;

    size_t offset = 0;
    while (offset < output_size) {
        uint64_t word;
        if (rdrand64(&word) < 0) {
            *output_length = 0;
            return PSA_ERROR_HARDWARE_FAILURE;
        }
        size_t count = output_size - offset;
        if (count > sizeof(word)) count = sizeof(word);
        for (size_t i = 0; i < count; i++)
            output[offset + i] = (uint8_t)(word >> (i * 8));
        offset += count;
    }
    *output_length = output_size;
    return PSA_SUCCESS;
}
