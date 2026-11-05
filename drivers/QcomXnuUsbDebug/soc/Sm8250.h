/* SPDX-License-Identifier: MIT
 * Kona (SM8250) GCC + QSMMUv500 + DWC3 profile for QcomXnuUsbDebug.
 *
 * Target: Xiaomi Redmi K40 "alioth", SM8250-AC, Adreno 650, 4 KiB pages.
 *
 * Register layout assumed = the SM8550 profile's layout: Qualcomm QSCRATCH
 * wrapper, clk_branch2 CBCRs, one GDSC, non-secure QSMMUv500 (SMMUv2
 * programming model), inherited ABL DEVICE-mode PHY and power.  Only the
 * resource values below differ, and two *checks* differ; see the notes.
 *
 * Every value names its source.  The workspace's own device trees carry no
 * USB node, so the hardware facts come from the pinned Linux tree that this
 * workspace already uses for SM8250 facts (accessible here as
 * alioth/tmp/kbuild/source -> /Volumes/Disk/xinyang/Lab/Bridge/tmp/gunyah-probe),
 * cross-checked against the Android DT that the board's UEFI ships in
 * NEW/uefi-alioth/Resources/DTBs/alioth.dts.
 */
#ifndef QUD_SM8250_H
#define QUD_SM8250_H
#define QUD_SOC_NAME "sm8250-inherited"

/* Development IDs inherited from m1n1 and from the SM8550 profile; not a
 * product allocation.  Kept identical so one host collector finds both. */
#define QUD_USB_VID 0x1209u
#define QUD_USB_PID 0x316du
#define QUD_USB_SERIAL "QcomXnuUsbDebug"
#define QUD_USB_PRODUCT "Qualcomm XNU Debug"

/* GCC: sm8250.dtsi:953-955  gcc: clock-controller@100000, reg 0x0 0x00100000 0x0 0x1f0000.
 *   alioth.dts:3729-3731 agreement: qcom,gcc@100000 reg <0x100000 0x1f0000>.
 * The 0x1f0000 size (not the SM8550 profile's 0x40000) is what covers the
 * 0x12000 QUSB2PHY_PRIM_BCR and the 0xf0xx USB3-PRIM window in one mapping. */
#define QUD_GCC_BASE 0x00100000ULL
#define QUD_GCC_BYTES 0x1f0000u

/* DWC3 core: sm8250.dtsi:4220-4222  usb_1_dwc3: usb@a600000, reg 0x0 0x0a600000 0x0 0xcd00.
 * The core window is 0xcd00, but the QSCRATCH wrapper window is a separate
 * node at 0x0a6f8800 (sm8250.dtsi:4174-4176).  Both are inside the single
 * 1 MiB window below, which the shared core address-whitelists as one range.
 * The window therefore spans the wrapper and the core exactly as the SM8550
 * profile's 0x100000 window does. */
#define QUD_DWC3_BASE 0x0a600000ULL
#define QUD_DWC3_BYTES 0x100000u

/* Apps SMMU: sm8250.dtsi:5878-5880  apps_smmu: iommu@15000000, reg 0x0 0x15000000 0x0 0x100000,
 * compatible "qcom,sm8250-smmu-500", "qcom,smmu-500", "arm,mmu-500".
 * Upstream (Android) form: alioth.dts:11910-11913 apps-smmu@15000000,
 * compatible "qcom,qsmmu-v500", reg <0x15000000 0x100000 0x15182000 0x20>. */
#define QUD_SMMU_BASE 0x15000000ULL
#define QUD_SMMU_BYTES 0x100000ULL

/* USB stream ID.  sm8250.dtsi:4224  iommus = <&apps_smmu 0x0 0x0>.
 * alioth.dts:18512 identical: iommus = <0x47 0x00 0x00>.
 * NOTE: this is 0x0 on SM8250, not the SM8550 profile's 0x40.  The module's
 * stream-match scans are ID-driven, so only this value changes. */
#define QUD_STREAM_ID 0x0u

/* Device-visible IOVA for the module's own DMA arena.  Legacy-style 32-bit
 * address, 2 MiB aligned, inside the SMMU output range.  The SM8550 profile
 * uses 0x90000000; this profile keeps the same value so the 2 MiB-aligned
 * three-page table builder's static assertion still holds and no new
 * constraint is introduced.  No SM8250 source in this workspace states which
 * IOVA the firmware used for its own USB buffers; this value is ours alone,
 * and it must not collide with a live stream (the installer proves that by
 * scanning the table before it writes). */
#define QUD_DMA_IOVA 0x90000000ULL

/* The five CBCRs inspected before the DWC3 is touched.
 * Source: Linux drivers/clk/qcom/gcc-sm8250.c, clk_branch halt_reg.
 *   master   (GCC_USB30_PRIM_MASTER_CLK,     id 173) 0xf010  BRANCH_HALT_VOTED  :2976-2977
 *   sleep    (GCC_USB30_PRIM_SLEEP_CLK,      id 178) 0xf018  BRANCH_HALT       :3012-3013
 *   utmi     (GCC_USB30_PRIM_MOCK_UTMI_CLK,  id 175) 0xf01c  BRANCH_HALT       :2994-2995
 *   phy-aux  (GCC_USB3_PRIM_PHY_AUX_CLK,     id 185) 0xf054  BRANCH_HALT       :3074-3075
 *   phy-com  (GCC_USB3_PRIM_PHY_COM_AUX_CLK, id 187) 0xf058  BRANCH_HALT       :3092-3093
 * Enable bit is BIT(0) and CLK_OFF is BIT(31) for each (clk-branch.c).
 * A sixth branch, gcc_usb3_prim_phy_pipe_clk 0xf05c (:3110), exists but is
 * deliberately NOT in the inspected set: it is BRANCH_HALT_DELAY and it is
 * clocked by the PHY, so it can legitimately read off while the PHY idles.
 * The SM8550 profile's five entries are Kalama offsets and do not apply. */
#define QUD_GCC_CBCR_OFFSETS {0xf010u, 0xf018u, 0xf01cu, 0xf054u, 0xf058u}

/* The last two CBCRs are the SuperSpeed PHY's auxiliary clocks. This board's
 * usb_3_ssphy is status = "disabled" in the firmware device tree and the controller runs at
 * high speed, so those two branches may legitimately read halted: nothing in the firmware
 * enables them, and refusing the controller because of them would refuse a working port.
 * The first three are the ones a high-speed gadget needs, and those are still required. */
#define QUD_GCC_CBCR_REQUIRED 3u

/* Block reset for the primary USB3 wrapper.
 * gcc_sm8250_resets[] :3570  [GCC_USB30_PRIM_BCR] = { 0xf000 } */
#define QUD_GCC_BCR_OFFSET 0xf000u

/* usb30_prim_gdsc: gcc-sm8250.c:3264-3270  .gdscr = 0xf004, pwrsts PWRSTS_RET_ON.
 * Legacy GDSCR bit semantics (gdsc.c:21,26,27): PWR_ON BIT(31),
 * HW_CONTROL BIT(1), SW_COLLAPSE BIT(0). */
#define QUD_GCC_GDSCR_OFFSET 0xf004u

/* CFG_GDSCR = GDSCR + 0x4 (gdsc.c:35), POWER_UP_COMPLETE BIT(16) (gdsc.c:32).
 * DIFFERENCE FROM SM8550, recorded because it changes a check:
 * usb30_prim_gdsc has no .flags and gcc-sm8250.c never uses POLL_CFG_GDSCR
 * (grep -c POLL_CFG_GDSCR = 0), so Linux's gdsc_check_status() takes the
 * legacy branch and tests PWR_ON on the GDSCR itself (gdsc.c:87-92).
 * The module's QcomUsbInspectInherited instead requires CFG_GDSCR bit 16.
 * On SM8250 that bit is not the documented power-up indicator.  Measured
 * value on hardware is unknown; the port therefore keeps the module's check
 * but ALSO requires GDSCR.PWR_ON, and reports which one failed through
 * --- the status word, so one flash distinguishes the two cases.
 */
#define QUD_GCC_CFG_GDSCR_OFFSET 0xf008u

/* This profile's GDSC has no qcom,support-cfg-gdscr flag, so the power state is the legacy
 * GDSCR.PWR_ON bit, not CFG_GDSCR.POWER_UP_COMPLETE. QcomUsbInspectInherited uses this macro to
 * pick the right bit; a profile that leaves it undefined keeps the CFG_GDSCR check. */
#define QUD_GDSC_POWER_BIT_LEGACY 31u

/* QSCRATCH inside the wrapper window.
 * Wrapper base 0x0a6f8800 (sm8250.dtsi:4176) + QSCRATCH_HS_PHY_CTRL 0x10
 * (Linux drivers/usb/dwc3/dwc3-qcom.c:26) -> absolute 0x0a6f8810.
 * Expressed as a window-relative offset: 0x0a6f8810 - 0x0a600000 = 0xf8810.
 * (The SM8550 profile uses the same 0xf8810 for its own wrapper, whose base
 * differs; the relationship, not the literal, is what carries over.)
 * Bit meanings (dwc3-qcom.c:27-28): UTMI_OTG_VBUS_VALID BIT(20),
 * SW_SESSVLD_SEL BIT(28).  dwc3_qcom_vbus_override_enable() sets both
 * (:117-129).  SS_PHY_CTRL 0x30 / LANE0_PWR_PRESENT BIT(24) (:30-31) is not
 * owned by this module because the board runs USB 2.0 only. */
#define QUD_QSCRATCH_HS_PHY_CTRL_OFFSET 0xf8810u
#define QUD_QSCRATCH_VBUS_MASK ((1u << 20) | (1u << 28))

/* PSCI SYSTEM_RESET through SMC is qualified on this board by the existing
 * diagnostic reset path already used for probe images.  The reboot command
 * still only installs the final restart callback when PE_halt_restart is
 * NULL, so it can never displace another owner. */
#define QUD_PSCI_SMC_RESET 1

/* Not software-visible from any source in this workspace, left at the
 * module defaults from include/QcomUsbScreenshot.h. */
#endif
