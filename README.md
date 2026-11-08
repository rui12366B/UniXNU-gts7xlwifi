# UniXNU-gts7xlwifi

iPadOS 17.7.11 (21H461) on the **Samsung Galaxy Tab S7+ Wi-Fi (SM-T970, gts7xlwifi, Qualcomm SM8250 / Adreno 650)**, as a
[UniXNU](https://unixnu.com/) device repository. 中文说明见下方。

## Status (2026-11-08, test build "combo7")

| Area | State |
| --- | --- |
| Boot, CPU | UEFI menu → XNU; all 8 cores, IPIs on SPI 544+ (`armcpu-spi-ipi-base`, SPIs 1008+ are secure here); 3 scheduling clusters 4 Silver + 3 Gold + 1 Prime (`t970-three-clusters.patch`, `die-cluster-id`) |
| CPU power | EPSS load + perfcontrol DVFS (`sheng-cpufreq=4`, `package/CpuFreqProbe.cpp`), TSENS throttle |
| Memory | DRAM beyond the first window and its holes (`t970-dram-extend`, `t970-dram-holes`) |
| Storage | Native UFS root (signed OS image) and persistent Data container (`drivers/ShengUfs`) |
| Display | S6TUUM3 dual-DSI panel via GOP framebuffer + DPU direct scanout, DCS brightness |
| GPU | Adreno 650 kernel driver (`drivers/Adreno650`) + AdrenoMetal (metal2vulkan → Turnip) compositing the whole UI; sRGB/BGRA8 display policy (12 `text_fixups`, `artwork-display-gamut = sRGB`) fixed the XR10 stripes; posters get the GPU by entitlement (`t970-sandbox-grant-entitlement.patch`) |
| Touch, keys | STM fts1ba90a over GPIO I2C → HID; PMIC keys |
| USB | DWC3 + SNPS femto HS PHY; CDC-ACM debug console (`drivers/QcomXnuUsbDebug`) works once the MAX77705 MUIC switch is closed (CONTROL1 0x3f → 0x09, `package/T970WifiProbe.cpp`) |
| Battery | MAX77705 fuel gauge + charger as IOPMPowerSource (`package/T970Battery.cpp`, `t970-battery=1`); charger watchdog kicked |
| Audio | Speakers work as the iOS output: ADSP loader (GLINK/SMEM/SMP2P/QMP), APR → AFE/ADM/ASM, PRI TDM, four CS35L41 amps, HAL plug-in (`package/T970Audio.cpp`, `hal/`, `t970-adsp=2 t970-audio=32`). Clean sound since SCLK_INV and the ring fix; quieter than Android (no Cirrus protection firmware yet) |
| Haptics | Vibration motor on PM8150L LDO7 (`package/T970Haptics.cpp`, `t970-haptics`) |
| Shutdown | Works: failsafe PSCI call after unmount (`t970-shutdown-failsafe.patch`), amps muted first |
| Wi-Fi | QCA6390 with Linux ath11k (stage 6, `t970-wifi=6`): firmware reaches mission mode, QRTR/QMI up, board-file download stops on an MHI BAD_TRE; not usable yet |
| Bluetooth | QCA6390 BT over QUP0 SE6 UART: power, clocks, SMMU and DMA up (`t970-bt`); HCI readback and firmware download in progress |
| Sensors | All behind the SLPI (FastRPC needed); fake accelerometer/gyro/orientation HID devices for rotation testing (`t970-sensors`); rotation not working yet |
| Camera | Front/rear/ultra-wide sensors answer over CCI and stream; CSIPHY/CSID receive works (`t970-camera`); nothing in iPadOS consumes frames yet |
| Known issues | GPU page fault can freeze the system (Settings > Wi-Fi), fix in testing; Apple USB device stack (Finder/usbmux) not started |

## Not in this repository

Apple components (kernelcache, IPSW, trust caches), the GPU userspace addon, and all Qualcomm/Samsung firmware. See
`proprietary-files.txt`; pull the files from your own tablet into `.cache/firmware/`, then run
`python3 scripts/gen-gpu-firmware-a650.py` to create `package/GpuFirmwareA650.c`.

## Build

With a UniXNU checkout (this repo was built against UniXNU-next) whose `unixnu.local.toml` points `device` here:

1. Apply `docs/host-patches/unixnu-devicetree-cluster-ids.patch` to UniXNU (cpu `die-cluster-id`/`cluster-core-id`), and
   `docs/host-patches/adreno-metal/*.patch` to `drivers/AdrenoMetal` (pin the result in the UniXNU superproject; `unixnu build
   kernel` resets the submodule to the pinned commit).
2. `UNIXNU_UEFI=<uefi tree> ./unixnu build kernel` (Python.org builds may need `SSL_CERT_FILE=/etc/ssl/cert.pem`).
3. `./unixnu build payload --trust-cache <owner trust cache>` — without `--trust-cache` the menu reports "Compromised Data".
4. Copy `EFI/UniXNU/Payloads/kernel.macho{,.adt}` out of `payload.img` (mtools) to the ESP's `Payloads/gts7xlwifi/`, with
   `docs/menu/unixnu.toml` as `EFI/UniXNU/unixnu.toml` (kernel command line ≤ 1000 bytes).

Logs: the boot log ring lives in a spare partition and is read from TWRP with `tools/read-boot-log.py --port <adb serial>`;
with the MUIC switch closed, the live CDC log is `/dev/cu.usbmodem*` on the Mac.

## 中文简介

在三星 Galaxy Tab S7+（SM-T970，骁龙 865）上运行 iPadOS 17.7.11。已经可以进入桌面并流畅使用：八核与 4+3+1 调度簇、按负载调频、
原生 UFS 存储与可持久化的 Data、GPU 加速合成（Adreno 650）、触摸与按键、亮度、USB 调试串口、电量显示，扬声器出声（音质正常，
音量比安卓小）、振动、关机断电。Wi-Fi（固件已启动，卡在板级数据下载）、蓝牙、传感器（屏幕旋转）、相机还在做；打开“设置”偶尔
会因 GPU 缺页卡死，修复测试中。仓库不包含任何苹果、高通、三星的二进制文件，需要从自己的设备提取，见 `proprietary-files.txt`。

## Credits

Built on [UniXNU](https://github.com/UniXNU/UniXNU), the SM8250 layer [FFT-infra/UniXNU-sm8250-common](https://github.com/FFT-infra/UniXNU-sm8250-common),
UniXNU-alioth (Adreno650, NativeUfs-alioth, QcomXnuUsbDebug), UniXNU-sheng (MhiBoot, Wi-Fi probe), and
[RigoLigoRLC/unixnu-sm-t733](https://github.com/RigoLigoRLC/unixnu-sm-t733) (display format policy, text repage, scheduler
topology, perfcontrol DVFS, battery service, PSCI power). Each vendored driver keeps its LICENSE; origins are listed in `feeds.toml`.
