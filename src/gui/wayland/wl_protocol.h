/**
 * @file    wl_protocol.h
 * @brief   Wayland 线协议常量：接口名、版本、opcode、事件号
 *
 * 单独抽出来是为了让"协议表"和"实现"分开：改协议版本只动这里，
 * 服务端逻辑不用跟着改。数值必须与 wayland.xml 完全一致 —— 客户端
 * 是按 opcode 无条件发请求的，错一位就是整条连接报废。
 */
#ifndef HBOS_WL_PROTOCOL_H
#define HBOS_WL_PROTOCOL_H

/* 内核是 -ffreestanding，没有 stddef.h 里的 NULL。 */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* ── 全局对象 ──────────────────────────────────────────────────
 *
 * wl_registry 里 name 是服务端分配的全局编号（不是对象 id）。
 * 客户端 bind(name, interface, version, new_id) 之后才真正创建对象。
 * 版本必须 <= 我们声明支持的上限，否则客户端会拿到它不认识的语义。 */

#define WL_COMPOSITOR_VERSION   4
#define WL_SHM_VERSION          1
#define WL_SEAT_VERSION         5
#define WL_XDG_WM_BASE_VERSION  2

/* ── wl_display（object id 固定为 1）──────────────────────────── */

#define WL_DISPLAY_SYNC                 0
#define WL_DISPLAY_GET_REGISTRY         1
#define WL_DISPLAY_EV_ERROR             0
#define WL_DISPLAY_E_DELETE_ID          1

/* ── wl_registry ─────────────────────────────────────────────── */

#define WL_REGISTRY_BIND                0
#define WL_REGISTRY_E_GLOBAL            0
#define WL_REGISTRY_E_GLOBAL_REMOVE     1

/* ── wl_callback ─────────────────────────────────────────────── */

#define WL_CALLBACK_E_DONE              0

/* ── wl_compositor ───────────────────────────────────────────── */

#define WL_COMPOSITOR_CREATE_SURFACE         0
#define WL_COMPOSITOR_CREATE_REGION          1

/* ── wl_surface ──────────────────────────────────────────────── */

#define WL_SURFACE_DESTROY              0
#define WL_SURFACE_ATTACH               1
#define WL_SURFACE_DAMAGE               2
#define WL_SURFACE_FRAME                3
#define WL_SURFACE_SET_OPAQUE_REGION    4
#define WL_SURFACE_SET_INPUT_REGION     5
#define WL_SURFACE_COMMIT               6
#define WL_SURFACE_SET_BUFFER_TRANSFORM 7
#define WL_SURFACE_SET_BUFFER_SCALE     8
#define WL_SURFACE_DAMAGE_BUFFER        9
#define WL_SURFACE_E_ENTER              0
#define WL_SURFACE_E_LEAVE              1

/* ── wl_shm ──────────────────────────────────────────────────── */

#define WL_SHM_CREATE_POOL              0
#define WL_SHM_E_FORMAT                 0

/* ── wl_shm_pool ─────────────────────────────────────────────── */

#define WL_SHM_POOL_CREATE_BUFFER       0
#define WL_SHM_POOL_DESTROY             1
#define WL_SHM_POOL_RESIZE              2

/* ── wl_shm 像素格式（DRM fourcc）───────────────────────────── */

#define WL_SHM_FORMAT_ARGB8888          0
#define WL_SHM_FORMAT_XRGB8888          1

/* ── wl_buffer ───────────────────────────────────────────────── */

#define WL_BUFFER_DESTROY               0
#define WL_BUFFER_E_RELEASE             0

/* ── wl_seat ─────────────────────────────────────────────────── */

#define WL_SEAT_GET_POINTER             0
#define WL_SEAT_GET_KEYBOARD            1
#define WL_SEAT_GET_TOUCH               2
#define WL_SEAT_RELEASE                 3
#define WL_SEAT_E_CAPABILITIES          0
#define WL_SEAT_E_NAME                  1

/* ── xdg_wm_base ─────────────────────────────────────────────── */

#define XDG_WM_BASE_DESTROY             0
#define XDG_WM_BASE_CREATE_POSITIONER   1
#define XDG_WM_BASE_GET_XDG_SURFACE     2
#define XDG_WM_BASE_PONG                3
#define XDG_WM_BASE_E_PING              0

/* ── xdg_surface ─────────────────────────────────────────────── */

#define XDG_SURFACE_DESTROY             0
#define XDG_SURFACE_GET_TOPLEVEL        1
#define XDG_SURFACE_GET_POPUP           2
#define XDG_SURFACE_SET_WINDOW_GEOMETRY 3
#define XDG_SURFACE_ACK_CONFIGURE       4
#define XDG_SURFACE_E_CONFIGURE         0

/* ── xdg_toplevel ────────────────────────────────────────────── */

#define XDG_TOPLEVEL_DESTROY             0
#define XDG_TOPLEVEL_SET_PARENT          1
#define XDG_TOPLEVEL_SET_TITLE           2
#define XDG_TOPLEVEL_SET_APP_ID          3
#define XDG_TOPLEVEL_SHOW_WINDOW_MENU    4
#define XDG_TOPLEVEL_MOVE                5
#define XDG_TOPLEVEL_RESIZE              6
#define XDG_TOPLEVEL_SET_MAX_SIZE        7
#define XDG_TOPLEVEL_SET_MIN_SIZE        8
#define XDG_TOPLEVEL_SET_MAXIMIZED       9
#define XDG_TOPLEVEL_UNSET_MAXIMIZED    10
#define XDG_TOPLEVEL_SET_FULLSCREEN     11
#define XDG_TOPLEVEL_UNSET_FULLSCREEN   12
#define XDG_TOPLEVEL_SET_MINIMIZED      13

#define XDG_TOPLEVEL_E_CONFIGURE            0
#define XDG_TOPLEVEL_E_CLOSE                1
#define XDG_TOPLEVEL_E_CONFIGURE_BOUNDS     2
#define XDG_TOPLEVEL_E_MAXIMIZED            3
#define XDG_TOPLEVEL_E_UNMAXIMIZED          4
#define XDG_TOPLEVEL_E_FULLSCREEN           5
#define XDG_TOPLEVEL_E_UNFULLSCREEN         6
#define XDG_TOPLEVEL_E_MINIMIZED            7

/* ── 接口名（客户端拿它和自己注册的 interface 做 strcmp）────── */

#define WL_IFACE_DISPLAY        "wl_display"
#define WL_IFACE_REGISTRY       "wl_registry"
#define WL_IFACE_CALLBACK       "wl_callback"
#define WL_IFACE_COMPOSITOR     "wl_compositor"
#define WL_IFACE_SURFACE        "wl_surface"
#define WL_IFACE_REGION         "wl_region"
#define WL_IFACE_SHM            "wl_shm"
#define WL_IFACE_SHM_POOL       "wl_shm_pool"
#define WL_IFACE_BUFFER         "wl_buffer"
#define WL_IFACE_SEAT           "wl_seat"
#define WL_IFACE_POINTER        "wl_pointer"
#define WL_IFACE_KEYBOARD       "wl_keyboard"
#define WL_IFACE_XDG_WM_BASE    "xdg_wm_base"
#define WL_IFACE_XDG_SURFACE    "xdg_surface"
#define WL_IFACE_XDG_TOPLEVEL   "xdg_toplevel"
#define WL_IFACE_OUTPUT         "wl_output"
#define WL_IFACE_DATA_DEVICE_MANAGER "wl_data_device_manager"
#define WL_IFACE_SUBCOMPOSITOR  "wl_subcompositor"

/* ── wl_data_device_manager ─────────────────────────────────────
 *
 * Chromium 用它做剪贴板/拖放，ozone-wayland 初始化时会 bind 它。
 * MVP 阶段只提供对象本身（bind 成功、创建 data_source/data_device），
 * 不实现实际的数据传输 —— 少了这个全局对象，Chromium 会在拿完
 * registry 之后停在原地，现象是"不报错、不退出、也不继续"。
 */
#define WL_DATA_DEVICE_MANAGER_VERSION 3
#define WL_OBJ_DATA_DEVICE_MANAGER_KIND 1
#define WL_DATA_DEVICE_MANAGER_CREATE_SOURCE 0
#define WL_DATA_DEVICE_MANAGER_GET_DEVICE  1
#define WL_SUBCOMPOSITOR_VERSION 1

/* ── wl_output ──────────────────────────────────────────────────
 *
 * 客户端靠它知道屏幕尺寸和刷新率；没有这个全局对象，Chromium 会在
 * 拿到 registry 之后停住 —— 它要等 output 的 mode 事件才能确定窗口
 * 该开多大，等不到就永远停在初始化里（现象是：连上了、发了 get_registry
 * 和 sync、收到全部事件，然后一个字节都不再发）。
 *
 * 事件号（wayland.xml wl_output）：
 *   0 geometry  1 mode  2 done  3 scale  4 name  5 description
 */
#define WL_OUTPUT_VERSION       3
#define WL_OUTPUT_E_GEOMETRY    0
#define WL_OUTPUT_E_MODE        1
#define WL_OUTPUT_E_DONE        2
#define WL_OUTPUT_E_SCALE       3

/* mode 事件的 flags 位 */
#define WL_OUTPUT_MODE_CURRENT  0x1
#define WL_OUTPUT_MODE_PREFERRED 0x2

/* geometry 的 subpixel / transform 取值 */
#define WL_OUTPUT_SUBPIXEL_UNKNOWN 0
#define WL_OUTPUT_TRANSFORM_NORMAL 0

/* ── wl_seat 能力位 ─────────────────────────────────────────── */

#define WL_SEAT_CAP_POINTER     1
#define WL_SEAT_CAP_KEYBOARD    2

#endif /* HBOS_WL_PROTOCOL_H */
