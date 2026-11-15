// SPDX-License-Identifier: BSD-2-Clause-Patent
// kona (SM8250) GCC registers used by UFS, transcribed from the public Linux
// gcc-sm8250.c clock driver and from this board's measured device tree. The offsets below
// are the SM8550 table shifted to the kona layout; NativeUfs only reads them.
//
// Board inputs (porting.toml, docs/evidence/device-fdt.dts):
//   ufshc@1d84000  reg 0x1d84000+0x3000 (ufs_mem), 0x1d90000+0x8000 (ufs_ice), SPI 265
//   ufsphy_mem@1d87000  reg 0x1d87000+0xe00, ref_aux_clk GCC 0xad
//   clocks core 0xa7, iface 0xa6, unipro 0xb3, ice 0xaa, bus_aggr 0x06
//   resets core_reset GCC 0x21, BCR 0x77000
//   no iommus property: UFS is not behind the apps SMMU on this platform
#ifndef NATIVE_UFS_GCC_UFS_H
#define NATIVE_UFS_GCC_UFS_H

// Physical windows from the sheng board DT (clock-controller@100000, ufshc@1d84000).
#define GCC_BASE                      0x00100000ULL
#define UFS_HC_BASE                   0x01d84000ULL
#define UFS_HC_SIZE                   0x3000ULL

#define GCC_UFS_PHY_GDSCR             0x77004
#define GCC_UFS_MEM_PHY_GDSCR         0x9e000

#define GCC_UFS_PHY_AXI_ENABLE        0x77010
#define GCC_UFS_PHY_AHB_ENABLE        0x77018
#define GCC_UFS_PHY_TX_SYMBOL0_ENABLE 0x7701C
#define GCC_UFS_PHY_RX_SYMBOL0_ENABLE 0x77020
#define GCC_UFS_PHY_UNIPRO_CORE_ENABLE 0x7705c
#define GCC_UFS_PHY_ICE_CORE_ENABLE   0x77064
#define GCC_UFS_PHY_PHY_AUX_ENABLE    0x7709c
#define GCC_UFS_PHY_RX_SYMBOL1_ENABLE 0x770b8
// bus_aggr_clk in the board DT: gcc_aggre_ufs_phy_axi_clk, halt/enable 0x770e4, HW clock gating bit1
#define GCC_AGGRE_UFS_PHY_AXI_ENABLE  0x770cc

#define GCC_UFS_BRANCH_ENABLE_BIT     0x1U

// gcc_ufs_phy_ahb_clk and gcc_aggre_ufs_phy_axi_clk set hwcg_bit=1 in gcc-sm8550.c, so CLK_OFF=1 on
// an idle branch is normal (hardware 2026-10-17 read ahb=80000003).
// The reset and RPMh/TCSR/QMP-PHY programming are not represented here: their
// semantics are not qualified and NativeUfs must not write them speculatively.
#endif