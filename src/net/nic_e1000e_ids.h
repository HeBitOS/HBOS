/**
 * @file net/nic_e1000e_ids.h
 * @brief Intel e1000e / igb 家族的 PCI device ID 与型号名对照表
 *
 * 来源：Linux 7.2.8 `drivers/net/ethernet/intel/e1000e/hw.h` 的
 * `E1000_DEV_ID_*` 定义，以及 `netdev.c` 里 `e1000_pci_tbl[]` 真正登记的
 * 条目（113 条，去重后 113 个 device id）。**逐条照抄，不要手写增删**——
 * 这张表的唯一目的是让 HBOS 能认出真机上的集成网卡。
 *
 * 为什么单独一个文件：net.c 里原来只有 11 个 ID，I217/I218/I219 全系列都
 * 认不出来，真机（尤其笔记本与 NUC）因此"没有网卡驱动"。把表放这里既避免
 * net.c 继续膨胀，也让"表和 Linux 对齐"这件事有一个明确的落点。
 *
 * 生成方式（可复现）：
 *   python3 - <<'EOF'
 *   <从 Linux hw.h + netdev.c 提取 E1000_DEV_ID_* → 本表>
 *   EOF
 * 见本仓库 docs/PERFORMANCE.md 或提交信息里的说明。
 */

#ifndef HBOS_NIC_E1000E_IDS_H
#define HBOS_NIC_E1000E_IDS_H

#include <stdint.h>

/** @brief 一条 device id → 型号名 的映射 */
typedef struct {
    uint16_t device_id;   /**< PCI device id */
    const char *model;    /**< 型号名（诊断输出用） */
} e1000e_id_entry_t;

/** @brief e1000e 家族 device id 表（113 条，照抄 Linux） */
static const e1000e_id_entry_t e1000e_id_table[] = {
    { 0x0D4CU, "I219-LM11" },
    { 0x0D4DU, "I219-V11" },
    { 0x0D4EU, "I219-LM10" },
    { 0x0D4FU, "I219-V10" },
    { 0x0D53U, "I219-LM12" },
    { 0x0D55U, "I219-V12" },
    { 0x0DC5U, "I219-LM23" },
    { 0x0DC6U, "I219-V23" },
    { 0x0DC7U, "I219-LM22" },
    { 0x0DC8U, "I219-V22" },
    { 0x1049U, "ICH8_IGP_M_AMT" },
    { 0x104AU, "ICH8_IGP_AMT" },
    { 0x104BU, "ICH8_IGP_C" },
    { 0x104CU, "ICH8_IFE" },
    { 0x104DU, "ICH8_IGP_M" },
    { 0x105EU, "82571EB_COPPER" },
    { 0x105FU, "82571EB_FIBER" },
    { 0x1060U, "82571EB_SERDES" },
    { 0x107DU, "82572EI_COPPER" },
    { 0x107EU, "82572EI_FIBER" },
    { 0x107FU, "82572EI_SERDES" },
    { 0x108BU, "82573E" },
    { 0x108CU, "82573E_IAMT" },
    { 0x1096U, "80003ES2LAN_COPPER_DPT" },
    { 0x1098U, "80003ES2LAN_SERDES_DPT" },
    { 0x109AU, "82573L" },
    { 0x10A4U, "82571EB_QUAD_COPPER" },
    { 0x10A5U, "82571EB_QUAD_FIBER" },
    { 0x10B9U, "82572EI" },
    { 0x10BAU, "80003ES2LAN_COPPER_SPT" },
    { 0x10BBU, "80003ES2LAN_SERDES_SPT" },
    { 0x10BCU, "82571EB_QUAD_COPPER_LP" },
    { 0x10BDU, "ICH9_IGP_AMT" },
    { 0x10BFU, "ICH9_IGP_M" },
    { 0x10C0U, "ICH9_IFE" },
    { 0x10C2U, "ICH9_IFE_G" },
    { 0x10C3U, "ICH9_IFE_GT" },
    { 0x10C4U, "ICH8_IFE_GT" },
    { 0x10C5U, "ICH8_IFE_G" },
    { 0x10CBU, "ICH9_IGP_M_V" },
    { 0x10CCU, "ICH10_R_BM_LM" },
    { 0x10CDU, "ICH10_R_BM_LF" },
    { 0x10CEU, "ICH10_R_BM_V" },
    { 0x10D3U, "82574L" },
    { 0x10D5U, "82571PT_QUAD_COPPER" },
    { 0x10D9U, "82571EB_SERDES_DUAL" },
    { 0x10DAU, "82571EB_SERDES_QUAD" },
    { 0x10DEU, "ICH10_D_BM_LM" },
    { 0x10DFU, "ICH10_D_BM_LF" },
    { 0x10E5U, "ICH9_BM" },
    { 0x10EAU, "PCH-M-HV-LM" },
    { 0x10EBU, "PCH-M-HV-LC" },
    { 0x10EFU, "PCH-D-HV-DM" },
    { 0x10F0U, "PCH-D-HV-DC" },
    { 0x10F5U, "ICH9_IGP_M_AMT" },
    { 0x10F6U, "82574LA" },
    { 0x1501U, "ICH8_82567V_3" },
    { 0x1502U, "PCH2-LV-LM" },
    { 0x1503U, "PCH2-LV-V" },
    { 0x150CU, "82583V" },
    { 0x1525U, "ICH10_D_BM_V" },
    { 0x153AU, "I217-LM" },
    { 0x153BU, "I217-V" },
    { 0x1559U, "I218-V" },
    { 0x155AU, "I218-LM" },
    { 0x156FU, "I219-LM" },
    { 0x1570U, "I219-V" },
    { 0x15A0U, "PCH-I218-LM2" },
    { 0x15A1U, "PCH-I218-V2" },
    { 0x15A2U, "PCH-I218-LM3" },
    { 0x15A3U, "PCH-I218-V3" },
    { 0x15B7U, "I219-LM2" },
    { 0x15B8U, "I219-V2" },
    { 0x15B9U, "PCH-LBG-I219-LM3" },
    { 0x15BBU, "I219-LM7" },
    { 0x15BCU, "I219-V7" },
    { 0x15BDU, "I219-LM6" },
    { 0x15BEU, "I219-V6" },
    { 0x15D6U, "I219-V5" },
    { 0x15D7U, "I219-LM4" },
    { 0x15D8U, "I219-V4" },
    { 0x15DFU, "I219-LM8" },
    { 0x15E0U, "I219-V8" },
    { 0x15E1U, "I219-LM9" },
    { 0x15E2U, "I219-V9" },
    { 0x15E3U, "I219-LM5" },
    { 0x15F4U, "I219-LM15" },
    { 0x15F5U, "I219-V15" },
    { 0x15F9U, "I219-LM14" },
    { 0x15FAU, "I219-V14" },
    { 0x15FBU, "I219-LM13" },
    { 0x15FCU, "I219-V13" },
    { 0x1A1CU, "I219-LM17" },
    { 0x1A1DU, "I219-V17" },
    { 0x1A1EU, "I219-LM16" },
    { 0x1A1FU, "I219-V16" },
    { 0x294CU, "ICH9_IGP_C" },
    { 0x550AU, "I219-LM18" },
    { 0x550BU, "I219-V18" },
    { 0x550CU, "I219-LM19" },
    { 0x550DU, "I219-V19" },
    { 0x550EU, "I219-LM20" },
    { 0x550FU, "I219-V20" },
    { 0x5510U, "I219-LM21" },
    { 0x5511U, "I219-V21" },
    { 0x57A0U, "I219-LM24" },
    { 0x57A1U, "I219-V24" },
    { 0x57B3U, "I219-LM25" },
    { 0x57B4U, "I219-V25" },
    { 0x57B7U, "I219-LM27" },
    { 0x57B8U, "I219-V27" },
    { 0x57B9U, "PCH-NVL-I219-LM29" },
    { 0x57BAU, "PCH-NVL-I219-V29" },
};

/** @brief 表中条目数 */
#define E1000E_ID_TABLE_COUNT \
    ((int)(sizeof(e1000e_id_table) / sizeof(e1000e_id_table[0])))

/**
 * @brief 查一个 device id 对应的型号名
 * @param device PCI device id
 * @return 型号名字符串；不是 e1000e 家族返回 NULL
 */
static inline const char *e1000e_model_name(uint16_t device) {
    for (int i = 0; i < E1000E_ID_TABLE_COUNT; i++) {
        if (e1000e_id_table[i].device_id == device) return e1000e_id_table[i].model;
    }
    return 0;
}

#endif /* HBOS_NIC_E1000E_IDS_H */
