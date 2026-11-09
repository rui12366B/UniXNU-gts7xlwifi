# USB plan: charging limits and QcomDwc3 on SM-T970 (iPadOS 17.7.11, 21H461)

Status 2026-11-08: part 1 (charging) is written and syntax-checked and waits for a boot. Part 2 (QcomDwc3) is a plan plus a
feasibility check. No QcomDwc3 driver code exists yet.

## 1. Charging (done in `package/T970Battery.cpp`, `t970-battery=1`)

**Symptom.** In combo1 to combo8b, `CHG_DETAILS_01` reads `0x31` (fast charge) and `CHGIN_OK` is set. At about 44 % the gauge
still reports −300 to −800 mA, so the tablet loses charge while plugged in.

**Cause.** This driver kicks the charger watchdog, but it never sets the input or charge current. After the watchdog reset,
`CHG_CNFG_09` CHGIN_ILIM and `CHG_CNFG_02` CHG_CC stay at their 500 mA power-on defaults, which is less than the screen and GPU
draw. periph15's `[t970 muic]` row shows what the source offers: `BC_STATUS` 0x82 (VBUS, CDP host port) and `CC_STATUS0` 0x71
(CCIStat 3, Rp 3.0 A). `CHG_INT_OK` bit 7 (AICL_OK) clears on some cables (0x5f), which means AICL is also limiting the input.

**Fix.** The driver now follows Samsung's `max77705_charger.c` sequence, with its own code:

| Step | Register |
| --- | --- |
| Unlock protected charger registers | `CHG_CNFG_06` 0xbd bits 3:2 = 11 |
| Take the input limit from the USBC firmware | USBC 0x25 opcode 0x57 (`SAMSUNG_FW_AUTOIBUS`) data 0x01; `CHG_DETAILS_00` bit 7 then reads 1 |
| Read the cable | `CC_STATUS0` 0x0a bits 5:4 CCIStat; `BC_STATUS` 0x08 bits 1:0 ChgTyp |
| Input limit | `CHG_CNFG_09` = 0x80 (CHG_EN) \| (mA / 25 − 1) |
| Charge current | `CHG_CNFG_02` bits 5:0 = mA / 50, with OTG_ILIM kept |

The current values come from this tablet's own battery node in the live FDT. The higher of the two cable readings wins:

| Cable | Input mA | Charge mA |
| --- | --- | --- |
| Rp default / SDP | 475 | 475 |
| CDP | 1500 | 1500 |
| Rp 1.5 A | 1400 | 2100 |
| DCP | 1800 | 2100 |
| Rp 3.0 A | 3000 | 3150 |

The charge current is lowered to 1000 mA at a gauge temperature of 45 °C or more, and to 100 mA at 50 °C or more or below 0 °C.
The limits are re-applied every 10 s and whenever the CHGIN or CC state changes. The driver waits for the MUIC route
(`t970_muic[0]`) before it sends its opcode, so the two opcode sequences cannot interleave.

**combo10 result (first fix, Mac port).** Row: `lim 3000/3150mA src e08271 cnfg 13ec7ff7`. The limits were written and read
back:

| Field | Register | Value | Meaning |
| --- | --- | --- | --- |
| cnfg | CHG_CNFG_09 | 0xf7 | Charger enabled, 3000 mA input |
| cnfg | CHG_CNFG_02 | 0x7f | 3150 mA charge |
| cnfg | CHG_CNFG_06 | 0xec | Protected registers unlocked |
| cnfg | CHG_CNFG_04 | 0x13 | 4.35 V float |
| src | CC_STATUS0 | 0x71 | Rp 3.0 A on CC1 |
| src | BC_STATUS | 0x82 | CDP |
| src | CHG_DETAILS_00 | 0xe0 | Bit 7 set: the charger owns the limit, so `ibus 0` was harmless |

`CHG_INT_OK` changed from 0xdf to 0x5f on the next sample: AICL took over. VBUS sags to the CHGIN regulation point, so the Mac
port and this cable deliver no more at 5 V. The battery went from −245 mA to about 0 before the UI load (−700 mA). Only a higher
input voltage (below) or a better 5 V source helps.

The `[t970 charge]` row now measures this, with the gauge registers Samsung reads:

| Field | Register | Unit |
| --- | --- | --- |
| iin | IIN 0xd0 | 0.125 mA |
| vbyp | VBYP 0xb3 | 0.427 mV |
| isys | ISYS 0x43 | 0.3125 mA, less 60 % of the battery current |
| vsys | VSYS 0xb1 | 0.156 mV |
| dtl | CNFG_12, CHG_DETAILS_00, CHG_DETAILS_02, VBADC | raw bytes |

`ibus` reads 2 at once when the charger already owns the limit.

**9 V input (`t970-hv=<bits>`, default 0, opt-in).** These commands go through the USBC firmware at I2C 0x25, after Samsung's
`max77705_pd.c`, `max77705_usbc.c` and `max77705-muic-afc.c`.

Reading the source capabilities is always on. About 3 s after the plug, opcode 0x30 (CURRENT_SRCCAP) is sent, and its reply
fills the `[t970 charge]` row. The reply is byte [1] = selected position << 3 | PDO count, then the PDOs, 4 bytes each, little
endian. In a fixed PDO, bits 31:30 are 0, bits 19:10 are the voltage in 50 mV steps, and bits 9:0 the current in 10 mA steps.

| Bit | Source | Request | Accepted when |
| --- | --- | --- | --- |
| 1 | PD | opcode 0x32 SRCCAP_REQUEST [position] for the highest fixed PDO of at most 9 V (`battery,max_input_voltage` 9000) | Reply is not 0xfe/0xff, and VBADC (`USBC_STATUS1` 0x06 bits 7:4) reads 7.5–10.5 V within 2 s |
| 2 | AFC (DCP, no PD) | opcode 0x21 with tx byte 0x46 (9 V) | Reply [1] = 0, and the same VBADC check |

The sequence, for both:
- Before asking, the input limit drops to 500 mA (`battery,pre_afc_input_current`).
- At 9 V the driver takes 1650 mA input and 3150 mA charge. The 1650 mA is the lowest of: 15 W / 9 V (`pd_charging_charge_power`),
  the HV TA group's 1650 mA, and the PDO's own current. The temperature derating still applies.
- At 50 °C or more it asks for 5 V again: PDO 1 for PD, tx byte 0x08 for AFC.
- If VBADC falls back to 5 V, the driver returns to the 5 V limits.
- There is one attempt per plug.

**Not done.**
- PPS (the 45 W mode) feeds the separate direct-charger IC and is not attempted.
- QC 2.0 (AFC result 4, MPing NACK) is not attempted.
- The SNKCAP data is not sent.

**HV log row.** `[t970 charge] hv <result><sel><count><reads><state> pdo P1 P2 P3 P4 in <mA> vbadc <n> req <pos>`. States:
1 = read, 2 = none / not asked, 3 = PD 9 V, 4 = AFC 9 V, 5 = failed, 6 = lost, 7 = cooled back to 5 V.

**Log.** The `[t970 battery]` row gains `lim IN/CHGmA src <d00><bc><cc0> cnfg <c04><c06><c02><c09> wr N ibus K`. On the Mac port,
expect `lim 3000/3150`, a cnfg low byte of 0xf7, bit 7 set in the d00 byte, ibus 1 or 2, and a positive mA reading.

## 2. QcomDwc3: Apple's USB device and host stacks on this board's DWC3

Goal: Apple's `AppleUSBXDCI` and `AppleUSBXHCI` running on the SM8250 DWC3. That gives Finder/usbmux pairing, lockdownd, USB 3,
and host mode. Upstream reference: UniXNU `docs/zh/porting/usb.md`, `docs/zh/drivers/qcom-dwc3.md` and the QcomDwc3 component
(e4efe95, SM8550, iPadOS 27 24A446).

### 2.1 Feasibility: 21H461 class layouts (`scripts/gen-usb17-abi.py` → `scripts/usb-abi-21H461.json`)

The board's kernelcache is `kernelcache.release.ipad7`, the iPad Pro 12.9" (2nd gen, T8011) cache. It is an **arm64 split
prelinked cache without pointer authentication**, so vtable entries are plain pointers. Upstream's discriminator search is not
needed, and a subclass only has to keep the slot order. The cache contains everything the port needs:
`com.apple.driver.AppleUSBXDCI`, `com.apple.driver.usb.AppleUSBXHCI`, `com.apple.iokit.IOUSBDeviceFamily`,
`com.apple.iokit.IOUSBHostFamily`, `com.apple.driver.AppleUSBDeviceMux`, plus Apple's T8011 subclasses of both controllers.

| Class | Size 21H461 | Size 24A446 | Slots 21H461 | Slots 24A446 | Notes |
| --- | --- | --- | --- | --- | --- |
| `IOUSBDeviceController` | 0x228 | — | 234 | — | Parent of `AppleUSBXDCI` |
| `AppleUSBXDCI` | 0x2440 | 0x2650 | 267 | 280 | Abstract. Pure slots: 179, 255, 256, 257, 258, 264, 265 |
| `AppleT8011USBXDCI` | 0x2478 | — | 268 | — | Apple's SoC subclass. Overrides 0, 1, 7, 14, 86, 132, 179, 190, 196, 197, 213, 214, 244, 255–258, 264, 265; adds 267 |
| `AppleUSBXHCI` | 0xe48 | 0xf88 | 299 | 323 | Concrete (no pure slots) |
| `AppleNubT8011USBXHCI` | 0xe68 | — | 301 | — | Overrides 0, 1, 7, 14, 86, 132, 177, 180, 182, 183, 193, 240, 273, 289–297; adds 299, 300 |

**Verdict: feasible.** The 21H461 XDCI keeps the same design as 24A446: the SoC subclass supplies a small set of platform hooks.
Upstream overrides 10 hooks on 24A446:

- slot 179 (KEEP_CONFIGURATIONS, the `IOPort` workaround)
- TRANSPORT_ACTIVE
- DESIRED_POWER
- POWER_ON
- POWER_OFF
- PARAMETERS
- CONTROLLER_RESET
- PERIODIC_USAGE
- PERIODIC_SET
- the xHCI DESIRED_POWER, READ64 and WRITE64

On 21H461 the matching set is the 7 pure slots plus the T8011 overrides. Slot 179 is pure in both versions, which suggests the
same `IOPort` hook. Which 21H461 slot is which hook comes from disassembling `AppleT8011USBXDCI`'s overrides and the callers in
`AppleUSBXDCI` (`goOnBus`, `activateEndpoint`), as upstream did for 24A446. That mapping is step 0 of the implementation below.
Instance-size and field offsets change too: upstream reads `this + 0x58c` (periodic usage), which must be re-derived for 0x2440.

What does not change:

- `IOUSBDeviceFamily`, `AppleUSBDeviceMux`, `IOUSBHostFamily`, `lockdownd` and `configd` run as shipped.
- The kexts are already in the prelinked cache. Only the prelink roots need to add them (below).

### 2.2 SM8250 hardware inputs (live FDT `/soc/ssusb@a600000` and its PHYs; mainline sm8250.dtsi; QcomXnuUsbDebug `soc/Sm8250.h`)

| Input | SM8550 (sheng) | SM-T970 |
| --- | --- | --- |
| DWC3 core / Apple window | 0x0a600000, window at +0xc000 | 0x0a600000 (core 0xd93c), window at +0xc000, QSCRATCH 0x0a6f8800 |
| Core interrupt | INTID 165 | SPI 133 → INTID 165 (live FDT `interrupts = <0 0x85 4>`) |
| SMMU stream | 0x40 | **0x0** (`iommus = <apps_smmu 0x0 0x0>`) |
| HS PHY | eUSB2 + PM8550B repeater | **SNPS femto** at 0x088e3000. QcomXnuUsbDebug `t970_hsphy_init` already runs it: BCR 0x12000, FSEL 19.2 MHz, param-override e7/0b/2f/03, host variant e7/09/2f/03 |
| SS PHY | QMP combo 0x88e8000 (V6 PCS) | QMP USB3-DP combo at 0x088e8000 + 0x3000, `qmp_combo_offsets_v3`, V4 PCS, mainline `sm8250_usb3dpphy_cfg` tables |
| GCC | 0x39xxx | USB30_PRIM: BCR 0xf000, GDSC 0xf004, master 0xf010, sleep 0xf018, mock UTMI 0xf01c, PHY aux 0xf054, com aux 0xf058, pipe 0xf05c; QUSB2PHY BCR 0x12000 |
| Rails | ldoe1/e3/b15/b5/f3 | HS PHY: ldoa5 0.88 V, ldoa12 1.8 V, ldoa2 3.072 V (already voted by the debug-console retry). QMP: vdd 0.912 V and core 1.2 V; read the RPMh names from the live FDT phandles (0x187, 0x7d) |
| Retimer | PS5169 | None found in the FDT. The orientation comes from the MAX77705 (`CC_STATUS0` CCPinStat) or the `samsung,cc_dir` GPIO 65 |
| Cable role | UCSI over charger GLINK | **MAX77705 USBC at I2C 0x25** on QUP SE0 (`t970_se0_*`), see 2.4 |

The QMP table is generated from the pinned Linux tree, not copied from the vendor DT. It is a copy of upstream's
`gen-qmp-usb3-table.py` with these changes:

- serdes: `sm8150_usb3_serdes_tbl`
- tx, rx, pcs, pcs_usb: `sm8250_usb3_*_tbl`
- PCS register names: `QPHY_V4_PCS_*`

The live FDT's `qcom,qmp-phy-init-seq` is used only as a read-only cross-check. Done (§3): `scripts/gen-qmp-sm8250-usb3.py`
writes `drivers/QcomXnuUsbDebug/soc/Sm8250QmpUsb3.h`, 140 writes, and the register offsets match the FDT's `qcom,qmp-phy-reg-offset`.
134 writes are the same as the FDT sequence. The other six are tuning values: RX 0x157c/0x197c (7f vs ff), 0x1594/0x1994 (b4 vs b6),
and PCS LOCK_DETECT_CONFIG1/2 0x1cc4/0x1cc8 (d0/07 vs c4/89). QcomDwc3 step 4 includes the same header.

### 2.3 Layering (what is reused and what is new)

```
package/T970Usb.cpp (new, t970-usb=<n>)        board entry: rails, MAX77705 role/VBUS, OTG boost, debug-console handover
  └─ drivers/QcomDwc3 (component, SM8250 port)
       QcomDwc3.cpp       bringUp() with an SoC table (SM8550 | SM8250), the Xdci/Xhci subclasses regenerated for 21H461
       QcomFemtoPhy.h     new: femto HS PHY init, moved out of QcomXnuUsbDebug's t970_hsphy_init (device + host override seq)
       QcomQmpUsb3Sm8250.h  generated QMP table (V3 COM, V4 PCS)
       QcomUcsi.h         not used here (no UCSI); role input comes through qcom_dwc3_vbus() + a new qcom_dwc3_role()
  └─ drivers/QcomXnuUsbDebug   unchanged, off when t970-usb is on (2.5)
```

The upstream component stays SoC-generic. The SM8250 differences are data (base addresses, SID, GCC offsets, rails, PHY kind) plus
one PHY implementation. The ABI header for 21H461 is generated from `usb-abi-21H461.json` the same way upstream generates its
header from the 24A446 JSON. Without PAC, a padded class of named virtual slots is enough.

SMMU: `AppleUSBXDCI` does no cache maintenance. Stream 0x0 needs a stage 1 identity context with write-back, inner-shareable
mappings. QcomXnuUsbDebug's `soc/QcomUsbDmaDomain.c` already builds a private SMMUv2 context for this SID; upstream
`QcomDwc3.cpp` has the same logic with a different attribute set, so use upstream's. The Wi-Fi port's private apps-SMMU context
for 0x1c01 (stage 5) shows the bank allocation working on this SMMU.

### 2.4 Cable role and VBUS from the MAX77705

Everything below is read over the SE0 bus that `T970Battery.cpp` already polls once a second. The USB role logic sits next to it
and is exported as `t970_usb_cable()`.

| Question | Register |
| --- | --- |
| Attached, and our power role | `CC_STATUS0` 0x0a bits 2:0 CCStat (1 = we are a sink, the partner sources; 2 = we source, Rd seen) |
| Orientation (for the QMP `TYPEC_CTRL` port select) | `CC_STATUS0` bits 7:6 CCPinStat (1 = CC1, 2 = CC2) |
| Host present or plain charger | `BC_STATUS` 0x08 ChgTyp: SDP/CDP = host; DCP = charger, so do not start the device stack |
| PD data role | `PD_STATUS1` 0x0d bit 7 |

Role rule, as upstream applies it to UCSI:

- Partner is a host (we sink, and SDP/CDP or PD UFP): run device mode.
- Partner is a peripheral (we source): run host mode.
- Anything else: device mode, the same as upstream when nothing answers.

Host mode needs 5 V on VBUS. The charger supplies it in OTG boost (`CHG_CNFG_00` MODE 0xA, OTG_ILIM 900 mA in `CHG_CNFG_02` bits
7:6), and the USBC firmware has to be in source role (CC opcode). That code belongs in the battery/charger file and is gated
behind the host-mode argument.

D+/D− routing: the MUIC switch must stay closed (CONTROL1 0x09, `T970WifiProbe.cpp` routeUsb). Move it into `T970Usb.cpp` when
that file exists, so USB no longer depends on the Wi-Fi probe.

### 2.5 Coexistence with the CDC-ACM debug console

Both drivers program the same controller, so only one runs:

- `t970-usb=1` (short, the boot line is near its limit) makes `qcom_usb_debug_init`/`qcom_usb_debug_t970_retry` return at once.
  The check sits in `T970WifiProbe.cpp`'s retry thread and in the early `PE_init_kprintf` path. `qcom-usb-debug=1` can stay on the
  line, so switching back means removing one argument.
- While QcomDwc3 owns the controller, logs still go to the winre log (`sheng-log=1`). The `[usb]` rows (`native_usb_stats`) come
  from `BootLog.c`.
- Early boot keeps the console. QcomDwc3 starts only after IOKit matching (about 25 s, as the console retry does now), so a debug
  build could use the console until then and hand over. That is a later option; it needs a DWC3 soft reset at handover (the same
  `CONTROLLER_RESET` path upstream uses between roles).

### 2.6 Pairing with a Mac (upstream §5)

| Requirement | This board |
| --- | --- |
| `chip-id` / `board-id` / `security-domain` in `/chosen` | Must match the j120ap (T8011) row of MobileDevice's device map, the same identity whose kernelcache we run. Check the current payload ADT before testing |
| Same fsid for `/System` and the system daemons | Port the `native-union-statfs-fsid` patch to the 21H461 kernel profile, if it is not already in it |
| Key-bag lock state 3 | `sheng-aks-sls=3` with the board's `sheng-aks=1` stand-in |

### 2.7 Steps and the expected log at each one

0. **ABI mapping (host only).** Name the 7 pure XDCI slots and the T8011 overrides by disassembly. Write
   `drivers/QcomDwc3/Usb17Abi.h` from the JSON and add a host compile test of the subclass layout (vtable indices and the
   `sizeof` check). Expected: the test compiles and every slot index is asserted.
1. **Bring-up only** (`t970-usb=1`, console off). Rails, GDSC 0xf004, clocks, BCR, SMMU context SID 0, femto PHY, UTMI-as-pipe
   (no QMP yet), core ID read through the Apple window. Expected: `[usb]` stage 9 and a DWC3 core ID of 0x5533xxxx.
2. **Device stack, USB 2.** Load `AppleUSBXDCI` and `IOUSBDeviceFamily`, then `preModLoad` / static initialiser / `postModLoad`,
   then publish the nub. Expected: the Mac's `ioreg -p IOUSB` lists the device at 480 Mbps.
3. **Pairing.** Check ADT identity, fsid and lock state. Expected: Trust dialog, paired, and the lockdown session starts.
4. **USB 3.** Generated QMP table, pipe clock from the PHY (GCC 0xf05c / mux), orientation from CCPinStat. Expected: Device
   Speed 4 on a USB 3 cable, in both cable orientations.
5. **Host mode.** OTG boost on the MAX77705 charger, source role, femto host override sequence, `AppleUSBXHCI` subclass. Expected:
   xHCI commands complete and a USB stick enumerates.

The first test image each time is built and flashed by the coordinating session. Each step is one boot.

### 2.8 Risks and open questions

- **Slot semantics.** Pure slots 255–258 and 264–265 are most likely power on/off, parameters, reset and periodic usage, but that
  is unconfirmed until disassembly. A wrong mapping fails at the first call.
- **Field offsets inside the 0x2440 instance.** Upstream pokes fields directly at 0x58c and in the xHCI. Re-derive them; do not
  reuse the 24A446 numbers.
- **Prelink roots.** `com.apple.driver.AppleUSBXDCI` has no personality, so add it, `IOUSBDeviceFamily`, `AppleUSBDeviceMux`,
  `IOUSBHostFamily` and `AppleUSBXHCI` to the board's prelink roots. Exclude `AppleSMC` / `AppleOnboardSerial` as upstream does,
  and check whether the T8011 subclass kexts drag in T8011 platform drivers.
- **No eUSB2 repeater, but the femto PHY's tuning** (`param-override-seq`) differs between device and host. Both sequences are in
  the live FDT.
- **SuperSpeed lanes.** No redriver appears in the FDT. If a USB 3 link fails in one orientation, look at the `samsung,cc_dir`
  GPIO and the DP_COM `TYPEC_CTRL` select first.
- **Shared SE0 bus.** Battery, MUIC route and the role poll share one GPIO-bitbang bus under one lock. Keep the role poll inside
  the battery worker's 1 s loop instead of adding another thread.

### 2.9 Files this work touches

| File | Part |
| --- | --- |
| `package/T970Battery.cpp`, `package/BootLog.c` (battery row) | 1 (done) |
| `scripts/gen-usb17-abi.py`, `scripts/usb-abi-21H461.json` | 2.1 (done) |
| `docs/usb-plan.md` | this plan |
| `scripts/gen-qmp-sm8250-usb3.py`, `drivers/QcomXnuUsbDebug/{soc/Sm8250QmpUsb3.h, kernel/QcomXnuUsbDebug.c, usb/Dwc3.[ch], scripts/build.py, tests/test_dwc3.c}`, `package/BootLog.c` (`[qud ss]` row) | 3 (READY, not booted) |
| `drivers/QcomDwc3/` (new component checkout, SM8250 port), `package/T970Usb.cpp`, `board.toml` prelink roots and recipe | 2.3–2.7 (next) |

## 3. USB 3 now: SuperSpeed for the CDC-ACM debug console (`t970-ss`, READY, not booted)

Upstream (UniXNU main and FFT-infra QcomDwc3 e4efe95) runs USB 3 only inside QcomDwc3 for SM8550: `QcomQmpUsb3.h`, generated from
Linux; `qmpInit` (DP_COM software port select and the table); DCFG left at SuperSpeed; descriptors from Apple's stack. Two parts
carry over unchanged in method: the QMP COM/PCS sequence and the generator. The DCFG and descriptor parts do not, because Apple's
stack builds its own descriptors. The faster debug link the user wants is the console (QcomXnuUsbDebug), which already runs on this
board and does not wait for QcomDwc3 steps 0–3. So USB 3 goes there first, on the console's existing hand-off path.

**Bring-up** (`qcom_usb_debug_t970_retry` → `t970_hsphy_init` → `t970_ssphy_init`, only when `t970-ss` bit 0 is set):

1. Supplies through `t970_rpmh_write` (weak): ldoa9 1.2 V (QMP core, FDT core-supply `pm8150_l9`) and ldoa18 0.912 V (vdd,
   `pm8150_l18`). Each gets voltage, HPM and enable. If any vote fails, no SuperSpeed attempt is made.
2. GCC: assert and release USB3_DP_PHY_PRIM_BCR 0x50008 and USB3_PHY_PRIM_BCR 0x50000. Set the aux RCG 0xf064/0xf068 to XO
   19.2 MHz. Enable the aux 0xf054, com_aux 0xf058 and pipe 0xf05c branches.
3. Lane select from the MAX77705, not a GPIO. The source is `CC_STATUS0` CCPinStat, which `routeUsb` stores in `t970_muic[6]`
   before the retry runs: 1 = CC1, normal; 2 = CC2, reverse. `t970-ss` bit 1 inverts the result, in case this board wires CC1/CC2
   opposite to the lanes. TLMM 65 (`samsung,cc_dir`) and every PM8150B Type-C register are left alone. This board's CC controller
   is the MAX77705, so PM8150B's typec block is not the cable source here.
4. QMP, as Linux `qmp_combo_com_init` and `qmp_combo_usb_power_on` (USB3-only mode):
   - COM: power, reset override, `TYPEC_CTRL = SW_PORTSELECT_MUX | rev`, `PHY_MODE_CTRL = USB3_MODE`, then out of reset.
   - The 140 table writes, then the PCS `SW_RESET` released and SERDES|PCS start.
   - Wait for `PCS_STATUS1.PHYSTATUS` to clear, up to 200 ms.
5. On success, QSCRATCH GENERAL_CFG does not select UTMI as the PIPE clock (bits 0 and 3 are cleared). On any failure the console
   keeps exactly today's high-speed path.
6. The gadget gets `platform.superspeed = 1`:
   - DCFG SuperSpeed; GUSB3PIPECTL.SUSPHY cleared (the PIPE PHY stays awake, as the USB2 PHY already does).
   - On a SuperSpeed CONNECTDONE: EP0 512, bulk 1024, bcdUSB 3.2 with bMaxPacketSize0 9, a BOS (USB 2.0 extension + SS
     capability), and SS endpoint companions (config 85 bytes).
   - SET_SEL (a 6-byte OUT stage in a 512-byte TRB), SET_ISOCH_DELAY, U1/U2/LTM enables (accepted, left off) and
     FUNCTION_SUSPEND.
   - Device qualifier and other-speed requests stall at SuperSpeed.
   - A USB 2 cable still trains at high speed and gets the unchanged USB 2 descriptors.

**Boot args:** add `t970-ss=1` to the line. Use `t970-ss=3` if `[qud ss]` reaches step 104 but `[qud retry]` dsts stays at speed 0 on
a USB 3 cable, which would mean the lane select is inverted. `qcom-usb-debug=1` and `t970-qud-retry=1` stay as they are.

**Log:**
- `[qud ss] step %x arg cc0 rev pcs polls aux com pipe rails`:
  - step 104 = PHY running.
  - -2 = no MUIC data; -3 = no CC orientation (CCPinStat 0/3); -4 = an aux branch is gated; -5 = PHYSTATUS did not clear (pcs
    shows bit 6).
- `[qud retry] dsts`: low 3 bits 4 = SuperSpeed, 0 = high speed.
- On the Mac: `system_profiler SPUSBDataType` shows the CDC device at "Up to 5 Gb/s".

**Checked here:**
- All five module units pass `-fsyntax-only` with the build's own recipe (pe_kprintf compile record, `scripts/build.py` flags).
  The 3 warnings are pre-existing.
- `BootLog.c` and `T970Battery.cpp` pass the kernel flags.
- `tests/test_dwc3.c` passes every existing high-speed case plus a new SuperSpeed case: DCFG, SUSPHY, EP0 512 / bulk 1024,
  bcdUSB 3.2, BOS, three companions, SET_SEL, U1/U2, SET_CONFIGURATION, line coding unchanged.
- The module's import allowlist (`scripts/build.py`) gains the two weak board symbols `_t970_muic` and `_t970_rpmh_write`.

**Not done / limits:**
- Not booted.
- The console's transfer size is still 4 KiB per TRB with a 16 KiB TX ring, so the polled loop, not the link, will cap
  throughput. Larger SS buffers are a follow-up once the link is seen.
- Host mode is not in the console (it has no xHCI). It belongs to QcomDwc3 step 5, which reuses this PHY bring-up with the
  femto host override sequence and the MAX77705 OTG boost.

