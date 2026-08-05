/**
 * @file io.h
 * @brief x86 端口 I/O 与短等待原语
 *
 * 驱动统一使用这些名字，避免每个源文件各自复制一套内联汇编。这里不提供
 * 设备级超时策略；有界轮询使用 wait.h。
 */

#ifndef HBOS_CORE_IO_H
#define HBOS_CORE_IO_H

#include <stdint.h>

static inline void io_out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void io_out16(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void io_out32(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t io_in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t io_in16(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint32_t io_in32(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/** 传统 ISA I/O 延时；端口 0x80 在 PC 平台上保留给 POST。 */
static inline void io_delay(void) {
    io_out8(0x80, 0);
}

/** 在轮询循环中降低执行单元和 SMT 同级线程的压力。 */
static inline void cpu_relax(void) {
    __asm__ volatile("pause");
}

/** 读取时间戳计数器。不能直接把返回值当作固定频率的墙上时钟。 */
static inline uint64_t cpu_rdtsc(void) {
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#endif /* HBOS_CORE_IO_H */
