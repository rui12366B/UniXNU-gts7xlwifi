# Vibration motor

## Hardware

The live GTS7XL device tree (read from TWRP) has Samsung's `msm_vibrator` node: compatible `vibrator`,
`samsung,vib_type = "COINDC"`, `motor-vdd_type = 1`, `vibr_vdd-supply` = PM8150L LDO7 (`ldoc7` in the RPMh command DB,
2856 mV). No other node uses that LDO. It is an ERM coin motor that spins while the rail is on, and Samsung's driver only
enables and disables the regulator. The PMIC's LRA haptics block (`qcom,haptics@c000`) is `status = "disabled"`, so it is not fitted.

## Kernel

`package/T970Haptics.cpp`, boot argument `t970-haptics=<bits>` (off by default):

| bit | effect |
|---|---|
| 1 | publish IOService `T970Haptics` (under IOResources, after IOBSD) |
| 2 | one 150 ms pulse at boot (test) |

A pulse is three VRM votes through `t970_rpmh_write` (the shared APPS RSC DRV2 lock in `T970FtsTouch.cpp`): `ldoc7` voltage
2856 mV, mode HPM, enable; then enable 0 after the on time. The on time is clamped to 10..1000 ms. The rail is always switched off again.
BootLog row: `[t970 haptics] st <stage> rc <vote rc> n <pulses> ms <last> fail <n> test <boot rc> refused <n> off <rc>`.

Userspace (root; the sandbox may refuse other callers):

```c
io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("T970Haptics"));
int ms = 40;
CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &ms);
IORegistryEntrySetCFProperty(s, CFSTR("Pulse"), n);   // kIOReturnBusy while a pulse is running
```

## iPadOS

iPadOS has no vibration path: no iPad has a motor, so MobileGestalt reports none, and UIFeedbackGenerator and
`kSystemSoundID_Vibrate` do nothing. Upstream UniXNU has no haptics service. The only reference port is hakios (alioth). It sets the
MobileGestalt haptics identity (DeviceSupportsClosedLoopHaptics), so iOS plays its haptics as waveforms on an "Actuator" CoreAudio
device. The audio plug-in then passes them to the kernel motor driver. For this tablet that route would be:

1. an "Actuator" output device in `hal/T970AudioPlugIn.c` (owned by the audio work) that turns the waveform's envelope into
   `Pulse` requests (an ERM cannot follow a waveform: on for the envelope's length, off after);
2. the haptics identity in the device tree / MobileGestalt, which an iPad identity does not normally carry; check what it changes in
   SpringBoard and Settings before shipping it.

Until then the motor is reachable only through the `Pulse` property.
