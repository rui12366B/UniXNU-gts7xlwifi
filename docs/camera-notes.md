# T970 cameras: hardware, iPadOS side, routes

Status 2026-11-08: C1, C2a, C3a and C4 device-verified (all three sensors answer over CCI and stream on their defaults; the front
sensor's MIPI stream is received cleanly by CSIPHY 3 + CSID-lite0, and one full 3280x2464 RAW10 frame lands in memory through
IFE-lite0, combo9; auto exposure converges, combo10); stage C5 (continuous capture, T970Camera user client, t970cam) in source.
`package/T970Camera.cpp`, boot argument `t970-camera=<bits>` (off by default).

## 0. Sensors (C1 readback, combo1 boot, kernel 60fc5e6c, `t970-camera=7`)

`[t970 camera] st 8 det 0 gdsc f822f000 cci 10070000 front 48ab0000 rear 30d5a001 wide 559b0000 irq 1 ms 56`

| slot | model ID (0x0000) | 0x0002/3 | part | resolution |
|---|---|---|---|---|
| front | 0x48ab | 0x0000 | Samsung ISOCELL S5K4HA (S5K4HAYX) | 8 MP, 1.12 µm |
| rear wide | 0x30d5 | 0xa001 | Samsung ISOCELL S5K3M5 | 13 MP, 1.0 µm, PDAF |
| rear ultra-wide | 0x559b | 0x0000 | Samsung S5K5E9 | 5 MP, 1.12 µm |

The parts come from matching the IDs against the chip-ID constants in public sensor drivers; no module identifier was read.
The 8/13/5 MP specs agree. GDSCR read back with PWR_ON set; CCI HW version 0x10070000 (v1.7); the whole C1 pass took 56 ms. The XO-fed CCI at about
100 kHz works on both CCI instances and on both masters of CCI0.


C2a (combo2, `t970-camera=23`): `sf 10901 sr 10a03 sw 10901`, i.e. frame_count 1 -> 9 (front), 3 -> 10 (rear), 1 -> 9 (wide)
over 300 ms with mode_select reading back 1 and no CCI errors. That is about 27, 23 and 27 fps on the defaults with the 19.2 MHz MCLK,
so all three timing generators and MIPI transmitters run without any init list. (BT, not the camera, reset the board at 72 s.)

C3a (combo5, `t970-camera=49`):
`[t970 camera c3] st 7 fmt 30a0a out 9a00cd0 len 9e20e68 rate 700 pll d8800005 rx 4003ff pkts 17852 ecc 0 crc 0 long 2b1004 short 1
csid 20000001 cfg 32100416 ref 2004 phy 20800104`

| field | value | meaning |
|---|---|---|
| fmt | 0x0a0a, lanes reg 3 | RAW10 in and out, 4 data lanes |
| out | 0x0cd0 x 0x09a0 | 3280 x 2464, the full 8 MP array |
| len | line 0x0e68 = 3688 pck, frame 0x09e2 = 2530 lines | |
| rate | 700 | 700 Mbit/s per lane (2.8 Gbit/s on 4 lanes) |
| pll | 0xd8800005 | PLL0 locked (bit 31), outputs on, RESET_N |
| rx | 0x4003ff | EOT and SOT seen on all four lanes (bits 0..7), long + short packets captured (8, 9); bit 22 = unmapped VC/DT, expected with no RDI configured |
| pkts / ecc / crc | 17852 / 0 / 0 | about 7.2 frames x 2466 packets in 300 ms (24 fps), no errors |
| long | 0x2b1004 | DT 0x2b RAW10, VC 0, word count 4100 = 3280 x 10 / 8 |
| cfg | settle 0x16, 4 lanes, assign 0x3210 | the computed settle count works as is |

So lane order 0-1-2-3, the 300 MHz timer settle of 22 and the 400 MHz CSID clock are right, and the receive side needs no change
for C4.

## 1. Hardware (live FDT, `evidence/recovery-live.dtb`, Samsung `cam_sensor` nodes)

The tablet was not on adb while this was written, so everything below comes from the FDT dump taken earlier in TWRP. Lane
counts and mode tables live in the camx sensor modules (`/vendor/lib64/camera/com.qti.sensormodule.*.bin` and Samsung's
`/vendor/etc/camera/*`), which were not read yet.

| | front (cell 1) | rear wide (cell 0) | rear ultra-wide (cell 2) |
|---|---|---|---|
| CCI | CCI0 `0xac4f000` master 1 (GPIO 103/104) | CCI0 master 0 (GPIO 101/102) | CCI1 `0xac50000` master 0 (GPIO 105/106) |
| address | 8-bit 0x5a, 7-bit 0x2d | 0x5a / 0x2d | 0x20 / 0x10 |
| CSIPHY | 3 (`0xac70000`) | 0 (`0xac6a000`) | 2 (`0xac6e000`) |
| MCLK | MCLK2, GPIO 96, 19.2 MHz | MCLK0, GPIO 94 | MCLK1, GPIO 95 |
| reset | GPIO 30 | GPIO 73 | GPIO 78 |
| VANA enable | GPIO 38 | GPIO 39 | GPIO 42 |
| VIO enable | GPIO 37 (shared by all) | GPIO 37 | GPIO 37 |
| extras | EEPROM 0xa2 (CCI1 m1) | actuator 0x18, EEPROM 0xb0, LED flash (CCI1 m1) | EEPROM 0xa0 |
| alt part | `cam-sensor@12` is a second-source part for the same slot (same pins) | | |

- Orientation: yaw 180 = rear, yaw 0 = front; roll 90 (rear) and 270 (front).
- Power: every rail is a GPIO load switch (`rgltr-*-voltage = 0`). The only supply node is `titan_top_gdsc` (camcc `0xad0c144`,
  parent MMCX, which the display already holds). The CSIPHYs also need `refgen` (`0x88e7000`) and `mipi-csi-vdd` = PM8150 L9 1.2 V.
- Clocks (camcc `0xad00000`, upstream `camcc-sm8250.c` offsets): CCI0/1 roots at 0xc0bc/0xc0d8 (37.5 MHz from PLL0 in Android;
  19.2 MHz XO is a valid table entry and is what C1 uses), MCLKn root at 0x5000+0x1c·n, branch at 0x5018+0x1c·n. CSIPHY timer 300 MHz,
  CSID/IFE 400-480 MHz, which need camcc PLL0/PLL2 (not yet programmed by anyone).
- CAMSS blocks in the FDT: CSIPHY 0-5 (`csiphy-v1.2.1`), CSID 0/1/2 (`csid480`, 2 = the "custom" one), CSID-lite 0/1, IFE 0/1,
  IFE-lite 0/1, plus BPS/IPE/ICP (Titan 480). SMMU: the `msm_cam_smmu_ife` context uses SIDs 0x800/0x801/0x840/0x841/0xc00/0xc01/
  0xc40/0xc41 (mask 0x400). ArmSmmu banks 6 (USB) and 7 (Wi-Fi) and stream 0x1801 (audio) are taken; camera has to pick a free bank.

## 2. Existing work

- Upstream UniXNU-next and the other device trees under `sources/` have no camera code at all: no CCI, CAMSS or camcc code anywhere.
  The only Linux pieces locally are dt-binding headers (`sources/periph/Ath12kWifi/deps/linux/include/dt-bindings/clock/qcom,camcc-sm8250.h`).
- Linux mainline has everything needed for a raw path on SM8250: `drivers/i2c/busses/i2c-qcom-cci.c`,
  `drivers/clk/qcom/camcc-sm8250.c`, and `drivers/media/platform/qcom/camss/` (camss-csiphy-3ph-1-0, camss-csid-gen2, camss-vfe-480).
  RB5 (SM8250) upstream streams IMX577 raw Bayer through CSIPHY → CSID → VFE RDI to memory. That is the reference.

## 3. How iPadOS takes frames (donor j120, iPad Pro 10.5 / A10X, 21H461)

- Kernel: one camera kext, `com.apple.driver.AppleH9CameraInterface` (class `AppleH9CamIn`, user client `AppleH9CamInUserClient`,
  `CamInChannel`, `AppleH9CamInFirmwareWorkProcessorRPCRequest`, `H9ISPLoadDataFileParams`). It drives Apple's own H9 ISP co-processor
  running RTKit firmware (`/usr/local/share/firmware/isp`) and expects ADT properties such as `isp-config`, `isp-clock-factor`,
  `isp-buffer`, `isp-aop-buffer`, `camera-hdr-version`.
- Userspace: `mediaserverd`/`cameracaptured` with the H9ISP/CMCapture stack talks to the user client. The protocol is ISP-firmware
  RPC (sensor bring-up, 3A, tuning blobs, output pools), not "here is a frame". The sensor drivers themselves live inside the ISP
  firmware and only know Sony/Apple modules.
- Also present: `com.apple.driver.AppleM2ScalerCSCDriver` (scaler/CSC, usable for format conversion later). iPadOS 17 also accepts
  USB (UVC) external cameras through AVFoundation's external device type. Where that stack lives in the system image was not checked
  (the system volume was not extracted here). It could be a second way in: a source that looks like an external camera.

## 4. Routes

### A. Raw Bayer through CAMSS + software debayer (realistic)

1. Done in C1: camcc GDSC, XO roots, CCI, sensor power and reset, chip ID.
2. Sensor init: the register list for one mode (e.g. 4-lane 10-bit full-size or a 2x2-binned preview). It comes from the camx sensor
   module on the device (read it from Android with adb; it does not go in the repo) or from a mainline/AOSP driver for the same part.
   Streaming on = 0x0100 = 1 for SMIA parts.
3. camcc PLL0 (and PLL2 for 24 MHz MCLK if the mode needs it), CSIPHY timer and CSID/IFE roots; refgen and L9 for the PHY.
4. CSIPHY 3ph-1-0 lane config (Linux `camss-csiphy-3ph-1-0.c`, gen2 tables for SM8250), CSID gen2 RDI0 pass-through, VFE-480 RDI
   write master to a DMA buffer (`camss-vfe-480.c`). One SMMU context bank for SIDs 0x800-0x841 (bypass or identity map), using a free
   bank, not 6/7, and not touching the audio stream.
5. Frames: RAW10 packed MIPI to RGGB/GRBG. Debayer, white balance, gamma and a simple AE loop (sensor gain/exposure over CCI) on the
   CPU, or as a Metal/Adreno compute shader once the GPU path takes compute. 8 MP at 15-30 fps is a lot for the CPU, so start with a
   binned mode around 2 MP.
6. Hand-off to iPadOS: there is no H9 ISP, so `AppleH9CamIn` cannot be fed directly. Options, cheapest first:
   a) a private IOService + user client that delivers NV12/BGRA IOSurfaces, plus a small app or test tool reading it (proves the
   pipeline, no system camera); b) present it as an external (UVC-style) camera if the iPadOS 17 external-camera path can be fed by a
   non-USB provider (needs investigating); c) emulate the `AppleH9CamIn` user-client protocol (reverse engineering the ISP RPC; large).

### B. Full ISP (Titan 480 IFE/BPS/IPE)

The IFE's own debayer, BLS, WB and CSC stages can be programmed directly without firmware (that is what the downstream `cam_isp`
driver does through CDM command buffers). BPS/IPE need the ICP (an A5 running Qualcomm firmware, loaded via PIL/SCM) and the
proprietary camx/chi tuning and node graph. That means a large userspace port of a closed stack. Realistic only for the IFE stages
(route A with IFE pixel path instead of RDI). Full ICP/BPS/IPE quality matching Android is out of scope.

## 5. Next stages (plan)

Order: front S5K4HA first (fewest extras: no actuator, no flash, CSIPHY 3, 8 MP). The rear S5K3M5 is the same family and comes next.

- **C2a, done:** the defaults stream (above).
- **C2b, mode table:** get the S5K4HA init and mode lists (global setting, then one 2-lane or 4-lane mode, ideally 2x2 binned
  ~1632x1224). Source: the device's own camx sensor module from Android (`adb pull /vendor/lib64/camera/com.qti.sensormodule.*.bin`
  into the gitignored `.cache/firmware/camera/`). Write a host script that turns it into a flat `{addr16, val8, delay}` list, and
  have the kernel read that list at runtime from `/private/var/ngpu/firmware/camera/` the way the Wi-Fi and GPU firmware are
  loaded. No vendor tables go in the repo. The same module gives the lane count, the data rate (for CSIPHY settle) and the
  Bayer order. Then check the frame counter again; the expected result is a steady fps.
- **C3a, done:** clean 4-lane RAW10 receive (above).
- **C3b (if C3a needs it):** proper S5K4HA mode from the camx module (C2b), e.g. 2x2 binned at a lower link rate.
- **C4, in source (`t970-camera=65`: front + bit 64; bit 64 includes the C3a setup):** CSID-lite0 RDI0 in payload-only mode for
  DT 0x2b, IFE-lite0 (0xacd9000, Linux camss-vfe-480 lite offsets) bus write master 0 in MIPI-raw mode, stride 4112 (4100 rounded
  to 16), one whole 3280x2464 frame (10.1 MB) into pil_cdsp_region (0x8e300000, 26 MiB, inside UEFI's "PIL Reserved"; no CDSP is
  started; 16 MiB mapped, so an overrun stays inside the carve-out). The apps SMMU gets one SMR (id 0x800, mask 0x441: the FDT's
  eight IFE stream IDs) pointing at a bypass bank, the same scheme T970Audio.cpp uses for 0x1801 (a bypass bank already in use is shared, else the first
  free one from NUMS2CB up), and it is removed after the frame. If another SMR already overlaps the group, C4 stops with route -4
  and the overlapping SMR in `det`. camnoc AXI goes to 400 MHz for the write. The buffer is filled with 0xa5 first. After the first
  write-master "done", the master is stopped, the sensor stops, and the frame is measured: written coverage, mean/min/max of the
  MSBs, plus a 64x48 one-Bayer-site thumbnail. Row `[t970 camera c4]` and 48 rows `[t970 camera thumb NN]`;
  `tools/camera-thumb.py logs/.../latest.txt --out front.pgm` turns them into a picture. `t970-camera-lines=<n>` crops vertically
  (RDI RPP vcrop), `t970-camera-pa=` moves the buffer. Risks: the SMMU writes (same as audio's), and AXI writes from a block
  nobody has configured QoS for. A freeze shows as `[t970 camera c4] st 4`.
  Expected: `st 7`, `top|bus` with bit 20 (bus bit 4 = WM0 done), `measure` = 0x09a00cd0 (2464 << 16 | 3280; the RDI format
  measure counts pixels, so it may show 4100 instead if it counts payload bytes), `written` close to 10.1M, and a thumbnail that
  shows the scene. On the power-on defaults it may be dark or flat, since nothing sets exposure or gain yet.
- **C4 combo6:** `[t970 camera c4] st -2 route fffffff7` = route -9: the S2CR and bank steps passed, but the single SMR id 0x800 mask 0x441
  did not read back (audio's exact-ID SMR 0x80001801 in the same boot did), so nothing was written and the route was torn down
  cleanly. Fixed: four SMRs as the FDT has them (0x800/0x801/0x840/0x841, mask 0x400), each retried as the exact SID if the masked
  form is refused. `route` now = kind | routed pairs << 4 (bit n+4 = exact-SID fallback) | first slot << 8 | bank << 16; `det`
  holds the first refused SMR as read back. If only exact SIDs stick and no frame arrives, IFE-lite writes with one of the
  `| 0x400` IDs (0xc00..0xc41), and those need their own exact SMRs next.
- **C4 combo9 (verified):** `[t970 camera c4] st 7 route 60852 ife 10000001 top|bus 110010 rdi 101ff8 measure 0 written 10093376
  stats 210f12 ms 65 pa 8e300000 stride 4112 fault 400` + 48 thumbnail rows showing a real (dark) scene with a light at the lower left.
  - `route 60852` (old layout, slot << 8 overlapped the exact-SID nibble): kind 2 = shared bypass bank, routed mask 0x5 = 0x800 and
    0x840 accepted with mask 0x400, 0x801 and 0x841 refused both masked and exact (read back 0, hence the main row's `det 7fffffff`),
    first slot 8, bank 6 (USB's bypass bank, shared as designed). So the hypervisor/SMMU accepts only some IDs, and IFE-lite0
    writes with an ID covered by 0x800/0x840 | 0x400. The layout is now kind | routed << 4 | slot << 12 | bank << 20.
  - `top|bus 110010`: top IRQ bit 4 (bus), bus bits 0 (RDI0 reg update) and 4 (WM0 composite done). `ms 65` from stream on to done.
  - `written 10093376` of 10102400 payload bytes (4100 x 2464; sampled every 64th byte, the rest are LSB bytes that happen to be 0xa5):
    the whole frame. `stride 4112` = 4100 rounded to 16. `measure 0`: the RDI format measure is not enabled the way gen2 wants; harmless.
  - `stats 210f12`: mean 18, min 15, max 33 (8-bit MSBs) = about 1 stop above the 10-bit pedestal of 64 (MSB 16): the power-on
    exposure/gain is far too short for the room. `fault 400` is the shared bank's FSR from earlier BT/audio traffic, as before.
  - camera-thumb.py's default `--scale 8` is why the PGM is 512x384 (64x48 x 8); `--scale 1` gives the raw 64x48.
- **C4b (in source, `t970-camera=193` = 1 + 64 + 128):** after C4's first frame, up to 8 more frames with coarse integration 0x0202
  and analogue gain 0x0204 (grouped hold 0x0104) scaled towards a mean MSB of 72 (pedestal 16 subtracted, at most x8 per step,
  /4 when clipped); exposure first up to FLL - margin (0x1006, keeps ~24 fps), then gain up to the sensor's 0x0086 max (fallback
  16x). The thumbnail and `stats` are then the last frame. Manual values: `t970-camera-exp=<lines>` / `t970-camera-gain=<code,
  0x20 = 1x>` (applied before streaming, with or without bit 128); `t970-camera-target=<24..230>`. Row:
  `[t970 camera c4b] st <iters | state << 8 | manual << 12> def <coarse | gain << 16 defaults> final <read back> gain <min | max << 16>
  coarse <min | margin << 16> means <frame 0..3, byte each, low first> <frame 4..7> tgt <target | black << 8 | ms << 16>`; state 1
  converged, 2 too dark at max exposure+gain, 3 too bright at min, 4 frame timeout, 5 iteration limit, 6 manual only, 7 CCI error.
  Expected: state 1 in 2-4 iterations, a thumbnail with a mean near 72. Adds ~0.5-1.5 s, no new hardware blocks.
  Optionally the full frame later through BootLog's export (needs coordination with the GPU pipeline-cache export).
- **C4b combo10 (verified):** `[t970 camera c4b] st 101 def 2004f0 final 7009de gain 2000020 coarse 40002 means 00004d18 00000000
  tgt ae1048`: converged (state 1) after 1 step in 174 ms. Defaults coarse 0x4f0 = 1264 lines at gain 0x20 (1x); final 0x9de = 2526
  lines (FLL 2530 - margin 4, ~1/24 s) at gain 0x70 (3.5x). Gain codes 0x20..0x200 (1x..16x), coarse min 2. Means: 24 -> 77
  (target 72 +- 10). Thumbnail mean 95 (one Bayer site, probably green), a recognisable room.
- **C5, getting frames into iOS (in source).** The stock stack (mediaserverd/cameracaptured -> AppleH9CameraInterface RTKit RPC)
  cannot be fed without emulating the H9 ISP firmware, so C5 is our own path:
  - **C5a, bit 256: continuous capture.** After C4 (+ C4b) the sensor keeps streaming; the write master is re-armed per frame into
    2..4 buffers of the pil_cdsp carve-out (now mapped whole, 26 MiB = 0x1a00000 per the FDT; full frames 2 x 10.16 MB, binned 4),
    never into the buffer the client reads. AE runs every 8 frames. At boot it streams `t970-camera-seconds` (default 5) and stops.
    Row `[t970 camera c5] st <state | sessions << 8> frames <n> timeouts <n> ms <n> fps10 <frames per 10 s> exp <coarse | gain << 16>
    mean <last AE mean> buf <buffers | KiB << 8> sites <MSB means (y0x0)|(y0x1)<<8|(y1x0)<<16|(y1x1)<<24> svc <service> uc <opens |
    starts << 8 | maps << 16 | closes << 24> ae <AE changes>`; state 1 streaming, 2 done, 3 stopped by the client, ff too many
    timeouts, fe no room. Re-arming after each done may skip every other sensor frame (expect 12-24 fps); `sites` tells the Bayer
    order (two alike = green; under warm light R > B).
  - **C5b, bit 512: `T970Camera` IOService + user client** (package/T970CameraService.cpp, ABI package/T970CameraAbi.h), published
    after the boot run (classes registered between preModLoad/postModLoad like the audio service; build step in
    scripts/build-gop-legacy-display.py). Memory type 0 = control page (seq/latest/writing/reading, size, stride, exposure), type 1
    = the carve-out read-only. Methods: Start (powers the front sensor and runs C4 + AE + C5a until Stop or the client closes), Stop,
    SetExposure(lines, gain; 0,0 = auto). Sandbox grant for the process name `t970cam`. Tear-free hand-off: the client sets
    `reading`, the kernel sets `writing`, each checks the other after a barrier.
  - **C5c: tools/t970cam/t970cam.c** (scripts/build-t970cam.py: ad hoc signed + trust.json): opens the service, starts, reports
    delivered fps, writes the last frame as a half-resolution BMP (RAW10 MSBs, black 16, gray-world WB, gamma 2.2, `-m` mirror,
    `-b all` writes all four Bayer orders, `-e/-g` manual exposure). Needs staging onto the device (not done here).
  - **combo11a (verified, t970-camera=961):** `[t970 camera c5] st 2 frames 66 timeouts 0 ms 5033 fps10 131 exp 5109de mean 71 buf
    26c002 sites 56383756 svc 1 uc 0 ae 0`: 13.1 fps delivered (every other sensor frame), no timeouts, AE held at 2526 lines / 2.5x,
    service published. Sites (y0x0) 86, (y0x1) 55, (y1x0) 56, (y1x1) 86: green on the (0,0)/(1,1) diagonal, so GRBG or GBRG.
  - Option `t970-camera-bin=1`: SMIA 2x2 binning (0x0900/0x0901) to 1640x1232 before the setup is read; untested on the S5K4HA.
  - **C6 (in source, 2026-11-11): the front camera as a CoreMediaIO camera extension -> AVFoundation external camera.** The 21H461
    image has no H9 ISP to feed and no UVCAssistant (j120 is Lightning), but the system pieces are all there: mediaserverd's
    BWFigCaptureDeviceVendor (CMCapture) is built with a CMIO extension discovery session (`_updateCMIOExtensionDevices`,
    `CMIOExtensionFigCaptureStream`), CoreMediaIO exports the full CMIOExtension API (provider/device/stream classes, iOS SDK .tbd), and
    com.apple.mediaserverd.plist publishes the launchd event stream `com.apple.cmio.registerassistantservice.system-extensions.matching`.
    A provider is exactly what macOS ships for its built-in cameras (appleh13camerad, cameracaptured, UVCAssistant launchd plists): a
    job subscribed to that stream with `CMIOExtensionBundleIdentifier`, a Mach service of the same name, and
    `CMIOExtension/CMIOExtensionMachServiceName` in the embedded Info.plist; `+[CMIOExtensionProvider startServiceWithProvider:]`.
    - `tools/t970camerad/t970camerad.m`: provider + device ("T970 Front Camera", transport 'usb ') + stream, formats 1280x720 (centre
      crop) and 1640x1232, 420v IOSurface pixel buffers; on stream start it calls T970CameraStart, takes frames tear-free, debayers
      GRBG (black 16, gray world, gamma 2.2, BT.601 video range) and sends them with the kernel's done timestamps (~13 fps).
      Reports stage bits to the kernel (new selector T970CameraReport -> `[t970 camera c5] ... d <d0> <d1> <frames> <error>`).
    - `scripts/build-t970camerad.py`: builds against the macOS SDK's CoreMediaIO headers (iOS has only the .tbd), one signed binary
      per identity (org.unixnu.t970camerad = d0, com.apple.cmio.uvcassistantextension = d1, in case mediaserverd only accepts the
      UVC provider name), adds RunAtLoad/KeepAlive root jobs with the LaunchEvents subscription to the BlueTool script's launchd
      cache, addon cam-bin/ + wifi-xpc/, trust module. Exec paths added to t970-wifi-service-mount.patch's exemption list.
    - d0/d1 stage bits: 1 started, 2 kernel service open, 4 provider service started, 8 device registered, 0x10 host connected
      (mediaserverd discovered it), 0x20 stream start requested, 0x40 kernel streaming, 0x80 first frame sent, 0x100 stopped,
      0x200 host disconnected; slot in bits 28+. Logs: /private/var/log/t970camerad(-uvc).log (exported by BootDiagExport.c).
    - **main5b (2026-11-11, cam1 addon, t970-camera=513):** `d 1f 1000001f 0 0`: both identities started, opened the kernel
      service, registered, and mediaserverd (pid 29) connected to both (logs: "client connected: pid 29"). No stream was ever
      requested: FaceTime showed black and offers no camera switch on this iPad. CoreMediaIO's device properties (13 exported on
      21H461) have no position/built-in flag, so a CMIO device can only be an *external* camera (AVCaptureDeviceTypeExternal,
      position unspecified); built-in front/back positions come from mediaserverd's default (H9 ISP) device. AVFCapture also knows
      an app Info.plist key `NSCameraUseExternalDeviceType`; no app on the 21H461 image declares it.
    - **C6b (in source): AVFoundation probe inside the daemon** (`-p 25`, first identity): 25 s after start it lists the cameras
      AVFoundation offers (log lines, `[17]`), then runs an AVCaptureSession + VideoDataOutput (BGRA) on our device for 6 s (so
      mediaserverd starts our stream exactly as an app would), counts frames (`[18]`) and writes the first frame to
      /private/var/log/t970camerad-avf.bmp (exported). Entitlements from 21H461 CMCapture: capture.allow, nonstandard-client.allow
      (+ allowed-media-types), background-camera-access, tcc.allow kTCCServiceCamera. Row: `... p <stage> <devices> <frames>`;
      stage 1 started, 2 listed, 4 ours listed, 8 input, 0x10 running, 0x20 first frame, 0x40 BMP, 0x80 done, | type << 8 (1
      external, 2 built-in); devices = total | built-in << 8 | external << 16 | ours << 24.
    - **C7 plan (built-in front camera for Camera/FaceTime/Control Center):** once C6b shows frames through AVFoundation, make
      mediaserverd present our CMIO device as the built-in front camera. That logic lives in CMCapture (dyld shared cache), so the
      route is a re-signed copy of mediaserverd with an added LC_LOAD_DYLIB (as the BT work re-signs bluetoothd and points its job
      at /private/var/ngpu) loading a small dylib that hooks BWFigCaptureDeviceVendor / FigCaptureSource creation to map position
      front + builtInWideAngleCamera onto our device. Alternative: emulate AppleH9CamIn (much larger).
 (FaceTime's camera menu, Safari getUserMedia, AVCaptureDevice
      .external). The Camera app and Control Center use only built-in cameras and stay black.
 a viewer app (IOSurface + Metal debayer) and then whether iPadOS 17's external-camera (UVC/CMIO extension) path
    can host a virtual camera fed by the service, so AVFoundation apps see it (route 4.A.6 b).

Risks: the S5K3M5 PDAF and actuator (0x0c on CCI1 m1) and the flash come later. CCI1 m1 also holds the EEPROMs (0xa0/0xa2/0xb0),
which carry calibration (shading, AWB) and are cheap to read once C2b lands.

## 6. Stage C1..C4 probe

`t970-camera=<bits>`: 1 front, 2 rear, 4 ultra-wide, 8 leave the last sensor powered, 16 stream test (C2a), 32 CSIPHY/CSID
receive test on the front sensor (C3a; second row `[t970 camera c3]`, fields in the header of T970Camera.cpp), 64 one-frame capture
(C4; rows `[t970 camera c4]` and `[t970 camera thumb NN]`), 128 auto exposure with 64 (C4b; row `[t970 camera c4b]`), 256 continuous capture and 512 the T970Camera service (C5; row
`[t970 camera c5]`). Row
`[t970 camera] st <stage> det <detail> gdsc <GDSCR> cci <CCI HW version> front <id> rear <id> wide <id> irq <last CCI IRQ> ms <n>`.
`front/rear/wide` are the four bytes at register 0x0000 (model ID high, low, then 0x0002/0x0003), or -1 NACK (`ffffffff`),
-2 timeout, -3 other error, 0 not tried. Stage 8 = done; negative stage = failed at that step (see the header of T970Camera.cpp).
`sf/sr/sw` (C2a): first frame_count | second << 8 | mode_select read back << 16 | CCI rc << 24 (rc 0xff = NACK).

## 7. To read from Android next time (adb)

```
adb shell ls /vendor/lib64/camera /vendor/etc/camera
adb shell 'dmesg | grep -iE "cam_sensor|cam_cci|csiphy|eeprom|sensor_id|slave"'
adb shell cat /sys/class/camera/rear/rear_camtype /sys/class/camera/front/front_camtype
```
Those give part numbers and the per-mode lane count, data rate and init lists (keep the binaries out of the repo).
