/**
 * @file compositor.h
 * @brief 窗口合成器头文件
 *
 * 提供离屏缓冲 + 脏区域跟踪 + 双缓冲的简单窗口合成器。
 * 在离屏缓冲中绘制窗口内容，仅将脏区域刷新到帧缓冲，
 * 减少不必要的像素拷贝。
 */

#ifndef HBOS_COMPOSITOR_H
#define HBOS_COMPOSITOR_H

#include <stdint.h>

/* ================================================================
 * 脏区域跟踪
 * ================================================================ */

#define COMPOSITOR_MAX_DIRTY 16  /**< 每帧最多脏矩形数 */

/** 脏矩形描述 */
typedef struct {
    uint64_t x, y;       /**< 左上角坐标 */
    uint64_t w, h;       /**< 宽高 */
} compositor_dirty_t;

/** 合成器操作类型 */
typedef enum {
    COMPOSITOR_OP_NONE = 0,  /**< 无操作 */
    COMPOSITOR_OP_FILL,      /**< 填充矩形 */
    COMPOSITOR_OP_BLIT,      /**< 拷贝矩形 */
    COMPOSITOR_OP_BLEND,     /**< Alpha混合 */
} compositor_op_t;

/* ================================================================
 * 合成器结构体
 * ================================================================ */

typedef struct {
    /** 离屏缓冲区（双缓冲） */
    uint32_t *buf[2];          /**< 两个缓冲区指针 */
    int       buf_front;       /**< 当前前缓冲索引 (0 或 1) */

    /** 帧缓冲尺寸 */
    uint64_t  width;
    uint64_t  height;
    uint64_t  pitch;           /**< 每行像素数 */

    /** 脏区域跟踪 */
    compositor_dirty_t dirty[COMPOSITOR_MAX_DIRTY];
    int                 dirty_count;

    /** 帧统计与性能 */
    int       frame_count;
    uint64_t  last_frame_time;   /**< 上一帧的时间戳（毫秒） */
    uint64_t  frame_time_avg;    /**< 平均帧时间（毫秒） */
} compositor_t;

/* ================================================================
 * 公共 API
 * ================================================================ */

/**
 * @brief 初始化合成器
 *
 * 分配离屏缓冲区（与帧缓冲同尺寸），初始化双缓冲结构。
 *
 * @param c 合成器状态指针
 * @return 成功返回 0，失败返回 -1
 */
int compositor_init(compositor_t *c);

/**
 * @brief 开始新一帧
 *
 * 清除脏区域列表，准备接收新的绘制操作。
 *
 * @param c 合成器状态指针
 */
void compositor_begin_frame(compositor_t *c);

/**
 * @brief 结束当前帧
 *
 * 将脏区域从离屏缓冲区刷新到帧缓冲，交换双缓冲。
 *
 * @param c 合成器状态指针
 */
void compositor_end_frame(compositor_t *c);

/**
 * @brief 标记脏区域
 *
 * 将指定矩形区域标记为"需要刷新"，合成器和外部绘制代码
 * 使用此函数通知合成器哪些区域已发生变化。
 *
 * @param c 合成器状态指针
 * @param x 左上角 X
 * @param y 左上角 Y
 * @param w 宽度
 * @param h 高度
 */
void compositor_damage_rect(compositor_t *c, uint64_t x, uint64_t y,
                            uint64_t w, uint64_t h);

/**
 * @brief 获取当前离屏缓冲区指针（前缓冲）
 *
 * 外部绘制代码将内容绘制到此缓冲区，然后标记脏区域。
 *
 * @param c 合成器状态指针
 * @return 前缓冲像素缓冲区指针
 */
uint32_t *compositor_get_buf(compositor_t *c);

/**
 * @brief 获取合成器宽度
 */
uint64_t compositor_get_width(compositor_t *c);

/**
 * @brief 获取合成器高度
 */
uint64_t compositor_get_height(compositor_t *c);

/**
 * @brief Alpha混合两个颜色
 *
 * 将源颜色按照alpha值混合到目标颜色上。
 * 
 * @param src 源颜色（ARGB格式）
 * @param dst 目标颜色（ARGB格式）
 * @return 混合后的颜色
 */
uint32_t compositor_blend(uint32_t src, uint32_t dst);

/**
 * @brief 在合成器缓冲区中绘制带alpha混合的矩形
 * 
 * @param c 合成器状态指针
 * @param x 左上角 X
 * @param y 左上角 Y
 * @param w 宽度
 * @param h 高度
 * @param color 颜色（ARGB格式）
 */
void compositor_fill_rect_alpha(compositor_t *c, uint64_t x, uint64_t y,
                                uint64_t w, uint64_t h, uint32_t color);

#endif /* HBOS_COMPOSITOR_H */