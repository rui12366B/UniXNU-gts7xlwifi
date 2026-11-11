# QcomDwc3, SM8250 port (SM-T970, iPadOS 17.7.11 21H461)

Device mode of [UniXNU/QcomDwc3](https://github.com/UniXNU/QcomDwc3) e4efe95 for this board: `QcomDwc3Xdci` derives from the
system's `AppleUSBXDCI` and supplies the register window, interrupt, power and PHYs. `IOUSBDeviceFamily`, `AppleUSBDeviceMux`,
configd's `USBDeviceConfiguration` and `lockdownd` run unchanged. `QcomDwc3Sm8250.cpp` is BSD-2-Clause-Patent
(`LICENSES/BSD-2-Clause-Patent.txt`), adapted from upstream's `QcomDwc3.cpp`; it includes the GPL-2.0-only QMP table
`../QcomXnuUsbDebug/soc/Sm8250QmpUsb3.h` generated from Linux.

| | SM8550 upstream | This port |
| --- | --- | --- |
| ABI | 24A446 arm64e fileset, hooks 179, 227, 267-278 | 21H461 arm64 split cache, hooks 179, 255-258, 263-265; registers at `this+0x238`, parameters at `+0x2410` |
| HS PHY | eUSB2 + PM8550B repeater | SNPS femto 0x088e3000, param-override e7/0b/2f/03 |
| SS PHY | QMP + PS5169 retimer, TLMM 11 | QMP (generated SM8250 table), lane from MAX77705 CC_STATUS0, no retimer (`t970-usb` bit 1) |
| SMMU | stream 0x40, highest free bank | stream 0x0, banks from NUMS2CB up, dead SMR reused, VMID 0xfc |
| Role | UCSI over GLINK, host mode | device only (host: later step) |

Build: `scripts/build-qcom-dwc3.py` (generates `OriginalUsbXdciABI.h`, audits the vtable, binds the original class).
Board entry: `package/T970UsbProbe.cpp`. Boot argument: `t970-usb=<bits>` (1 run, 2 SuperSpeed, 4 sampling, 8 no SMMU
context, 16 invert lane). Log: `[dwc3]` and `[dwc3 d]` rows. Plan and evidence: `docs/usb-plan.md` section 4.
