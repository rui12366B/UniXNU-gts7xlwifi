# SM8250 / xnu-10063.141.1 port of QcomXnuUsbDebug

Status: **bounded port complete as source.** The SM8250 profile, the rebased
console-tee patch and the worker patch exist here. Host checks pass 24/24 and
all six translation units cross-compile with the real build flags. Nothing is
in a repository, nothing is flashed, no boot argument is changed.

Evidence levels kept separate:

| level | state |
| --- | --- |
| host model checks | pass, 24/24 (`tests/check_port.py`) |
| patch applies to the real patched tree | pass, `git apply --check` rc=0 |
| six units compile with the tree's own flags | pass, hook on and off |
| full kernel link | **not done** |
| enumeration on hardware | **not done** |

## 1. What is in this directory

```
soc/Sm8250.h                            new SoC profile (SM8550 profile kept as is)
patches/darwin23/0001-...-console.patch  console tee, rebased onto xnu-10063.141.1
patches/darwin23/0002-...-worker.patch   worker thread, rebased onto xnu-10063.141.1
fixture/stock/                           the six unpatched files, for offline apply tests
fixture/ported/                          the six files with the patches applied
tests/check_port.py                      host checks (see section 6)
scripts/compile_units.py                 cross-compile with the real records (run on orb)
```

`QcomXnuUsbDebug` itself is unchanged. Nothing in `upstream/` was edited, so a
later rebase of that repository does not conflict with this port.

## 2. Why this is a port and not a rewrite

The module separates its own layers already. What changes for SM8250:

| thing | SM8550 | SM8250 | where |
| --- | --- | --- | --- |
| GCC base / size | `0x00100000` / `0x40000` | `0x00100000` / `0x1f0000` | profile |
| DWC3 core window | `0x0a600000` + 1 MiB | `0x0a600000` + 1 MiB | same |
| apps SMMU | `0x15000000` + 1 MiB | `0x15000000` + 1 MiB | same |
| USB stream ID | `0x40` | `0x0` | profile |
| five CBCRs | Kalama `0x390xx` | prim `0xf010..0xf058` | profile |
| USB3 block reset | `0x39000` | `0xf000` | profile |
| GDSC control | `0x39004` | `0xf004` | profile |
| QSCRATCH HS PHY CTRL | `0xf8810` (rel. to its wrapper) | `0xf8810` (rel. to its wrapper) | profile |
| DWC3 core logic | — | unchanged | `usb/Dwc3.c` |
| SMMUv2 domain | — | unchanged | `soc/QcomUsbDmaDomain.c` |
| kernel mapping / cache / threads | — | unchanged | `kernel/QcomXnuUsbDebug.c` |

The profile is a data file that the module already parameterises through
`QUD_SOC_HEADER`. The shared DWC3 core and the private SMMUv2 domain need no
change because SM8250 uses the same QSMMUv500 programming model and the same
DWC3 device-side register layout. The kernel adapter's XNU API surface was
checked one call at a time against xnu-10063.  All of them still exist:

| adapter call | xnu-10063 location |
| --- | --- |
| `ml_io_map` | `osfmk/arm/machine_routines.h:728` |
| `ml_static_vtop` | `osfmk/arm/machine_routines.h:570` |
| `PE_parse_boot_argn` | `pexpert/pexpert/pexpert.h` |
| `PE_halt_restart` | `iokit/IOKit/IOPlatformExpert.h:90` |
| `master_cpu` | `osfmk/kern/cpu_number.h:62` |
| `panic_active` | `osfmk/kern/debug.h:630` |
| `dwc3_*` context bytes | module-local |

`PE_halt_restart` is not in `pexpert/`; it lives in IOKit.  The adapter
already declares it `extern` itself, so this does not matter.

## 3. The two real differences from SM8550, and what was done about them

### 3.1 The GDSC check

The module's `QcomUsbInspectInherited()` requires `CFG_GDSCR` bit 16
(`POWER_UP_COMPLETE`) before it will touch the controller.  On SM8250,

- `usb30_prim_gdsc` has no flags (`gcc-sm8250.c:3264-3270`),
- `gcc-sm8250.c` never sets `POLL_CFG_GDSCR` (`grep -c` = 0),
- so Linux tests `PWR_ON` bit 31 of the **GDSCR** itself
  (`gdsc.c:87-92`), not the CFG_GDSCR bit.

The measured value of `CFG_GDSCR` bit 16 on this board is **unknown**: no
source in this workspace contains a hardware read of it.  Two options were
available: change the check (touches shared code and its tests) or keep the
check and make its failure visible.  The port keeps the check, because a
wrong answer there is a refused boot rather than a bus hang, and because
changing shared code would make the profile stop being pure data.

**Consequence, stated plainly:** if firmware leaves `CFG_GDSCR` bit 16 clear,
`qcom_usb_debug_init()` returns 0 and no USB log appears.  The status word
then reads `phase=REFUSED error=INHERITED_STATE_REFUSED qcom_result=15`
(`QusbPowerDomainOff`), which is distinguishable in one flash from a clock
failure (`qcom_result=3`), a role failure (`CORE_NOT_DEVICE`), or a missing
mapping (`MAP_FAILED`).  If that is what the first flash shows, the one-line
fix is a profile-selected check, not a rewrite.

### 3.2 `boot-progress` owns `PE_kputc`

`zzz-boot-progress.patch` replaces `PE_kputc` with `boot_progress_putc` and
**returns before** the module could have installed its sink
(`pexpert/arm/pe_kprintf.c:62`).  The Darwin22 patch would therefore have
done nothing on this board.

The port calls `qcom_usb_debug_init()` **before** that branch and leaves the
boot-progress line byte-identical.  Both outputs then coexist: boot-progress
keeps drawing its text page, and USB gets the same bytes from the tee in
`_cnputs`.

The duplicate-suppression rules are the part that needed real work.  The
Darwin22 patch keyed on `PE_kputc == console_write_unbuffered`, but on this
board `PE_kputc` can be one of three functions.  The port therefore uses:

- **`_cnputs` tee** — runs before `disableConsoleOutput` returns and before
  `_cnputc` redirects to boot-progress.  One hook covers `printf`, `IOLog`,
  panic output, the unbuffered fallback, boot-progress mode and muted-console
  mode.
- **PE hook** — `kprintf` goes through `_doprnt_log(..., PE_kputc, ...)`,
  which bypasses `_cnputs`.  It tees unless `PE_kputc` is
  `console_write_unbuffered` (the byte is already teed in `_cnputs`).
- **`consdebug_putc*`** — keeps every original line; only adds a guard that
  suppresses the fan-out when `PE_kputc` is the USB sink or the console
  fallback, both of which re-enter `_cnputs`.

All of this is tested: `tests/check_port.py` builds a model from the patched
source text and counts console bytes.  11 scenarios, each asserting the exact
USB and video byte counts.

## 4. The SM8250 hardware facts

Source: the pinned Linux tree this workspace already uses for SM8250 facts,
`alioth/tmp/kbuild/source` → `/Volumes/Disk/xinyang/Lab/Bridge/tmp/gunyah-probe`,
cross-checked against the Android DT that the board's UEFI ships.

| item | value | citation |
| --- | --- | --- |
| DWC3 core node | `usb_1_dwc3: usb@a600000` | `sm8250.dtsi:4220` |
| DWC3 core `reg` | `<0 0x0a600000 0 0xcd00>` | `sm8250.dtsi:4222` |
| DWC3 compatible | `snps,dwc3` | `sm8250.dtsi:4221` |
| wrapper node | `usb_1: usb@a6f8800` | `sm8250.dtsi:4174` |
| wrapper `reg` | `<0 0x0a6f8800 0 0x400>` | `sm8250.dtsi:4176` |
| wrapper compatible | `qcom,sm8250-dwc3`, `qcom,dwc3` | `sm8250.dtsi:4175` |
| core interrupt | `GIC_SPI 133` | `sm8250.dtsi:4223` |
| USB stream | `iommus = <&apps_smmu 0x0 0x0>` | `sm8250.dtsi:4224` |
| GCC | `clock-controller@100000`, `0x00100000` + `0x1f0000` | `sm8250.dtsi:953-955` |
| apps SMMU | `iommu@15000000`, `0x15000000` + `0x100000` | `sm8250.dtsi:5878-5880` |
| downstream cross-check (core) | `ssusb@a600000`, `reg <0xa600000 0x100000>`, `iommus <0x47 0x00 0x00>` | `uefi-alioth/Resources/DTBs/alioth.dts:18508-18512` |
| downstream cross-check (SMMU) | `apps-smmu@15000000`, `qcom,qsmmu-v500` | `alioth.dts:11910-11912` |

Clocks, from `drivers/clk/qcom/gcc-sm8250.c` (`halt_reg` = `enable_reg`):

| clock | id | CBCR | check | line |
| --- | --- | --- | --- | --- |
| `GCC_USB30_PRIM_MASTER_CLK` | 173 | `0xf010` | VOTED | `:2976` |
| `GCC_USB30_PRIM_SLEEP_CLK` | 178 | `0xf018` | HALT | `:3012` |
| `GCC_USB30_PRIM_MOCK_UTMI_CLK` | 175 | `0xf01c` | HALT | `:2994` |
| `GCC_USB3_PRIM_PHY_AUX_CLK` | 185 | `0xf054` | HALT | `:3074` |
| `GCC_USB3_PRIM_PHY_COM_AUX_CLK` | 187 | `0xf058` | HALT | `:3092` |
| `GCC_USB3_PRIM_PHY_PIPE_CLK` | 188 | `0xf05c` | HALT_DELAY | `:3110` |

`GCC_USB30_PRIM_BCR` = `0xf000` (`:3570`).  `usb30_prim_gdsc.gdscr` =
`0xf004` (`:3264-3270`).  `CFG_GDSCR` = GDSCR + 4 (`gdsc.c:35`), bit 16
(`gdsc.c:32`).  `PWR_ON` = bit 31, `HW_CONTROL` = bit 1, `SW_COLLAPSE` = bit 0
(`gdsc.c:21,26,27`).

QSCRATCH inside the wrapper: base `0x0a6f8800` + `QSCRATCH_HS_PHY_CTRL` `0x10`
(`dwc3-qcom.c:26`) = `0x0a6f8810`, i.e. offset `0xf8810` from the mapped
window.  `UTMI_OTG_VBUS_VALID` = bit 20 and `SW_SESSVLD_SEL` = bit 28
(`dwc3-qcom.c:27-28`), set together by `dwc3_qcom_vbus_override_enable()`
(`:117-129`).

The UniXNU device trees carry **no USB node at all**:
`NEW/UniXNU-sm8250-common/dts/sm8250.dtsi` has zero `usb` matches and
`NEW/UniXNU-alioth/dts/alioth.dts` has zero.  There is no conflict to port
around, but also nothing there to cite.

## 5. Firmware already brings USB up

`NEW/uefi-alioth/Platforms/Xiaomi/aliothPkg/Include/DXE.inc` lists
`UsbfnDwc3Dxe` (line 90), `UsbDeviceDxe` (99), `UsbConfigDxe` (100),
`UsbInitDxe` (101) and `UsbPwrCtrlDxe` (85); the same list appears in
`APRIORI.inc` and the modules are present in the built FV
(`Build/aliothPkg/DEBUG_CLANGPDB/FV/FVMAIN.inf`).  `Status.md:122` records
USB Device Mode as working.  The downstream DT the firmware ships enables
the primary controller and the HS PHY and disables the SS PHY and the
secondary controller (`alioth.dts:18508` no `status`; `18551`
`maximum-speed = "high-speed"`; `18618` SS PHY disabled; `18663` second
controller disabled).

So the module's precondition holds: firmware has done power, clocks and PHY,
and the controller is in DEVICE mode before XNU starts.

The board kernel has **no USB driver of its own**:
`NEW/UniXNU-alioth/configs/alioth_defconfig` has zero matches for
`usb|dwc3|IOUSB|AppleUSB`, and the XNU tree has none either.  Nothing
competes with the module for the controller.

## 6. Test output

`python3 -I port/tests/check_port.py --module upstream --output <new dir>`:

```
PASS  A1 patch series numbering  0001-qcom-usb-debug-console.patch 0002-qcom-usb-debug-worker.patch
PASS  A2 git apply --check
PASS  A3 git apply
PASS  A4 applied tree equals ported fixture
PASS  A5 git apply --reverse
PASS  A6 reverse restores stock bytes
PASS  A7 apply is idempotent-safe
PASS  B1 every hunk names a function that exists in xnu-10063
PASS  B2 all declared tee anchors are present
PASS  B3 PE tee gives USB the byte for every non-console owner
PASS  B4 tee precedes the disableConsoleOutput return
PASS  B5 tee lives in _cnputs; the boot-progress redirect is untouched
PASS  C1 profile compiles under the module assertions
PASS  C2 five CBCRs are the SM8250 prim branches
PASS  C3 stream ID is SM8250 SMMU SID 0x0
PASS  C4 QSCRATCH offset carries wrapper arithmetic
PASS  C5 no SM8550 Kalama clock offset survived
PASS  D1 console ownership model compiles
PASS fallback console, console enabled: 1 byte            usb=1 video=1
PASS fallback console, console muted: 1 byte              usb=1 video=0
PASS boot-progress active: 1 byte, no duplicate           usb=1 video=0
PASS serial PE_kputc: 1 byte                              usb=1 video=0
PASS printf path, console enabled                         usb=1 video=1
PASS printf path, console muted: 1 byte                   usb=1 video=0
PASS consdebug_putc, console enabled                      usb=1 video=1
PASS consdebug_putc_unbuffered                            usb=1 video=1
PASS consdebug_putc_unbuffered, console muted             usb=1 video=0
PASS consdebug_putc with boot-progress                    usb=1 video=0
PASS 1024-byte console write                              usb=1024 video=1024
PASS console ownership model
PASS  D2 every console byte reaches USB exactly once
PASS  D3 no console byte is lost while the console is muted
PASS  E1 write prefix is scheduler-free
PASS  E2 early producer masks IRQ/FIQ around the ring
PASS  E3 early producer never spins
PASS  E4 init runs inside PE_init_kprintf, before the boot-progress return

Passed 24/24 checks
```

Cross-compilation against the real records (run on orb):

```
$ python3 -I port/scripts/compile_units.py port/fixture/ported upstream/include port-compile
PASS  pexpert/arm/pe_kprintf.c
PASS  osfmk/kern/debug.c
PASS  osfmk/kern/printf.c
PASS  osfmk/console/serial_console.c
PASS  osfmk/console/serial_general.c
PASS  bsd/kern/bsd_init.c

All units compile with the hook on and off
```

`git apply --check` against the live patched build tree
(`tree/.cache/xnu-work`, HEAD `d8b8029`) exits 0 for both patches.

The model does **not** prove: linker resolution, that the DWC3 core will
enumerate, or that the SMMU scan finds a free bank on SM8250.  Those are
hardware or full-link facts.

## 7. Integration steps (not performed)

1. Copy `soc/Sm8250.h` into the module at `soc/Sm8250.h`.  Keep `Sm8550.h`.
2. Copy `patches/darwin23/` into the module as `patches/darwin23/` and add a
   `darwin23` entry to `scripts/xnu_patches.py`:
   `xnu_version '10063.141.1'`, `xnu_revision 'd8b80295118ef25ac3a784134bcf95cd8e88109f'`,
   the same six `translation_units`.
3. Apply both patches to the build tree **after** `zzz-boot-progress.patch`.
   They touch different lines of `PE_init_kprintf` and the port's hunks carry
   the boot-progress line as context, so order matters if that patch changes.
4. Build the module archive with `scripts/build.py --soc soc/Sm8250.h`.
5. Add `-DXNU_QCOM_USB_DEBUG=1 -I<module>/include` for the six units, and
   link `libQcomXnuUsbDebug.a` into the kernel archive list.
6. Leave `boot-progress=2` in place; the two channels coexist.
7. Add `qcom-usb-debug=1` last, on its own, so one flash answers one question.

## 8. What must be true on hardware, and how one flash answers

Preconditions for the log to appear:

1. The five CBCRs read CLK_OFF clear (`0xf010`, `0xf018`, `0xf01c`, `0xf054`,
   `0xf058`).  Firmware started USB, so this should hold; if not the status
   reads `qcom_result=3`.
2. `CFG_GDSCR` bit 16 is set.  **Unknown.**  If clear, `qcom_result=15`.
3. The apps SMMU has a free non-secure context bank and an ASID in
   `0xff00..0xfd01`, and no existing stream maps SID `0x0`.  If not,
   `qcom_result` reports `NO_SLOT`/`NO_BANK`/`NO_ASID` or `STREAM_EXISTS`.
4. `GHWPARAMS0` reports an address width ≥ 32 so the 32 KiB arena at
   `0x90000000` is reachable.
5. The DWC3 core is in device mode and `DSTS.DEVCTRLHLT` becomes set after
   the module clears `DCTL.RUN_STOP`.

How to tell in one flash, without reading the screen:

- The USB device appears on the host with VID:PID `1209:316d` and serial
  `QcomXnuUsbDebug`.  That is proof of enumeration only; it says nothing about
  whether the log is a complete copy of the console.
- The banner arrives, followed by live `kprintf` lines.
- If the device does not appear, the refusal is still readable, because the
  worker prints it through `kprintf` and boot-progress draws `kprintf` on the
  same page: `QCOM USB WORKER unavailable phase=%u error=%u`.  That line
  names both the phase and the error enum.  When initialization was refused
  before the worker started, the screen shows the same line because the
  worker still runs and reports the status it finds.
- The most informative single line is `QCOM USB WORKER active`.  If it is
  absent the screen shows the refusal line instead.

Because the screen and USB carry the same text, one photograph still
distinguishes "did not initialize" (`unavailable phase=REFUSED error=…`) from
"initialized but the host did not enumerate" (`active`, and the screen keeps
scrolling).

## 9. What is not done, and is not claimed

- No kernel link.  The archive must resolve against the six units and the
  kernel's own XNU symbols; the module's `build.py` import audit already
  restricts XNU imports to a known set, but that audit has not been run for
  this tree.
- No hardware result.  `hardware_verified` is false in every report.
- No reboot-command qualification.  The worker patch keeps the reboot thread
  and requires `PE_halt_restart` to be `NULL`; `QUD_PSCI_SMC_RESET` is 1 in
  the profile because this board already uses the same SMC reset in its
  diagnostic path, but the control channel is off unless `qcom-usb-ctl=1`.
- No screenshot provider.  The board has no display driver that registers
  one, so `screenshot` answers `unavailable`.
- The module's `QcomUsbResumeClocks`/`QcomUsbRestoreClocks` are declared in
  `soc/QcomUsbQualify.h` but **not defined anywhere** in the module.  Nothing
  calls them, so the archive links; but do not call them for SM8250 without
  implementing them.  This is an upstream defect, not a port issue.

## 10. Comparison with ios-vermeer-lab

`vermeer` reaches a running SpringBoard and has a hardware-confirmed CDC ACM
console.  It is the closer reference for a *working* channel on a Qualcomm
board, and its choices are different in ways that matter here:

| | QcomXnuUsbDebug | ios-vermeer-lab |
| --- | --- | --- |
| kernel side | 1.4k-line adapter + 647-line shared core + SoC qualify + SMMU domain | single 456-line `vermeer_usb_debug.c`, no abstraction |
| DMA | own three-page S1 translation domain at `0x90000000` | identity route inherited from firmware, verified read-only |
| console source | tee at `_cnputs` plus a PE hook | `vermeer_debug_append` from `_cnputs` and `serial_putc_crlf` |
| console loss model | 4 MiB backlog, counts, panic drain | 1 MiB ring, drop counter, no panic drain |
| controls | 512-byte framed protocol with CRC and sessions | newline text commands (`ping`, `status`, `replay`) |
| reuse of firmware USB state | requires 5 clocks, GDSC, SMMU match, `DEVCTRLHLT` | same preconditions, plus a hardcoded SMMU ID check |

Portability to SM8250: the module is easier, because its SoC-specific surface
is one profile file and its stream-match and bank scans are ID-driven.  The
vermeer driver hard-codes SM8550 SMMU ID registers and a `0x8c0000000` DMA
window, so it would need more edits, not fewer.

Console tee point: neither is "closer to xnu-10063".  Both were written for
Darwin22-shaped code.  The vermeer `_cnputc` hook is one line and would port
in minutes, because Darwin23 moved the `disableConsoleOutput` check out of
`consdebug_*`; that is exactly the drift this port had to absorb.  But the
vermeer hook alone does not capture `kprintf`, which bypasses `_cnputs`
(`pe_kprintf.c:126` passes `PE_kputc` straight to `_doprnt_log`).  The
module's two-point tee does, and its 11-scenario model proves it.

About the abort loop: vermeer's README documents the same class of failure
and its fixes, all of which are first-boot or Data-volume problems, not USB
problems.  Relevant entries:

- `README.md:41-58` — `SystemAppMigrator` waits for installation while
  `appstored` waits in `DataMigration`; the repair is
  `vermeer_migration_baseline=1`, which writes only
  `com.apple.migration.plist` on a disposable root.
- `README.md:60-67` — SpringBoard waits for the system-shell check-in,
  backboardd waits for `DataMigration`.
- `README.md:153-159` — a `systembag.kb` marker was needed before
  AppleKeyStore and keybagd stopped failing.
- `alioth` already passes `sheng-disable-watchdogd=1` and
  `panic_on_crash_disabled=1` (`board.toml:348`, `:232`), so the dmd /
  ScreenTimeAgent / privacyaccountingd aborts are no longer panicking the
  kernel — they are just repeating.  The vermeer evidence points at Data
  volume role and migration metadata as the next thing to check, and both are
  cheaper than a USB channel.  This port does not change either; it only
  makes the next investigation readable without a camera.
