# S Pen (Wacom EMR digitizer) on gts7xlwifi

Status 2026-11-09: combo23 (t970-pen=23) ran: power, query (fw 0x403d, MPU 0x45, max 26712 x 16714, pressure 4095, tilt 63/63) and
streaming work; tip/pressure/side decode correctly from the raw packets; garage PDCT edges are seen. 673 HID reports were accepted by
handleReport but the UI ignored them: the stylus application collection was Pen (0x02), so the service's PrimaryUsage was 2, which
backboardd does not take (1, 4, 0x20), and the event system reads it once when the service appears (the later stamp did nothing).
READY 2: the collection is Digitizer (0x01) and the stylus is dispatched as a collection child, as an iPad digitizer sends the Pencil.
combo24b (READY 2, t970-pen=23): 2733 reports accepted, still nothing in the UI. The diag export's tracev3 strings show why: in both pen
boots backboardd's "primaryPage 0xD, display builtin" service set (the GraphicsOrientation update) holds only the touch device's
IOHIDEventDriver (transport Virtual); the pen service (transport I2C, product "Stylus digitizer") appears in no backboardd or event-system
line at all. The separate service is the problem, not its usages. READY 3: the pen rides in the touch device's own report 3
(see HID).
Note: GPIO 93 (the 3.3 V enable) was off under UEFI; the driver turns it on.

## Hardware (live FDT, `evidence/recovery-live.dts`)

| What | Where |
| --- | --- |
| Controller | `wacom@56`, compatible `wacom,w90xx` (W9021 family; query MPU 0x45 expected), boot loader address 0x09 |
| Bus | QUP SE17 `i2c@88c000` (QUP2), pins GPIO 52 SDA / 53 SCL, function qup17, TLMM SOUTH tile (0x0f500000) |
| IRQ | GPIO 136, TLMM WEST (0x0f100000), level low while a packet is pending (Samsung requests `IRQF_TRIGGER_LOW`) |
| Pen detect (PDCT) | GPIO 7, NORTH: high = pen in its garage, low = out |
| FWE | GPIO 6, NORTH: firmware-update enable, driven low for normal mode |
| Power | `wacom_avdd` 3.3 V fixed regulator `vdd_spen_3p3`, enable GPIO 93 (NORTH, active high, `regulator-boot-on`) |
| Hall (garage) | `hall_wacom`, GPIO 110, NORTH (`hall,gpio_wacom_cover`) |
| Ranges (DT) | `max_coords` 0x5880 x 0x3750 (22656 x 14160, used only with `use_dt_coord`, not set: the query wins), pressure 0xfff, tilt 63/63, height 255 |
| Orientation (DT) | `wacom,invert = <0 1 1>`: y inverted, x/y switched, to Android's portrait natural space |

TLMM tile per pin from pinctrl-kona.c. None of 6, 7, 52, 53, 93, 110, 136 is on the TrustZone-secure list (20-23, 28-31, 40-43);
the driver panics rather than touch a secure pin.

## Protocol (Samsung `drivers/input/wacom/wacom_i2c.c`, github.com/ianmacd/gts7xl, read only)

- All transfers are plain I2C reads/writes at 0x56, no register address.
- Query: read 31 bytes; the query block starts at byte 16: [0] header 0x0f, [1:2] max x, [3:4] max y, [5:6] max pressure,
  [7:8] firmware version, [9] MPU, [10] boot loader version, [11]/[12] max tilt x/y, [13] max height.
- Event: on IRQ low read 17 bytes; type = byte0 & 15: 1 coordinate, 3 AOP, 13 notification, 14 reply.
- Coordinate: byte0 bit7 ready (in range), bit6 rubber end, bit5 side button, bit4 tip; x = b1:b2, y = b3:b4,
  pressure = (b5 & 15):b6, height b7, tilt x/y signed b8/b9. Ready clear = out of range.
- Commands (1 byte): 0x2d survey exit, 0x31 sample-rate start, 0x30 stop, 0x3b garage-only survey.
- Probe order: FWE low; power on (if FWE was high: power cycle 100 ms off, 200 ms settle); query. Nothing else is needed to stream.

## Driver (`t970-pen=<bits>`, off by default)

| Bit | Meaning |
| --- | --- |
| 1 | Stage 1: map, power, FWE low, query (fw version, ranges) |
| 2 | Stage 2: stream; packets polled from the IRQ line (1 ms while in range, 8 ms idle) |
| 4 | Stage 3: HID (needs `offline-hid-input=1`): a Stylus collection inside the touch device's report 3 |
| 8 | HID fallback: the tip as one more finger (contact 10) in the touch device's report 3 |
| 16 | Stage 4: text lines for pen in/out (PDCT) and hall changes (counters are always kept) |
| 32 | Power-cycle first |
| 64 | Skip survey exit + sample-rate start before streaming |
| 128 | Log every coordinate packet to the raw ring |
| 256 | Separate device (2048) only: application collection Pen (0x02) instead of Digitizer (0x01) (combo23 layout) |
| 512 | Separate device only: stamp the event driver PrimaryUsage 4 |
| 1024 | Separate device only: no DigitizerCollectionDispatch |
| 2048 | The old separate HID pen device (combo23/24b; backboardd never listed its service) |
| 4096 | Separate device only: application collection Stylus (0x20), backboardd's pencil digitizer pair 0x0D/0x20 |

`t970-pen-xform=<bits>` (1 swap raw axes, 2 mirror x, 4 mirror y; default 4) and `t970-pen-i2c=<half period us>` (default 4).

The default mapping: the raw x axis is the long one (22656 vs 14160). Android's y-invert + switch gives portrait (X short); the touch
core maps that portrait space to the 2800 x 1752 frame buffer by swapping back, so in raw terms only y stays mirrored. Confirm with the
`xy` field of the boot row against a known corner.

## HID

Default (bit 4): no HID device of its own. The touch device (NativeHidInput with sheng-touch-hid=1, the service backboardd takes touches
from) carries the pen in its report 3:
- Stylus: a Stylus physical collection inside its Touch Screen application collection, after Contact Count (report 3 grows from 62 to
  72 bytes): Tip Switch, Barrel Switch, Invert, Eraser, In Range, Contact Identifier 10, X/Y 0..32767, Tip Pressure 0..4095, X/Y Tilt
  -63..63. IOHIDEventDriver makes it a Stylus transducer and sends it as a child of the same collection event as the fingers. The
  edge-tip wrapper skips stylus children.
- Fallback (bit 8): the tip is one more finger (contact 10) in the ordinary ten-finger report; the descriptor does not change.
- Each pen packet sends report 3 with the fingers last sent as down plus the pen; touch reports carry the pen's last state too. While the
  pen stays away (out of range; for the fallback, tip up) nothing is sent; one report when it leaves.
- Same report, not a report of its own: in collection dispatch IOHIDEventDriver logs "generates null event" for every transducer
  without an element in the report (ten per pen packet at ~225 packets/s). A stylus out of range and not touching makes no event (the
  driver drops repeated untouch), so touch reports carry it for free.
- IOHIDEventDriver of this IOHIDFamily ignores X/Y Tilt (handled only); pressure goes into the event's tip pressure.

Separate device (bits 4 + 2048, combo23/24b): report 7 Digitizer application with one Stylus collection (or with bit 8 a one-contact
touch screen, report 8), Built-In, DisplayIntegrated, DigitizerCollectionDispatch, stamped by its own stampPen. handleReport accepted
every report, but backboardd never listed the service.

### What backboardd does (donor 21H461 backboardd, disassembled read-only)

- `-[BKHIDDirectTouchEventProcessor digitizerMatchingDictionaries]`: three matchers, DeviceUsagePage 0x0D with DeviceUsage 4, 1, 0x20.
- `-[BKHIDDirectTouchEventProcessor matcher:servicesDidMatch:]` logs "digitizer attached (sender ..)" for each matched service, then
  files it by `PrimaryUsagePage<<16|PrimaryUsage`: 0xD0004 main display digitizer, 0xD0001 "AOP digitizer", 0xD0020 pencil digitizer
  (`mainDisplayPencilDigitizerService`), anything else "unexpected digitizer usage pair". The iPad's Pencil arrives from that separate
  pencil service, not inside the touch screen's events.
- backboardd's own log lines are readable with tv3.py plus the donor backboardd binary (main executable format strings).
- combo24b: no "digitizer attached" for the separate pen service at all (only the touch service, 0xd/0x4), although 0x0D/1 matches: the
  user-space HID event system apparently never offered that service. `hid` field 2 bits 8..15 now count the user clients on the separate
  device's event driver (0 = nobody opened it).
- combo25 (stylus inside report 3) and combo25p (pen as finger, t970-pen=31): nothing in the UI, touch fine. Both used contact
  identifier 0xfe; backboardd's addContact drops path index >= 31 (`cmp x23, #0x1f`), so the pen contact was thrown away. READY 5: 10.

## Boot log rows

`[t970 pen] st .. err .. pins .. fw .. mpu .. max x/y/p tilt .. pk co oth types i2c stuck xy p tilt fl pdct hall hov tip side rate
sink q ldo bits t hid status/started/sent/refused/dropped/result`, then `[t970 pen log]` (query bytes, power actions, first packets of
other types, garage changes) and `[t970 pen raw]` (the last four raw packets).

- st 4 and a `fw` = query ok. st -4: no valid query; `err` = NACKs | bus errors << 8 | stretch timeouts << 16; `q` bit 8 = the boot
  loader answered at 0x09 (firmware in flash mode or broken).
- st -3: SDA/SCL not high after setup (bus held or pins wrong).
- `hid` = separate-device status (0 when the pen rides in the touch report) / mode (1 separate started, 2 its driver stamped, 3 stylus in
  report 3, 4 finger in report 3) / sent / refused / dropped / last result.
- st 5 = streaming. `co` grows only with the pen near the screen; `rate` (hex) = packets in the last second | best second << 16.

## Open items

- Android cross-check (KernelSU): `getevent -lp` for `sec_e-pen` (axis ranges), `/sys/bus/i2c/devices/*-0056/name`,
  `dmesg | grep -i wacom` (query line and firmware version).
- iPadOS behaviour of a third-party stylus transducer from the touch service: whether UIKit reports `UITouchTypePencil`, and whether
  backboardd routes hover. If the stylus is dropped there too (ApplePencilSessionFilter is loaded), bit 8 gives plain touch.
- Why the separate service never reached backboardd is not known (the diag has no shared-cache format strings).
- Pen insert/eject are only logged; no iPadOS consumer for them yet.
