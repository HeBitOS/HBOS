# HBOS GUI改进文档

## 概述

本次GUI改进主要集中在以下几个方面：
1. **合成器性能优化** - 改进脏区域管理和Alpha混合支持
2. **窗口动画系统** - 添加平滑的窗口动画效果
3. **视觉效果库** - 提供阴影、圆角、渐变等高级视觉效果

## 改进详情

### 1. 合成器优化 (`src/gui/compositor.c/h`)

#### 改进的脏区域管理
- **智能合并算法**: 只有当合并后的区域面积增长不超过50%时才合并，减少不必要的重绘
- **面积优化**: 避免过度合并导致重绘大面积不需要更新的区域

#### Alpha混合支持
- **compositor_blend()**: 高效的Alpha混合函数
- **compositor_fill_rect_alpha()**: 支持透明度的矩形填充
- 完全透明/不透明的快速路径优化

#### 性能统计
- 添加帧时间跟踪
- 平均帧时间记录
- 为未来的性能分析和优化提供数据支持

**使用示例:**
```c
compositor_t comp;
compositor_init(&comp);

compositor_begin_frame(&comp);

// 绘制半透明矩形
uint32_t semi_transparent = 0x80FF0000;  // 50%透明度的红色
compositor_fill_rect_alpha(&comp, 100, 100, 200, 150, semi_transparent);

// 标记脏区域
compositor_damage_rect(&comp, 100, 100, 200, 150);

compositor_end_frame(&comp);
```

### 2. 窗口动画系统 (`src/gui/wm.c/h`)

#### 动画类型
- **WM_ANIM_MINIMIZE**: 最小化动画（淡出效果）
- **WM_ANIM_MAXIMIZE**: 最大化动画（平滑缩放）
- **WM_ANIM_RESTORE**: 还原动画（平滑过渡）

#### 窗口不透明度支持
- 每个窗口独立的不透明度属性 (0-255)
- 在最小化动画中使用淡出效果

#### 缓动函数
- 使用 ease-out-cubic 缓动函数，提供更自然的动画效果
- 公式: `eased = 1 - (1 - progress)³`

#### 动画API
```c
// 更新所有窗口的动画状态（每帧调用）
void wm_update_animations(wm_state_t *wm);

// 检查是否有活动动画
int wm_has_active_animations(wm_state_t *wm);
```

**集成示例:**
```c
// 在主渲染循环中
while (running) {
    // 更新动画
    wm_update_animations(&wm);
    
    // 渲染窗口
    render_windows(&wm);
    
    // 如果有活动动画，继续刷新
    if (wm_has_active_animations(&wm)) {
        continue_rendering = 1;
    }
}
```

### 3. 视觉效果库 (`src/gui/effects.c/h`)

#### 颜色处理函数

**color_blend()** - 混合两个颜色
```c
uint32_t result = color_blend(0xFFFF0000, 0xFF0000FF, 128);  // 50%混合
```

**color_darken()** - 使颜色变暗
```c
uint32_t darker = color_darken(0xFFFF0000, 128);  // 变暗50%
```

**color_lighten()** - 使颜色变亮
```c
uint32_t lighter = color_lighten(0xFF000080, 64);  // 变亮25%
```

#### 阴影效果

**draw_shadow()** - 绘制窗口阴影
- 在窗口右侧和底部绘制渐变阴影
- 可配置模糊半径和不透明度
- 距离越远，阴影越淡

```c
// 为窗口添加阴影
draw_shadow(buffer, pitch, screen_w, screen_h,
           win_x, win_y, win_w, win_h,
           8,      // 模糊半径
           64);    // 不透明度
```

#### 圆角矩形

**point_in_rounded_rect()** - 检查点是否在圆角矩形内
- 支持四个圆角的精确碰撞检测

**draw_rounded_rect()** - 绘制圆角矩形
- 自动限制圆角半径
- 支持Alpha通道

```c
// 绘制圆角窗口背景
draw_rounded_rect(buffer, pitch, screen_w, screen_h,
                 x, y, w, h,
                 12,          // 圆角半径
                 0xFFF0F0F0); // 颜色
```

#### 渐变效果

**draw_gradient_vertical()** - 垂直渐变
```c
// 标题栏渐变效果
draw_gradient_vertical(buffer, pitch, screen_w, screen_h,
                      title_x, title_y, title_w, title_h,
                      0xFF4080FF,  // 顶部颜色
                      0xFF2060DD); // 底部颜色
```

**draw_gradient_horizontal()** - 水平渐变
```c
// 按钮悬停效果
draw_gradient_horizontal(buffer, pitch, screen_w, screen_h,
                        btn_x, btn_y, btn_w, btn_h,
                        0xFFE0E0E0,  // 左侧颜色
                        0xFFC0C0C0); // 右侧颜色
```

## 构建说明

新的GUI效果模块已集成到构建系统中：

```bash
make clean
make
```

构建系统会自动编译：
- `src/gui/compositor.c`
- `src/gui/wm.c`
- `src/gui/effects.c` (新增)

## 性能考虑

### 优化建议

1. **脏区域管理**
   - 只标记实际改变的区域
   - 避免标记整个窗口为脏

2. **Alpha混合**
   - 尽可能使用完全不透明的颜色
   - 透明度为0或255时会使用快速路径

3. **动画**
   - 动画期间保持较高的刷新率
   - 动画完成后停止不必要的重绘

4. **阴影和圆角**
   - 阴影绘制开销较大，建议：
     - 仅为活动窗口绘制阴影
     - 使用较小的模糊半径（4-8像素）
   - 圆角矩形需要逐像素检查，建议：
     - 使用较小的圆角半径（4-12像素）
     - 缓存圆角窗口的形状

### 内存使用

- 合成器使用双缓冲，内存占用 = `屏幕宽度 × 屏幕高度 × 8字节`
- 1920×1080分辨率约需要16MB内存
- 视觉效果库不额外分配内存，直接在提供的缓冲区上绘制

## 未来改进方向

### 短期目标
1. **窗口阴影优化** - 使用预计算的阴影纹理
2. **硬件加速** - 利用GPU进行合成和特效
3. **更多动画** - 添加窗口打开/关闭动画

### 中期目标
1. **主题系统** - 支持自定义颜色方案和视觉风格
2. **特效管线** - 模糊、发光等高级特效
3. **性能分析工具** - 帧率显示和性能指标

### 长期目标
1. **Wayland协议支持** - 现代化的窗口系统协议
2. **多显示器支持** - 每个显示器独立的合成器
3. **3D窗口效果** - 透视变换和3D过渡效果

## 示例应用场景

### 现代化的窗口标题栏
```c
// 使用渐变背景
draw_gradient_vertical(buf, pitch, w, h,
                      title_x, title_y, title_w, 34,
                      is_active ? 0xFF4080FF : 0xFF808080,
                      is_active ? 0xFF2060DD : 0xFF606060);

// 圆角窗口
draw_rounded_rect(buf, pitch, w, h,
                 win_x, win_y, win_w, win_h,
                 8, 0xFFF0F0F0);

// 添加阴影
draw_shadow(buf, pitch, w, h,
           win_x, win_y, win_w, win_h, 6, 48);
```

### 半透明通知窗口
```c
// 半透明背景
uint32_t bg_color = 0xD0202020;  // 81%不透明度
compositor_fill_rect_alpha(&comp, x, y, w, h, bg_color);

// 使用动画淡入
win->opacity = (uint8_t)(255 * anim_progress);
```

## 兼容性

- 所有改进保持向后兼容
- 不使用新特性的代码继续正常工作
- 新API是可选的增强功能

## 测试建议

1. **基本功能测试**
   - 窗口最小化/最大化/还原
   - 多窗口切换
   - 窗口拖动和调整大小

2. **性能测试**
   - 同时打开多个窗口
   - 频繁触发动画
   - 监控帧率和CPU使用率

3. **视觉效果测试**
   - 阴影渲染正确性
   - 圆角边缘平滑度
   - 渐变色过渡自然度
   - Alpha混合正确性

## 贡献指南

欢迎对GUI系统的改进和扩展！建议的贡献方向：

1. 优化现有算法的性能
2. 添加新的视觉效果
3. 实现更多动画类型
4. 改进文档和示例代码
5. 报告和修复bug

## 许可证

本改进遵循HBOS项目的GPL-3.0许可证。
