# Net — 网络子系统 (`src/net.c` + `net.h`)

> 1549 行, 4 种网卡驱动检测 + E1000/PCnet 完整实现, DHCP/ARP/ICMP/DNS/TCP/HTTP 协议栈

## 驱动类型 (`net.h`)

```c
typedef enum {
    NET_DRIVER_NONE = 0,
    NET_DRIVER_E1000,           // Intel E1000 (QEMU e1000, 物理 Intel PRO/1000)
    NET_DRIVER_RTL8139,         // Realtek RTL8139 (检测但未实现!)
    NET_DRIVER_VIRTIO_NET,       // VirtIO (检测但未实现!)
    NET_DRIVER_PCNET,           // AMD PCnet-PCI II 0x1022:0x2000 (VirtualBox 默认) ✅
    NET_DRIVER_UNKNOWN_ETHERNET
} net_driver_t;
```

## PCI 驱动检测 (`detect_driver`, 行 266)

```c
if (vendor == 0x8086) {        // Intel
    switch (device) { case 0x1004..0x10D3: → NET_DRIVER_E1000 }
}
if (vendor == 0x1022 && device == 0x2000) → NET_DRIVER_PCNET;   // AMD Am79C970A
if (vendor == 0x10EC && device == 0x8139) → NET_DRIVER_RTL8139;  // 未实现
if (vendor == 0x1AF4) → NET_DRIVER_VIRTIO_NET;                   // 未实现
```

## Vtable 驱动抽象 (重构后)

```c
// net_device_t 里的 function pointer:
int (*send)(const void *frame, uint16_t len);

// net_init() 设置:
if (primary.driver == NET_DRIVER_E1000) {
    primary.send = e1000_send;
    e1000_init_hw(&dev);
} else if (primary.driver == NET_DRIVER_PCNET) {
    primary.send = pcnet_send;
    pcnet_init_hw(&dev);
}

// 所有协议栈调用: primary.send(frame, len)
// net_poll() 内部 switch 分发到 e1000_poll / pcnet_poll
```

## E1000 驱动 (MMIO, 行 ~310-700)

```c
// BAR0 = MMIO 基地址 (32-bit)
static volatile uint8_t *mmio;
static inline void reg_write(uint32_t reg, uint32_t val) { *(volatile uint32_t*)(mmio + reg) = val; }
static inline uint32_t reg_read(uint32_t reg) { return *(volatile uint32_t*)(mmio + reg); }

// 常量
#define TX_COUNT 8
#define RX_COUNT 16
#define PKT_SIZE 2048

// 关键寄存器偏移
#define E1000_STATUS  0x0008   // bit 1 = link up
#define E1000_RCTL    0x0100   // RX control
#define E1000_TCTL    0x0400   // TX control
#define E1000_RDBAL   0x2800   // RX desc base low
#define E1000_RDBAH   0x2804
#define E1000_RDLEN   0x2808
#define E1000_RDH     0x2810   // RX head
#define E1000_RDT     0x2818   // RX tail
#define E1000_TDBAL   0x3800   // TX desc base low
#define E1000_TDBAH   0x3804
#define E1000_TDLEN   0x3808
#define E1000_TDH     0x3810
#define E1000_TDT     0x3818
#define E1000_RA      0x5400   // Receive Address (MAC)

// 描述符结构
typedef struct { uint64_t addr; uint16_t length; uint16_t status; } e1000_rx_desc_t;
typedef struct { uint64_t addr; uint16_t length; uint8_t cso; uint8_t cmd; uint8_t status; } e1000_tx_desc_t;

// 函数签名
static int e1000_init_hw(pci_device_t *dev);   // BAR0 MMIO, MAC, 描述符环, 启动
static int e1000_send(const void *frame, uint16_t len);
static int e1000_poll(packet_cb_t cb, void *arg, uint32_t spins);
```

## PCnet 驱动 (I/O Port, 行 ~770-920, 新加的)

```c
// BAR0 = I/O 端口基地址
static uint32_t pcnet_iobase;

// CSR 访问: RAP=+0x12, RDP=+0x10, BDP=+0x16
static uint16_t pcnet_read_csr(uint16_t reg);   // outw RAP=reg, inw RDP
static void pcnet_write_csr(uint16_t reg, uint16_t val);
static uint16_t pcnet_read_bcr(uint16_t reg);   // BCR 访问
static void pcnet_write_bcr(uint16_t reg, uint16_t val);

// 常量
#define PCNET_TX_COUNT 8
#define PCNET_RX_COUNT 16
#define PCNET_BUF_SIZE 2048

// 描述符 (12 bytes, packed)
typedef struct {
    uint32_t base;      // buffer 物理地址 (16-byte aligned)
    int16_t  length;     // 负数=设备持有, 正数=主机持有
    uint16_t status;     // 0x8000=OWN, 0x0200=STP, 0x0100=ENP
    uint32_t msg_len;
} pcnet_desc_t;

// InitBlock (24 bytes, 16-byte aligned, packed)
typedef struct {
    uint16_t mode;       // 0x0000
    uint8_t  padr[6];    // MAC
    uint8_t  ladrf[8];   // logical address filter
    uint32_t rx_ring;    // RX descriptor ring phys addr
    uint32_t tx_ring;
} pcnet_init_block_t;

// 函数签名
static int pcnet_init_hw(pci_device_t *dev);
static int pcnet_send(const void *frame, uint16_t len);
static int pcnet_poll(packet_cb_t cb, void *arg, uint32_t spins);
```

### PCnet 初始化序列
```
1. BAR0 = primary.bar0_base (I/O port)
2. PCI bus mastering enable (cmd reg offset 4, bit 2)
3. BCR20: SSIZE32 (32-bit mode)
4. BCR9:  Software Style = PCnet-PCI
5. CSR0: STOP → wait → STRT → verify
6. MAC: CSR12 low word, CSR13 mid, CSR14 high
7. kmalloc descriptor rings (16-byte aligned)
8. kmalloc packet buffers (2KB each)
9. Fill RX descriptors: base=buf, length=-2048, status=OWN
10. Build InitBlock (kmalloc, 16-byte aligned)
11. CSR1/2 = InitBlock addr low/high
12. CSR0 = INIT+STRT (0x0041)
13. Wait for IDON (CSR0 bit 8)
14. CSR0 = STRT+INIT+IENA (0x0043)
15. link_ready = true
```

### PCnet TX
```
1. Check tx_desc[idx].status & OWN (0x8000) — must be clear
2. memcpy data to tx_buf[idx]
3. tx_desc[idx].length = -len (negative → device owns)
4. tx_desc[idx].status = 0x8300 (OWN|STP|ENP)
5. tx_tail = (idx+1) % 8
6. CSR0 |= 0x0048 (TDMD — demand transmit)
```

### PCnet RX
```
1. Loop RX descriptors: if OWN bit clear → data received
2. msg_len & 0xFFF = received bytes
3. Call cb(rx_buf[i], rlen, arg)
4. Return descriptor: length = -2048, status = OWN
```

## 协议栈 (同一文件)

```c
// DHCP
int net_dhcp(void);                          // 发起 DHCP discover→request→ack
// ARP
int net_arp_resolve(uint32_t ip, uint8_t mac[6]);  // 广播 ARP request
// ICMP
int net_ping(uint32_t ip);                   // ICMP echo request, 等 reply (8 次轮询)
// DNS
int net_dns_resolve(const char *host, uint32_t *ip);  // UDP DNS query→answer
// TCP
int net_tcp_connect(uint32_t ip, uint16_t port);
int net_tcp_send(int fd, const void *data, uint32_t len);
int net_tcp_recv(int fd, void *buf, uint32_t len);
void net_tcp_close(int fd);
// HTTP
int net_http_get(const char *url, uint8_t **buf, uint32_t *len);  // 内部调 DNS+TCP
```

## 新增网卡驱动步骤 (Agent 参考)

1. `net.h`: 枚举加新 `NET_DRIVER_XXX`
2. `net.c:detect_driver()`: 加 PCI vendor/device 映射
3. `net.c:net_driver_name()`: 加 case 返回字符串
4. 实现 `xxx_init_hw()`, `xxx_send()`, `xxx_poll()` (static)
5. `net_init()`: 加 `else if` 分支, 设置 `primary.send` 和调用 init
6. `net_poll()`: 加 case
7. `make` 构建, `drivers` 命令验证

## 当前状态

| 驱动 | 检测 | init | send | poll | 测试 |
|------|------|------|------|------|------|
| Intel E1000 | ✅ | ✅ | ✅ | ✅ | QEMU e1000 |
| AMD PCnet | ✅ | ✅ | ✅ | ✅ | VirtualBox 默认 |
| RTL8139 | ✅ | ❌ | ❌ | ❌ | — |
| VirtIO | ✅ | ❌ | ❌ | ❌ | — |

| 协议 | 状态 |
|------|------|
| ARP | ✅ |
| DHCP | ✅ |
| ICMP ping | ✅ |
| DNS | ✅ |
| TCP (单连接) | ✅ |
| HTTP GET | ✅ |
| UDP | ⚠️ (仅 DNS 用到) |
