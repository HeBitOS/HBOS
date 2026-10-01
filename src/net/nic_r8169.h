/**
 * @file nic_r8169.h
 * @brief Realtek RTL8168/8111/8169/8101/8125 千兆/2.5G 网卡驱动接口（纯轮询）
 *
 * 寄存器时序照抄 Linux `drivers/net/ethernet/realtek/r8169_main.c` /
 * `r8169_phy_config.c`，只保留"能收能发"必需的部分：不做中断、不做 MSI、
 * 不做 WOL/DASH/LED、不下载 PHY 固件（rtl_nic 下的 .fw 微码）。
 *
 * 调用模型与 `net.c` 里其它后端（e1000/rtl8139/pcnet/virtio）一致：
 * 上层先 `r8169_probe()` 认领 device id，再 `r8169_init_hw()` 完成硬件初始化，
 * 之后循环 `r8169_send()` / `r8169_poll()`，全程不需要中断服务路径。
 *
 * 注意：本驱动**不**处理链路状态变化后的重新协商；链路就绪只在
 * `r8169_init_hw()` 返回时报告一次。
 */

#ifndef HBOS_NIC_R8169_H
#define HBOS_NIC_R8169_H

#include "pci.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief 判断某个 PCI device id 是否属于 r8169 家族。
 *
 * 只按 `rtl8169_pci_tbl`（r8169_main.c:224-248）里登记的 device id 判定；
 * vendor 只在 0x10ec 与表里的贴牌厂商（0x1a03/0x1186/0x1113/0x16ec/0x1737/
 * 0x0001）之间放行，避免误认其它厂商的同号设备。
 *
 * 真实型号（8168b/c/d/e/f/g/h…）**不能**由 device id 区分——它们多是同一个
 * 0x8168，型号来自芯片的 XID 寄存器，见 `r8169_device_mac_version()`。
 *
 * @param vendor PCI 厂商 ID
 * @param device PCI 设备 ID
 * @return 1 支持，0 不支持
 */
int r8169_probe(uint16_t vendor, uint16_t device);

/**
 * @brief 驱动名（用于日志/启动横幅）。
 * @return 常量字符串 "Realtek RTL8168/8111"
 */
const char *r8169_driver_name(void);

/**
 * @brief 上次 `r8169_init_hw()` 在芯片 XID 寄存器里识别出的真实型号名。
 *
 * `r8169_driver_name()` 是固定的"驱动"名字；本函数给出"这块卡到底是什么"，
 * 例如 "RTL8168h/8111h"。`r8169_init_hw()` 之前返回 "unknown"。
 *
 * @return 常量字符串，静态存储期
 */
const char *r8169_device_name(void);

/**
 * @brief 上次识别出的 MAC 版本（`rtl_giga_mac_ver_t` 的取值）。
 *
 * 用于上层日志与"这块卡是否需要固件"的降级提示。`r8169_init_hw()` 之前
 * 返回 `RTL_GIGA_MAC_NONE`。
 *
 * @return MAC 版本枚举值
 */
int r8169_device_mac_version(void);

/**
 * @brief 探测并初始化网卡，打开收发。
 *
 * 步骤（照 r8169_main.c:5595-5817 + :4097-4130 的顺序裁剪）：
 *  1. 取 BAR（优先 MMIO，退化为 I/O 端口）并用 `vmm_map_mmio()` 映射；
 *     8125/8126/8127 用到 0x60B0，因此映射窗口按 ≥4 KiB 分块直到够用。
 *  2. 读 `TxConfig`(0x40) 自检（`== ~0U` 说明 BAR 不对/设备没使能），
 *     再由 `(TxConfig >> 20) & 0xfcf` 得到 XID 查表定 MAC 版本。
 *  3. `Cfg9346` 解锁 → 版本 `hw_initialize` → `ChipCmd` 软复位 → 版本
 *     `hw_start`（含 PHY 配置、magic 序列、MAC OCP/ERI 写）→ `Cfg9346` 回锁。
 *  4. 装好 RX/TX 描述符环（文件内静态、256 字节对齐、恒等映射地址）并把
 *     `TxDescStartAddr`/`RxDescAddr` 写进器件，开 `ChipCmd.TxEnb|RxEnb`。
 *  5. `IntrMask = 0` 且清 `IntrStatus`——纯轮询，绝不能留中断使能。
 *  6. PHY 配置（按版本）+ 自协商重启，随后有界轮询 `BMSR` 判链路。
 *
 * `link_ready` 是"返回时刻"的快照，不是承诺：千兆自协商典型要 1-3 s，
 * 本函数最多等 `R8169_LINK_WAIT_MAX_MS`，等不到就返回 false 而不是失败
 * ——设备本身已经可以收发，链路会在之后自行就绪。调用方若需要确认链路，
 * 用 `r8169_link_up()` 继续轮询。
 *
 * 失败时不改变调用方的任何状态：已映射的窗口与已申请的 I/O 窗口按获得顺序
 * 逆序释放，设备停在软复位后的停止态（`ChipCmd.TxEnb/RxEnb = 0`）。
 *
 * @param dev        由 net.c 找到的 PCI 设备，不可为 NULL
 * @param mac        out：6 字节 MAC 地址（`*mac_valid` 为真时有效）
 * @param mac_valid  out：是否成功从器件读到过 MAC
 * @param link_ready out：返回时刻链路是否已就绪（可为 NULL）
 * @return 0 成功，-1 失败
 */
int r8169_init_hw(const pci_device_t *dev, uint8_t mac[6],
                  bool *mac_valid, bool *link_ready);

/**
 * @brief 发送一个以太网帧。
 *
 * 帧被拷进文件内静态 TX 缓冲（不 kmalloc），随后填描述符、置 OWN、敲
 * `TxPoll` 门铃。等不到空闲描述符（TX 环满或硬件卡住）时在
 * `R8169_TX_SLOT_WAIT_MS` 内重试，超时返回 -1，调用方可重试。
 *
 * 长度会被补齐到 60 字节：8168evl（VER_34）与 8125 家族要求最小帧长，
 * 见 r8169_main.c:4445-4453 rtl_quirk_packet_padto()。
 *
 * @param frame 帧首地址（含以太网头，不含 FCS）
 * @param len   帧长（字节），0 或超过 `R8169_TX_BUF_SIZE` 直接失败
 * @return 0 已交给硬件，-1 失败
 */
int r8169_send(const void *frame, uint16_t len);

/**
 * @brief 排空 RX 环，对每个收到的帧调用一次 cb。
 *
 * 判据与 rtl_rx()（r8169_main.c:4764-4848）一致：描述符 `opts1.DescOwn == 0`
 * 表示硬件已写回一帧；`RxRES` 置位视为错误帧丢弃；长度取 `opts1 & 0x3fff`
 * 并减去 4 字节 FCS；分片帧（FS/LS 不齐）直接丢弃。处理完立刻把描述符连
 * `RingEnd` 位一起还给硬件。
 *
 * @param cb     帧回调；返回非 0 表示"上层要停下"，本函数随即返回该值
 * @param arg    透传给 cb
 * @param spins  最多处理的描述符个数（0 视为 1）
 * @return 0 无帧或不要求停止；>0 回调要求停止；-1 未初始化
 */
int r8169_poll(int (*cb)(const uint8_t *pkt, uint16_t len, void *arg),
               void *arg, uint32_t spins);

/**
 * @brief 现场读 MDIO `BMSR`(0x01) 判断链路是否已就绪。
 *
 * 不依赖 `r8169_init_hw()` 时那次快照，可在上层轮询循环里反复调用。
 * `BMSR.BMSR_LSTATUS` 是 latch-low 语义：读一帧"链路仍在"要读两次。
 * 链路寄存器只读，不改变任何器件状态，可安全地反复调用。
 *
 * @return 1 链路已就绪，0 未就绪或器件不可用
 */
int r8169_link_up(void);

#endif /* HBOS_NIC_R8169_H */
