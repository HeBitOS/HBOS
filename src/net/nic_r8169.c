/**
 * @file nic_r8169.c
 * @brief Realtek RTL8168/8111/8169/8101/8125 千兆/2.5G 网卡驱动（纯轮询）
 *
 * 寄存器时序、magic 序列与 PHY 配置全部照抄 Linux 7.0.12
 * `drivers/net/ethernet/realtek/r8169_main.c`、`r8169.h`、`r8169_phy_config.c`，
 * 只做三件事的裁剪：去掉中断（改轮询）、去掉 phylib（PHY 读写自己实现）、
 * 去掉固件下载（rtl_nic 目录下的 PHY 微码 .fw 文件，裸机拿不到，见下文"未实测"）。
 *
 * 不变式 / 性能要点
 * -----------------
 * 1. **DMA 地址 = 虚拟地址**。HBOS 是恒等映射，所以描述符里的 64 位地址就是
 *    `(uint64_t)(uintptr_t)buf`，不需要 IOMMU/DMA 映射层。
 * 2. **全部缓冲是文件内静态 BSS**：TX/RX 环各 256 字节对齐，RX/TX 数据缓冲
 *    16 字节对齐。这样做的代价是"同一时刻只能有一个实例"，所以本驱动是
 *    **单实例、不可重入**的：`r8169_send()`/`r8169_poll()` 之间不持锁，由
 *    net.c 的调用序保证串行（与 e1000/rtl8139 后端同样的假设）。
 * 3. **描述符环是唯一的所有权凭据**。RTL 的描述符 `opts1.DescOwn` 是硬件/
 *    驱动之间的唯一握手位；写侧必须"先填 addr/opts2，再置 OWN"，读侧必须
 *    "先确认 OWN 已清，再读长度/addr"——两次跨越都必须有屏障，否则弱序
 *    CPU（或编译器重排）会把旧数据当作新包交出去。
 * 4. **一个环的尾描述符带 `RingEnd`**，硬件在它之后自动回绕到环首，因此
 *    驱动只需要 `idx % 环长`，不需要"半环/rx_offset"那套老机制
 *    （现代 r8169 里搜 `rx_offset`/`RxRead` 是零命中）。
 * 5. 只保留 1518 字节以内的标准帧：`RxMaxSize` 写 0x4000 关掉长度过滤，
 *    但每个 RX 缓冲只有 `R8169_RX_BUF_SIZE` 字节。
 */

#include "nic_r8169.h"

#include "pci.h"
#include "string.h"
#include "core/io.h"
#include "core/vmm.h"
#include "core/wait.h"

/* ------------------------------------------------------------------ *
 * 0. 规模常量
 * ------------------------------------------------------------------ */

/** 描述符环长度（Linux 用 256；轮询驱动每轮最多处理 64 个，16 足够）。
 *  取值必须是 2 的幂，`idx & (环长-1)` 才能代替取模。 */
#define R8169_TX_DESC_COUNT     16u
#define R8169_RX_DESC_COUNT     16u

/** 单个 RX/TX 数据缓冲大小。Linux 给每个 RX 描述符挂 16 KiB
 *  （R8169_RX_BUF_SIZE = SZ_16K-1），代价是 256×16 KiB；这里只支持
 *  1518 字节标准帧，2 KiB 足够且省 96% 内存。 */
#define R8169_TX_BUF_SIZE       2048u
#define R8169_RX_BUF_SIZE       2048u

/** 环尾长度字段写 16383（r8169_main.c:72 R8169_RX_BUF_SIZE = SZ_16K-1）。
 *  硬件用它与 RxMaxSize 比较来判超长，写大一点等于"不要按这个过滤"，
 *  真正的截断由 2 KiB 缓冲的物理边界保证——所以只支持标准帧。 */
#define R8169_RX_DESC_LEN       0x3FFFu

/** 以太网最小帧长（不含 FCS）：VER_34 与 8125 家族要求硬件侧帧长 >= 60。 */
#define R8169_ETH_ZLEN          60u

/** 门铃：普通芯片写 TxPoll(0x38) = NPQ(0x40)。
 *  8125 的 TxPoll 搬到 0x90 且位不同（写 1），见 rtl8169_doorbell()。 */
#define R8169_TXPOLL_NPQ        0x40u

/** 最长等一个空闲 TX 描述符的时间：1 ms 量级，够硬件回收一两帧。 */
#define R8169_TX_SLOT_WAIT_MS   2u

/** `r8169_init_hw()` 里等链路就绪的上限。千兆自协商典型 1-3 s；这里只做
 *  "返回时刻的快照"，等不到不算失败（器件已可收发），故取 400 ms。 */
#define R8169_LINK_WAIT_MAX_MS  400u

/* ------------------------------------------------------------------ *
 * 1. MAC 版本枚举
 * ------------------------------------------------------------------ */

/**
 * @brief 芯片 MAC 版本，取值与 Linux `enum mac_version`（r8169.h:15-77）
 *        **逐项一致**，便于与源码对照。
 *
 * 关键：这个枚举**不连续**（VER_01/11/12/13/16/27/41/45/47/49/50/60 已被
 * Linux 删除），所以任何语义判断都必须写成下面的 `rtl_is_*` 谓词或显式
 * `switch`，不要凭空用数值大小推断。
 */
typedef enum {
    RTL_GIGA_MAC_VER_02 = 0,  /**< RTL8169s */
    RTL_GIGA_MAC_VER_03,      /**< RTL8110s */
    RTL_GIGA_MAC_VER_04,      /**< RTL8169sb/8110sb */
    RTL_GIGA_MAC_VER_05,      /**< RTL8169sc/8110sc */
    RTL_GIGA_MAC_VER_06,      /**< RTL8169sc/8110sc */
    RTL_GIGA_MAC_VER_07,      /**< RTL8102e */
    RTL_GIGA_MAC_VER_08,      /**< RTL8102e */
    RTL_GIGA_MAC_VER_09,      /**< RTL8102e/RTL8103e */
    RTL_GIGA_MAC_VER_10,      /**< RTL8101e/RTL8100e */
    RTL_GIGA_MAC_VER_14,      /**< RTL8401 */
    RTL_GIGA_MAC_VER_17,      /**< RTL8168b/8111b */
    RTL_GIGA_MAC_VER_18,      /**< RTL8168cp/8111cp */
    RTL_GIGA_MAC_VER_19,      /**< RTL8168c/8111c */
    RTL_GIGA_MAC_VER_20,      /**< RTL8168c/8111c */
    RTL_GIGA_MAC_VER_21,      /**< RTL8168c/8111c */
    RTL_GIGA_MAC_VER_22,      /**< RTL8168c/8111c */
    RTL_GIGA_MAC_VER_23,      /**< RTL8168cp/8111cp */
    RTL_GIGA_MAC_VER_24,      /**< RTL8168cp/8111cp */
    RTL_GIGA_MAC_VER_25,      /**< RTL8168d/8111d */
    RTL_GIGA_MAC_VER_26,      /**< RTL8168d/8111d */
    RTL_GIGA_MAC_VER_28,      /**< RTL8168dp/8111dp */
    RTL_GIGA_MAC_VER_29,      /**< RTL8105e */
    RTL_GIGA_MAC_VER_30,      /**< RTL8105e */
    RTL_GIGA_MAC_VER_31,      /**< RTL8168dp/8111dp */
    RTL_GIGA_MAC_VER_32,      /**< RTL8168e/8111e */
    RTL_GIGA_MAC_VER_33,      /**< RTL8168e/8111e */
    RTL_GIGA_MAC_VER_34,      /**< RTL8168evl/8111evl */
    RTL_GIGA_MAC_VER_35,      /**< RTL8168f/8111f */
    RTL_GIGA_MAC_VER_36,      /**< RTL8168f/8111f */
    RTL_GIGA_MAC_VER_37,      /**< RTL8402 */
    RTL_GIGA_MAC_VER_38,      /**< RTL8411 */
    RTL_GIGA_MAC_VER_39,      /**< RTL8106e */
    RTL_GIGA_MAC_VER_40,      /**< RTL8168g/8111g */
    RTL_GIGA_MAC_VER_42,      /**< RTL8168gu/8111gu */
    RTL_GIGA_MAC_VER_43,      /**< RTL8106eus */
    RTL_GIGA_MAC_VER_44,      /**< RTL8411b */
    RTL_GIGA_MAC_VER_46,      /**< RTL8168h/8111h */
    RTL_GIGA_MAC_VER_48,      /**< RTL8107e */
    RTL_GIGA_MAC_VER_51,      /**< RTL8168ep/8111ep */
    RTL_GIGA_MAC_VER_52,      /**< RTL8168fp/RTL8117 */
    RTL_GIGA_MAC_VER_61,      /**< RTL8125A */
    RTL_GIGA_MAC_VER_63,      /**< RTL8125B */
    RTL_GIGA_MAC_VER_64,      /**< RTL8125D/8125K/9151A */
    RTL_GIGA_MAC_VER_66,      /**< RTL8125BP */
    RTL_GIGA_MAC_VER_70,      /**< RTL8126A */
    RTL_GIGA_MAC_VER_80,      /**< RTL8127A */
    RTL_GIGA_MAC_NONE = 0xFF  /**< 未知芯片 */
} rtl_giga_mac_ver_t;

/* ------------------------------------------------------------------ *
 * 2. 寄存器偏移（照抄 enum rtl_registers 等，r8169_main.c:252-443）
 * ------------------------------------------------------------------ */

/* 通用寄存器 */
#define R8169_MAC0              0x00u   /**< MAC 地址字节 0-3 */
#define R8169_MAC4              0x04u   /**< MAC 地址字节 4-5（低 16 位有效） */
#define R8169_MAR0              0x08u   /**< 组播哈希过滤 */
#define R8169_TX_DESC_LO        0x20u   /**< TxDescStartAddrLow */
#define R8169_TX_DESC_HI        0x24u   /**< TxDescStartAddrHigh */
#define R8169_CHIP_CMD          0x37u   /**< ChipCmd */
#define R8169_TX_POLL           0x38u   /**< TxPoll（<VER_61） */
#define R8169_INTR_MASK         0x3Cu   /**< IntrMask（16 位，<VER_61） */
#define R8169_INTR_STATUS       0x3Eu   /**< IntrStatus（16 位，写 1 清） */
#define R8169_TX_CONFIG         0x40u   /**< TxConfig；bit20-31 是 XID 来源 */
#define R8169_RX_CONFIG         0x44u   /**< RxConfig */
#define R8169_CFG9346           0x50u   /**< 配置寄存器锁：0xC0 解锁 / 0x00 回锁 */
#define R8169_CONFIG1           0x51u
#define R8169_CONFIG2           0x52u
#define R8169_CONFIG3           0x53u
#define R8169_CONFIG4           0x54u
#define R8169_CONFIG5           0x56u
#define R8169_PHYAR             0x60u   /**< 老式 PHY(MDIO) 访问（<VER_40） */
#define R8169_CSIDR             0x64u
#define R8169_CSIAR             0x68u
#define R8169_PHYSTATUS         0x6Cu   /**< 现代驱动不读，仅作兜底参考 */
#define R8169_ERIDR             0x70u   /**< ERI 数据端口 */
#define R8169_ERIAR             0x74u   /**< ERI 地址/命令端口 */
#define R8169_EPHYAR            0x80u   /**< 内部 EPHY 访问 */
#define R8169_GPHY_OCP          0xB8u   /**< GPHY OCP（>=VER_40 的 PHY 通道） */
#define R8169_EFUSEAR           0xDCu   /**< eFuse 读 */
#define R8169_DLLPR             0xD0u   /**< 与 RDSAR1 同偏移；PFM_EN / TX_10M_PS_EN */
#define R8169_DBG_REG           0xD1u   /**< FIX_NAK_1/FIX_NAK_2 */
#define R8169_MCU               0xD3u   /**< NOW_IS_OOB / EN_NDP / LINK_LIST_RDY */
#define R8169_RX_MAX_SIZE       0xDAu   /**< RxMaxSize（16 位） */
#define R8169_CPLUS_CMD         0xE0u   /**< CPlusCmd（16 位） */
#define R8169_INTR_MITIGATE     0xE2u   /**< 中断合并（老文档 MultiIntr），一律写 0 */
#define R8169_RX_DESC_LO        0xE4u   /**< RxDescAddrLow（= RDSAR） */
#define R8169_RX_DESC_HI        0xE8u   /**< RxDescAddrHigh */
#define R8169_EARLY_TX_THRES    0xECu   /**< 与 MaxTxPacketSize 同偏移 */
#define R8169_MISC              0xF0u   /**< 与 FuncEvent 同偏移 */

/* 8125 家族特有条目（enum rtl8125_registers，r8169_main.c:427-443）。
 * 注意 0x3C 在 8125 上是**状态**而不是屏蔽寄存器，0x38 才是屏蔽——这是
 * 本驱动里最容易写错的一处，见 rtl_get_events()/rtl_irq_disable()。 */
#define R8169_8125_INT_CFG0     0x34u
#define R8169_8125_INTR_MASK    0x38u   /**< 32 位 */
#define R8169_8125_INTR_STATUS  0x3Cu   /**< 32 位 */
#define R8169_8125_INT_CFG1     0x7Au
#define R8169_8125_TX_POLL      0x90u   /**< 16 位；写 BIT(0) 触发 */
#define R8169_8125_MAC0_BKP     0x19E0u /**< MAC 地址备份 */
#define R8169_8125_RSS_CTRL     0x4500u
#define R8169_8125_Q_NUM_CTRL   0x4800u
#define R8169_8125_EEE_TXIDLE   0x6048u
#define R8169_TX_CONFIG_V2      0x60B0u /**< 扩展 XID 寄存器 */

/* 位定义（enum rtl_register_content / rtl_desc_bit） */
#define R8169_CHIPCMD_RESET     0x10u
#define R8169_CHIPCMD_RX_ENB    0x08u
#define R8169_CHIPCMD_TX_ENB    0x04u

#define R8169_CFG9346_LOCK      0x00u
#define R8169_CFG9346_UNLOCK    0xC0u

#define R8169_DESC_OWN          0x80000000u /**< 硬件拥有该描述符 */
#define R8169_DESC_RING_END     0x40000000u /**< 环尾（硬件在它之后回绕） */

#define R8169_RX_DESC_RXRES     0x00200000u /**< 接收错误总标志 */
#define R8169_RX_DESC_RXRWT     0x00400000u /**< 超长帧 */
#define R8169_RX_DESC_RXRUNT    0x00100000u /**< 短帧 */
#define R8169_RX_DESC_RXCRC     0x00080000u /**< CRC 错 */
#define R8169_RX_DESC_RXRES_LEN 0x00003FFFu /**< 长度字段 */

#define R8169_MISC_RXDV_GATED   0x00080000u /**< RXDV_GATED_EN */
#define R8169_MISC_TXPLA_RST    0x20000000u /**< TXPLA_RST */
#define R8169_MISC_PWM_EN       0x00400000u
#define R8169_MISC_DISABLE_LAN  0x00800000u
#define R8169_MISC_EARLY_TALLY  0x00010000u

#define R8169_MCU_NOW_IS_OOB    0x80u
#define R8169_MCU_EN_NDP        0x08u
#define R8169_MCU_EN_OOB_RESET  0x04u
#define R8169_MCU_LINK_LIST_RDY 0x02u
#define R8169_MCU_RXTX_EMPTY    0x30u   /**< TX_EMPTY|RX_EMPTY */

#define R8169_DLLPR_PFM_EN      0x40u
#define R8169_DLLPR_TX_10M_PS   0x80u
#define R8169_DLLPR_MASK        0xFFu

#define R8169_DBG_FIX_NAK_1     0x10u
#define R8169_DBG_FIX_NAK_2     0x08u

#define R8169_CONFIG1_SPEED_DOWN 0x10u
#define R8169_CONFIG1_LEDS0      0x40u
#define R8169_CONFIG1_LEDS1      0x80u
#define R8169_CONFIG1_MEMMAP     0x08u
#define R8169_CONFIG1_IOMAP      0x04u
#define R8169_CONFIG1_VPD        0x02u
#define R8169_CONFIG1_PMENABLE   0x01u
#define R8169_CONFIG1_CLKREQ_MASK 0x10u
#define R8169_CONFIG3_BEACON_EN  0x01u
#define R8169_CONFIG3_RDY_TO_L23 0x02u
#define R8169_CONFIG5_SPI_EN     0x08u

#define R8169_CPLUS_PCIMULRW     0x0008u
#define R8169_CPLUS_ENANAPLL     0x4000u
#define R8169_CPLUS_CPCMD_MASK   0x2063u /**< Normal_mode|RxVlan|RxChkSum|INTT_MASK */

/* RxConfig 组成（rtl_init_rxcfg() r8169_main.c:2589-2615） */
#define R8169_RXCFG_FIFO_THRESH  (7u << 13)
#define R8169_RXCFG_128_INT_EN   (1u << 15)
#define R8169_RXCFG_MULTI_EN     (1u << 14)
#define R8169_RXCFG_EARLY_OFF    (1u << 11)
#define R8169_RXCFG_PAUSE_SLOT   (1u << 11) /**< 8125b 起复用该位 */
#define R8169_RXCFG_DMA_BURST    (7u << 8)
#define R8169_RXCFG_FETCH_8125   (8u << 27)

/** 接收过滤位低 6 位：广播 + 组播 + 本机地址（常见 STATION 模式）。 */
#define R8169_RXCFG_ACCEPT_MODE  0x0Eu
#define R8169_RXCFG_ACCEPT_MASK  0x3Fu

/* ERI 访问（_rtl_eri_write / _rtl_eri_read，r8169_main.c:1047-1077） */
#define R8169_ERIAR_FLAG        0x80000000u
#define R8169_EPHYAR_FLAG       0x80000000u  /**< EPHYAR_FLAG：忙位 / 写命令位 */
#define R8169_ERIAR_WRITE       R8169_ERIAR_FLAG
#define R8169_ERIAR_MASK_0001   0x00001000u
#define R8169_ERIAR_MASK_0011   0x00002000u
#define R8169_ERIAR_MASK_1111   0x0000F000u

/* MAC OCP（OCPDR 0xB0，r8169_main.c:1136-1185）—— 无忙等，写完即返回 */
#define R8169_OCPDR             0xB0u   /**< OCP 数据端口（Linux 枚举名 OCPDR） */
#define R8169_OCP_FLAG          0x80000000u  /**< OCPAR_FLAG：发起一次 OCP 访问 */

/* GPHY OCP（GPHY_OCP 0xB8，r8169_main.c:1111-1134） */
#define R8169_GPHY_OCP_FLAG     0x80000000u
/** @brief 8125 家族的 EEE TX-IDLE 定时器（Linux EEE_TXIDLE_TIMER_8125，r8169_main.c:442） */
#define R8169_8125_TXIDLE_TIMER 0x6048u
#define R8169_OCP_STD_PHY_BASE  0xA400u  /**< 标准 MII 寄存器所在的 OCP 基址 */

/* eFuse（rtl8168d_efuse_read，r8169_main.c:1624-1630） */
#define R8169_EFUSEAR_FLAG      0x80000000u

/* MII（include/uapi/linux/mii.h） */
#define R8169_MII_BMCR          0x00u
#define R8169_MII_BMSR          0x01u
#define R8169_MII_PHYSID2       0x03u
#define R8169_MII_ANAR          0x04u
#define R8169_MII_ANLPAR        0x05u
#define R8169_MII_CTL1000       0x09u
#define R8169_MII_STAT1000      0x0Au
#define R8169_MII_PAGE_SEL      0x1Fu

#define R8169_BMCR_RESET        0x8000u
#define R8169_BMCR_ANENABLE     0x1000u
#define R8169_BMCR_ANRESTART    0x0200u
#define R8169_BMSR_LSTATUS      0x0004u
#define R8169_BMSR_ANEGCOMPLETE 0x0020u
#define R8169_ADVERTISE_CSMA    0x0001u
#define R8169_ADVERTISE_10HALF  0x0020u
#define R8169_ADVERTISE_10FULL  0x0040u
#define R8169_ADVERTISE_100HALF 0x0080u
#define R8169_ADVERTISE_100FULL 0x0100u
#define R8169_ADVERTISE_PAUSE   0x0400u
#define R8169_ADVERTISE_1000HALF 0x0100u
#define R8169_ADVERTISE_1000FULL 0x0200u

/* ------------------------------------------------------------------ *
 * 3. 静态状态与寄存器访问
 * ------------------------------------------------------------------ */

/** 描述符：16 字节，{opts1, opts2, addr64}。**全系列（含 8125）同一种**：
 *  r8169 用 `struct TxDesc/RxDesc`（r8169_main.c:646-656）都是这个布局；
 *  8125 硬件支持的"新描述符格式"被驱动显式关掉（MAC OCP 0xeb58 bit0=0，
 *  见 rtl_hw_start_8125_common()）。32 字节描述符属于 rtase(RTL906x) 世代。 */
typedef struct {
    volatile uint32_t opts1;
    volatile uint32_t opts2;
    volatile uint64_t addr;
} rtl_desc_t;

/** C99 没有标准的 _Static_assert（C11 才有），这里做一个等价检查。 */
#define R8169_STATIC_ASSERT(cond, name) \
    typedef char r8169_static_assert_##name[(cond) ? 1 : -1]

R8169_STATIC_ASSERT(sizeof(rtl_desc_t) == 16, desc_is_16_bytes);
R8169_STATIC_ASSERT(sizeof(((rtl_desc_t *)0)->addr) == 8, addr_is_64_bit);

/** 访问通道种类。三种 PHY 通道不能混用，靠这个枚举分派。 */
typedef enum {
    R8169_ACCESS_MDIO = 0,  /**< PHYAR 0x60（<VER_40，28/31 另加 0xD0 bit17） */
    R8169_ACCESS_GPHY_OCP,  /**< GPHY_OCP 0xB8 + "页"模拟（>=VER_40，含 8125） */
    R8169_ACCESS_MMIO_IO    /**< 纯 MMIO / 纯 I/O 端口，通道本身无关 */
} rtl_access_t;

static rtl_giga_mac_ver_t g_mac_version = RTL_GIGA_MAC_NONE;
static const char *g_device_name = "unknown";
static bool g_ready;                /**< 收发环是否已装载 */
static int g_supports_gmii = 1;     /**< 由 device id 0x8136 决定（唯一带
                                         driver_data 的条目，r8169_main.c:5614） */
static rtl_access_t g_phy_access = R8169_ACCESS_MDIO;
static uint32_t g_ocp_base = R8169_OCP_STD_PHY_BASE;

/** 寄存器窗口。二选一：MMIO 窗口或 I/O 端口基址。 */
static volatile uint8_t *g_mmio;
static uint16_t g_io_base;
static bool g_is_io;
/** `vmm_map_mmio()` 返回的窗口起点与页数：按页记录才能在失败路径上逆序释放。 */
static void *g_mapped_pages[64];
static uint32_t g_mapped_count;
static uint32_t g_mapped_bytes;

static uint32_t g_cur_tx;
static uint32_t g_dirty_tx;
static uint32_t g_cur_rx;

/** 收发环与数据缓冲：文件内静态、对齐到硬件要求。256 字节对齐来自
 *  Linux 的 dma_alloc_coherent 注释（r8169_main.c:5083-5086）。 */
static rtl_desc_t g_tx_ring[R8169_TX_DESC_COUNT] __attribute__((aligned(256)));
static rtl_desc_t g_rx_ring[R8169_RX_DESC_COUNT] __attribute__((aligned(256)));
static uint8_t g_tx_buf[R8169_TX_DESC_COUNT][R8169_TX_BUF_SIZE] __attribute__((aligned(16)));
static uint8_t g_rx_buf[R8169_RX_DESC_COUNT][R8169_RX_BUF_SIZE] __attribute__((aligned(16)));

/** 编译期确认对齐没被排版破坏。 */
R8169_STATIC_ASSERT(sizeof(g_tx_ring) == R8169_TX_DESC_COUNT * 16, tx_ring_bytes);
R8169_STATIC_ASSERT(sizeof(g_rx_ring) == R8169_RX_DESC_COUNT * 16, rx_ring_bytes);

/** 读写屏障：描述符归属权移交前后各一次。 */
static inline void rtl_wmb(void) { __asm__ volatile("sfence" ::: "memory"); }
static inline void rtl_rmb(void) { __asm__ volatile("lfence" ::: "memory"); }

static inline uint8_t rtl_r8(uint16_t reg) {
    if (g_is_io) return io_in8((uint16_t)(g_io_base + reg));
    return *(volatile uint8_t *)(g_mmio + reg);
}
static inline void rtl_w8(uint16_t reg, uint8_t val) {
    if (g_is_io) { io_out8((uint16_t)(g_io_base + reg), val); return; }
    *(volatile uint8_t *)(g_mmio + reg) = val;
}
static inline uint16_t rtl_r16(uint16_t reg) {
    if (g_is_io) return io_in16((uint16_t)(g_io_base + reg));
    return *(volatile uint16_t *)(g_mmio + reg);
}
static inline void rtl_w16(uint16_t reg, uint16_t val) {
    if (g_is_io) { io_out16((uint16_t)(g_io_base + reg), val); return; }
    *(volatile uint16_t *)(g_mmio + reg) = val;
}
static inline uint32_t rtl_r32(uint32_t reg) {
    if (g_is_io) return io_in32((uint16_t)(g_io_base + (uint16_t)reg));
    return *(volatile uint32_t *)(g_mmio + reg);
}
static inline void rtl_w32(uint32_t reg, uint32_t val) {
    if (g_is_io) { io_out32((uint16_t)(g_io_base + (uint16_t)reg), val); return; }
    *(volatile uint32_t *)(g_mmio + reg) = val;
}

/** 读一次 ChipCmd 把前一条 PCI 写提交下去（rtl_pci_commit()，:825-829）。 */
static inline void rtl_pci_commit(void) { (void)rtl_r8(R8169_CHIP_CMD); }

/** @brief 微秒级短延时：@p us 次 I/O 写，在 1 GHz 以上的 PCIe 平台上
 *         单次约 1 us（Linux 在早期启动阶段也是这么凑的）。 */
static void rtl_delay_us(uint32_t us) {
    while (us--) io_delay();
}

/**
 * @brief 毫秒级延时，双截止：PIT tick + 自旋上限。
 *
 * PIT 尚未开始递增时（极早期启动）`pit_get_ticks()` 恒为常数，只靠 tick
 * 判定会立即"超时"或永不超时。所以先看 tick 是否在走，不在走就退化到
 * "自旋够多次数就返回"，宁可少等也不能挂死。
 *
 * @param ms 期望的毫秒数（0 视为 1）
 */
static void rtl_delay_ms(uint32_t ms) {
    if (!ms) ms = 1;
    uint32_t hz = pit_get_frequency_hz();
    if (hz) {
        uint64_t start = pit_get_ticks();
        uint64_t need = pit_ticks_from_ms(ms);
        if (!need) need = 1;
        while (pit_get_ticks() - start < need) cpu_relax();
        return;
    }
    /* 没有 PIT：按 1 ms 约 4000 次 pause 的保守估计自旋。 */
    uint32_t spins = ms * 4000u;
    while (spins--) cpu_relax();
}

/**
 * @brief 有界等待寄存器满足条件（对应 rtl_loop_wait()，r8169_main.c:893-908）。
 *
 * @param reg     寄存器偏移
 * @param mask    参与比较的位
 * @param want_high 期望的极性：真=等这些位变 1，假=等它们变 0
 * @param rounds  最多轮次
 * @param delay_us 每轮之间的延时（微秒）
 * @return 1 条件达成，0 超时
 */
static int rtl_wait_bits(uint32_t reg, uint32_t mask, int want_high,
                         uint32_t rounds, uint32_t delay_us) {
    while (rounds--) {
        int high = (rtl_r32(reg) & mask) != 0;
        if (high == (want_high != 0)) return 1;
        if (delay_us) rtl_delay_us(delay_us);
    }
    return 0;
}

/**
 * @brief 有界等待一个 8 位寄存器满足条件。
 *
 * @param reg      寄存器偏移
 * @param mask     参与比较的位
 * @param want_high 期望极性
 * @param rounds   最多轮次
 * @param delay_us 每轮延时（微秒）
 * @return 1 条件达成，0 超时
 */
static int rtl_wait_bits8(uint16_t reg, uint8_t mask, int want_high,
                          uint32_t rounds, uint32_t delay_us) {
    while (rounds--) {
        int high = (rtl_r8(reg) & mask) != 0;
        if (high == (want_high != 0)) return 1;
        if (delay_us) rtl_delay_us(delay_us);
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * 4. 版本谓词（照抄 r8169_main.c:861-878 / :4530-4542）
 * ------------------------------------------------------------------ */

static int rtl_is_8125(void) { return g_mac_version >= RTL_GIGA_MAC_VER_61; }

/**
 * @brief 是否属于"8168e-vl 及以后、8125 之前"这一代。
 * 区间取法与 Linux 一致，且**显式排除** VER_39（RTL8106e 不算这一代）。
 */
static int rtl_is_8168evl_up(void) {
    return g_mac_version >= RTL_GIGA_MAC_VER_34 &&
           g_mac_version != RTL_GIGA_MAC_VER_39 &&
           g_mac_version <= RTL_GIGA_MAC_VER_52;
}

/* ------------------------------------------------------------------ *
 * 5. 间接访问通道
 * ------------------------------------------------------------------ */

/**
 * @brief MAC OCP 写（`__r8168_mac_ocp_write`，r8169_main.c:1136-1142）。
 *
 * 硬件没有忙位可等：写完立即可以发下一条，所以这里不加轮询。
 *
 * @param reg  OCP 地址（必须 < 0x10000 且偶数）
 * @param data 16 位数据
 */
static void rtl_mac_ocp_write(uint32_t reg, uint16_t data) {
    rtl_w32(R8169_OCPDR, R8169_OCP_FLAG | (reg << 15) | data);
}

/**
 * @brief MAC OCP 读（`__r8168_mac_ocp_read`，r8169_main.c:1153-1161）。
 * @param reg OCP 地址
 * @return 16 位数据
 */
static uint16_t rtl_mac_ocp_read(uint32_t reg) {
    rtl_w32(R8169_OCPDR, reg << 15);
    return (uint16_t)(rtl_r32(R8169_OCPDR) & 0xFFFFu);
}

/**
 * @brief MAC OCP 读改写（`r8168_mac_ocp_modify`，r8169_main.c:1175-1185）。
 * @param reg  OCP 地址
 * @param mask 要清零的位
 * @param set  要置上的位
 */
static void rtl_mac_ocp_modify(uint32_t reg, uint16_t mask, uint16_t set) {
    uint16_t data = rtl_mac_ocp_read(reg);
    rtl_mac_ocp_write(reg, (uint16_t)((data & ~mask) | set));
}

/**
 * @brief ERI（扩展寄存器间接）写（`_rtl_eri_write`，r8169_main.c:1047-1060）。
 *
 * ERI 有忙位：写完命令后必须等到 bit31 自清（100 us × 100 = 10 ms 上限）。
 *
 * @param addr 4 字节对齐的 ERI 地址
 * @param mask 字节使能掩码（R8169_ERIAR_MASK_*）
 * @param val  写入值
 * @return 1 完成，0 超时（调用方一般不因超时中止）
 */
static int rtl_eri_write(uint32_t addr, uint32_t mask, uint32_t val) {
    rtl_w32(R8169_ERIDR, val);
    rtl_w32(R8169_ERIAR, R8169_ERIAR_WRITE | mask | addr);
    return rtl_wait_bits(R8169_ERIAR, R8169_ERIAR_FLAG, 0, 100, 100);
}

/**
 * @brief ERI 读（`_rtl_eri_read`，r8169_main.c:1068-1077）。
 * @param addr 4 字节对齐的 ERI 地址
 * @return 读到的 32 位值；超时返回 ~0
 */
static uint32_t rtl_eri_read(uint32_t addr) {
    rtl_w32(R8169_ERIAR, R8169_ERIAR_MASK_1111 | addr);
    if (!rtl_wait_bits(R8169_ERIAR, R8169_ERIAR_FLAG, 1, 100, 100)) return ~0u;
    return rtl_r32(R8169_ERIDR);
}

/** @brief ERI 读改写（`rtl_w0w1_eri`）：先清 @p mask 再置 @p set。 */
static void rtl_eri_modify(uint32_t addr, uint32_t set, uint32_t mask) {
    uint32_t val = rtl_eri_read(addr);
    (void)rtl_eri_write(addr, R8169_ERIAR_MASK_1111, (val & ~mask) | set);
}

/** @brief ERI 置位（`rtl_eri_set_bits`）。 */
static void rtl_eri_set_bits(uint32_t addr, uint32_t bits) {
    rtl_eri_modify(addr, bits, 0);
}

/** @brief ERI 清位（`rtl_eri_clear_bits`）。 */
static void rtl_eri_clear_bits(uint32_t addr, uint32_t bits) {
    rtl_eri_modify(addr, 0, bits);
}

/**
 * @brief GPHY OCP 写（`r8168_phy_ocp_write`，r8169_main.c:1111-1119）。
 * @param reg  OCP 地址（PHY 寄存器经页映射后的地址）
 * @param data 16 位数据
 */
static void rtl_gphy_ocp_write(uint32_t reg, uint16_t data) {
    rtl_w32(R8169_GPHY_OCP, R8169_GPHY_OCP_FLAG | (reg << 15) | data);
    (void)rtl_wait_bits(R8169_GPHY_OCP, R8169_GPHY_OCP_FLAG, 0, 10, 25);
}

/**
 * @brief GPHY OCP 读（`r8168_phy_ocp_read`，r8169_main.c:1121-1134）。
 * @param reg OCP 地址
 * @return 16 位数据；超时返回 0
 */
static uint16_t rtl_gphy_ocp_read(uint32_t reg) {
    rtl_w32(R8169_GPHY_OCP, reg << 15);
    if (!rtl_wait_bits(R8169_GPHY_OCP, R8169_GPHY_OCP_FLAG, 1, 10, 25)) return 0;
    return (uint16_t)(rtl_r32(R8169_GPHY_OCP) & 0xFFFFu);
}

/**
 * @brief 内部 EPHY 读（`rtl_ephy_read`，r8169_main.c:1408-1414）。
 * @param reg_addr EPHY 寄存器号（5 位）
 * @return 16 位数据；超时返回 ~0
 */
static uint16_t rtl_ephy_read(uint8_t reg_addr) {
    rtl_w32(R8169_EPHYAR, (uint32_t)(reg_addr & 0x1Fu) << 16);
    if (!rtl_wait_bits(R8169_EPHYAR, R8169_EPHYAR_FLAG, 1, 100, 10)) return 0xFFFFu;
    return (uint16_t)(rtl_r32(R8169_EPHYAR) & 0xFFFFu);
}

/**
 * @brief 内部 EPHY 写（`rtl_ephy_write`，r8169_main.c:1398-1406）。
 * @param reg_addr EPHY 寄存器号（5 位）
 * @param value    16 位数据
 */
static void rtl_ephy_write(uint8_t reg_addr, uint16_t value) {
    rtl_w32(R8169_EPHYAR, R8169_EPHYAR_FLAG | (uint32_t)value |
                          ((uint32_t)(reg_addr & 0x1Fu) << 16));
    (void)rtl_wait_bits(R8169_EPHYAR, R8169_EPHYAR_FLAG, 0, 100, 10);
    rtl_delay_us(10);
}

/* ------------------------------------------------------------------ *
 * 6. PHY（MDIO）读写 —— 三条通道
 * ------------------------------------------------------------------ */

/** 8168dp（VER_28/31）用 0xD0 的 bit17 作为 MDIO 访问使能。 */
#define R8169_DP_MDIO_ACCESS   0x00020000u
/** 该通道下 MII_PHYSID2 必须返回固定值，规避芯片 bug（r8169_main.c:1350-1352）。 */
#define R8169_DP_PHYSID2_FAKE  0xC912u

/**
 * @brief 老式 PHY 访问（`r8169_mdio_write`，r8169_main.c:1291-1301）。
 * @param reg   MII 寄存器号（5 位）
 * @param value 16 位数据
 */
static void rtl_mdio_write_direct(uint8_t reg, uint16_t value) {
    rtl_w32(R8169_PHYAR, 0x80000000u | ((uint32_t)(reg & 0x1Fu) << 16) |
                          (uint32_t)value);
    (void)rtl_wait_bits(R8169_PHYAR, 0x80000000u, 0, 20, 25);
    /* 硬件规范要求写完成指示之后、下一条命令之前再等 20 us。 */
    rtl_delay_us(20);
}

/**
 * @brief 老式 PHY 读（`r8169_mdio_read`，r8169_main.c:1303-1319）。
 * @param reg MII 寄存器号（5 位）
 * @return 16 位数据；超时返回 0
 */
static uint16_t rtl_mdio_read_direct(uint8_t reg) {
    rtl_w32(R8169_PHYAR, (uint32_t)(reg & 0x1Fu) << 16);
    uint16_t value = 0;
    if (rtl_wait_bits(R8169_PHYAR, 0x80000000u, 1, 20, 25))
        value = (uint16_t)(rtl_r32(R8169_PHYAR) & 0xFFFFu);
    rtl_delay_us(20);
    return value;
}

/**
 * @brief 现代 PHY 写：把 MII 寄存器号映射到 GPHY OCP 地址
 *        （`r8168g_mdio_write`，r8169_main.c:1244-1258）。
 *
 * 映射规则：写 0x1F 是"选页"，把 OCP 基址切成 `page << 4`；其余寄存器在
 * 非标准页下要减 0x10，最终 OCP 地址 = `ocp_base + reg' * 2`。
 *
 * @param reg   MII 寄存器号
 * @param value 16 位数据
 */
static void rtl_mdio_write_gphy(uint8_t reg, uint16_t value) {
    if (reg == R8169_MII_PAGE_SEL) {
        g_ocp_base = value ? ((uint32_t)value << 4) : R8169_OCP_STD_PHY_BASE;
        return;
    }
    if (g_ocp_base != R8169_OCP_STD_PHY_BASE) reg = (uint8_t)(reg - 0x10);
    rtl_gphy_ocp_write(g_ocp_base + (uint32_t)reg * 2, value);
}

/**
 * @brief 现代 PHY 读（`r8168g_mdio_read`，r8169_main.c:1260-1269）。
 * @param reg MII 寄存器号
 * @return 16 位数据
 */
static uint16_t rtl_mdio_read_gphy(uint8_t reg) {
    if (reg == R8169_MII_PAGE_SEL)
        return (uint16_t)(g_ocp_base == R8169_OCP_STD_PHY_BASE ? 0 : g_ocp_base >> 4);
    if (g_ocp_base != R8169_OCP_STD_PHY_BASE) reg = (uint8_t)(reg - 0x10);
    return rtl_gphy_ocp_read(g_ocp_base + (uint32_t)reg * 2);
}

/** @brief 统一的 PHY 写（`rtl_writephy`，r8169_main.c:1364-1378）。
 *
 * 三条通道的分派：VER_28/31 走 PHYAR + 0xD0 bit17；>=VER_40 走 GPHY OCP；
 * 其余走普通 PHYAR。选错通道的典型现象是"PHY 读回全 0/全 1、链路不上"。
 */
static void rtl_phy_write(uint8_t reg, uint16_t value) {
    if (g_phy_access == R8169_ACCESS_GPHY_OCP) {
        rtl_mdio_write_gphy(reg, value);
        return;
    }
    if (g_mac_version == RTL_GIGA_MAC_VER_28 || g_mac_version == RTL_GIGA_MAC_VER_31) {
        rtl_w32(R8169_DLLPR, rtl_r32(R8169_DLLPR) & ~R8169_DP_MDIO_ACCESS);
        rtl_mdio_write_direct(reg, value);
        rtl_w32(R8169_DLLPR, rtl_r32(R8169_DLLPR) | R8169_DP_MDIO_ACCESS);
        return;
    }
    rtl_mdio_write_direct(reg, value);
}

/** @brief 统一的 PHY 读（`rtl_readphy`，r8169_main.c:1380-1391）。 */
static uint16_t rtl_phy_read(uint8_t reg) {
    if (g_phy_access == R8169_ACCESS_GPHY_OCP) return rtl_mdio_read_gphy(reg);
    if (g_mac_version == RTL_GIGA_MAC_VER_28 || g_mac_version == RTL_GIGA_MAC_VER_31) {
        if (reg == R8169_MII_PHYSID2) return R8169_DP_PHYSID2_FAKE;
        rtl_w32(R8169_DLLPR, rtl_r32(R8169_DLLPR) & ~R8169_DP_MDIO_ACCESS);
        uint16_t value = rtl_mdio_read_direct(reg);
        rtl_w32(R8169_DLLPR, rtl_r32(R8169_DLLPR) | R8169_DP_MDIO_ACCESS);
        return value;
    }
    return rtl_mdio_read_direct(reg);
}

/* ------------------------------------------------------------------ *
 * 7. PHY 配置原语（r8169_phy_config.c 顶部几个辅助函数的等价物）
 * ------------------------------------------------------------------ */

/**
 * @brief 选页写 PHY 寄存器（`phy_write_paged`：存页 → 写 0x1F → 写寄存器 → 还页）。
 *
 * 页寄存器在选页通道下不是普通寄存器，必须"先存后还"，否则调用方上下文
 * 里的页会被悄悄改掉，后续 magic 全部落到错误的页上。
 *
 * @param page 页号
 * @param reg  MII 寄存器号
 * @param val  16 位数据
 */
static void rtl_phy_write_paged(uint16_t page, uint16_t reg, uint16_t val) {
    uint16_t old = rtl_phy_read(R8169_MII_PAGE_SEL);
    if (old != page) rtl_phy_write(R8169_MII_PAGE_SEL, page);
    rtl_phy_write((uint8_t)reg, val);
    if (old != page) rtl_phy_write(R8169_MII_PAGE_SEL, old);
}

/**
 * @brief 选页读 PHY 寄存器。
 * @param page 页号
 * @param reg  MII 寄存器号
 * @return 读到的 16 位值
 */
static uint16_t rtl_phy_read_paged(uint16_t page, uint16_t reg) {
    uint16_t old = rtl_phy_read(R8169_MII_PAGE_SEL);
    if (old != page) rtl_phy_write(R8169_MII_PAGE_SEL, page);
    uint16_t val = rtl_phy_read((uint8_t)reg);
    if (old != page) rtl_phy_write(R8169_MII_PAGE_SEL, old);
    return val;
}

/**
 * @brief 选页读改写（`phy_modify_paged`）：`new = (old & ~mask) | set`。
 * @param page 页号
 * @param reg  MII 寄存器号
 * @param mask 要清零的位
 * @param set  要置上的位
 */
static void rtl_phy_modify_paged(uint16_t page, uint16_t reg,
                                uint16_t mask, uint16_t set) {
    uint16_t val = rtl_phy_read_paged(page, reg);
    rtl_phy_write_paged(page, reg, (uint16_t)((val & ~mask) | set));
}

/** @brief 非选页读改写（`phy_modify`）。 */
static void rtl_phy_modify(uint16_t reg, uint16_t mask, uint16_t set) {
    uint16_t val = rtl_phy_read((uint8_t)reg);
    rtl_phy_write((uint8_t)reg, (uint16_t)((val & ~mask) | set));
}

/**
 * @brief 写"扩展页"寄存器组（`r8168d_modify_extpage`，r8169_phy_config.c:20-29）。
 *
 * 语义与"选页"不同：先跳到页 7，把扩展页号写进 0x1E，再改 @p reg——这套只
 * 对 8168d/e/f 一代的 PHY 有效，不能与 rtl_phy_modify_paged 混用。
 */
static void rtl_phy_modify_extpage(uint16_t extpage, uint16_t reg,
                                   uint16_t mask, uint16_t set) {
    uint16_t old = rtl_phy_read(R8169_MII_PAGE_SEL);
    rtl_phy_write(R8169_MII_PAGE_SEL, 0x0007);
    rtl_phy_write(0x1E, extpage);
    uint16_t val = rtl_phy_read(reg);
    rtl_phy_write(reg, (uint16_t)((val & ~mask) | set));
    rtl_phy_write(R8169_MII_PAGE_SEL, old);
}

/**
 * @brief 写"参数寄存器组"（`r8168d_phy_param`，r8169_phy_config.c:31-40）。
 * 选页 5 → 参数号写 0x05 → 读改写 0x06。
 */
static void rtl_phy_param_d(uint16_t parm, uint16_t mask, uint16_t val) {
    uint16_t old = rtl_phy_read(R8169_MII_PAGE_SEL);
    rtl_phy_write(R8169_MII_PAGE_SEL, 0x0005);
    rtl_phy_write(0x05, parm);
    uint16_t data = rtl_phy_read(0x06);
    rtl_phy_write(0x06, (uint16_t)((data & ~mask) | val));
    rtl_phy_write(R8169_MII_PAGE_SEL, old);
}

/**
 * @brief 写 "g" 代 PHY 的参数寄存器组（`r8168g_phy_param`，:42-51）。
 * 选页 0x0A43 → 参数号写 0x13 → 读改写 0x14（经 GPHY OCP 映射落到 0xA436）。
 */
static void rtl_phy_param_g(uint16_t parm, uint16_t mask, uint16_t val) {
    rtl_phy_modify_paged(0x0A43, 0x13, 0xFFFF, parm);
    rtl_phy_modify_paged(0x0A43, 0x14, mask, val);
}

/**
 * @brief 8125 的参数寄存器组（`rtl8125_phy_param`，:53-60）。
 *
 * MMD/VEND2 的 0xB87C/0xB87E 在 r8169 里**不是真正的 C45 帧**——驱动把
 * 这两个"regnum"直接当 OCP 地址用（r8169_main.c:5392-5417）。所以这里
 * 也直接操作 OCP，不需要 C45 编码。
 */
static void rtl_phy_param_8125(uint16_t parm, uint16_t mask, uint16_t val) {
    rtl_gphy_ocp_write(0xB87C, parm);
    uint16_t data = rtl_gphy_ocp_read(0xB87E);
    rtl_gphy_ocp_write(0xB87E, (uint16_t)((data & ~mask) | val));
}

/** @brief 一组"寄存器=值"的批量写（`__rtl_writephy_batch`）。 */
typedef struct { uint16_t reg; uint16_t val; } rtl_phy_reg_t;

static void rtl_phy_batch(const rtl_phy_reg_t *regs, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) rtl_phy_write((uint8_t)regs[i].reg, regs[i].val);
}

/** @brief 一组"EPHY 寄存器: 清掩码 置位"的批量写（`__rtl_ephy_init`）。 */
typedef struct { uint8_t offset; uint16_t mask; uint16_t bits; } rtl_ephy_reg_t;

static void rtl_ephy_batch(const rtl_ephy_reg_t *regs, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint16_t w = (uint16_t)((rtl_ephy_read(regs[i].offset) & ~regs[i].mask) | regs[i].bits);
        rtl_ephy_write(regs[i].offset, w);
    }
}

/* ------------------------------------------------------------------ *
 * 8. 共用 magic 序列
 * ------------------------------------------------------------------ */

/** @brief 复位包过滤器（`rtl_reset_packet_filter`，r8169_main.c:1613-1617）。 */
static void rtl_reset_packet_filter(void) {
    rtl_eri_clear_bits(0xDC, 0x00000001u);
    rtl_eri_set_bits(0xDC, 0x00000001u);
}

/** @brief 关 RXDV gate（`rtl_disable_rxdvgate`，:2749-2752）。 */
static void rtl_disable_rxdvgate(void) {
    rtl_w32(R8169_MISC, rtl_r32(R8169_MISC) & ~R8169_MISC_RXDV_GATED);
}

/** @brief 开 RXDV gate 并等 FIFO 排空（`rtl_enable_rxdvgate`，:2754-2759）。
 *
 * 5/6/8125 起芯片把"停止收发"做成 gate + FIFO 排空两段，不等排空就复位会
 * 导致 FIFO 里残留的帧被当成新包重新入环。 */
static void rtl_enable_rxdvgate(void) {
    rtl_w32(R8169_MISC, rtl_r32(R8169_MISC) | R8169_MISC_RXDV_GATED);
    rtl_delay_ms(2);
    if (g_mac_version >= RTL_GIGA_MAC_VER_40 && g_mac_version <= RTL_GIGA_MAC_VER_52) {
        (void)rtl_wait_bits(R8169_TX_CONFIG, 0x800u, 1, 100, 42); /* TXCFG_EMPTY */
        (void)rtl_wait_bits8(R8169_MCU, (uint8_t)R8169_MCU_RXTX_EMPTY, 1, 100, 42);
    } else if (g_mac_version >= RTL_GIGA_MAC_VER_63) {
        rtl_w8(R8169_CHIP_CMD, (uint8_t)(rtl_r8(R8169_CHIP_CMD) | 0x80u)); /* StopReq */
        (void)rtl_wait_bits8(R8169_MCU, (uint8_t)R8169_MCU_RXTX_EMPTY, 1, 100, 42);
    } else if (g_mac_version == RTL_GIGA_MAC_VER_61) {
        (void)rtl_wait_bits8(R8169_MCU, (uint8_t)R8169_MCU_RXTX_EMPTY, 1, 100, 42);
    }
}

/** @brief 关配置寄存器写入保护（`rtl_unlock_config_regs`，:820-823）。
 *  Config0..5 / MAC0..4 / RxMaxSize 等寄存器在 0xC0 之外**静默丢弃**写入，
 *  忘了解锁的症状是"MAC 地址读回全 0、配置不生效但没有任何报错"。 */
static void rtl_unlock_config(void) { rtl_w8(R8169_CFG9346, (uint8_t)R8169_CFG9346_UNLOCK); }
static void rtl_lock_config(void) { rtl_w8(R8169_CFG9346, (uint8_t)R8169_CFG9346_LOCK); }

/** @brief 配置寄存器读改写（`rtl_mod_config5` 的通用形式）。 */
static void rtl_mod_config(uint16_t reg, uint8_t clear, uint8_t set) {
    uint8_t val = rtl_r8(reg);
    rtl_w8(reg, (uint8_t)((val & ~clear) | set));
}

/**
 * @brief 关 ASPM / 时钟请求（`rtl_hw_aspm_clkreq_enable(false)`，:3159-3181）。
 *
 * HBOS 不做电源管理，也不需要 OS 侧 ASPM 控制，所以这里只做实驱动里
 * "关闭"那一半：清 LTR、关 clkreq、关 Config5.ASPM_en。保留它的意义是
 * **顺序**——访问 ephy/PHY 之前必须先关 ASPM，否则部分板子会随机挂死。
 */
static void rtl_aspm_clkreq_disable(void) {
    if (g_mac_version < RTL_GIGA_MAC_VER_32) return;
    if (g_mac_version >= RTL_GIGA_MAC_VER_46 && g_mac_version <= RTL_GIGA_MAC_VER_48)
        rtl_mac_ocp_modify(0xE092, 0x00FF, 0x0000);
    else if (g_mac_version >= RTL_GIGA_MAC_VER_61)
        rtl_mac_ocp_modify(0xE092, 0x00FF, 0x0000);
    if (g_mac_version == RTL_GIGA_MAC_VER_70 || g_mac_version == RTL_GIGA_MAC_VER_80) {
        uint8_t v = rtl_r8(R8169_8125_INT_CFG0);
        rtl_w8(R8169_8125_INT_CFG0, (uint8_t)(v & ~0x08u)); /* INT_CFG0_CLKREQEN */
    } else {
        rtl_mod_config(R8169_CONFIG2, 0x80u, 0);  /* ClkReqEn */
    }
    rtl_mod_config(R8169_CONFIG5, 0x01u, 0);      /* ASPM_en */
}

/* ------------------------------------------------------------------ *
 * 9. 各代 hw_start（rtl_hw_config[]，r8169_main.c:3983-4031）
 * ------------------------------------------------------------------ */

/** @brief 8169 家族的 0x7C magic（`rtl8169_set_magic_reg`，:2813-2828）。 */
static void rtl_hw_start_8169_magic(void) {
    uint32_t val;
    if (g_mac_version == RTL_GIGA_MAC_VER_05)      val = 0x000FFF00u;
    else if (g_mac_version == RTL_GIGA_MAC_VER_06) val = 0x00FFFF00u;
    else return;
    /* Config2.PCI_Clock_66MHz 置位时再补低字节。 */
    if (rtl_r8(R8169_CONFIG2) & 0x01u) val |= 0xFFu;
    rtl_w32(0x7Cu, val);
}

/** @brief 8168b（VER_17）：只清 Beacon_en。 */
static void rtl_hw_start_8168b(void) {
    rtl_mod_config(R8169_CONFIG3, R8169_CONFIG3_BEACON_EN, 0);
}

/** @brief 8168cp 公共部分（`__rtl_hw_start_8168cp`，:3206-3213）。
 *  `Speed_down` 是"没有链路时自动降速"的省电位，cp 一代要求打开。 */
static void rtl_hw_start_8168cp_common(void) {
    rtl_mod_config(R8169_CONFIG1, 0, (uint8_t)R8169_CONFIG1_SPEED_DOWN);
    rtl_mod_config(R8169_CONFIG3, R8169_CONFIG3_BEACON_EN, 0);
}

/** @brief 8168cp_1（VER_18）。 */
static void rtl_hw_start_8168cp_1(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x01, 0,      0x0001 }, { 0x02, 0x0800, 0x1000 },
        { 0x03, 0,      0x0042 }, { 0x06, 0x0080, 0x0000 },
        { 0x07, 0,      0x2000 },
    };
    rtl_ephy_batch(e_info, 5);
    rtl_hw_start_8168cp_common();
}

/** @brief 8168cp_2（VER_23）。 */
static void rtl_hw_start_8168cp_2(void) {
    rtl_mod_config(R8169_CONFIG3, R8169_CONFIG3_BEACON_EN, 0);
}

/** @brief 8168cp_3（VER_24）。`DBG_REG = 0x20` 是驱动注释里明写的 "Magic."。 */
static void rtl_hw_start_8168cp_3(void) {
    rtl_hw_start_8168cp_2();
    rtl_w8(R8169_DBG_REG, 0x20u);
}

/** @brief 8168c_1（VER_19）：DBG_REG 修 NAK + 一条 EPHY 表。 */
static void rtl_hw_start_8168c_1(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x02, 0x0800, 0x1000 }, { 0x03, 0, 0x0002 }, { 0x06, 0x0080, 0x0000 },
    };
    rtl_w8(R8169_DBG_REG, (uint8_t)(0x06u | R8169_DBG_FIX_NAK_1 | R8169_DBG_FIX_NAK_2));
    rtl_ephy_batch(e_info, 3);
    rtl_hw_start_8168cp_common();
}

/** @brief 8168c_2（VER_20/21）。 */
static void rtl_hw_start_8168c_2(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x01, 0, 0x0001 }, { 0x03, 0x0400, 0x0020 },
    };
    rtl_ephy_batch(e_info, 2);
    rtl_hw_start_8168cp_common();
}

/** @brief 8168c_4（VER_22）：只有公共部分。 */
static void rtl_hw_start_8168c_4(void) { rtl_hw_start_8168cp_common(); }

/** @brief 8168d（VER_25/26/31）：无额外 magic。 */
static void rtl_hw_start_8168d(void) { }

/** @brief 8168d_4（VER_28）：EPHY 表 + 打开时钟请求。 */
static void rtl_hw_start_8168d_4(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x0B, 0x0000, 0x0048 }, { 0x19, 0x0020, 0x0050 },
        { 0x0C, 0x0100, 0x0020 }, { 0x10, 0x0004, 0x0000 },
    };
    rtl_ephy_batch(e_info, 4);
}

/** @brief 8168e_1（VER_32/33）：13 条 EPHY 表 + 复位 TX FIFO 指针。
 *  `MISC.TXPLA_RST` 脉冲是把 TX FIFO 指针拉回起点，缺了会出现"发几帧后
 *  彻底停住"。 */
static void rtl_hw_start_8168e_1(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x00, 0x0200, 0x0100 }, { 0x00, 0x0000, 0x0004 },
        { 0x06, 0x0002, 0x0001 }, { 0x06, 0x0000, 0x0030 },
        { 0x07, 0x0000, 0x2000 }, { 0x00, 0x0000, 0x0020 },
        { 0x03, 0x5800, 0x2000 }, { 0x03, 0x0000, 0x0001 },
        { 0x01, 0x0800, 0x1000 }, { 0x07, 0x0000, 0x4000 },
        { 0x1E, 0x0000, 0x2000 }, { 0x19, 0xFFFF, 0xFE6C },
        { 0x0A, 0x0000, 0x0040 },
    };
    rtl_ephy_batch(e_info, 13);
    uint32_t misc = rtl_r32(R8169_MISC);
    rtl_w32(R8169_MISC, misc | R8169_MISC_TXPLA_RST);
    rtl_w32(R8169_MISC, rtl_r32(R8169_MISC) & ~R8169_MISC_TXPLA_RST);
    rtl_mod_config(R8169_CONFIG5, R8169_CONFIG5_SPI_EN, 0);
}

/** @brief 8168e_2（VER_34）：FIFO 尺寸 + ERI magic。 */
static void rtl_hw_start_8168e_2(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x09, 0x0000, 0x0080 }, { 0x19, 0x0000, 0x0224 },
        { 0x00, 0x0000, 0x0004 }, { 0x0C, 0x3DF0, 0x0200 },
    };
    rtl_ephy_batch(e_info, 4);
    (void)rtl_eri_write(0xC0, R8169_ERIAR_MASK_0011, 0x0000u);
    (void)rtl_eri_write(0xB8, R8169_ERIAR_MASK_1111, 0x0000u);
    (void)rtl_eri_write(0xC8, R8169_ERIAR_MASK_1111, (0x10u << 16) | 0x02u);
    (void)rtl_eri_write(0xE8, R8169_ERIAR_MASK_1111, (0x10u << 16) | 0x06u);
    rtl_eri_set_bits(0x1D0, 0x00000002u);
    rtl_reset_packet_filter();
    rtl_eri_set_bits(0x1B0, 0x00000010u);
    (void)rtl_eri_write(0xCC, R8169_ERIAR_MASK_1111, 0x00000050u);
    (void)rtl_eri_write(0xD0, R8169_ERIAR_MASK_1111, 0x07FF0060u);
    rtl_mod_config(R8169_MCU, R8169_MCU_NOW_IS_OOB, 0);
    rtl_mod_config(R8169_DLLPR, 0, (uint8_t)R8169_DLLPR_PFM_EN);
    rtl_w32(R8169_MISC, rtl_r32(R8169_MISC) | R8169_MISC_PWM_EN);
    rtl_mod_config(R8169_CONFIG5, R8169_CONFIG5_SPI_EN, 0);
}

/** @brief 8168f 公共部分（`rtl_hw_start_8168f`，:3372-3391）。 */
static void rtl_hw_start_8168f_common(void) {
    (void)rtl_eri_write(0xC0, R8169_ERIAR_MASK_0011, 0x0000u);
    (void)rtl_eri_write(0xB8, R8169_ERIAR_MASK_1111, 0x0000u);
    (void)rtl_eri_write(0xC8, R8169_ERIAR_MASK_1111, (0x10u << 16) | 0x02u);
    (void)rtl_eri_write(0xE8, R8169_ERIAR_MASK_1111, (0x10u << 16) | 0x06u);
    rtl_reset_packet_filter();
    rtl_eri_set_bits(0x1B0, 0x00000010u);
    rtl_eri_set_bits(0x1D0, 0x00000012u);
    (void)rtl_eri_write(0xCC, R8169_ERIAR_MASK_1111, 0x00000050u);
    (void)rtl_eri_write(0xD0, R8169_ERIAR_MASK_1111, 0x00000060u);
    rtl_mod_config(R8169_MCU, R8169_MCU_NOW_IS_OOB, 0);
    rtl_mod_config(R8169_DLLPR, 0, (uint8_t)R8169_DLLPR_PFM_EN);
    rtl_w32(R8169_MISC, rtl_r32(R8169_MISC) | R8169_MISC_PWM_EN);
    rtl_mod_config(R8169_CONFIG5, R8169_CONFIG5_SPI_EN, 0);
}

/** @brief 8168f_1（VER_35/36）。 */
static void rtl_hw_start_8168f_1(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x06, 0x00C0, 0x0020 }, { 0x08, 0x0001, 0x0002 },
        { 0x09, 0x0000, 0x0080 }, { 0x19, 0x0000, 0x0224 },
        { 0x00, 0x0000, 0x0008 }, { 0x0C, 0x3DF0, 0x0200 },
    };
    rtl_hw_start_8168f_common();
    rtl_ephy_batch(e_info, 6);
}

/** @brief 8411（VER_38）：8168f + 关 L2/L3。 */
static void rtl_hw_start_8411(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x06, 0x00C0, 0x0020 }, { 0x0F, 0xFFFF, 0x5200 },
        { 0x19, 0x0000, 0x0224 }, { 0x00, 0x0000, 0x0008 },
        { 0x0C, 0x3DF0, 0x0200 },
    };
    rtl_hw_start_8168f_common();
    rtl_mod_config(R8169_CONFIG3, R8169_CONFIG3_RDY_TO_L23, 0);
    rtl_ephy_batch(e_info, 5);
}

/** @brief 关 L2/L3（`rtl_pcie_state_l2l3_disable`，:2988-2992）。 */
static void rtl_pcie_l2l3_disable(void) {
    rtl_mod_config(R8169_CONFIG3, R8169_CONFIG3_RDY_TO_L23, 0);
}

/** @brief 8168g 公共部分（`rtl_hw_start_8168g`，:3425-3444）。 */
static void rtl_hw_start_8168g_common(void) {
    (void)rtl_eri_write(0xC8, R8169_ERIAR_MASK_1111, (0x08u << 16) | 0x02u);
    (void)rtl_eri_write(0xE8, R8169_ERIAR_MASK_1111, (0x10u << 16) | 0x06u);
    (void)rtl_eri_write(0xCC, R8169_ERIAR_MASK_0001, 0x00000038u);
    (void)rtl_eri_write(0xD0, R8169_ERIAR_MASK_0001, 0x00000048u);
    rtl_reset_packet_filter();
    (void)rtl_eri_write(0x2F8, R8169_ERIAR_MASK_0011, 0x1D8Fu);
    rtl_disable_rxdvgate();
    (void)rtl_eri_write(0xC0, R8169_ERIAR_MASK_0011, 0x0000u);
    (void)rtl_eri_write(0xB8, R8169_ERIAR_MASK_0011, 0x0000u);
    rtl_eri_modify(0x2FC, 0x01u, 0x06u);
    rtl_eri_clear_bits(0x1B0, 0x00001000u);
    rtl_pcie_l2l3_disable();
}

/** @brief 8168g_1（VER_40）。 */
static void rtl_hw_start_8168g_1(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x00, 0x0008, 0x0000 }, { 0x0C, 0x3FF0, 0x0820 },
        { 0x1E, 0x0000, 0x0001 }, { 0x19, 0x8000, 0x0000 },
    };
    rtl_hw_start_8168g_common();
    rtl_ephy_batch(e_info, 4);
}

/** @brief 8168g_2（VER_42/43）。 */
static void rtl_hw_start_8168g_2(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x00, 0x0008, 0x0000 }, { 0x0C, 0x3FF0, 0x0820 },
        { 0x19, 0xFFFF, 0x7C00 }, { 0x1E, 0xFFFF, 0x20EB },
        { 0x0D, 0xFFFF, 0x1666 }, { 0x00, 0xFFFF, 0x10A3 },
        { 0x06, 0xFFFF, 0xF050 }, { 0x04, 0x0000, 0x0010 },
        { 0x1D, 0x4000, 0x0000 },
    };
    rtl_hw_start_8168g_common();
    rtl_ephy_batch(e_info, 9);
}

/** @brief 8411b 的 PHY 掉电后 RX 混乱修复（`rtl8411b_fix_phy_down`，:3477-3502）。
 *  68 个 16 位字从 MAC OCP 0xF800 开始连续写。 */
static void rtl_8411b_fix_phy_down(void) {
    static const uint16_t fix_data[] = {
        0xE008, 0xE00A, 0xE00C, 0xE00E, 0xE027, 0xE04F, 0xE05E, 0xE065,
        0xC602, 0xBE00, 0x0000, 0xC502, 0xBD00, 0x074C, 0xC302, 0xBB00,
        0x080A, 0x6420, 0x48C2, 0x8C20, 0xC516, 0x64A4, 0x49C0, 0xF009,
        0x74A2, 0x8CA5, 0x74A0, 0xC50E, 0x9CA2, 0x1C11, 0x9CA0, 0xE006,
        0x74F8, 0x48C4, 0x8CF8, 0xC404, 0xBC00, 0xC403, 0xBC00, 0x0BF2,
        0x0C0A, 0xE434, 0xD3C0, 0x49D9, 0xF01F, 0xC526, 0x64A5, 0x1400,
        0xF007, 0x0C01, 0x8CA5, 0x1C15, 0xC51B, 0x9CA0, 0xE013, 0xC519,
        0x74A0, 0x48C4, 0x8CA0, 0xC516, 0x74A4, 0x48C8, 0x48CA, 0x9CA4,
        0xC512, 0x1B00, 0x9BA0, 0x1B1C, 0x483F, 0x9BA2, 0x1B04, 0xC508,
        0x9BA0, 0xC505, 0xBD00, 0xC502, 0xBD00, 0x0300, 0x051E, 0xE434,
        0xE018, 0xE092, 0xDE20, 0xD3C0, 0xC50F, 0x76A4, 0x49E3, 0xF007,
        0x49C0, 0xF103, 0xC607, 0xBE00, 0xC606, 0xBE00, 0xC602, 0xBE00,
        0x0C4C, 0x0C28, 0x0C2C, 0xDC00, 0xC707, 0x1D00, 0x8DE2, 0x48C1,
        0xC502, 0xBD00, 0x00AA, 0xE0C0, 0xC502, 0xBD00, 0x0132,
    };
    for (uint32_t i = 0; i < sizeof(fix_data) / sizeof(fix_data[0]); i++)
        rtl_mac_ocp_write(0xF800u + 2u * i, fix_data[i]);
}

/** @brief 8411_2（VER_44）：8168g + 大段 MAC OCP magic + F800 表。 */
static void rtl_hw_start_8411_2(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x00, 0x0008, 0x0000 }, { 0x0C, 0x37D0, 0x0820 },
        { 0x1E, 0x0000, 0x0001 }, { 0x19, 0x8021, 0x0000 },
        { 0x1E, 0x0000, 0x2000 }, { 0x0D, 0x0100, 0x0200 },
        { 0x00, 0x0000, 0x0080 }, { 0x06, 0x0000, 0x0010 },
        { 0x04, 0x0000, 0x0010 }, { 0x1D, 0x0000, 0x4000 },
    };
    rtl_hw_start_8168g_common();
    rtl_ephy_batch(e_info, 10);
    /* Realtek 提供的 magic：修 PHY 掉电后 RX 单元混乱。 */
    for (uint32_t reg = 0xFC28u; reg <= 0xFC36u; reg += 2u) rtl_mac_ocp_write(reg, 0x0000);
    rtl_delay_ms(3);
    rtl_mac_ocp_write(0xFC26u, 0x0000);
    rtl_8411b_fix_phy_down();
    rtl_mac_ocp_write(0xFC26u, 0x8000);
    rtl_mac_ocp_write(0xFC2Au, 0x0743);
    rtl_mac_ocp_write(0xFC2Cu, 0x0801);
    rtl_mac_ocp_write(0xFC2Eu, 0x0BE9);
    rtl_mac_ocp_write(0xFC30u, 0x02FD);
    rtl_mac_ocp_write(0xFC32u, 0x0C25);
    rtl_mac_ocp_write(0xFC34u, 0x00A9);
    rtl_mac_ocp_write(0xFC36u, 0x012D);
}

/**
 * @brief 8168h（VER_46/48）——**本机真机路径**（10ec:8168 rev15, XID 0x541）。
 *
 * 这段不能简化成"通用 8168"：它含 8168h 专属的 MAC OCP 参数
 * （0xd412/0xe056/0xe052/0xe0d6/0xd420/0xe63e/0xc094/0xc09e），
 * 其中 0xd412 把 PHY 的 saw-counter 换算成 1 ms 计数写回 MAC，
 * 缺了会让 LED/省电状态机与链路不同步。
 */
static void rtl_hw_start_8168h_1(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x1E, 0x0800, 0x0001 }, { 0x1D, 0x0000, 0x0800 },
        { 0x05, 0xFFFF, 0x2089 }, { 0x06, 0xFFFF, 0x5881 },
        { 0x04, 0xFFFF, 0x854A }, { 0x01, 0xFFFF, 0x068B },
    };
    rtl_ephy_batch(e_info, 6);
    (void)rtl_eri_write(0xC8, R8169_ERIAR_MASK_1111, (0x08u << 16) | 0x02u);
    (void)rtl_eri_write(0xE8, R8169_ERIAR_MASK_1111, (0x10u << 16) | 0x06u);
    (void)rtl_eri_write(0xCC, R8169_ERIAR_MASK_0001, 0x00000038u);
    (void)rtl_eri_write(0xD0, R8169_ERIAR_MASK_0001, 0x00000048u);
    rtl_reset_packet_filter();
    rtl_eri_set_bits(0xDC, 0x0000001Cu);
    (void)rtl_eri_write(0x5F0, R8169_ERIAR_MASK_0011, 0x4F87u);
    rtl_disable_rxdvgate();
    (void)rtl_eri_write(0xC0, R8169_ERIAR_MASK_0011, 0x0000u);
    (void)rtl_eri_write(0xB8, R8169_ERIAR_MASK_0011, 0x0000u);
    rtl_mod_config(R8169_DLLPR, R8169_DLLPR_PFM_EN, 0);
    rtl_mod_config(0xF2u, 0x40u, 0);                 /* MISC_1.PFM_D3COLD_EN */
    rtl_mod_config(R8169_DLLPR, R8169_DLLPR_TX_10M_PS, 0);
    rtl_eri_clear_bits(0x1B0, 0x00001000u);
    rtl_pcie_l2l3_disable();

    uint16_t rg_saw_cnt = (uint16_t)(rtl_phy_read_paged(0x0C42, 0x13) & 0x3FFFu);
    if (rg_saw_cnt > 0) {
        uint16_t sw_cnt = (uint16_t)((16000000u / rg_saw_cnt) & 0x0FFFu);
        rtl_mac_ocp_modify(0xD412u, 0x0FFF, sw_cnt);
    }
    rtl_mac_ocp_modify(0xE056u, 0x00F0, 0x0000);
    rtl_mac_ocp_modify(0xE052u, 0x6000, 0x8008);
    rtl_mac_ocp_modify(0xE0D6u, 0x01FF, 0x017F);
    rtl_mac_ocp_modify(0xD420u, 0x0FFF, 0x047F);
    rtl_mac_ocp_write(0xE63Eu, 0x0001);
    rtl_mac_ocp_write(0xE63Eu, 0x0000);
    rtl_mac_ocp_write(0xC094u, 0x0000);
    rtl_mac_ocp_write(0xC09Eu, 0x0000);
}

/** @brief 8168ep 公共部分（`rtl_hw_start_8168ep`，:3609-3632）。 */
static void rtl_hw_start_8168ep_common(void) {
    (void)rtl_eri_write(0xC8, R8169_ERIAR_MASK_1111, (0x08u << 16) | 0x02u);
    (void)rtl_eri_write(0xE8, R8169_ERIAR_MASK_1111, (0x10u << 16) | 0x06u);
    (void)rtl_eri_write(0xCC, R8169_ERIAR_MASK_0001, 0x0000002Fu);
    (void)rtl_eri_write(0xD0, R8169_ERIAR_MASK_0001, 0x0000005Fu);
    rtl_reset_packet_filter();
    (void)rtl_eri_write(0x5F0, R8169_ERIAR_MASK_0011, 0x4F87u);
    rtl_disable_rxdvgate();
    (void)rtl_eri_write(0xC0, R8169_ERIAR_MASK_0011, 0x0000u);
    (void)rtl_eri_write(0xB8, R8169_ERIAR_MASK_0011, 0x0000u);
    rtl_eri_modify(0x2FC, 0x01u, 0x06u);
    rtl_mod_config(R8169_DLLPR, R8169_DLLPR_TX_10M_PS, 0);
    rtl_pcie_l2l3_disable();
}

/** @brief 8168ep_3（VER_51）：EPHY 表 + 一组 MAC OCP 参数。 */
static void rtl_hw_start_8168ep_3(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x00, 0x0000, 0x0080 }, { 0x0D, 0x0100, 0x0200 },
        { 0x19, 0x8021, 0x0000 }, { 0x1E, 0x0000, 0x2000 },
    };
    rtl_ephy_batch(e_info, 4);
    rtl_hw_start_8168ep_common();
    rtl_mod_config(R8169_DLLPR, R8169_DLLPR_PFM_EN, 0);
    rtl_mod_config(0xF2u, 0x40u, 0);
    rtl_mac_ocp_modify(0xD3E2u, 0x0FFF, 0x0271);
    rtl_mac_ocp_modify(0xD3E4u, 0x00FF, 0x0000);
    rtl_mac_ocp_modify(0xE860u, 0x0000, 0x0080);
}

/** @brief 8117/VB（VER_52）：8168ep + 更大一组 MAC OCP magic。 */
static void rtl_hw_start_8117(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x19, 0x0040, 0x1100 }, { 0x59, 0x0040, 0x1100 },
    };
    rtl_ephy_batch(e_info, 2);
    rtl_hw_start_8168ep_common();
    rtl_eri_set_bits(0xD4, 0x00000010u);
    rtl_mod_config(R8169_DLLPR, R8169_DLLPR_PFM_EN, 0);
    rtl_mod_config(0xF2u, 0x40u, 0);
    rtl_mod_config(R8169_DLLPR, R8169_DLLPR_TX_10M_PS, 0);
    rtl_eri_clear_bits(0x1B0, 0x00001000u);
    rtl_pcie_l2l3_disable();
    uint16_t rg_saw_cnt = (uint16_t)(rtl_phy_read_paged(0x0C42, 0x13) & 0x3FFFu);
    if (rg_saw_cnt > 0)
        rtl_mac_ocp_modify(0xD412u, 0x0FFF, (uint16_t)((16000000u / rg_saw_cnt) & 0x0FFFu));
    rtl_mac_ocp_modify(0xE056u, 0x00F0, 0x0000);
    rtl_mac_ocp_write(0xEA80u, 0x0003);
    rtl_mac_ocp_modify(0xE052u, 0x0000, 0x0009);
    rtl_mac_ocp_modify(0xD420u, 0x0FFF, 0x047F);
    rtl_mac_ocp_write(0xE63Eu, 0x0001);
    rtl_mac_ocp_write(0xE63Eu, 0x0000);
    rtl_mac_ocp_write(0xC094u, 0x0000);
    rtl_mac_ocp_write(0xC09Eu, 0x0000);
}

/** @brief 8102e_1（VER_07）。 */
static void rtl_hw_start_8102e_1(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x01, 0, 0x6E65 }, { 0x02, 0, 0x091F }, { 0x03, 0, 0xC2F9 },
        { 0x06, 0, 0xAFB5 }, { 0x07, 0, 0x0E00 }, { 0x19, 0, 0xEC80 },
        { 0x01, 0, 0x2E65 }, { 0x01, 0, 0x6E65 },
    };
    rtl_w8(R8169_DBG_REG, (uint8_t)R8169_DBG_FIX_NAK_1);
    rtl_w8(R8169_CONFIG1, (uint8_t)(R8169_CONFIG1_LEDS1 | R8169_CONFIG1_LEDS0 |
                                    R8169_CONFIG1_SPEED_DOWN | R8169_CONFIG1_MEMMAP |
                                    R8169_CONFIG1_IOMAP | R8169_CONFIG1_VPD |
                                    R8169_CONFIG1_PMENABLE));
    rtl_mod_config(R8169_CONFIG3, R8169_CONFIG3_BEACON_EN, 0);
    uint8_t cfg1 = rtl_r8(R8169_CONFIG1);
    if ((cfg1 & R8169_CONFIG1_LEDS0) && (cfg1 & R8169_CONFIG1_LEDS1))
        rtl_w8(R8169_CONFIG1, (uint8_t)(cfg1 & ~R8169_CONFIG1_LEDS0));
    rtl_ephy_batch(e_info, 8);
}

/** @brief 8102e_2（VER_09）。 */
static void rtl_hw_start_8102e_2(void) {
    rtl_w8(R8169_CONFIG1, (uint8_t)(R8169_CONFIG1_MEMMAP | R8169_CONFIG1_IOMAP |
                                    R8169_CONFIG1_VPD | R8169_CONFIG1_PMENABLE));
    rtl_mod_config(R8169_CONFIG3, R8169_CONFIG3_BEACON_EN, 0);
}

/** @brief 8102e_3（VER_08）：8102e_2 + EPHY 0x03。 */
static void rtl_hw_start_8102e_3(void) {
    rtl_hw_start_8102e_2();
    rtl_ephy_write(0x03, 0xC2F9);
}

/** @brief 8401（VER_14）。 */
static void rtl_hw_start_8401(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x01, 0xFFFF, 0x6FE5 }, { 0x03, 0xFFFF, 0x0599 },
        { 0x06, 0xFFFF, 0xAF25 }, { 0x07, 0xFFFF, 0x8E68 },
    };
    rtl_ephy_batch(e_info, 4);
    rtl_mod_config(R8169_CONFIG3, R8169_CONFIG3_BEACON_EN, 0);
}

/** @brief 8105e_1（VER_29）：关 Early Tally + 开 NDP/OOB reset。 */
static void rtl_hw_start_8105e_1(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x07, 0, 0x4000 }, { 0x19, 0, 0x0200 }, { 0x19, 0, 0x0020 },
        { 0x1E, 0, 0x2000 }, { 0x03, 0, 0x0001 }, { 0x19, 0, 0x0100 },
        { 0x19, 0, 0x0004 }, { 0x0A, 0, 0x0020 },
    };
    rtl_w32(R8169_MISC, rtl_r32(R8169_MISC) | 0x00002800u);
    rtl_w32(R8169_MISC, rtl_r32(R8169_MISC) & ~R8169_MISC_EARLY_TALLY);
    rtl_mod_config(R8169_MCU, 0, (uint8_t)(R8169_MCU_EN_NDP | R8169_MCU_EN_OOB_RESET));
    rtl_mod_config(R8169_DLLPR, 0, (uint8_t)R8169_DLLPR_PFM_EN);
    rtl_ephy_batch(e_info, 8);
    rtl_pcie_l2l3_disable();
}

/** @brief 8105e_2（VER_30）：8105e_1 + EPHY 0x1E bit15。 */
static void rtl_hw_start_8105e_2(void) {
    rtl_hw_start_8105e_1();
    rtl_ephy_write(0x1E, (uint16_t)(rtl_ephy_read(0x1E) | 0x8000u));
}

/** @brief 8402（VER_37）：FIFO 全 0 + 关 EEE。 */
static void rtl_hw_start_8402(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x19, 0xFFFF, 0xFF64 }, { 0x1E, 0, 0x4000 },
    };
    rtl_w32(R8169_MISC, rtl_r32(R8169_MISC) | 0x00002800u);
    rtl_mod_config(R8169_MCU, R8169_MCU_NOW_IS_OOB, 0);
    rtl_ephy_batch(e_info, 2);
    (void)rtl_eri_write(0xC8, R8169_ERIAR_MASK_1111, 0x00000002u);
    (void)rtl_eri_write(0xE8, R8169_ERIAR_MASK_1111, 0x00000006u);
    rtl_reset_packet_filter();
    (void)rtl_eri_write(0xC0, R8169_ERIAR_MASK_0011, 0x0000u);
    (void)rtl_eri_write(0xB8, R8169_ERIAR_MASK_0011, 0x0000u);
    rtl_eri_modify(0x0D4, 0x0E00u, 0xFF00u);
    (void)rtl_eri_write(0x1B0, R8169_ERIAR_MASK_0011, 0x0000u);
    rtl_pcie_l2l3_disable();
}

/** @brief 8106（VER_39）：不是 8168evl 一代，单独一条路。 */
static void rtl_hw_start_8106(void) {
    rtl_w32(R8169_MISC, rtl_r32(R8169_MISC) | 0x00002800u);
    rtl_w32(R8169_MISC, (rtl_r32(R8169_MISC) | R8169_MISC_DISABLE_LAN) &
                        ~R8169_MISC_EARLY_TALLY);
    rtl_mod_config(R8169_MCU, 0, (uint8_t)(R8169_MCU_EN_NDP | R8169_MCU_EN_OOB_RESET));
    rtl_mod_config(R8169_DLLPR, R8169_DLLPR_PFM_EN, 0);
    (void)rtl_eri_write(0x1D0, R8169_ERIAR_MASK_0011, 0x0000u);
    (void)rtl_eri_write(0x1B0, R8169_ERIAR_MASK_0011, 0x0000u);
    rtl_pcie_l2l3_disable();
}

/** @brief 8125 公共 magic（`rtl_hw_start_8125_common`，:3856-3923）。
 *
 * 约 30 次 MAC OCP 读写，**不能省**：少写这串的典型症状是"能认卡、能协商、
 * 但收不到包也发不出去"。其中 0xeb58 bit0 必须清 0，那是硬件支持的
 * "新 TX 描述符格式"开关——不关掉，16 字节 legacy 描述符会全程错位。 */
static void rtl_hw_start_8125_common(void) {
    rtl_pcie_l2l3_disable();
    rtl_w16(0x382u, 0x221B);
    rtl_w32(R8169_8125_RSS_CTRL, 0);
    rtl_w16(R8169_8125_Q_NUM_CTRL, 0);
    rtl_mac_ocp_modify(0xD40Au, 0x0010, 0x0000);              /* 关 UPS */
    rtl_mod_config(R8169_CONFIG1, R8169_CONFIG1_CLKREQ_MASK, 0);
    rtl_mac_ocp_write(0xC140u, 0xFFFF);
    rtl_mac_ocp_write(0xC142u, 0xFFFF);
    rtl_mac_ocp_modify(0xD3E2u, 0x0FFF, 0x03A9);
    rtl_mac_ocp_modify(0xD3E4u, 0x00FF, 0x0000);
    rtl_mac_ocp_modify(0xE860u, 0x0000, 0x0080);
    rtl_mac_ocp_modify(0xEB58u, 0x0001, 0x0000);              /* 关新描述符格式 */
    if (g_mac_version == RTL_GIGA_MAC_VER_70 || g_mac_version == RTL_GIGA_MAC_VER_80)
        rtl_mod_config(0xD8u, 0x02u, 0);
    if (g_mac_version == RTL_GIGA_MAC_VER_80)
        rtl_mac_ocp_modify(0xE614u, 0x0F00, 0x0F00);
    else if (g_mac_version == RTL_GIGA_MAC_VER_70)
        rtl_mac_ocp_modify(0xE614u, 0x0700, 0x0400);
    else if (g_mac_version == RTL_GIGA_MAC_VER_63)
        rtl_mac_ocp_modify(0xE614u, 0x0700, 0x0200);
    else
        rtl_mac_ocp_modify(0xE614u, 0x0700, 0x0300);
    if (g_mac_version == RTL_GIGA_MAC_VER_63)
        rtl_mac_ocp_modify(0xE63Eu, 0x0C30, 0x0000);
    else
        rtl_mac_ocp_modify(0xE63Eu, 0x0C30, 0x0020);
    rtl_mac_ocp_modify(0xC0B4u, 0x0000, 0x000C);
    rtl_mac_ocp_modify(0xEB6Au, 0x00FF, 0x0033);
    rtl_mac_ocp_modify(0xEB50u, 0x03E0, 0x0040);
    rtl_mac_ocp_modify(0xE056u, 0x00F0, 0x0000);
    rtl_mac_ocp_modify(0xE040u, 0x1000, 0x0000);
    rtl_mac_ocp_modify(0xEA1Cu, 0x0003, 0x0001);
    if (g_mac_version == RTL_GIGA_MAC_VER_70 || g_mac_version == RTL_GIGA_MAC_VER_80)
        rtl_mac_ocp_modify(0xEA1Cu, 0x0300, 0x0000);
    else
        rtl_mac_ocp_modify(0xEA1Cu, 0x0004, 0x0000);
    rtl_mac_ocp_modify(0xE0C0u, 0x4F0F, 0x4403);
    rtl_mac_ocp_modify(0xE052u, 0x0080, 0x0068);
    rtl_mac_ocp_modify(0xD430u, 0x0FFF, 0x047F);
    rtl_mac_ocp_modify(0xEA1Cu, 0x0004, 0x0000);
    rtl_mac_ocp_modify(0xEB54u, 0x0000, 0x0001);
    rtl_delay_us(1);
    rtl_mac_ocp_modify(0xEB54u, 0x0001, 0x0000);
    rtl_w16(0x1880u, (uint16_t)(rtl_r16(0x1880u) & ~0x0030u));
    rtl_mac_ocp_write(0xE098u, 0xC302);
    /* 等 MAC OCP 0xE00E bit13 落下（1000 us × 10）。 */
    for (uint32_t i = 0; i < 10; i++) {
        if (!(rtl_mac_ocp_read(0xE00Eu) & 0x2000u)) break;
        rtl_delay_us(1000);
    }
    rtl_disable_rxdvgate();
}

/** @brief 8125A（VER_61）。 */
static void rtl_hw_start_8125a_2(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x04, 0xFFFF, 0xD000 }, { 0x0A, 0xFFFF, 0x8653 },
        { 0x23, 0xFFFF, 0xAB66 }, { 0x20, 0xFFFF, 0x9455 },
        { 0x21, 0xFFFF, 0x99FF }, { 0x29, 0xFFFF, 0xFE04 },
        { 0x44, 0xFFFF, 0xD000 }, { 0x4A, 0xFFFF, 0x8653 },
        { 0x63, 0xFFFF, 0xAB66 }, { 0x60, 0xFFFF, 0x9455 },
        { 0x61, 0xFFFF, 0x99FF }, { 0x69, 0xFFFF, 0xFE04 },
    };
    rtl_ephy_batch(e_info, 12);
    rtl_hw_start_8125_common();
}

/** @brief 8125B（VER_63）。 */
static void rtl_hw_start_8125b(void) {
    static const rtl_ephy_reg_t e_info[] = {
        { 0x0B, 0xFFFF, 0xA908 }, { 0x1E, 0xFFFF, 0x20EB },
        { 0x4B, 0xFFFF, 0xA908 }, { 0x5E, 0xFFFF, 0x20EB },
        { 0x22, 0x0030, 0x0020 }, { 0x62, 0x0030, 0x0020 },
    };
    rtl_ephy_batch(e_info, 6);
    rtl_hw_start_8125_common();
}

/** @brief 8125D/8125K/9151A（VER_64）与 8125BP（VER_66）：只有 cold 部分。 */
static void rtl_hw_start_8125d(void) { rtl_hw_start_8125_common(); }

/* ------------------------------------------------------------------ *
 * 10. hw_initialize（rtl_hw_initialize[]，r8169_main.c:5486-5533）
 * ------------------------------------------------------------------ */

/** @brief 等 MCU link-list 共享 FIFO 就绪（`r8168g_wait_ll_share_fifo_ready`，:5364-5367）。 */
static void rtl_wait_ll_share_fifo_ready(void) {
    (void)rtl_wait_bits8(R8169_MCU, (uint8_t)R8169_MCU_LINK_LIST_RDY, 1, 42, 100);
}

/** @brief 8168g 代的上电初始化（`rtl_hw_init_8168g`，:5486-5499）。
 *
 * 这一步在软复位**之前**，作用是先把收发门关掉、清 OOB 标志，再让
 * MAC 与 PHY 的共享 FIFO 同步一次；跳过它会出现"复位后第一次收包丢"。
 */
static void rtl_hw_init_8168g(void) {
    rtl_enable_rxdvgate();
    rtl_w8(R8169_CHIP_CMD, (uint8_t)(rtl_r8(R8169_CHIP_CMD) &
                                     ~(R8169_CHIPCMD_TX_ENB | R8169_CHIPCMD_RX_ENB)));
    rtl_delay_ms(1);
    rtl_mod_config(R8169_MCU, R8169_MCU_NOW_IS_OOB, 0);
    rtl_mac_ocp_modify(0xE8DEu, 0x4000, 0x0000);
    rtl_wait_ll_share_fifo_ready();
    rtl_mac_ocp_modify(0xE8DEu, 0x0000, 0x8000);
    rtl_wait_ll_share_fifo_ready();
}

/** @brief 8125 代的上电初始化（`rtl_hw_init_8125`，:5501-5516）。 */
static void rtl_hw_init_8125(void) {
    rtl_hw_init_8168g();
    rtl_mac_ocp_write(0xC0AAu, 0x07D0);
    rtl_mac_ocp_write(0xC0A6u, 0x0150);
    rtl_mac_ocp_write(0xC01Eu, 0x5555);
    rtl_wait_ll_share_fifo_ready();
}

/* ------------------------------------------------------------------ *
 * 11. PHY 配置（r8169_phy_config.c 逐版本）
 * ------------------------------------------------------------------ */

/** @brief 关 ALDPS（`rtl8168g_disable_aldps`，r8169_phy_config.c:720-723）。 */
static void rtl_8168g_disable_aldps(void) {
    rtl_phy_modify_paged(0x0A43, 0x10, 0x0004, 0x0000);
}

/** @brief 打开 GPHY 的 10M 能力（`rtl8168g_enable_gphy_10m`，:725-728）。 */
static void rtl_8168g_enable_gphy_10m(void) {
    rtl_phy_modify_paged(0x0A44, 0x11, 0x0000, 0x0800);
}

/** @brief 关 EEE（`rtl8168g_config_eee_phy`，:88-91）。HBOS 不做 EEE。 */
static void rtl_8168g_config_eee_phy(void) {
    rtl_phy_modify_paged(0x0A43, 0x11, 0x0000, 0x0010);
}

/** @brief 8168h 的 EEE 配置（`rtl8168h_config_eee_phy`，:93-99）。 */
static void rtl_8168h_config_eee_phy(void) {
    rtl_8168g_config_eee_phy();
    rtl_phy_modify_paged(0x0A4A, 0x11, 0x0000, 0x0200);
    rtl_phy_modify_paged(0x0A42, 0x14, 0x0000, 0x0080);
}

/** @brief 8125 系列公共 EEE 配置（`rtl8125_common_config_eee_phy`，:101-106）。 */
static void rtl_8125_common_config_eee_phy(void) {
    rtl_phy_modify_paged(0x0A6D, 0x14, 0x0010, 0x0000);
    rtl_phy_modify_paged(0x0A42, 0x14, 0x0080, 0x0000);
    rtl_phy_modify_paged(0x0A4A, 0x11, 0x0200, 0x0000);
}

/** @brief 8125 系列 EEE 配置（`rtl8125_config_eee_phy`，:108-112）。 */
static void rtl_8125_config_eee_phy(void) {
    rtl_8168g_config_eee_phy();
    rtl_8125_common_config_eee_phy();
}

/** @brief 8125 公共配置（`rtl8125_common_config`，:101-106 不含 EEE 的部分）。 */
static void rtl_8125_legacy_force_mode(void) {
    rtl_phy_modify_paged(0x0A5B, 0x12, 0x8000, 0x0000);
}

/** @brief 8168g 的 10M ALDPS 调整（`rtl8168g_phy_adjust_10m_aldps`，:730-736）。 */
static void rtl_8168g_phy_adjust_10m_aldps(void) {
    rtl_phy_modify_paged(0x0BCC, 0x14, 0x0100, 0x0000);
    rtl_phy_modify_paged(0x0A44, 0x11, 0x0000, 0x00C0);
    rtl_phy_param_g(0x8084, 0x6000, 0x0000);
    rtl_phy_modify_paged(0x0A43, 0x10, 0x0000, 0x1003);
}

/** @brief 8169s 的 PHY magic（VER_02/03，r8169_phy_config.c:114-180）。 */
static void rtl_phy_cfg_8169s(void) {
    static const rtl_phy_reg_t regs[] = {
        { 0x1F, 0x0001 }, { 0x06, 0x006E }, { 0x08, 0x0708 }, { 0x15, 0x4000 },
        { 0x18, 0x65C7 }, { 0x1F, 0x0001 }, { 0x03, 0x00A1 }, { 0x02, 0x0008 },
        { 0x01, 0x0120 }, { 0x00, 0x1000 }, { 0x04, 0x0800 }, { 0x04, 0x0000 },
        { 0x03, 0xFF41 }, { 0x02, 0xDF60 }, { 0x01, 0x0140 }, { 0x00, 0x0077 },
        { 0x04, 0x7800 }, { 0x04, 0x7000 }, { 0x03, 0x802F }, { 0x02, 0x4F02 },
        { 0x01, 0x0409 }, { 0x00, 0xF0F9 }, { 0x04, 0x9800 }, { 0x04, 0x9000 },
        { 0x03, 0xDF01 }, { 0x02, 0xDF20 }, { 0x01, 0xFF95 }, { 0x00, 0xBA00 },
        { 0x04, 0xA800 }, { 0x04, 0xA000 }, { 0x03, 0xFF41 }, { 0x02, 0xDF20 },
        { 0x01, 0x0140 }, { 0x00, 0x00BB }, { 0x04, 0xB800 }, { 0x04, 0xB000 },
        { 0x03, 0xDF41 }, { 0x02, 0xDC60 }, { 0x01, 0x6340 }, { 0x00, 0x007D },
        { 0x04, 0xD800 }, { 0x04, 0xD000 }, { 0x03, 0xDF01 }, { 0x02, 0xDF20 },
        { 0x01, 0x100A }, { 0x00, 0xA0FF }, { 0x04, 0xF800 }, { 0x04, 0xF000 },
        { 0x1F, 0x0000 }, { 0x0B, 0x0000 }, { 0x00, 0x9200 },
    };
    rtl_phy_batch(regs, (uint32_t)(sizeof(regs) / sizeof(regs[0])));
}

/** @brief 8169sb 的 PHY magic（VER_04）。 */
static void rtl_phy_cfg_8169sb(void) { rtl_phy_write_paged(0x0002, 0x01, 0x90D0); }

/** @brief 8169scd 的 PHY magic（VER_05，:188-232）。 */
static void rtl_phy_cfg_8169scd(void) {
    static const rtl_phy_reg_t regs[] = {
        { 0x1F, 0x0001 }, { 0x04, 0x0000 }, { 0x03, 0x00A1 }, { 0x02, 0x0008 },
        { 0x01, 0x0120 }, { 0x00, 0x1000 }, { 0x04, 0x0800 }, { 0x04, 0x9000 },
        { 0x03, 0x802F }, { 0x02, 0x4F02 }, { 0x01, 0x0409 }, { 0x00, 0xF099 },
        { 0x04, 0x9800 }, { 0x04, 0xA000 }, { 0x03, 0xDF01 }, { 0x02, 0xDF20 },
        { 0x01, 0xFF95 }, { 0x00, 0xBA00 }, { 0x04, 0xA800 }, { 0x04, 0xF000 },
        { 0x03, 0xDF01 }, { 0x02, 0xDF20 }, { 0x01, 0x101A }, { 0x00, 0xA0FF },
        { 0x04, 0xF800 }, { 0x04, 0x0000 }, { 0x1F, 0x0000 }, { 0x1F, 0x0001 },
        { 0x10, 0xF41B }, { 0x14, 0xFB54 }, { 0x18, 0xF5C7 }, { 0x1F, 0x0000 },
        { 0x1F, 0x0001 }, { 0x17, 0x0CC0 }, { 0x1F, 0x0000 },
    };
    rtl_phy_batch(regs, (uint32_t)(sizeof(regs) / sizeof(regs[0])));
}

/** @brief 8169sce 的 PHY magic（VER_06，:234-286）。 */
static void rtl_phy_cfg_8169sce(void) {
    static const rtl_phy_reg_t regs[] = {
        { 0x1F, 0x0001 }, { 0x04, 0x0000 }, { 0x03, 0x00A1 }, { 0x02, 0x0008 },
        { 0x01, 0x0120 }, { 0x00, 0x1000 }, { 0x04, 0x0800 }, { 0x04, 0x9000 },
        { 0x03, 0x802F }, { 0x02, 0x4F02 }, { 0x01, 0x0409 }, { 0x00, 0xF099 },
        { 0x04, 0x9800 }, { 0x04, 0xA000 }, { 0x03, 0xDF01 }, { 0x02, 0xDF20 },
        { 0x01, 0xFF95 }, { 0x00, 0xBA00 }, { 0x04, 0xA800 }, { 0x04, 0xF000 },
        { 0x03, 0xDF01 }, { 0x02, 0xDF20 }, { 0x01, 0x101A }, { 0x00, 0xA0FF },
        { 0x04, 0xF800 }, { 0x04, 0x0000 }, { 0x1F, 0x0000 }, { 0x1F, 0x0001 },
        { 0x0B, 0x8480 }, { 0x1F, 0x0000 }, { 0x1F, 0x0001 }, { 0x18, 0x67C7 },
        { 0x04, 0x2000 }, { 0x03, 0x002F }, { 0x02, 0x4360 }, { 0x01, 0x0109 },
        { 0x00, 0x3022 }, { 0x04, 0x2800 }, { 0x1F, 0x0000 }, { 0x1F, 0x0001 },
        { 0x17, 0x0CC0 }, { 0x1F, 0x0000 },
    };
    rtl_phy_batch(regs, (uint32_t)(sizeof(regs) / sizeof(regs[0])));
}

/** @brief 8102e 的 PHY magic（VER_07/08/09，:920-934）。 */
static void rtl_phy_cfg_8102e(void) {
    static const rtl_phy_reg_t regs[] = {
        { 0x1F, 0x0003 }, { 0x08, 0x441D }, { 0x01, 0x9100 }, { 0x1F, 0x0000 },
    };
    rtl_phy_modify(0x11, 0x0000, 0x1000);
    rtl_phy_modify(0x19, 0x0000, 0x2000);
    rtl_phy_modify(0x10, 0x0000, 0x8000);
    rtl_phy_batch(regs, (uint32_t)(sizeof(regs) / sizeof(regs[0])));
}

/** @brief 8401 的 PHY magic（VER_14）。 */
static void rtl_phy_cfg_8401(void) {
    rtl_phy_modify(0x11, 0x0000, 0x1000);
    rtl_phy_modify_paged(0x0002, 0x0F, 0x0000, 0x0003);
}

/** @brief 8168b（VER_17，:288-292）。 */
static void rtl_phy_cfg_8168b(void) { rtl_phy_write_paged(0x0001, 0x10, 0xF41B); }

/** @brief 8168cp_1（VER_18，:294-299）。 */
static void rtl_phy_cfg_8168cp_1(void) {
    rtl_phy_write(0x1D, 0x0F00);
    rtl_phy_write_paged(0x0002, 0x0C, 0x1EC8);
}

/** @brief 8168cp_2（VER_23/24，:301-307）。 */
static void rtl_phy_cfg_8168cp_2(void) {
    rtl_phy_modify(0x14, 0x0000, 0x0020);
    rtl_phy_modify(0x0D, 0x0000, 0x0020);
    rtl_phy_write_paged(0x0001, 0x1D, 0x3D98);
}

/** @brief 8168c_1（VER_19，:309-336）。 */
static void rtl_phy_cfg_8168c_1(void) {
    static const rtl_phy_reg_t regs[] = {
        { 0x1F, 0x0001 }, { 0x12, 0x2300 }, { 0x1F, 0x0002 }, { 0x00, 0x88D4 },
        { 0x01, 0x82B1 }, { 0x03, 0x7002 }, { 0x08, 0x9E30 }, { 0x09, 0x01F0 },
        { 0x0A, 0x5500 }, { 0x0C, 0x00C8 }, { 0x1F, 0x0003 }, { 0x12, 0xC096 },
        { 0x16, 0x000A }, { 0x1F, 0x0000 }, { 0x1F, 0x0000 }, { 0x09, 0x2000 },
        { 0x09, 0x0000 },
    };
    rtl_phy_batch(regs, (uint32_t)(sizeof(regs) / sizeof(regs[0])));
    rtl_phy_modify(0x14, 0x0000, 0x0020);
    rtl_phy_modify(0x0D, 0x0000, 0x0020);
}

/** @brief 8168c_2（VER_20，:338-364）。 */
static void rtl_phy_cfg_8168c_2(void) {
    static const rtl_phy_reg_t regs[] = {
        { 0x1F, 0x0001 }, { 0x12, 0x2300 }, { 0x03, 0x802F }, { 0x02, 0x4F02 },
        { 0x01, 0x0409 }, { 0x00, 0xF099 }, { 0x04, 0x9800 }, { 0x04, 0x9000 },
        { 0x1D, 0x3D98 }, { 0x1F, 0x0002 }, { 0x0C, 0x7EB8 }, { 0x06, 0x0761 },
        { 0x1F, 0x0003 }, { 0x16, 0x0F0A }, { 0x1F, 0x0000 },
    };
    rtl_phy_batch(regs, (uint32_t)(sizeof(regs) / sizeof(regs[0])));
    rtl_phy_modify(0x16, 0x0000, 0x0001);
    rtl_phy_modify(0x14, 0x0000, 0x0020);
    rtl_phy_modify(0x0D, 0x0000, 0x0020);
}

/** @brief 8168c_3（VER_21/22，:366-386）。 */
static void rtl_phy_cfg_8168c_3(void) {
    static const rtl_phy_reg_t regs[] = {
        { 0x1F, 0x0001 }, { 0x12, 0x2300 }, { 0x1D, 0x3D98 }, { 0x1F, 0x0002 },
        { 0x0C, 0x7EB8 }, { 0x06, 0x5461 }, { 0x1F, 0x0003 }, { 0x16, 0x0F0A },
        { 0x1F, 0x0000 },
    };
    rtl_phy_batch(regs, (uint32_t)(sizeof(regs) / sizeof(regs[0])));
    rtl_phy_modify(0x16, 0x0000, 0x0001);
    rtl_phy_modify(0x14, 0x0000, 0x0020);
    rtl_phy_modify(0x0D, 0x0000, 0x0020);
}

/** @brief 8168d_4 的 PHY magic（VER_28，:527-533）。 */
static void rtl_phy_cfg_8168d_4(void) {
    rtl_phy_write_paged(0x0001, 0x17, 0x0CC0);
    rtl_phy_modify_extpage(0x002D, 0x18, 0xFFFF, 0x0040);
    rtl_phy_modify(0x0D, 0x0000, 0x0020);
}

/** @brief 8105e 的 PHY magic（VER_29/30，:944-956）。
 *
 * 注意原版会先关 ALDPS、`msleep(100)`、再下载固件。HBOS 不带固件，这里
 * 保留"关 ALDPS + 延时"（那是 PHY 稳定的前提），跳过固件部分。 */
static void rtl_phy_cfg_8105e(void) {
    rtl_phy_write(0x18, 0x0310);
    rtl_delay_ms(100);
    rtl_phy_write_paged(0x0005, 0x1A, 0x0000);
    rtl_phy_write_paged(0x0004, 0x1C, 0x0000);
    rtl_phy_write_paged(0x0001, 0x15, 0x7701);
}

/** @brief 8402 的 PHY magic（VER_37，:958-972，跳过固件）。 */
static void rtl_phy_cfg_8402(void) {
    rtl_phy_write(0x18, 0x0310);
    rtl_delay_ms(20);
    static const rtl_phy_reg_t regs[] = {
        { 0x1F, 0x0004 }, { 0x10, 0x401F }, { 0x19, 0x7030 }, { 0x1F, 0x0000 },
    };
    rtl_phy_batch(regs, (uint32_t)(sizeof(regs) / sizeof(regs[0])));
}

/** @brief 8106e 的 PHY magic（VER_39，:974-996，跳过固件）。 */
static void rtl_phy_cfg_8106e(void) {
    static const rtl_phy_reg_t regs[] = {
        { 0x1F, 0x0004 }, { 0x10, 0xC07F }, { 0x19, 0x7030 }, { 0x1F, 0x0000 },
    };
    rtl_phy_write(0x18, 0x0310);
    rtl_delay_ms(100);
    rtl_phy_batch(regs, (uint32_t)(sizeof(regs) / sizeof(regs[0])));
}

/** @brief 8168e_1 的 PHY magic（VER_32/33，:535-578，跳过固件）。 */
static void rtl_phy_cfg_8168e_1(void) {
    static const rtl_phy_reg_t regs[] = {
        { 0x1F, 0x0001 }, { 0x0B, 0x6C20 }, { 0x07, 0x2872 }, { 0x1C, 0xEFFF },
        { 0x1F, 0x0003 }, { 0x14, 0x6420 }, { 0x1F, 0x0000 },
    };
    rtl_phy_param_d(0x8B80, 0xFFFF, 0xC896);
    rtl_phy_batch(regs, (uint32_t)(sizeof(regs) / sizeof(regs[0])));
    rtl_phy_modify_extpage(0x002F, 0x15, 0xFFFF, 0x1919);
    rtl_phy_modify_extpage(0x00AC, 0x18, 0xFFFF, 0x0006);
    rtl_phy_modify_extpage(0x0023, 0x17, 0x0000, 0x0006);
    rtl_phy_modify_paged(0x0002, 0x08, 0x7F00, 0x8000);
    rtl_phy_modify_extpage(0x002D, 0x18, 0x0000, 0x0050);
    rtl_phy_modify(0x14, 0x0000, 0x8000);
    rtl_phy_param_d(0x8B86, 0x0000, 0x0001);
    rtl_phy_param_d(0x8B85, 0x2000, 0x0000);
    rtl_phy_modify_extpage(0x0020, 0x15, 0x1100, 0x0000);
    rtl_phy_write_paged(0x0006, 0x00, 0x5A00);
}

/** @brief 8168f 公共 PHY 部分（`rtl8168f_hw_phy_config`，:622-636）。 */
static void rtl_phy_cfg_8168f_common(void) {
    rtl_phy_param_d(0x8B80, 0x0000, 0x0006);
    rtl_phy_modify_extpage(0x002D, 0x18, 0x0000, 0x0010);
    rtl_phy_modify(0x14, 0x0000, 0x8000);
    rtl_phy_param_d(0x8B86, 0x0000, 0x0001);
    /* rtl8168f_config_eee_phy */
    rtl_phy_modify_extpage(0x0020, 0x15, 0x0000, 0x0100);
    rtl_phy_param_d(0x8B85, 0x0000, 0x2000);
}

/** @brief 8168e_2 的 PHY magic（VER_34，:580-620，跳过固件）。 */
static void rtl_phy_cfg_8168e_2(void) {
    rtl_phy_modify_extpage(0x00AC, 0x18, 0xFFFF, 0x0006);
    rtl_phy_write_paged(0x0003, 0x09, 0xA20F);
    rtl_phy_param_d(0x8B5B, 0xFFFF, 0x9222);
    rtl_phy_param_d(0x8B6D, 0xFFFF, 0x8000);
    rtl_phy_param_d(0x8B76, 0xFFFF, 0x8000);
    rtl_phy_write(0x1F, 0x0005);
    rtl_phy_write(0x05, 0x8B80);
    rtl_phy_modify(0x17, 0x0000, 0x0006);
    rtl_phy_write(0x1F, 0x0000);
    rtl_phy_modify_extpage(0x002D, 0x18, 0x0000, 0x0010);
    rtl_phy_modify(0x14, 0x0000, 0x8000);
    rtl_phy_param_d(0x8B86, 0x0000, 0x0001);
    rtl_phy_param_d(0x8B85, 0x0000, 0x4000);
    /* rtl8168f_config_eee_phy */
    rtl_phy_modify_extpage(0x0020, 0x15, 0x0000, 0x0100);
    rtl_phy_param_d(0x8B85, 0x0000, 0x2000);
    rtl_phy_write(0x1F, 0x0003);
    rtl_phy_modify(0x19, 0x0000, 0x0001);
    rtl_phy_modify(0x10, 0x0000, 0x0400);
    rtl_phy_write(0x1F, 0x0000);
    rtl_phy_modify_paged(0x0005, 0x01, 0x0000, 0x0100);
}

/** @brief 8168f_1 的 PHY magic（VER_35，:638-664，跳过固件）。 */
static void rtl_phy_cfg_8168f_1(void) {
    rtl_phy_write_paged(0x0003, 0x09, 0xA20F);
    rtl_phy_param_d(0x8B55, 0xFFFF, 0x0000);
    rtl_phy_param_d(0x8B5E, 0xFFFF, 0x0000);
    rtl_phy_param_d(0x8B67, 0xFFFF, 0x0000);
    rtl_phy_param_d(0x8B70, 0xFFFF, 0x0000);
    rtl_phy_modify_extpage(0x0078, 0x17, 0xFFFF, 0x0000);
    rtl_phy_modify_extpage(0x0078, 0x19, 0xFFFF, 0x00FB);
    rtl_phy_param_d(0x8B79, 0xFFFF, 0xAA00);
    rtl_phy_write_paged(0x0003, 0x01, 0x328A);
    rtl_phy_cfg_8168f_common();
    rtl_phy_param_d(0x8B85, 0x0000, 0x4000);
}

/** @brief 8411 的 PHY magic（VER_38，:674-718，跳过固件）。 */
static void rtl_phy_cfg_8411(void) {
    rtl_phy_cfg_8168f_common();
    rtl_phy_param_d(0x8B85, 0x0000, 0x4000);
    rtl_phy_write_paged(0x0003, 0x09, 0xA20F);
    rtl_phy_param_d(0x8B55, 0xFFFF, 0x0000);
    rtl_phy_param_d(0x8B5E, 0xFFFF, 0x0000);
    rtl_phy_param_d(0x8B67, 0xFFFF, 0x0000);
    rtl_phy_param_d(0x8B70, 0xFFFF, 0x0000);
    rtl_phy_modify_extpage(0x0078, 0x17, 0xFFFF, 0x0000);
    rtl_phy_modify_extpage(0x0078, 0x19, 0xFFFF, 0x00AA);
    rtl_phy_param_d(0x8B79, 0xFFFF, 0xAA00);
    rtl_phy_write_paged(0x0003, 0x01, 0x328A);
    rtl_phy_param_d(0x8B54, 0x0800, 0x0000);
    rtl_phy_param_d(0x8B5D, 0x0800, 0x0000);
    rtl_phy_param_d(0x8A7C, 0x0100, 0x0000);
    rtl_phy_param_d(0x8A7F, 0x0000, 0x0100);
    rtl_phy_param_d(0x8A82, 0x0100, 0x0000);
    rtl_phy_param_d(0x8A85, 0x0100, 0x0000);
    rtl_phy_param_d(0x8A88, 0x0100, 0x0000);
    rtl_phy_param_d(0x8B85, 0x0000, 0x8000);
    rtl_phy_write(0x1F, 0x0003);
    rtl_phy_modify(0x19, 0x0001, 0x0000);
    rtl_phy_modify(0x10, 0x0400, 0x0000);
    rtl_phy_write(0x1F, 0x0000);
}

/** @brief 8168g_1 的 PHY magic（VER_40，:738-784，跳过固件）。 */
static void rtl_phy_cfg_8168g_1(void) {
    uint16_t r;
    r = rtl_phy_read_paged(0x0A46, 0x10);
    rtl_phy_modify_paged(0x0BCC, 0x12, 0x8000, (r & 0x0100) ? 0x0000 : 0x8000);
    r = rtl_phy_read_paged(0x0A46, 0x13);
    rtl_phy_modify_paged(0x0C41, 0x15, (r & 0x0100) ? 0x0002 : 0x0000,
                         (r & 0x0100) ? 0x0000 : 0x0002);
    rtl_phy_modify_paged(0x0A44, 0x11, 0x0000, 0x000C);
    rtl_8168g_phy_adjust_10m_aldps();
    rtl_phy_modify_paged(0x0A4B, 0x11, 0x0000, 0x0004);
    rtl_phy_param_g(0x8012, 0x0000, 0x8000);
    rtl_phy_modify_paged(0x0C42, 0x11, 0x2000, 0x4000);
    /* Improve SWR Efficiency */
    rtl_phy_write(0x1F, 0x0BCD);
    rtl_phy_write(0x14, 0x5065);
    rtl_phy_write(0x14, 0xD065);
    rtl_phy_write(0x1F, 0x0BC8);
    rtl_phy_write(0x11, 0x5655);
    rtl_phy_write(0x1F, 0x0BCD);
    rtl_phy_write(0x14, 0x1065);
    rtl_phy_write(0x14, 0x9065);
    rtl_phy_write(0x14, 0x1065);
    rtl_phy_write(0x1F, 0x0000);
    rtl_8168g_disable_aldps();
    rtl_8168g_config_eee_phy();
}

/**
 * @brief 读 8168h 的 ADC bias ioffset（`rtl8168h_2_get_adc_bias_ioffset`，
 *        r8169_main.c:2511-2530）。
 *
 * 这个值由 MAC OCP 的 ADC 校准寄存器算出来，随后要写进 PHY 页 0x0BCF/0x16；
 * 读不到时返回 0xFFFF，调用方必须跳过而不是写 0——写 0 会把 TX LPF 的
 * 偏置拉飞。
 *
 * @return ioffset（0xFFFF 表示无效）
 */
static uint16_t rtl_8168h_get_adc_bias_ioffset(void) {
    rtl_mac_ocp_write(0xDD02u, 0x807D);
    uint16_t d1 = rtl_mac_ocp_read(0xDD02u);
    uint16_t d2 = rtl_mac_ocp_read(0xDD00u);
    (void)d1;
    return (uint16_t)((d2 >> 1) & 0x7FF8u);
}

/**
 * @brief 8168h 的 PHY magic（VER_46/48，r8169_phy_config.c:793-831）。
 *
 * **本机真机路径**。顺序不能动：CHIN EST 参数 → R-tune/PGA-retune →
 * 10M GPHY → ADC bias（页 0x0BCF/0x16）→ rlen 微调（读页 0x0BCD/0x16 低 4 位
 * 派生出 4 个半字节）→ 关 PHY PFM → 关 10M PLL off → ALDPS/EEE。
 *
 * 原版第一步是下载 PHY 固件；HBOS 不带固件（见文件头说明），直接进入
 * 后续的寄存器 magic——这两部分在实驱动里也是"固件之后再覆盖参数"的关系。
 */
static void rtl_phy_cfg_8168h_2(void) {
    rtl_phy_param_g(0x808A, 0x003F, 0x000A);
    rtl_phy_param_g(0x0811, 0x0000, 0x0800);
    rtl_phy_modify_paged(0x0A42, 0x16, 0x0000, 0x0002);
    rtl_8168g_enable_gphy_10m();

    uint16_t ioffset = rtl_8168h_get_adc_bias_ioffset();
    if (ioffset != 0xFFFFu) rtl_phy_write_paged(0x0BCF, 0x16, ioffset);

    /* rlen（TX LPF 拐点）微调：低 4 位 > 3 时取 data-3，并复制到四个半字节。 */
    uint16_t data = (uint16_t)(rtl_phy_read_paged(0x0BCD, 0x16) & 0x000Fu);
    uint16_t rlen = (data > 3u) ? (uint16_t)(data - 3u) : 0u;
    data = (uint16_t)(rlen | (rlen << 4) | (rlen << 8) | (rlen << 12));
    rtl_phy_write_paged(0x0BCD, 0x17, data);

    rtl_phy_modify_paged(0x0A44, 0x11, 0x0080, 0x0000);
    rtl_phy_modify_paged(0x0A43, 0x10, 0x0001, 0x0000);
    rtl_8168g_disable_aldps();
    rtl_8168g_config_eee_phy();
}

/** @brief 8168ep_2 的 PHY magic（VER_51，:833-881）。 */
static void rtl_phy_cfg_8168ep_2(void) {
    static const uint16_t chan[][2] = {
        { 0x80F3, 0x8B00 }, { 0x80F0, 0x3A00 }, { 0x80EF, 0x0500 },
        { 0x80F6, 0x6E00 }, { 0x80EC, 0x6800 }, { 0x80ED, 0x7C00 },
        { 0x80F2, 0xF400 }, { 0x80F4, 0x8500 }, { 0x8110, 0xA800 },
        { 0x810F, 0x1D00 }, { 0x8111, 0xF500 }, { 0x8113, 0x6100 },
        { 0x8115, 0x9200 }, { 0x810E, 0x0400 }, { 0x810C, 0x7C00 },
        { 0x810B, 0x5A00 }, { 0x80D1, 0xFF00 }, { 0x80CD, 0x9E00 },
        { 0x80D3, 0x0E00 }, { 0x80D5, 0xCA00 }, { 0x80D7, 0x8400 },
    };
    rtl_8168g_phy_adjust_10m_aldps();
    rtl_phy_param_g(0x8012, 0x0000, 0x8000);
    rtl_phy_modify_paged(0x0C42, 0x11, 0x2000, 0x4000);
    for (uint32_t i = 0; i < sizeof(chan) / sizeof(chan[0]); i++)
        rtl_phy_param_g(chan[i][0], 0xFF00, chan[i][1]);
    rtl_phy_write(0x1F, 0x0BCD);
    rtl_phy_write(0x14, 0x5065);
    rtl_phy_write(0x14, 0xD065);
    rtl_phy_write(0x1F, 0x0BC8);
    rtl_phy_write(0x12, 0x00ED);
    rtl_phy_write(0x1F, 0x0BCD);
    rtl_phy_write(0x14, 0x1065);
    rtl_phy_write(0x14, 0x9065);
    rtl_phy_write(0x14, 0x1065);
    rtl_phy_write(0x1F, 0x0000);
    rtl_8168g_disable_aldps();
    rtl_8168g_config_eee_phy();
}

/** @brief 8117 的 PHY magic（VER_52，:883-918）。 */
static void rtl_phy_cfg_8117(void) {
    static const uint16_t chan[][2] = {
        { 0x808E, 0x4800 }, { 0x8090, 0xCC00 }, { 0x8092, 0xB000 },
        { 0x8088, 0x6000 }, { 0x808B, 0x0B00 }, { 0x808D, 0x0600 },
        { 0x808C, 0xB000 }, { 0x80A0, 0x2800 }, { 0x80A2, 0x5000 },
        { 0x809B, 0xB000 }, { 0x809A, 0x4B00 }, { 0x809D, 0x0800 },
        { 0x80A1, 0x7000 }, { 0x809F, 0x0300 }, { 0x809E, 0x8800 },
        { 0x80B2, 0x2200 }, { 0x80AD, 0x9800 }, { 0x80AF, 0x0800 },
        { 0x80B3, 0x6F00 }, { 0x80B1, 0x0300 }, { 0x80B0, 0x9300 },
    };
    static const uint16_t chan_mask[][2] = {
        { 0x808B, 0x3F00 }, { 0x808D, 0x1F00 }, { 0x809B, 0xF800 },
        { 0x809D, 0x3F00 }, { 0x809F, 0x1F00 }, { 0x80AD, 0xF800 },
        { 0x80AF, 0x3F00 }, { 0x80B1, 0x1F00 },
    };
    for (uint32_t i = 0; i < sizeof(chan) / sizeof(chan[0]); i++) {
        uint16_t mask = 0xFF00;
        for (uint32_t k = 0; k < sizeof(chan_mask) / sizeof(chan_mask[0]); k++)
            if (chan_mask[k][0] == chan[i][0]) mask = chan_mask[k][1];
        rtl_phy_param_g(chan[i][0], mask, chan[i][1]);
    }
    rtl_phy_param_g(0x8011, 0x0000, 0x0800);
    rtl_8168g_enable_gphy_10m();
    rtl_phy_param_g(0x8016, 0x0000, 0x0400);
    rtl_8168g_disable_aldps();
    rtl_8168h_config_eee_phy();
}

/** @brief 8125A 的 PHY magic（VER_61，:998-1058，跳过固件）。 */
static void rtl_phy_cfg_8125a(void) {
    rtl_phy_modify_paged(0x0AD4, 0x17, 0x0000, 0x0010);
    rtl_phy_modify_paged(0x0AD1, 0x13, 0x03FF, 0x03FF);
    rtl_phy_modify_paged(0x0AD3, 0x11, 0x003F, 0x0006);
    rtl_phy_modify_paged(0x0AC0, 0x14, 0x1100, 0x0000);
    rtl_phy_modify_paged(0x0ACC, 0x10, 0x0003, 0x0002);
    rtl_phy_modify_paged(0x0AD4, 0x10, 0x00E7, 0x0044);
    rtl_phy_modify_paged(0x0AC1, 0x12, 0x0080, 0x0000);
    rtl_phy_modify_paged(0x0AC8, 0x10, 0x0300, 0x0000);
    rtl_phy_modify_paged(0x0AC5, 0x17, 0x0007, 0x0002);
    rtl_phy_write_paged(0x0AD4, 0x16, 0x00A8);
    rtl_phy_write_paged(0x0AC5, 0x16, 0x01FF);
    rtl_phy_modify_paged(0x0AC8, 0x15, 0x00F0, 0x0030);
    rtl_phy_param_8125(0x80A2, 0xFFFF, 0x0153);
    rtl_phy_param_8125(0x809C, 0xFFFF, 0x0153);
    /* TX FIR 系数表：写 0x1F=0x0A43、0x13=0x81B3，然后 16 个系数 + 25 个 0。 */
    static const uint16_t coeff[] = {
        0x0043, 0x00A7, 0x00D6, 0x00EC, 0x00F6, 0x00FB, 0x00FD, 0x00FF,
        0x00BB, 0x0058, 0x0029, 0x0013, 0x0009, 0x0004, 0x0002, 0x0000,
    };
    rtl_phy_write(0x1F, 0x0A43);
    rtl_phy_write(0x13, 0x81B3);
    rtl_phy_write(0x14, 0x0043);
    for (uint32_t i = 1; i < sizeof(coeff) / sizeof(coeff[0]); i++)
        rtl_phy_write(0x14, coeff[i]);
    for (uint32_t i = 0; i < 25; i++) rtl_phy_write(0x14, 0x0000);
    rtl_phy_write(0x1F, 0x0000);
    rtl_phy_param_g(0x8257, 0xFFFF, 0x020F);
    rtl_phy_param_g(0x80EA, 0xFFFF, 0x7843);
    rtl_phy_modify_paged(0x0D06, 0x14, 0x0000, 0x2000);
    rtl_phy_param_g(0x81A2, 0x0000, 0x0100);
    rtl_phy_modify_paged(0x0B54, 0x16, 0xFF00, 0xDB00);
    rtl_phy_modify_paged(0x0A45, 0x12, 0x0001, 0x0000);
    rtl_phy_modify_paged(0x0A5D, 0x12, 0x0000, 0x0020);
    rtl_phy_modify_paged(0x0AD4, 0x17, 0x0010, 0x0000);
    rtl_phy_modify_paged(0x0A86, 0x15, 0x0001, 0x0000);
    rtl_8168g_enable_gphy_10m();
    rtl_8168g_disable_aldps();
    rtl_8125_config_eee_phy();
}

/** @brief 8125B 的 PHY magic（VER_63，:1060-1093，跳过固件）。 */
static void rtl_phy_cfg_8125b(void) {
    rtl_8168g_enable_gphy_10m();
    rtl_phy_modify_paged(0x0AC4, 0x13, 0x00F0, 0x0090);
    rtl_phy_modify_paged(0x0AD3, 0x10, 0x0003, 0x0001);
    rtl_phy_param_8125(0x80F5, 0xFFFF, 0x760E);
    rtl_phy_param_8125(0x8107, 0xFFFF, 0x360E);
    rtl_phy_param_8125(0x8551, 0xFF00, 0x0800);
    rtl_phy_modify_paged(0x0BF0, 0x10, 0xE000, 0xA000);
    rtl_phy_modify_paged(0x0BF4, 0x13, 0x0F00, 0x0300);
    for (uint16_t reg = 0x8044; reg <= 0x807A; reg = (uint16_t)(reg + 6))
        rtl_phy_param_g(reg, 0xFFFF, 0x2417);
    rtl_phy_modify_paged(0x0A4C, 0x15, 0x0000, 0x0040);
    rtl_phy_modify_paged(0x0BF8, 0x12, 0xE000, 0xA000);
    rtl_8125_legacy_force_mode();
    rtl_8168g_disable_aldps();
    rtl_8125_config_eee_phy();
}

/** @brief 8125D/K/9151A（VER_64，:1095-1103，跳过固件）。 */
static void rtl_phy_cfg_8125d(void) {
    rtl_8168g_enable_gphy_10m();
    rtl_8125_legacy_force_mode();
    rtl_8168g_disable_aldps();
    rtl_8125_config_eee_phy();
}

/** @brief 8125BP（VER_66，:1105-1121，跳过固件）。 */
static void rtl_phy_cfg_8125bp(void) {
    rtl_8168g_enable_gphy_10m();
    rtl_phy_param_g(0x8010, 0x0800, 0x0000);
    rtl_phy_param_8125(0x8088, 0xFF00, 0x9000);
    rtl_phy_param_8125(0x808F, 0xFF00, 0x9000);
    rtl_phy_param_g(0x8174, 0x2000, 0x1800);
    rtl_8125_legacy_force_mode();
    rtl_8168g_disable_aldps();
    rtl_8125_config_eee_phy();
}

/** @brief 8126A（VER_70，:1123-1131，跳过固件）。 */
static void rtl_phy_cfg_8126a(void) {
    rtl_8168g_enable_gphy_10m();
    rtl_8125_legacy_force_mode();
    rtl_8168g_disable_aldps();
    rtl_8125_common_config_eee_phy();
}

/** @brief 8127A（VER_80）：本驱动只做公共部分（原版还有约 150 行 magic，
 *         10G/SFP 场景远超 HBOS 目标，未移植）。 */
static void rtl_phy_cfg_8127a(void) {
    rtl_8168g_enable_gphy_10m();
    rtl_8125_legacy_force_mode();
    rtl_8168g_disable_aldps();
    rtl_8125_common_config_eee_phy();
}

/**
 * @brief 按 MAC 版本分派 PHY 配置（`r8169_hw_phy_config`，
 *        r8169_phy_config.c:1298-1354）。
 *
 * 这是真机能否链路起来的关键：同一段 magic 写到错误版本的 PHY 上会导致
 * 链路不稳/掉速/丢包，所以这里逐版本对应，不做"取个近似版本"的简化。
 */
static void rtl_hw_phy_config(void) {
    switch (g_mac_version) {
    case RTL_GIGA_MAC_VER_02:
    case RTL_GIGA_MAC_VER_03: rtl_phy_cfg_8169s(); break;
    case RTL_GIGA_MAC_VER_04: rtl_phy_cfg_8169sb(); break;
    case RTL_GIGA_MAC_VER_05: rtl_phy_cfg_8169scd(); break;
    case RTL_GIGA_MAC_VER_06: rtl_phy_cfg_8169sce(); break;
    case RTL_GIGA_MAC_VER_07:
    case RTL_GIGA_MAC_VER_08:
    case RTL_GIGA_MAC_VER_09: rtl_phy_cfg_8102e(); break;
    case RTL_GIGA_MAC_VER_10: break;                     /* 无 PHY 配置 */
    case RTL_GIGA_MAC_VER_14: rtl_phy_cfg_8401(); break;
    case RTL_GIGA_MAC_VER_17: rtl_phy_cfg_8168b(); break;
    case RTL_GIGA_MAC_VER_18: rtl_phy_cfg_8168cp_1(); break;
    case RTL_GIGA_MAC_VER_19: rtl_phy_cfg_8168c_1(); break;
    case RTL_GIGA_MAC_VER_20: rtl_phy_cfg_8168c_2(); break;
    case RTL_GIGA_MAC_VER_21:
    case RTL_GIGA_MAC_VER_22: rtl_phy_cfg_8168c_3(); break;
    case RTL_GIGA_MAC_VER_23:
    case RTL_GIGA_MAC_VER_24: rtl_phy_cfg_8168cp_2(); break;
    case RTL_GIGA_MAC_VER_25:
    case RTL_GIGA_MAC_VER_26: break;   /* 8168d 依赖固件，见文件头说明 */
    case RTL_GIGA_MAC_VER_28: rtl_phy_cfg_8168d_4(); break;
    case RTL_GIGA_MAC_VER_29:
    case RTL_GIGA_MAC_VER_30: rtl_phy_cfg_8105e(); break;
    case RTL_GIGA_MAC_VER_31: break;                     /* 无 PHY 配置 */
    case RTL_GIGA_MAC_VER_32:
    case RTL_GIGA_MAC_VER_33: rtl_phy_cfg_8168e_1(); break;
    case RTL_GIGA_MAC_VER_34: rtl_phy_cfg_8168e_2(); break;
    case RTL_GIGA_MAC_VER_35: rtl_phy_cfg_8168f_1(); break;
    case RTL_GIGA_MAC_VER_36: rtl_phy_cfg_8168f_common(); break;
    case RTL_GIGA_MAC_VER_37: rtl_phy_cfg_8402(); break;
    case RTL_GIGA_MAC_VER_38: rtl_phy_cfg_8411(); break;
    case RTL_GIGA_MAC_VER_39: rtl_phy_cfg_8106e(); break;
    case RTL_GIGA_MAC_VER_40: rtl_phy_cfg_8168g_1(); break;
    case RTL_GIGA_MAC_VER_42:
    case RTL_GIGA_MAC_VER_43:
    case RTL_GIGA_MAC_VER_44: rtl_8168g_config_eee_phy(); break;
    case RTL_GIGA_MAC_VER_46:
    case RTL_GIGA_MAC_VER_48: rtl_phy_cfg_8168h_2(); break;
    case RTL_GIGA_MAC_VER_51: rtl_phy_cfg_8168ep_2(); break;
    case RTL_GIGA_MAC_VER_52: rtl_phy_cfg_8117(); break;
    case RTL_GIGA_MAC_VER_61: rtl_phy_cfg_8125a(); break;
    case RTL_GIGA_MAC_VER_63: rtl_phy_cfg_8125b(); break;
    case RTL_GIGA_MAC_VER_64: rtl_phy_cfg_8125d(); break;
    case RTL_GIGA_MAC_VER_66: rtl_phy_cfg_8125bp(); break;
    case RTL_GIGA_MAC_VER_70: rtl_phy_cfg_8126a(); break;
    case RTL_GIGA_MAC_VER_80: rtl_phy_cfg_8127a(); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ *
 * 12. 芯片识别
 * ------------------------------------------------------------------ */

/** @brief XID → MAC 版本表（照抄 rtl_chip_infos[]，r8169_main.c:98-214）。 */
typedef struct {
    uint32_t mask;
    uint32_t val;
    rtl_giga_mac_ver_t ver;
    const char *name;
} rtl_chip_info_t;

static const rtl_chip_info_t g_chip_infos[] = {
    /* 8127A */
    { 0x7CF, 0x6C9, RTL_GIGA_MAC_VER_80, "RTL8127A" },
    /* 8126A */
    { 0x7CF, 0x64A, RTL_GIGA_MAC_VER_70, "RTL8126A" },
    { 0x7CF, 0x649, RTL_GIGA_MAC_VER_70, "RTL8126A" },
    /* 8125BP */
    { 0x7CF, 0x681, RTL_GIGA_MAC_VER_66, "RTL8125BP" },
    /* 8125D 家族 */
    { 0x7CF, 0x68B, RTL_GIGA_MAC_VER_64, "RTL9151A" },
    { 0x7CF, 0x68A, RTL_GIGA_MAC_VER_64, "RTL8125K" },
    { 0x7CF, 0x689, RTL_GIGA_MAC_VER_64, "RTL8125D" },
    { 0x7CF, 0x688, RTL_GIGA_MAC_VER_64, "RTL8125D" },
    /* 8125B / 8125A */
    { 0x7CF, 0x641, RTL_GIGA_MAC_VER_63, "RTL8125B" },
    { 0x7CF, 0x609, RTL_GIGA_MAC_VER_61, "RTL8125A" },
    /* 8117 / 8168ep */
    { 0x7CF, 0x54B, RTL_GIGA_MAC_VER_52, "RTL8168fp/RTL8117" },
    { 0x7CF, 0x54A, RTL_GIGA_MAC_VER_52, "RTL8168fp/RTL8117" },
    { 0x7CF, 0x502, RTL_GIGA_MAC_VER_51, "RTL8168ep/8111ep" },
    /* 8168H */
    { 0x7CF, 0x541, RTL_GIGA_MAC_VER_46, "RTL8168h/8111h" },
    { 0x7CF, 0x6C0, RTL_GIGA_MAC_VER_46, "RTL8168M" },
    /* 8168G */
    { 0x7CF, 0x5C8, RTL_GIGA_MAC_VER_44, "RTL8411b" },
    { 0x7CF, 0x509, RTL_GIGA_MAC_VER_42, "RTL8168gu/8111gu" },
    { 0x7CF, 0x4C0, RTL_GIGA_MAC_VER_40, "RTL8168g/8111g" },
    /* 8168F */
    { 0x7C8, 0x488, RTL_GIGA_MAC_VER_38, "RTL8411" },
    { 0x7CF, 0x481, RTL_GIGA_MAC_VER_36, "RTL8168f/8111f" },
    { 0x7CF, 0x480, RTL_GIGA_MAC_VER_35, "RTL8168f/8111f" },
    /* 8168E */
    { 0x7C8, 0x2C8, RTL_GIGA_MAC_VER_34, "RTL8168evl/8111evl" },
    { 0x7CF, 0x2C1, RTL_GIGA_MAC_VER_32, "RTL8168e/8111e" },
    { 0x7C8, 0x2C0, RTL_GIGA_MAC_VER_33, "RTL8168e/8111e" },
    /* 8168D */
    { 0x7CF, 0x281, RTL_GIGA_MAC_VER_25, "RTL8168d/8111d" },
    { 0x7C8, 0x280, RTL_GIGA_MAC_VER_26, "RTL8168d/8111d" },
    /* 8168DP */
    { 0x7CF, 0x28A, RTL_GIGA_MAC_VER_28, "RTL8168dp/8111dp" },
    { 0x7CF, 0x28B, RTL_GIGA_MAC_VER_31, "RTL8168dp/8111dp" },
    /* 8168C */
    { 0x7CF, 0x3C9, RTL_GIGA_MAC_VER_23, "RTL8168cp/8111cp" },
    { 0x7CF, 0x3C8, RTL_GIGA_MAC_VER_18, "RTL8168cp/8111cp" },
    { 0x7C8, 0x3C8, RTL_GIGA_MAC_VER_24, "RTL8168cp/8111cp" },
    { 0x7CF, 0x3C0, RTL_GIGA_MAC_VER_19, "RTL8168c/8111c" },
    { 0x7CF, 0x3C2, RTL_GIGA_MAC_VER_20, "RTL8168c/8111c" },
    { 0x7CF, 0x3C3, RTL_GIGA_MAC_VER_21, "RTL8168c/8111c" },
    { 0x7C8, 0x3C0, RTL_GIGA_MAC_VER_22, "RTL8168c/8111c" },
    /* 8168B */
    { 0x7C8, 0x380, RTL_GIGA_MAC_VER_17, "RTL8168b/8111b" },
    /* 8101 家族 */
    { 0x7C8, 0x448, RTL_GIGA_MAC_VER_39, "RTL8106e" },
    { 0x7C8, 0x440, RTL_GIGA_MAC_VER_37, "RTL8402" },
    { 0x7CF, 0x409, RTL_GIGA_MAC_VER_29, "RTL8105e" },
    { 0x7C8, 0x408, RTL_GIGA_MAC_VER_30, "RTL8105e" },
    { 0x7CF, 0x349, RTL_GIGA_MAC_VER_08, "RTL8102e" },
    { 0x7CF, 0x249, RTL_GIGA_MAC_VER_08, "RTL8102e" },
    { 0x7CF, 0x348, RTL_GIGA_MAC_VER_07, "RTL8102e" },
    { 0x7CF, 0x248, RTL_GIGA_MAC_VER_07, "RTL8102e" },
    { 0x7CF, 0x240, RTL_GIGA_MAC_VER_14, "RTL8401" },
    { 0x7C8, 0x348, RTL_GIGA_MAC_VER_09, "RTL8102e/RTL8103e" },
    { 0x7C8, 0x248, RTL_GIGA_MAC_VER_09, "RTL8102e/RTL8103e" },
    { 0x7C8, 0x340, RTL_GIGA_MAC_VER_10, "RTL8101e/RTL8100e" },
    /* 8110 家族 */
    { 0xFC8, 0x980, RTL_GIGA_MAC_VER_06, "RTL8169sc/8110sc" },
    { 0xFC8, 0x180, RTL_GIGA_MAC_VER_05, "RTL8169sc/8110sc" },
    { 0xFC8, 0x100, RTL_GIGA_MAC_VER_04, "RTL8169sb/8110sb" },
    { 0xFC8, 0x040, RTL_GIGA_MAC_VER_03, "RTL8110s" },
    { 0xFC8, 0x008, RTL_GIGA_MAC_VER_02, "RTL8169s" },
};

/**
 * @brief XID → 芯片条目（`rtl8169_get_chip_version`，r8169_main.c:2442-2466）。
 *
 * 线性匹配 `(xid & mask) == val`；表尾的 `{0,0}` 条目保证一定能命中，
 * 所以不会越界（Linux 靠同一个兜底条目退出 while）。
 *
 * 两个用 `supports_gmii` 分流的特例：VER_42 + 百兆 PHY = RTL8106eus
 * （按 VER_43 处理）；VER_46 + 百兆 PHY = RTL8107e（按 VER_48 处理）。
 * `supports_gmii == 0` 只由 device id 0x8136 触发。
 */
static const rtl_chip_info_t *rtl_chip_lookup(uint32_t xid) {
    static const rtl_chip_info_t none = { 0, 0, RTL_GIGA_MAC_NONE, "unknown" };
    static const rtl_chip_info_t gmii_100 = { 0, 0, RTL_GIGA_MAC_VER_43, "RTL8106eus" };
    static const rtl_chip_info_t gmii_100b = { 0, 0, RTL_GIGA_MAC_VER_48, "RTL8107e" };
    for (uint32_t i = 0; i < sizeof(g_chip_infos) / sizeof(g_chip_infos[0]); i++) {
        if ((xid & g_chip_infos[i].mask) == g_chip_infos[i].val) {
            if (g_chip_infos[i].ver == RTL_GIGA_MAC_VER_42 && !g_supports_gmii)
                return &gmii_100;
            if (g_chip_infos[i].ver == RTL_GIGA_MAC_VER_46 && !g_supports_gmii)
                return &gmii_100b;
            return &g_chip_infos[i];
        }
    }
    return &none;
}

/* ------------------------------------------------------------------ *
 * 13. device id 表（rtl8169_pci_tbl[]，r8169_main.c:224-248）
 * ------------------------------------------------------------------ */

/**
 * @brief 探测表的一条：厂商 + 设备 + 驱动名 + 是否支持千兆。
 *
 * 表尾 `val = 0` 的兜底条目把未知 device id 判为"不支持"。
 * `supported` 为 0 时不参与匹配。
 */
typedef struct {
    uint16_t vendor;
    uint16_t device;
    uint8_t supported;
    uint8_t gmii;      /**< 0 = 百兆 PHY（对应 Linux 的 RTL_CFG_NO_GBIT） */
    const char *name;
} rtl_pci_id_t;

static const rtl_pci_id_t g_pci_ids[] = {
    { 0x10EC, 0x2502, 1, 1, "RTL8168/8111" },
    { 0x10EC, 0x2600, 1, 1, "RTL8168/8111" },
    { 0x10EC, 0x8129, 1, 1, "RTL8168/8111" },
    { 0x10EC, 0x8136, 1, 0, "RTL810x/8136" },   /* 唯一的 RTL_CFG_NO_GBIT */
    { 0x10EC, 0x8161, 1, 1, "RTL8168/8111" },
    { 0x10EC, 0x8162, 1, 1, "RTL8168/8111" },
    { 0x10EC, 0x8167, 1, 1, "RTL8168/8111" },
    { 0x10EC, 0x8168, 1, 1, "RTL8168/8111" },
    { 0x1A03, 0x8168, 1, 1, "RTL8168/8111" },   /* NCUBE 贴牌 */
    { 0x10EC, 0x8169, 1, 1, "RTL8169/8110" },
    { 0x1186, 0x4300, 1, 1, "RTL8169/8110" },   /* D-Link 贴牌 */
    { 0x1186, 0x4302, 1, 1, "RTL8169/8110" },
    { 0x1113, 0xC107, 1, 1, "RTL8169/8110" },   /* Accton 贴牌 */
    { 0x16EC, 0x0116, 1, 1, "RTL8169/8110" },   /* USR 贴牌 */
    { 0x1737, 0x1032, 1, 1, "RTL8169/8110" },   /* Linksys 贴牌 */
    { 0x0001, 0x8168, 1, 1, "RTL8168/8111" },   /* 贴牌 */
    { 0x10EC, 0x8125, 1, 1, "RTL8125" },
    { 0x10EC, 0x8126, 1, 1, "RTL8126" },
    { 0x10EC, 0x8127, 1, 1, "RTL8127" },
    { 0x10EC, 0x3000, 1, 1, "RTL8125" },
    { 0x10EC, 0x5000, 1, 1, "RTL8125" },
    { 0x10EC, 0x0E10, 1, 1, "RTL8125" },
};

/** @brief 查 device id 表；返回 NULL 表示不支持。 */
static const rtl_pci_id_t *rtl_pci_lookup(uint16_t vendor, uint16_t device) {
    for (uint32_t i = 0; i < sizeof(g_pci_ids) / sizeof(g_pci_ids[0]); i++) {
        if (g_pci_ids[i].vendor == vendor && g_pci_ids[i].device == device)
            return &g_pci_ids[i];
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * 14. 公共 API
 * ------------------------------------------------------------------ */

int r8169_probe(uint16_t vendor, uint16_t device) {
    return rtl_pci_lookup(vendor, device) ? 1 : 0;
}

const char *r8169_driver_name(void) { return "Realtek RTL8168/8111"; }

const char *r8169_device_name(void) { return g_device_name; }

int r8169_device_mac_version(void) { return (int)g_mac_version; }

/* ------------------------------------------------------------------ *
 * 15. BAR 选择与映射
 * ------------------------------------------------------------------ */

/**
 * @brief 释放已映射的 MMIO 窗口（按获得顺序逆序）。
 *
 * 只处理 `vmm_map_mmio()` 成功返回过的页；`g_mmio` 与页表项在
 * `vmm_unmap_page()` 之后不再被引用。
 */
static void rtl_unmap_window(void) {
    while (g_mapped_count) {
        g_mapped_count--;
        vmm_unmap_page((uint64_t)(uintptr_t)g_mapped_pages[g_mapped_count]);
        g_mapped_pages[g_mapped_count] = 0;
    }
    g_mmio = 0;
    g_mapped_bytes = 0;
}

/**
 * @brief 映射 @p need 字节的 MMIO 窗口（按 4 KiB 页分块）。
 *
 * 不假设 BAR 大小：先按 4 KiB 逐页映射直到够用。8125/8126/8127 用到
 * 0x60B0，因此 @p need 至少 0x7000；其它型号 4 KiB 就够。
 *
 * @param phys 窗口物理基址
 * @param need 需要的字节数
 * @return 0 成功，-1 失败（已映射的页原地释放）
 */
static int rtl_map_window(uint64_t phys, uint32_t need) {
    uint32_t pages = (need + 0xFFFu) / 0x1000u;
    if (pages == 0) pages = 1;
    if (pages > sizeof(g_mapped_pages) / sizeof(g_mapped_pages[0])) return -1;
    for (uint32_t i = 0; i < pages; i++) {
        void *p = vmm_map_mmio(phys + (uint64_t)i * 0x1000u, 0x1000u);
        if (!p) { rtl_unmap_window(); return -1; }
        g_mapped_pages[g_mapped_count++] = p;
    }
    g_mmio = (volatile uint8_t *)g_mapped_pages[0];
    g_mapped_bytes = pages * 0x1000u;
    return 0;
}

/**
 * @brief 选出寄存器 BAR：优先内存 BAR，其次 I/O BAR。
 *
 * Linux 只用最低编号的 IORESOURCE_MEM（r8169_main.c:5634-5641），因为
 * r8169 从没在 I/O 空间上跑过；但任务要求兼容"只有 I/O BAR"的
 * 8169/8101 老卡，所以这里先扫内存 BAR，一个都没有再退回 I/O BAR。
 * 两种通道共用同一套 rtl_r 系列与 rtl_w 系列存取函数，上层分支为零。
 *
 * @param dev 目标设备
 * @return 0 成功（`g_is_io` / `g_mmio` / `g_io_base` 已就位），-1 失败
 */
static int rtl_pick_bar(const pci_device_t *dev) {
    for (uint8_t i = 0; i < 6; i++) {
        int is_io = 0;
        uint64_t base = pci_bar_address(dev->bus, dev->slot, dev->func, i, &is_io);
        if (!base || is_io) continue;
        /* 8125 家族需要 0x60B0 → 0x7000；其余 4 KiB 够。 */
        if (rtl_map_window(base, 0x7000u) < 0) return -1;
        g_is_io = false;
        return 0;
    }
    for (uint8_t i = 0; i < 6; i++) {
        int is_io = 0;
        uint64_t base = pci_bar_address(dev->bus, dev->slot, dev->func, i, &is_io);
        if (!base || !is_io) continue;
        g_io_base = (uint16_t)base;
        g_is_io = true;
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 * 16. 初始化序列
 * ------------------------------------------------------------------ */

/** @brief 屏蔽并清除全部中断（`rtl8169_irq_mask_and_ack`，r8169_main.c:1664-1668）。
 *
 * 纯轮询也必须做：`IntrStatus` 是**写 1 清除**的，留着会一直置位；
 * 8125 上这两个寄存器的偏移与位宽都不同（0x38/0x3C，32 位）。 */
static void rtl_irq_mask_and_ack(void) {
    if (rtl_is_8125()) {
        rtl_w32(R8169_8125_INTR_MASK, 0);
        rtl_w32(R8169_8125_INTR_STATUS, 0xFFFFFFFFu);
    } else {
        rtl_w16(R8169_INTR_MASK, 0);
        rtl_w16(R8169_INTR_STATUS, 0xFFFFu);
    }
    rtl_pci_commit();
}

/** @brief 软复位（`rtl_hw_reset`，r8169_main.c:2671-2676）。
 *  写 `ChipCmd.CmdReset` 后必须等它自清，否则后续寄存器访问全被忽略。 */
static int rtl_hw_reset(void) {
    rtl_w8(R8169_CHIP_CMD, (uint8_t)R8169_CHIPCMD_RESET);
    return rtl_wait_bits8(R8169_CHIP_CMD, (uint8_t)R8169_CHIPCMD_RESET, 0, 100, 100);
}

/** @brief 按版本执行上电初始化（`rtl_hw_initialize`，r8169_main.c:5518-5533）。 */
static void rtl_hw_initialize(void) {
    if (g_mac_version >= RTL_GIGA_MAC_VER_61) {
        rtl_hw_init_8125();
    } else if (g_mac_version >= RTL_GIGA_MAC_VER_40 && g_mac_version <= RTL_GIGA_MAC_VER_52) {
        rtl_hw_init_8168g();
    }
}

/**
 * @brief 写 RxConfig 初值（`rtl_init_rxcfg`，r8169_main.c:2589-2615）。
 *
 * 这一段决定 FIFO 阈值、PCI 突发长度、以及 8125 的取帧深度；写错的表现是
 * 收包吞吐极低或直接收不到。L1/L2/L3/L4 的 accept 位不在这里，由
 * `rtl_set_rx_mode()` 补。
 */
static void rtl_init_rxcfg(void) {
    uint32_t val;
    if (g_mac_version >= RTL_GIGA_MAC_VER_63) {
        val = R8169_RXCFG_FETCH_8125 | R8169_RXCFG_DMA_BURST | R8169_RXCFG_PAUSE_SLOT;
    } else if (g_mac_version == RTL_GIGA_MAC_VER_61) {
        val = R8169_RXCFG_FETCH_8125 | R8169_RXCFG_DMA_BURST;
    } else if (g_mac_version <= RTL_GIGA_MAC_VER_06 ||
               (g_mac_version >= RTL_GIGA_MAC_VER_10 && g_mac_version <= RTL_GIGA_MAC_VER_17)) {
        val = R8169_RXCFG_FIFO_THRESH | R8169_RXCFG_DMA_BURST;
    } else if ((g_mac_version >= RTL_GIGA_MAC_VER_18 && g_mac_version <= RTL_GIGA_MAC_VER_24) ||
               (g_mac_version >= RTL_GIGA_MAC_VER_34 && g_mac_version <= RTL_GIGA_MAC_VER_36) ||
               g_mac_version == RTL_GIGA_MAC_VER_38) {
        val = R8169_RXCFG_128_INT_EN | R8169_RXCFG_MULTI_EN | R8169_RXCFG_DMA_BURST;
    } else if (g_mac_version >= RTL_GIGA_MAC_VER_40 && g_mac_version <= RTL_GIGA_MAC_VER_52) {
        val = R8169_RXCFG_128_INT_EN | R8169_RXCFG_MULTI_EN | R8169_RXCFG_DMA_BURST |
              R8169_RXCFG_EARLY_OFF;
    } else {
        val = R8169_RXCFG_128_INT_EN | R8169_RXCFG_DMA_BURST;
    }
    rtl_w32(R8169_RX_CONFIG, val);
}

/** @brief 写 TxConfig（`rtl_set_tx_config_registers`，r8169_main.c:2783-2792）。
 *  `TXCFG_AUTO_FIFO` 只对 8168e-vl..8168ep 这一代有意义。 */
static void rtl_set_tx_config(void) {
    uint32_t val = (7u << 8) | (3u << 24);
    if (rtl_is_8168evl_up()) val |= (1u << 7);
    rtl_w32(R8169_TX_CONFIG, val);
}

/** @brief 接收过滤 + 组播哈希（`rtl_set_rx_mode` 的简化版，r8169.main.c:2830-2868）。
 *
 * HBOS 不做组播过滤表，直接放行广播/组播/本机地址（0x0E），并把 MAR0/MAR4
 * 写全 1 表示"不移交哈希过滤"。注意写 `RxConfig` 的 accept 位时要保留高位
 * 配置（0x3F 掩码之外的原值）。 */
static void rtl_set_rx_mode(void) {
    rtl_w32(R8169_MAR0, 0xFFFFFFFFu);
    rtl_w32(R8169_MAR0 + 4u, 0xFFFFFFFFu);
    uint32_t tmp = rtl_r32(R8169_RX_CONFIG);
    rtl_w32(R8169_RX_CONFIG, (tmp & ~R8169_RXCFG_ACCEPT_MASK) | R8169_RXCFG_ACCEPT_MODE);
}

/** @brief 读 MAC 地址（`rtl_read_mac_address` + `rtl_read_mac_from_reg`）。
 *
 * 三个来源按优先级：8168evl..8168ep 从 ERI 0xE0/0xE4 取；8125 从
 * `MAC0_BKP`(0x19E0) 取；其余（以及取到全 0 时）回退到 `MAC0`。
 * 注意这一步必须在 `Cfg9346` 解锁状态下做。 */
static void rtl_read_mac(uint8_t mac[6]) {
    bool got = false;
    if (rtl_is_8168evl_up() && g_mac_version != RTL_GIGA_MAC_VER_34) {
        uint32_t lo = rtl_eri_read(0xE0);
        uint32_t hi = rtl_eri_read(0xE4);
        mac[0] = (uint8_t)(lo & 0xFFu);
        mac[1] = (uint8_t)((lo >> 8) & 0xFFu);
        mac[2] = (uint8_t)((lo >> 16) & 0xFFu);
        mac[3] = (uint8_t)((lo >> 24) & 0xFFu);
        mac[4] = (uint8_t)(hi & 0xFFu);
        mac[5] = (uint8_t)((hi >> 8) & 0xFFu);
        got = (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) != 0;
    } else if (rtl_is_8125()) {
        for (int i = 0; i < 6; i++) mac[i] = rtl_r8((uint16_t)(R8169_8125_MAC0_BKP + (uint16_t)i));
        got = (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) != 0;
    }
    if (!got) {
        for (int i = 0; i < 6; i++) mac[i] = rtl_r8((uint16_t)(R8169_MAC0 + (uint16_t)i));
    }
}

/** @brief 写 MAC 地址（`rtl_rar_set`，r8169_main.c:2559-2573）。
 *
 * 顺序有讲究：先 `MAC4`（高 16 位）再 `MAC0`（低 32 位），中间各插一次
 * `rtl_pci_commit()`——芯片把这两次写当作一个整体生效。 */
static void rtl_write_mac(const uint8_t mac[6]) {
    rtl_w32(R8169_MAC4, (uint32_t)mac[4] | ((uint32_t)mac[5] << 8));
    rtl_pci_commit();
    rtl_w32(R8169_MAC0, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
                        ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24));
    rtl_pci_commit();
}

/** @brief 清空两个描述符环并把 RX 缓冲全部交给硬件。 */
static void rtl_ring_init(void) {
    memset((void *)g_tx_ring, 0, sizeof(g_tx_ring));
    memset((void *)g_rx_ring, 0, sizeof(g_rx_ring));
    memset(g_tx_buf, 0, sizeof(g_tx_buf));
    for (uint32_t i = 0; i < R8169_RX_DESC_COUNT; i++) {
        uint32_t eor = (i == R8169_RX_DESC_COUNT - 1u) ? R8169_DESC_RING_END : 0u;
        g_rx_ring[i].opts2 = 0;
        g_rx_ring[i].addr = (uint64_t)(uintptr_t)g_rx_buf[i];
        rtl_wmb();
        g_rx_ring[i].opts1 = R8169_DESC_OWN | eor | R8169_RX_DESC_LEN;
    }
    rtl_wmb();
    g_cur_tx = 0;
    g_dirty_tx = 0;
    g_cur_rx = 0;
}

/** @brief 把环基址写进器件（`rtl_set_rx_tx_desc_registers`，:2800-2811）。
 *  先写高 32 位再写低 32 位——Linux 注释说某些平台必须这个顺序。 */
static void rtl_set_desc_registers(void) {
    uint64_t tx = (uint64_t)(uintptr_t)g_tx_ring;
    uint64_t rx = (uint64_t)(uintptr_t)g_rx_ring;
    rtl_w32(R8169_TX_DESC_HI, (uint32_t)(tx >> 32));
    rtl_w32(R8169_TX_DESC_LO, (uint32_t)(tx & 0xFFFFFFFFu));
    rtl_w32(R8169_RX_DESC_HI, (uint32_t)(rx >> 32));
    rtl_w32(R8169_RX_DESC_LO, (uint32_t)(rx & 0xFFFFFFFFu));
}

/**
 * @brief 打开收发（`rtl_hw_start`，r8169_main.c:4097-4130）。
 *
 * 顺序是照抄的，不能随意调换：解锁 → 关 ASPM → CPlusCmd → 版本 hw_start
 * → RxMaxSize → 环基址 → 回锁 → ChipCmd 开收发 → RxConfig/TxConfig
 * → 接收过滤 → IntrMask=0。
 *
 * @param cp_cmd CPlusCmd 的初始值（读回后加过 PCIMulRW/EnAnaPLL）
 * @return 0 成功，-1 失败
 */
static int rtl_hw_start(uint16_t cp_cmd) {
    rtl_unlock_config();
    rtl_aspm_clkreq_disable();
    rtl_w16(R8169_CPLUS_CMD, cp_cmd);
    /* EEE TX-IDLE 定时器（rtl_set_eee_txidle_timer，r8169_main.c:2213-2228）：
     * 值 = MTU(1500) + ETH_HLEN(14) + 0x20，HBOS 不支持 jumbo，MTU 固定 1500。
     * Linux 的 switch 只列 VER_46/48 走 OCP 0xe048，VER_47 不在分支内。 */
    if (g_mac_version == RTL_GIGA_MAC_VER_46 ||
        g_mac_version == RTL_GIGA_MAC_VER_48) {
        rtl_mac_ocp_write(0xE048u, (uint16_t)(1514u + 14u + 0x20u));
    } else if (g_mac_version >= RTL_GIGA_MAC_VER_61) {
        rtl_w16(R8169_8125_TXIDLE_TIMER, (uint16_t)(1514u + 14u + 0x20u));
    }

    if (g_mac_version <= RTL_GIGA_MAC_VER_06) {
        rtl_w8(R8169_EARLY_TX_THRES, 0x3Fu);
        cp_cmd |= R8169_CPLUS_PCIMULRW;
        if (g_mac_version == RTL_GIGA_MAC_VER_02 || g_mac_version == RTL_GIGA_MAC_VER_03)
            cp_cmd |= R8169_CPLUS_ENANAPLL;
        rtl_w16(R8169_CPLUS_CMD, cp_cmd);
        rtl_hw_start_8169_magic();
        rtl_w16(R8169_INTR_MITIGATE, 0);
    } else if (rtl_is_8125()) {
        rtl_w8(R8169_8125_INT_CFG0, 0x00);
        uint32_t end = (g_mac_version == RTL_GIGA_MAC_VER_63 ||
                        g_mac_version == RTL_GIGA_MAC_VER_70) ? 0xA80u : 0xB00u;
        for (uint32_t r = 0xA00u; r < end; r += 4u) rtl_w32(r, 0);
        if (g_mac_version != RTL_GIGA_MAC_VER_61)
            rtl_w16(R8169_8125_INT_CFG1, 0x0000);
        rtl_mac_ocp_modify(0xEA84u, 0x0000, 0x0003);
        /* 版本 hw_start：VER_61/63 有 EPHY 表，其余只有 common。 */
        if (g_mac_version == RTL_GIGA_MAC_VER_61) rtl_hw_start_8125a_2();
        else if (g_mac_version == RTL_GIGA_MAC_VER_63) rtl_hw_start_8125b();
        else if (g_mac_version == RTL_GIGA_MAC_VER_70) {
            /* rtl_disable_zrxdc_timeout：优先走 PCIe 扩展配置，退化为 CSI。 */
            rtl_hw_start_8125_common();
        } else rtl_hw_start_8125d();
    } else {
        if (rtl_is_8168evl_up()) rtl_w8(R8169_EARLY_TX_THRES, 0x27u);
        else rtl_w8(R8169_EARLY_TX_THRES, 0x3Fu);
        switch (g_mac_version) {
        case RTL_GIGA_MAC_VER_07: rtl_hw_start_8102e_1(); break;
        case RTL_GIGA_MAC_VER_08: rtl_hw_start_8102e_3(); break;
        case RTL_GIGA_MAC_VER_09: rtl_hw_start_8102e_2(); break;
        case RTL_GIGA_MAC_VER_10: break;
        case RTL_GIGA_MAC_VER_14: rtl_hw_start_8401(); break;
        case RTL_GIGA_MAC_VER_17: rtl_hw_start_8168b(); break;
        case RTL_GIGA_MAC_VER_18: rtl_hw_start_8168cp_1(); break;
        case RTL_GIGA_MAC_VER_19: rtl_hw_start_8168c_1(); break;
        case RTL_GIGA_MAC_VER_20:
        case RTL_GIGA_MAC_VER_21: rtl_hw_start_8168c_2(); break;
        case RTL_GIGA_MAC_VER_22: rtl_hw_start_8168c_4(); break;
        case RTL_GIGA_MAC_VER_23: rtl_hw_start_8168cp_2(); break;
        case RTL_GIGA_MAC_VER_24: rtl_hw_start_8168cp_3(); break;
        case RTL_GIGA_MAC_VER_25:
        case RTL_GIGA_MAC_VER_26:
        case RTL_GIGA_MAC_VER_31: rtl_hw_start_8168d(); break;
        case RTL_GIGA_MAC_VER_28: rtl_hw_start_8168d_4(); break;
        case RTL_GIGA_MAC_VER_29: rtl_hw_start_8105e_1(); break;
        case RTL_GIGA_MAC_VER_30: rtl_hw_start_8105e_2(); break;
        case RTL_GIGA_MAC_VER_32:
        case RTL_GIGA_MAC_VER_33: rtl_hw_start_8168e_1(); break;
        case RTL_GIGA_MAC_VER_34: rtl_hw_start_8168e_2(); break;
        case RTL_GIGA_MAC_VER_35:
        case RTL_GIGA_MAC_VER_36: rtl_hw_start_8168f_1(); break;
        case RTL_GIGA_MAC_VER_37: rtl_hw_start_8402(); break;
        case RTL_GIGA_MAC_VER_38: rtl_hw_start_8411(); break;
        case RTL_GIGA_MAC_VER_39: rtl_hw_start_8106(); break;
        case RTL_GIGA_MAC_VER_40: rtl_hw_start_8168g_1(); break;
        case RTL_GIGA_MAC_VER_42:
        case RTL_GIGA_MAC_VER_43: rtl_hw_start_8168g_2(); break;
        case RTL_GIGA_MAC_VER_44: rtl_hw_start_8411_2(); break;
        case RTL_GIGA_MAC_VER_46:
        case RTL_GIGA_MAC_VER_48: rtl_hw_start_8168h_1(); break;
        case RTL_GIGA_MAC_VER_51: rtl_hw_start_8168ep_3(); break;
        case RTL_GIGA_MAC_VER_52: rtl_hw_start_8117(); break;
        default: break;
        }
        rtl_w16(R8169_INTR_MITIGATE, 0x0000);
    }

    rtl_w16(R8169_RX_MAX_SIZE, 0x4000u);   /* 16K：关掉长度过滤，注释见 Linux */
    rtl_set_desc_registers();
    rtl_lock_config();

    rtl_pci_commit();
    rtl_w8(R8169_CHIP_CMD, (uint8_t)(R8169_CHIPCMD_TX_ENB | R8169_CHIPCMD_RX_ENB));
    rtl_init_rxcfg();
    rtl_set_tx_config();
    rtl_set_rx_mode();
    rtl_irq_mask_and_ack();
    return 0;
}

/**
 * @brief 重启自协商（`phy_start_aneg` 的裸机等价物）。
 *
 * `ANAR` 用 `ADVERTISE_ALL`（10/100 全双工与半双工），千兆能力另外写
 * `CTL1000`（`supports_gmii` 才写，百兆 PHY 写它会让链路状态机混乱）。
 */
static void rtl_phy_restart_aneg(void) {
    rtl_phy_write(R8169_MII_ANAR, (uint16_t)(R8169_ADVERTISE_CSMA |
                  R8169_ADVERTISE_10HALF | R8169_ADVERTISE_10FULL |
                  R8169_ADVERTISE_100HALF | R8169_ADVERTISE_100FULL |
                  R8169_ADVERTISE_PAUSE));
    if (g_supports_gmii)
        rtl_phy_write(R8169_MII_CTL1000,
                      (uint16_t)(R8169_ADVERTISE_1000FULL | R8169_ADVERTISE_1000HALF));
    uint16_t bmcr = rtl_phy_read(R8169_MII_BMCR);
    rtl_phy_write(R8169_MII_BMCR,
                  (uint16_t)(bmcr | R8169_BMCR_ANENABLE | R8169_BMCR_ANRESTART));
}

/**
 * @brief PHY 软复位（`genphy_soft_reset`，r8169_main.c:2556）。
 *
 * 只置 `BMCR_RESET`，不要顺带写别的位——复位期间写其它位会被丢弃。
 */
static void rtl_phy_soft_reset(void) {
    uint16_t bmcr = rtl_phy_read(R8169_MII_BMCR);
    rtl_phy_write(R8169_MII_BMCR, (uint16_t)(bmcr | R8169_BMCR_RESET));
    for (uint32_t i = 0; i < 100; i++) {
        if (!(rtl_phy_read(R8169_MII_BMCR) & R8169_BMCR_RESET)) break;
        rtl_delay_us(500);
    }
}

int r8169_link_up(void) {
    if (!g_ready || g_mac_version == RTL_GIGA_MAC_NONE) return 0;
    /* BMSR 的 LinkStatus 是 latch-low：读一帧"链路仍在"要读两次。 */
    (void)rtl_phy_read(R8169_MII_BMSR);
    uint16_t bmsr = rtl_phy_read(R8169_MII_BMSR);
    return (bmsr & R8169_BMSR_LSTATUS) ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 * 17. 初始化入口
 * ------------------------------------------------------------------ */

/**
 * @brief 读 XID 定 MAC 版本与 PHY 访问通道。
 *
 * 主 XID 在 `TxConfig` 的 bit20-31；`0xFFFFFFFF` 说明 BAR 不对或设备没使能。
 * `(xid & 0x7cf) == 0x7c8` 是"扩展芯片"标记，需要再读 `TX_CONFIG_V2`
 * (0x60B0)——这也是 8125 家族要求映射 ≥0x7000 字节窗口的原因。
 *
 * @return 0 成功，-1 无法识别
 */
static int rtl_detect_chip(void) {
    uint32_t txconfig = rtl_r32(R8169_TX_CONFIG);
    if (txconfig == 0xFFFFFFFFu || txconfig == 0) return -1;
    uint32_t xid = (txconfig >> 20) & 0xFCFu;
    const rtl_chip_info_t *chip = rtl_chip_lookup(xid);
    if (chip->ver == RTL_GIGA_MAC_NONE) return -1;
    g_mac_version = chip->ver;
    g_device_name = chip->name;
    /* PHY 访问通道：>=VER_40 走 GPHY OCP，其余走 PHYAR。 */
    g_phy_access = (g_mac_version >= RTL_GIGA_MAC_VER_40) ? R8169_ACCESS_GPHY_OCP
                                                           : R8169_ACCESS_MDIO;
    g_ocp_base = R8169_OCP_STD_PHY_BASE;
    return 0;
}

int r8169_init_hw(const pci_device_t *dev, uint8_t mac[6],
                  bool *mac_valid, bool *link_ready) {
    if (!dev || !mac) return -1;
    if (mac_valid) *mac_valid = false;
    if (link_ready) *link_ready = false;

    /* 每次初始化都从干净状态开始（单实例驱动，见文件头不变式 2）。 */
    g_ready = false;
    g_mac_version = RTL_GIGA_MAC_NONE;
    g_device_name = "unknown";
    g_is_io = false;
    g_mmio = 0;
    g_io_base = 0;
    g_mapped_count = 0;
    g_mapped_bytes = 0;
    g_supports_gmii = 1;
    const rtl_pci_id_t *id = rtl_pci_lookup(dev->vendor_id, dev->device_id);
    if (id) g_supports_gmii = id->gmii ? 1 : 0;

    pci_enable_bus_master_mmio(dev);
    if (rtl_pick_bar(dev) < 0) return -1;

    if (rtl_detect_chip() < 0) { rtl_unmap_window(); return -1; }

    uint16_t cp_cmd = (uint16_t)(rtl_r16(R8169_CPLUS_CMD) & R8169_CPLUS_CPCMD_MASK);
    rtl_init_rxcfg();
    rtl_irq_mask_and_ack();
    rtl_unlock_config();                /* hw_initialize 里要清 Config 位 */
    rtl_hw_initialize();
    if (rtl_hw_reset() == 0) { rtl_unmap_window(); return -1; }

    rtl_ring_init();
    if (rtl_hw_start(cp_cmd) < 0) { rtl_unmap_window(); return -1; }

    rtl_unlock_config();
    rtl_read_mac(mac);
    rtl_write_mac(mac);
    rtl_hw_phy_config();
    rtl_phy_soft_reset();
    rtl_lock_config();

    if (mac_valid) *mac_valid = (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) != 0;
    g_ready = true;
    rtl_phy_restart_aneg();

    /* 有界等链路。等不到不算失败：器件已经能收发，千兆自协商会在这之后
     * 自行完成，调用方可用 r8169_link_up() 继续观察。 */
    uint32_t waited = 0;
    while (waited < R8169_LINK_WAIT_MAX_MS) {
        if (r8169_link_up()) break;
        rtl_delay_ms(20);
        waited += 20;
    }
    if (link_ready) *link_ready = r8169_link_up() != 0;
    return 0;
}

/* ------------------------------------------------------------------ *
 * 18. 收发
 * ------------------------------------------------------------------ */

/** @brief 回收已发完的 TX 描述符（`rtl_tx` 的 OWN 判定，r8169_main.c:4702-4747）。 */
static void rtl_tx_reap(void) {
    while (g_dirty_tx != g_cur_tx) {
        uint32_t e = g_dirty_tx & (R8169_TX_DESC_COUNT - 1u);
        if (g_tx_ring[e].opts1 & R8169_DESC_OWN) break;
        g_dirty_tx++;
    }
}

/** @brief 敲 TX 门铃（`rtl8169_doorbell`，r8169_main.c:4544-4550）。
 *  8125 的门铃在 0x90 且写 1，不是 0x38 写 NPQ——写错就是"描述符填了但
 *  硬件永远不动"。 */
static void rtl_doorbell(void) {
    if (rtl_is_8125()) rtl_w16(R8169_8125_TX_POLL, 0x0001u);
    else rtl_w8(R8169_TX_POLL, (uint8_t)R8169_TXPOLL_NPQ);
}

int r8169_send(const void *frame, uint16_t len) {
    if (!g_ready || !frame || len == 0 || len > R8169_TX_BUF_SIZE) return -1;

    /* 等一个空闲描述符：TX 环满说明硬件还没回收，重试而不是丢弃。 */
    uint32_t waited = 0;
    for (;;) {
        rtl_tx_reap();
        if ((g_cur_tx - g_dirty_tx) < R8169_TX_DESC_COUNT) break;
        if (waited >= R8169_TX_SLOT_WAIT_MS) return -1;
        rtl_delay_ms(1);
        waited += 1;
    }

    uint32_t entry = g_cur_tx & (R8169_TX_DESC_COUNT - 1u);
    uint16_t tx_len = len;
    /* 8168evl 与 8125 家族要求最小帧长（rtl_quirk_packet_padto）。 */
    if ((g_mac_version == RTL_GIGA_MAC_VER_34 || rtl_is_8125()) &&
        tx_len < R8169_ETH_ZLEN) tx_len = R8169_ETH_ZLEN;

    memset(g_tx_buf[entry], 0, tx_len);
    memcpy(g_tx_buf[entry], frame, len);

    g_tx_ring[entry].addr = (uint64_t)(uintptr_t)g_tx_buf[entry];
    g_tx_ring[entry].opts2 = 0;
    rtl_wmb();
    g_tx_ring[entry].opts1 = R8169_DESC_OWN | 0x20000000u | 0x10000000u |
                             (uint32_t)tx_len |
                             ((entry == R8169_TX_DESC_COUNT - 1u) ? R8169_DESC_RING_END : 0u);
    rtl_wmb();
    g_cur_tx++;

    rtl_doorbell();
    /* 顺手回收一次：交完立刻看能不能腾出描述符，减少下一次 send 的等待。 */
    rtl_tx_reap();
    return 0;
}

int r8169_poll(int (*cb)(const uint8_t *pkt, uint16_t len, void *arg),
               void *arg, uint32_t spins) {
    if (!g_ready) return -1;
    if (spins == 0) spins = 1;
    uint32_t processed = 0;
    while (processed < spins) {
        uint32_t e = g_cur_rx & (R8169_RX_DESC_COUNT - 1u);
        uint32_t status = g_rx_ring[e].opts1;
        if (status & R8169_DESC_OWN) break;   /* 硬件还没写回 */
        rtl_rmb();                            /* 确认 OWN 已清再读其它字段 */
        processed++;
        g_cur_rx++;

        uint32_t err = status & (R8169_RX_DESC_RXRES | R8169_RX_DESC_RXRWT |
                                 R8169_RX_DESC_RXRUNT | R8169_RX_DESC_RXCRC);
        /* 分片帧（FS/LS 不齐）说明帧长超过 MTU，驱动不支持，直接丢。 */
        uint32_t frag = status & 0x30000000u;
        uint32_t pkt_size = status & R8169_RX_DESC_RXRES_LEN;
        if (!err && frag == 0x30000000u && pkt_size > 4u) {
            pkt_size -= 4u;                   /* 硬件交上来的长度含 4 字节 FCS */
            if (pkt_size > R8169_RX_BUF_SIZE) pkt_size = R8169_RX_BUF_SIZE;
            int rc = cb ? cb(g_rx_buf[e], (uint16_t)pkt_size, arg) : 0;
            /* 归还描述符：必须保留 RingEnd 位（rtl8169_mark_to_asic()）。 */
            uint32_t eor = status & R8169_DESC_RING_END;
            g_rx_ring[e].opts2 = 0;
            rtl_wmb();
            g_rx_ring[e].opts1 = R8169_DESC_OWN | eor | R8169_RX_DESC_LEN;
            if (rc) return rc;
            continue;
        }
        uint32_t eor = status & R8169_DESC_RING_END;
        g_rx_ring[e].opts2 = 0;
        rtl_wmb();
        g_rx_ring[e].opts1 = R8169_DESC_OWN | eor | R8169_RX_DESC_LEN;
    }
    return 0;
}
