# Migration plan: S7plus-iPadOS → UniXNU-gts7xlwifi

The existing project at `../` stays untouched as the fallback; the device
currently boots its candidates (kernel b287). This repository is the new format 2 port. Nothing
here is wired to a build or the device until a component is moved, built and
recorded in `porting.toml`.

## Layers

| Layer | Source | Pin |
| --- | --- | --- |
| ARM64 + OS profile | UniXNU core `profiles/arch/arm64`, `profiles/os/ipados-17.7.11-21H461` | `../sources/UniXNU` 066c8d8 |
| SoC (SM8250, same as umi) | `FFT-infra/UniXNU-sm8250-common` (CPU topology, GICv3, timer, UFS/GPU/QUP ranges, 2 SMP patches, A650 GMU kit) | 166235e (`feeds.toml`) |
| SM8250 drivers proven on hardware | `reAAAq/UniXNU-umi` (Mi 10): UFS host, platform power, SMMU takeover, ST FTS over GPI with RPMh/SPMI power sequence, persistent Data store | 3eb7257 |
| Samsung tablet + 21H461 userland | `RigoLigoRLC/unixnu-sm-t733` (Tab S7 FE, reaches lock screen): AppleKeyStoreState registration, software display registration, persistent Data mode, boot policy, PMIC keys | 4c679b4 |
| Board facts | Only this device: `../board-t970/hardware.json`, recovery DTB, Samsung kernel `pascua28/android_kernel_samsung_sm8250` 159176c | — |

Hardware values (addresses, GPIOs, regulators, partition LBAs, chip identity)
are never copied from umi or T733.

## Component mapping

Each row moves as its own component (drivers/<Name> repository or
`package/`), with a build recipe, startup hook and `porting.toml` area.

| Area | Existing in `../` | Reference to align with | Target here | State |
| --- | --- | --- | --- | --- |
| Touch: ST FTS1BA90A protocol, QUP0/GPI transfer, HID report bridge | `native-touch-prep` (src/T970NativeTouch.cpp, T970TouchHidBridge.cpp, T970Gpi.inc), `native-touch-provider-prep` (T970TouchProvider.cpp), `native-hid-carrier-integration` | umi `package/touch` (UmiFts*, UmiFtsGpi.inc, NativeHidInput.cpp); T733 `T733TouchHid.cpp` personality install | `drivers/NativeStFtsTouch` + `package/T970TouchHid` | to move first |
| Touch / panel power: L13/L14 rails, RPMh, SPMI | provider contract in `native-touch-provider-prep` (no real power code yet) | umi `UmiFtsPowerSequence.h`, `UmiRpmh.inc`, `UmiSpmi.inc`, `UmiPlatformPower.cpp` | `package/T970PlatformPower` | to move with touch |
| Power/volume keys | `menu-t970/s7p_keys.c` (UEFI only), `native-touch-provider-prep/references/s7p_keys.*` | T733 `NativePmicKeys` + `T733PmicKeysHid.cpp` (PMIC differs: PM8150 vs PM7325) | `package/T970PmicKeys` | after touch |
| UFS (OS/SystemOS/Data) | `ufs-t970`, `native-data-ufs`, `native-ufs-*` | umi `UmiNativeUfsHost.inc`, community `ShengUfs`/`NativeUfs` | community component + board bindings | decide: reuse community vs ours |
| Persistent Data | Apple APFS on Windows39 slice (`native-apple-apfs-*`, `deploy/apfs-*`) | T733 `data-layout.toml` mode 2 + UniXNU data seed (`seed-data-skip-setup.py`); umi `data-store` | `package/data-layout.toml` | decide after lsd/persona fix |
| Keystore | `../native-keystore-prep` (NativeKeyStoreEvents, T733-style, physically live in b287) | T733 `KeyStoreRegistration.cpp` | component `AppleKeyStoreState` + `package/T970KeyStore` | ready to move |
| Display | `native-display-prep` (HakiOS MiniIOMobileFramebuffer) | T733 NativeLegacyDisplay + `DisplayRegistration.cpp`; umi `legacy-display` | `package/T970Display` | later |
| AMFI / sandbox / services | `native-amfi-upstream`, `native-sandbox-upstream`, MiniIONetworkStack | UniXNU OS profile defaults | profile components | review |
| Loader / menu | `darwinboot`, `menu-t970` | UniXNU DarwinBoot + core menu | component `DarwinBoot` | later |
| UEFI | `firmware-t970` | umi `uefi-umi`, T733 Mu-Silicium | separate `uefi-gts7xlwifi` repo | later |

## Open inputs (REQUIRED in board.toml / DTS)

- One contiguous XNU memory range: measured DRAM is three banks and
  0xc0000000..0xd0000000 holds UEFI state (`../board-t970/STATUS.txt`).
- Board ADT identity (`chip_id`, unique chip id) from this device.
- UEFI DSC/FD paths once `firmware-t970` is split out.
- Partition geometry: ESP sda37, Windows39 slice (`../board-t970/partition-range.json`).

## Rules

- Android stays the default menu entry and fallback.
- Every moved component keeps its upstream provenance and license.
- A component counts as done only after a build plus a recorded device result.
