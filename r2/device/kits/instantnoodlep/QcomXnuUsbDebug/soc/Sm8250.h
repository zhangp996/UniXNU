/* SPDX-License-Identifier: MIT
 * SM8250 / instantnoodlep resource candidate, not hardware-qualified.
 * Source: user-supplied coloros16_sm8250_kernel-android16:
 * arch/arm64/boot/dts/vendor/qcom/kona-usb.dtsi (USB0/SID/PHY),
 * arch/arm64/boot/dts/vendor/oplus/instantnoodlep/kona-mtp.dtsi:2070
 * (all three PHY overrides), drivers/clk/qcom/gcc-kona.c (GCC offsets).
 * The kernel files' hashes and exact resource locations are recorded separately.
 */
#ifndef QUD_SM8250_H
#define QUD_SM8250_H
#define QUD_SOC_NAME "sm8250-instantnoodlep-candidate"
#define QUD_USB_VID 0x1209u
#define QUD_USB_PID 0x316du
#define QUD_USB_SERIAL "QcomXnuUsbDebug"
#define QUD_USB_PRODUCT "Qualcomm XNU Debug"
#define QUD_GCC_BASE 0x00100000ULL
#define QUD_GCC_BYTES 0x100000u
#define QUD_DWC3_BASE 0x0a600000ULL
#define QUD_DWC3_BYTES 0x100000u
#define QUD_SMMU_BASE 0x15000000ULL
#define QUD_SMMU_BYTES 0x100000ULL
#define QUD_PHY_BASE 0x088e3000ULL
#define QUD_PHY_BYTES 0x110u
#define QUD_STREAM_ID 0x0u
#define QUD_DMA_IOVA 0x90000000ULL
/* Qualify a private translated lease. Never silently use the old fork's bypass. */
#define QUD_DMA_PREFER_IDENTITY 0
#define QUD_DMA_BYPASS_BANK 0
#define QUD_PREPARE_KEEP_HALTED 1
#define QUD_GCC_CBCR_OFFSETS {0xf010u,0xf018u,0xf01cu,0xf054u,0xf058u}
#define QUD_GCC_CLOCK_OFFSETS {0xf010u,0xf018u,0xf01cu,0xf054u,0xf058u,0xf05cu,0xf07cu,0xf080u,0x8c010u}
#define QUD_GCC_CLOCK_NO_POLL_OFFSETS {0xf05cu,0x8c010u}
#define QUD_GCC_QUSB2PHY_BCR_OFFSET 0x12000u
#define QUD_GCC_AHB2PHY_BCR_OFFSET 0x6a000u
#define QUD_GCC_BCR_OFFSET 0xf000u
#define QUD_GCC_GDSCR_OFFSET 0xf004u
#define QUD_GCC_CFG_GDSCR_OFFSET 0xf008u
#define QUD_GDSC_LEGACY_PWR_ON 1
#define QUD_QSCRATCH_HS_PHY_CTRL_OFFSET 0xf8810u
#define QUD_QSCRATCH_VBUS_MASK (1u<<20)
#define QUD_PHY_OVERRIDE_X0_VALUE 0x67u
#define QUD_PHY_OVERRIDE_X1_VALUE 0x0bu
#define QUD_PHY_OVERRIDE_X2_VALUE 0x28u
#define QUD_PSCI_SMC_RESET 0
#endif
