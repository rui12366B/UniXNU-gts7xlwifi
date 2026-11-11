# UniXNU-gts7xlwifi

iPadOS 17.7.11 (21H461) on the **Samsung Galaxy Tab S7+ Wi-Fi (SM-T970, gts7xlwifi, Qualcomm SM8250 / Adreno 650)**, as a
[UniXNU](https://unixnu.com/) device repository. 中文说明见下方。

## Status (2026-11-11, test build "main7", on upstream UniXNU main)

| Area | State |
| --- | --- |
| Boot, CPU | UEFI menu → XNU; all 8 cores, IPIs on SPI 544+ (`armcpu-spi-ipi-base`, SPIs 1008+ are secure here); 3 scheduling clusters 4 Silver + 3 Gold + 1 Prime (`t970-three-clusters.patch`, `die-cluster-id`) |
| CPU power | EPSS load + perfcontrol DVFS (`cpufreq=4`, `package/CpuFreqProbe.cpp`), TSENS throttle |
| Memory | About 6.7 GiB usable: DRAM beyond the first window and its holes (`t970-dram-extend`, `t970-dram-holes`) plus Android-RAM runs below the window adopted page by page after the VM starts (`t970-memory-outside.patch`, `memory-outside=1 memory-outside-floor=0xd0000000`) |
| Storage | Native UFS root (signed OS image) and persistent Data container (`drivers/ShengUfs`) |
| Display | S6TUUM3 dual-DSI panel via GOP framebuffer + DPU direct scanout, DCS brightness |
| GPU | Adreno 650 kernel driver (`drivers/Adreno650`) + AdrenoMetal (metal2vulkan → Turnip) compositing the whole UI; sRGB/BGRA8 display policy (12 `text_fixups`, `artwork-display-gamut = sRGB`) fixed the XR10 stripes; posters get the GPU by entitlement (`t970-sandbox-grant-entitlement.patch`) |
| Touch, keys | STM fts1ba90a over GPIO I2C → HID; PMIC keys |
| USB | QcomDwc3 device mode with Apple's USB stack (`drivers/QcomDwc3`, `t970-usb=5 t970-qud-retry=1`): the Mac's usbmuxd and lockdownd see an iPad and Finder lists it (high speed). Fallback: the CDC-ACM debug console (`drivers/QcomXnuUsbDebug`, `qcom-usb-debug=1`); USB 3 PHY up but the link trains at high speed so far |
| Battery | MAX77705 fuel gauge + charger as IOPMPowerSource (`package/T970Battery.cpp`, `t970-battery=1`); charger watchdog kicked |
| Audio | Speakers work as the iOS output: ADSP loader (GLINK/SMEM/SMP2P/QMP), APR → AFE/ADM/ASM, PRI TDM, four CS35L41 amps, HAL plug-in (`package/T970Audio.cpp`, `hal/`, `t970-adsp=2 t970-audio=32`). Clean sound since SCLK_INV and the ring fix; quieter than Android (no Cirrus protection firmware yet). Microphone works (DMIC3 via the TX macro, on by default; Voice Memos records and plays back) |
| Haptics | Vibration motor on PM8150L LDO7 (`package/T970Haptics.cpp`, `t970-haptics`) |
| Shutdown | Works: failsafe PSCI call after unmount (`t970-shutdown-failsafe.patch`), amps muted first |
| Wi-Fi | QCA6390 with Linux ath11k and a wpa_supplicant service (`t970-wifi=6 t970-wifi-svc=1`): joins WPA2, password kept across boots, status-bar icon with signal bars, Safari and Apple News load pages (Safari needs `no-lc=1`) |
| Bluetooth | Works: controller firmware at 3 Mbaud over the UART pipe with a replacement BlueTool (`t970-bt=257`) and a patched bluetoothd (skips commands this QCA6390 lacks, legacy LE only); Settings turns Bluetooth on |
| Sensors | SLPI boots and announces the sensor service without FastRPC (`t970-sensors=243`); accelerometer/gyro/orientation HID devices are opened by locationd and backboardd follows them (a fake gravity source rotates the UI). Real accelerometer streaming is next; rotation currently triggers a GPU-side reset under investigation, so the default holds portrait |
| S Pen | Works as an Apple Pencil: Wacom digitizer on I2C (`package/T970Pen.cpp`, `t970-pen`) published as a pencil digitizer service (0x0D/0x20), correct orientation, pressure in Notes (force scaled to UIKit's 0..1000 range, `AccurateMaxDigitizerPressureValue`). Side button, eraser and hover are not offered by iPadOS for this Pencil-1 model |
| Camera | Front sensor streams RAW10 with auto exposure into a T970Camera service; a CoreMediaIO camera extension (`tools/t970camerad`, `t970-camera=513`) is discovered by mediaserverd as an external camera, but no app uses it yet |
| Charging | Rp/BC1.2-based input and charge limits; 9 V PD/AFC opt-in (`t970-hv`) |
| Keychain | Software key wrapping (AppleKeyStoreState 8c70daf with `native_aks_device_id`); system daemons can create directories under /var again (`t970-union-ro-create.patch`) |
| Known issues | UI rotation can reset the SoC (being bisected); the first boot after a new addon writes files and takes longer; Notes needed a GPU sandbox grant (`MobileNotes` in `NativeSandboxGrantProcess`) |

## Not in this repository

Apple components (kernelcache, IPSW, trust caches), the GPU userspace addon, and all Qualcomm/Samsung firmware. See
`proprietary-files.txt`; pull the files from your own tablet into `.cache/firmware/`, then run
`python3 scripts/gen-gpu-firmware-a650.py` to create `package/GpuFirmwareA650.c`.

## Build

With a UniXNU checkout of upstream `main` (tested at 62be7bc) whose `unixnu.local.toml` points `device` here:

1. Apply the core carries in `docs/host-patches/unixnu-main/*.patch` to UniXNU (`git am`: cluster ids in the device tree, the
   Turnip retire-safe BO patch, the self-bind fix, the AppleKeyStoreState 8c70daf pin), then
   `docs/host-patches/adreno-metal-main/*.patch` to `drivers/AdrenoMetal` and `docs/host-patches/gop-framebuffer-main/*.patch` to
   `drivers/GopFramebuffer`, and pin both results in the superproject (`unixnu build kernel` resets submodules to the pin).
2. `./unixnu defconfig gts7xlwifi_defconfig`, then `UNIXNU_UEFI=<uefi tree> ./unixnu build kernel` (Python.org builds may need
   `SSL_CERT_FILE=/etc/ssl/cert.pem`).
3. `./unixnu build payload --trust-cache <owner trust cache>` — without `--trust-cache` the menu reports "Compromised Data".
4. Copy `EFI/UniXNU/Payloads/kernel.macho{,.adt}` out of `payload.img` (mtools) to the ESP's `Payloads/gts7xlwifi/`, with
   `docs/menu/unixnu.toml` as `EFI/UniXNU/unixnu.toml` (kernel command line ≤ 1023 bytes). Boot arguments use upstream's names
   without the old `sheng-` prefix.

Logs: the boot log ring lives in a spare partition and is read from a root recovery shell with `tools/read-boot-log.py --port <adb serial>`;
with the MUIC switch closed and `qcom-usb-debug=1`, the live CDC log is `/dev/cu.usbmodem*` on the Mac.

## 中文简介

在三星 Galaxy Tab S7+（SM-T970，骁龙 865）上运行 iPadOS 17.7.11，基于最新的上游 UniXNU。已经可以日常使用：八核与 4+3+1 调度簇、按负载调频、
约 6.7 GB 可用内存、原生 UFS 存储与可持久化的 Data、GPU 加速合成（Adreno 650，出错后可自动恢复）、触摸与按键、亮度、扬声器和麦克风、振动、
关机断电、充电。Wi-Fi 可以上网（有信号格，Safari 和 Apple News 可用，密码会保存），蓝牙可以打开使用，S Pen 可作为 Apple Pencil 使用（方向正确、有压感），
USB 连接 Mac 时 Finder 会识别为 iPad。传感器协处理器已能启动，自动旋转和相机还在做。仓库不包含任何苹果、高通、三星的二进制文件，
需要从自己的设备提取，见 `proprietary-files.txt`。

## Credits

Built on [UniXNU](https://github.com/UniXNU/UniXNU), the SM8250 layer [FFT-infra/UniXNU-sm8250-common](https://github.com/FFT-infra/UniXNU-sm8250-common),
UniXNU-alioth (Adreno650, NativeUfs-alioth, QcomXnuUsbDebug), UniXNU-sheng (MhiBoot, Wi-Fi probe), and
[RigoLigoRLC/unixnu-sm-t733](https://github.com/RigoLigoRLC/unixnu-sm-t733) (display format policy, text repage, scheduler
topology, perfcontrol DVFS, battery service, PSCI power). Each vendored driver keeps its LICENSE; origins are listed in `feeds.toml`.
