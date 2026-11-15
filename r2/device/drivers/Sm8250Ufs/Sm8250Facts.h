// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8250 (kona) UFS facts.
//
// Every value is transcribed from the device's own vendor kernel:
//   arch/arm64/boot/dts/vendor/qcom/kona.dtsi           (windows, lanes, reset, clocks)
//   drivers/clk/qcom/gcc-kona.c                         (branch registers, GDSC, BCR)
//   drivers/phy/qualcomm/phy-qcom-ufs-qmp-v4.h/.c        (PHY tables and sequences)
//
// The vendor tree is Linux 4.19 and carries the qmp-v4 PHY driver, not the mainline
// phy-qcom-qmp-ufs.c. There is no "sm8250 PHY table" upstream: the tables below are the
// vendor v4 tables kona actually executes.
#ifndef SM8250_UFS_FACTS_H
#define SM8250_UFS_FACTS_H

#include <stdint.h>

// Windows, from kona.dtsi ufshc@1d84000 / ufsphy_mem@1d87000 / gcc / ufs_phy_gdsc.
#define SM_GCC_BASE          0x00100000ULL
#define SM_UFS_HC_BASE       0x01d84000ULL
#define SM_UFS_HC_SIZE       0x3000ULL
#define SM_UFS_ICE_BASE      0x01d90000ULL
#define SM_UFS_ICE_SIZE      0x8000ULL
#define SM_UFS_PHY_BASE      0x01d87000ULL
#define SM_UFS_PHY_SIZE      0x00000e00ULL
#define SM_UFS_PHY_GDSC      0x00177004ULL   // qcom,gdsc@177004 = GCC_BASE + 0x77004
// The gdsc driver's layout: bit 0 holds the domain in collapse, bit 31 reports PWR_ON.
#define SM_UFS_PHY_GDSC_SW_COLLAPSE (1U << 0)
#define SM_UFS_PHY_GDSC_PWR_ON      (1U << 31)

// gcc-kona.c UFS branch registers (CBCR). halt and enable share the register; bit 0 enables,
// bit 31 reports CLK_OFF, and hwcg_bit=1 makes an idle branch report CLK_OFF normally.
#define SM_GCC_UFS_AXI_CBCR            0x77010
#define SM_GCC_UFS_AHB_CBCR            0x77018
#define SM_GCC_UFS_TX_SYMBOL0_CBCR     0x7701c
#define SM_GCC_UFS_RX_SYMBOL0_CBCR     0x77020
#define SM_GCC_UFS_UNIPRO_CORE_CBCR    0x7705c
#define SM_GCC_UFS_ICE_CORE_CBCR       0x77064
#define SM_GCC_UFS_PHY_AUX_CBCR        0x7709c
#define SM_GCC_UFS_RX_SYMBOL1_CBCR     0x770b8
#define SM_GCC_AGGRE_UFS_AXI_CBCR      0x770cc   // on SM8550 this offset is rx_symbol_1
#define SM_GCC_UFS_BCR                 0x77000   // GCC_UFS_PHY_BCR, bit 0

#define SM_GCC_BRANCH_ENABLE   (1U << 0)
#define SM_GCC_BRANCH_HW_GATE  (1U << 1)   // hardware gating control; 0 holds the branch on
#define SM_GCC_BRANCH_CLK_OFF  (1U << 31)

// The branch registers above only gate the leaf; the root each one hangs from must be running for
// the branch to deliver a clock. These four roots are the gcc-kona.c clk_rcg2 blocks the UFS
// controller depends on. clk-rcg2.h's CMD_RCGR layout: bit0 UPDATE in flight, bit1 the software
// root force, bit31 ROOT_OFF (clk_rcg2_is_enabled inverts it, so 0 means the root is running).
// All four carry enable_safe_config in gcc-kona.c, so a disabled root parks at CXO.
#define SM_GCC_UFS_AXI_RCG        0x77024   // gcc_ufs_phy_axi_clk_src
#define SM_GCC_UFS_ICE_RCG        0x7706c   // gcc_ufs_phy_ice_core_clk_src
#define SM_GCC_UFS_UNIPRO_RCG     0x77084   // gcc_ufs_phy_unipro_core_clk_src
#define SM_GCC_UFS_PHY_AUX_RCG    0x770a0   // gcc_ufs_phy_phy_aux_clk_src

#define SM_RCG_CMD            0x00
#define SM_RCG_CFG            0x04
#define SM_RCG_M              0x08
#define SM_RCG_N              0x0c
#define SM_RCG_D              0x10

#define SM_RCG_CMD_UPDATE     (1U << 0)
#define SM_RCG_CMD_ROOT_EN    (1U << 1)
#define SM_RCG_CMD_ROOT_OFF   (1U << 31)
#define SM_RCG_CFG_DIV_MASK   0x1fU
#define SM_RCG_CFG_SRC_SHIFT  8
#define SM_RCG_CFG_SRC_MASK   (0x7U << SM_RCG_CFG_SRC_SHIFT)
#define SM_RCG_CFG_HW_CTRL    (1U << 20)   // CFG_HW_CLK_CTRL_MASK; gcc-kona sets it on every configure

// Host controller standard registers (UFSHCI), offsets into SM_UFS_HC_BASE. The last group is
// the Qualcomm vendor area from the vendor ufs-qcom.h.
#define SM_UFS_REG_CAP         0x000
#define SM_UFS_REG_VERSION     0x008
#define SM_UFS_REG_IS          0x020
#define SM_UFS_REG_HCS         0x030
#define SM_UFS_REG_HCE         0x034
#define SM_UFS_REG_UTRLBA      0x050
#define SM_UFS_REG_UTRLBAU     0x054
#define SM_UFS_REG_UTRLDBR     0x058
#define SM_UFS_REG_UTRLRSR     0x060
#define SM_UFS_REG_UTMRLBA     0x070
#define SM_UFS_REG_UTMRLBAU    0x074
#define SM_UFS_REG_UTMRLRSR    0x080
#define SM_UFS_REG_UICCMD      0x090
#define SM_UFS_REG_UICARG1     0x094
#define SM_UFS_REG_UICARG2     0x098
#define SM_UFS_REG_UICARG3     0x09c
#define SM_UFS_REG_UIC_ERR_PA  0x038   // PHY adapter layer error code, cleared by reading
#define SM_UFS_REG_UIC_ERR_DL  0x03c   // data link layer
#define SM_UFS_REG_UIC_ERR_NL  0x040   // network layer
#define SM_UFS_REG_UIC_ERR_TL  0x044   // transport layer
#define SM_UFS_REG_UIC_ERR_DME 0x048   // DME
#define SM_UFS_REG_CFG1        0x0dc
#define SM_UFS_REG_CFG2        0x0e0
#define SM_UFS_REG_HW_VERSION  0x0e4

// IS bit 2 is UIC_ERROR (ufshci.h: UIC_ERROR = 0x4). The UIC command completion bit is 10.
#define SM_UFS_IS_UIC_ERROR    (1U << 2)

// ARM SMMU-500 stream mapping: S2CR bits [17:16] are the type (0 trans, 1 bypass, 2 fault).
// The bypass value is easy to put in the wrong field, which turns a bypassed stream into a
// translation through context bank 0.
#define SM_SMMU_S2CR_BYPASS    0x00010000U
#define SM_SMMU_NS_SGFSR       0x448
#define SM_SMMU_TLBIALLNSNH    0x068
#define SM_SMMU_TLBIALLH       0x06c
#define SM_SMMU_CB_FSR         0x058
#define SM_SMMU_CB_RESUME      0x008
#define SM_SMMU_CB_FSYNR0      0x068
#define SM_SMMU_RESUME_TERMINATE 1

#define SM_UFS_CFG1_QUNIPRO_SEL     (1U << 0)
#define SM_UFS_CFG1_PHY_SOFT_RESET  (1U << 1)

// UIC command opcode for the DME_LINKSTARTUP that starts the Unipro link; the completion bit in
// the interrupt status register is shared with the request-list completion bits.
#define SM_UIC_CMD_DME_LINK_STARTUP 0x16
#define SM_UIC_CMD_DME_HIBER_EXIT   0x18
#define SM_UIC_COMMAND_COMPL        (1U << 10)
#define SM_UFS_HCS_UP_MODE_CHANGE   (1U << 12)

// GCC UFS block base used by the SM8550 driver to map the branch window.
#define SM_GCC_UFS_BLOCK       0x77000

// PHY (qmp-v4) register map, relative to SM_UFS_PHY_BASE.
#define SM_PHY_START           0x0c00
#define SM_PHY_POWER_DOWN      0x0c04
#define SM_PHY_SW_RESET        0x0c08
#define SM_PHY_PCS_READY       0x0d80
#define SM_PHY_LINECFG_DISABLE 0x0d48
#define SM_PHY_MULTI_LANE_CTRL 0x0de0
#define SM_PHY_RX0_IF_MODE     0x0734
#define SM_PHY_RX1_IF_MODE     0x0b34
#define SM_PHY_PLL_CNTL        0x0c2c

#define SM_PHY_RX_IF_MODE_CLOCK_EDGE_BIT (1U << 5)
#define SM_PHY_SERDES_START_BIT          (1U << 0)
#define SM_PHY_PCS_READY_BIT             (1U << 0)

struct SmPhiWrite {
  uint16_t offset;
  uint32_t value;
};

// phy_cal_table_rate_A (G4) — phy-qcom-ufs-qmp-v4.h:196-273, 76 writes.
static const struct SmPhiWrite kSmPhyRateA[] = {
  {0x0c04, 0x01}, {0x0094, 0xd9}, {0x0158, 0x11}, {0x015c, 0x00}, {0x00a4, 0x01},
  {0x010c, 0x02}, {0x0058, 0x0f}, {0x0124, 0x00}, {0x01bc, 0x11}, {0x00bc, 0x82},
  {0x0074, 0x06}, {0x007c, 0x16}, {0x0084, 0x36}, {0x00ac, 0xff}, {0x00b0, 0x0c},
  {0x01ac, 0xac}, {0x01b0, 0x1e}, {0x00c4, 0x98}, {0x0078, 0x06}, {0x0080, 0x16},
  {0x0088, 0x36}, {0x00b4, 0x32}, {0x00b8, 0x0f}, {0x01b4, 0xdd}, {0x01b8, 0x23},
  {0x04d8, 0x06}, {0x04dc, 0x03}, {0x04e0, 0x01}, {0x04e4, 0x00}, {0x0484, 0xe5},
  {0x04b8, 0x0c}, {0x0720, 0x24}, {0x071c, 0x0f}, {0x0724, 0x1e}, {0x0728, 0x18},
  {0x0630, 0x0a}, {0x0634, 0x5a}, {0x0644, 0xf1}, {0x063c, 0x80}, {0x0648, 0x81},
  {0x0608, 0x0e}, {0x0614, 0x04}, {0x0680, 0x6f}, {0x06e8, 0x04}, {0x06ec, 0x00},
  {0x06f0, 0x09}, {0x06f4, 0x07}, {0x0710, 0x17}, {0x0714, 0x00}, {0x0700, 0x20},
  {0x06f8, 0x80}, {0x06fc, 0x01}, {0x0770, 0x3f}, {0x0774, 0xff}, {0x0778, 0xff},
  {0x077c, 0x7f}, {0x0780, 0x2c}, {0x0784, 0x6d}, {0x0788, 0x6d}, {0x078c, 0xed},
  {0x0790, 0x3b}, {0x0794, 0x3c}, {0x0798, 0xe0}, {0x079c, 0xc8}, {0x07a0, 0xc8},
  {0x07a4, 0x3b}, {0x07a8, 0xb1}, {0x07bc, 0x0c}, {0x06dc, 0x0f}, {0x0d58, 0x6d},
  {0x0c30, 0x10}, {0x0c38, 0x02}, {0x0dd8, 0x43}, {0x0d24, 0x1f}, {0x0d50, 0xff},
  {0x0c60, 0x0a},
};

// phy_cal_table_2nd_lane (G4, lanes == 2) — phy-qcom-ufs-qmp-v4.h:356-402, 46 writes.
static const struct SmPhiWrite kSmPhySecondLane[] = {
  {0x08d8, 0x06}, {0x08dc, 0x03}, {0x08e0, 0x01}, {0x08e4, 0x00}, {0x0884, 0xe5},
  {0x08b8, 0x0c}, {0x0b20, 0x24}, {0x0b1c, 0x0f}, {0x0b24, 0x1e}, {0x0b28, 0x18},
  {0x0a30, 0x0a}, {0x0a34, 0x5a}, {0x0a44, 0xf1}, {0x0a3c, 0x80}, {0x0a48, 0x81},
  {0x0a08, 0x0e}, {0x0a14, 0x04}, {0x0a80, 0x6f}, {0x0ae8, 0x04}, {0x0aec, 0x00},
  {0x0af0, 0x09}, {0x0af4, 0x07}, {0x0b10, 0x17}, {0x0b14, 0x00}, {0x0b00, 0x20},
  {0x0af8, 0x80}, {0x0afc, 0x01}, {0x0b70, 0x3f}, {0x0b74, 0xff}, {0x0b78, 0xff},
  {0x0b7c, 0x7f}, {0x0b80, 0x2c}, {0x0b84, 0x6d}, {0x0b88, 0x6d}, {0x0b8c, 0xed},
  {0x0b90, 0x3b}, {0x0b94, 0x3c}, {0x0b98, 0xe0}, {0x0b9c, 0xc8}, {0x0ba0, 0xc8},
  {0x0ba4, 0x3b}, {0x0ba8, 0xb1}, {0x0bbc, 0x0c}, {0x0adc, 0x0f}, {0x0de0, 0x02},
};

// phy_cal_table_rate_B — one override write; only used for pre-G4 devices.
#define SM_PHY_RATE_B_OFFSET  0x010c
#define SM_PHY_RATE_B_VALUE   0x06

#endif
