// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Starts the ShengHidInput device on this board.
//
// NativeHidInput is an IOHIDDevice compiled into this kernel, not a kext, so IOKit has no
// personality to match it against and nothing would ever start it. The driver adaption guide
// names the two ways a service starts: one matched through its provider, which needs a
// personality and probe/start, and one the board starts explicitly once its resources and OS
// dependencies are ready. A statically linked driver is the second kind, and the same guide
// points at the board initialisation calls in native_legacy_display_late_probe as where those
// calls belong, which is where the touch core is started from.
//
// The sequence itself is the reference implementation's, not something the documentation
// describes: register the class (which is why its static initialiser was globalised when the
// object was built), then allocate, init, attach and start. UniXNU-sheng and UniXNU-pipa both
// do exactly this; the only board-specific parts here are the boot argument and the provider.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>

extern kmod_info_t g_kernel_kmod_info;
// The static initialiser of the publisher, globalised by build-sheng-hid-input.py so that a
// statically linked class can be registered from here instead of by a kext load.
extern "C" void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *native_hid_input_allocate(void);
extern "C" IOService *native_hid_touch_allocate(void);
extern "C" IOService *native_hid_accel_allocate(void);
extern "C" IOService *native_hid_orient_allocate(void);
extern "C" IOService *native_hid_gyro_allocate(void);
extern "C" IOService *native_hid_pen_allocate(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" size_t qcom_usb_debug_write(const void *, size_t);

extern "C" {
// Published for the diagnostic panel and the console report.
volatile int native_hid_matching_load_status = -1;
volatile uint32_t native_hid_stage, native_hid_resource_found;
volatile int native_hid_load_status = -1, native_hid_input_meta_status = -1;
volatile uint32_t native_hid_class_sizes[5];
volatile uint64_t native_hid_input_object, native_hid_touch_object;
// 0 not asked for, 1 started, 2 the device refused to start.
volatile uint32_t native_hid_touch_status;
// gts7xlwifi accelerometer device (t970-sensors bit 16 or 32): 0 not asked for, 1 started, 2 refused to start.
volatile uint32_t native_hid_accel_status, native_hid_orient_status, native_hid_gyro_status;
volatile uint64_t native_hid_accel_object;
// gts7xlwifi S Pen device (t970-pen bit 4): 0 not asked for, 1 started, 2 refused to start.
volatile uint32_t native_hid_pen_status;
volatile uintptr_t native_hid_props_driver;
// The publisher's tick asks whether it should run. The touch core feeds it directly through
// the sink, so nothing has to poll: answer yes once the board has asked for the digitizer.
volatile uint32_t native_hid_input_command;
}

// The publisher's tick asks whether to send the Home/consumer report. Answering yes made it
// press Home every 1.1 s, which competes with touch and changes the screen on its own. There is
// no scripted press here, so nothing is requested; the reference board answers yes only while
// an offline auto-home command is armed.
// C linkage: NativeHidInput.cpp asks for the C symbol (UniXNU-alioth hit the C++ mangling here and kept its copy in the touch kit).
extern "C" bool native_hid_input_requested(void) {
  return false;
}

// Runs once, from native_legacy_display_late_probe, after the display and the touch core are up.
// Returns 0 when the device started. Idempotent: a second call returns the first result.
extern "C" int native_hid_input_probe(void) {
  static int result = -1;
  if (result >= 0) return result;
  unsigned enabled = 0;
  if (!PE_parse_boot_argn("offline-hid-input", &enabled, sizeof(enabled)) || enabled != 1) {
    result = 1;                       // not asked for on this boot
    return result;
  }
  native_hid_stage = 1;
  // IOHIDFamily must be loaded first: the device attaches to the IOHIDResource it publishes.
  native_hid_load_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
  /* Two kexts, not one. IOHIDFamily provides the device class and IOHIDResource, but the thing
   * that makes a report reach the system is IOHIDEventDriver, which is a separate kext. Without
   * it the publisher starts, publishes, and every report is dropped by its first line
   * (hasEventDriverCached() finds no child service), so native_touch_hid_reports stays zero
   * while contacts arrive. Measured on boot-usb92: stage reached 8 with zero reports. The
   * reference board loads both, in this order. */
  if (!native_hid_load_status)
    native_hid_matching_load_status = OSKext::loadKextWithIdentifier(
        "com.apple.iokit.IOHIDEventDriver", true, false, kOSKextExcludeNone, kOSKextExcludeNone, nullptr);
  const char *names[5] = {"IOHIDUserDevice", "IOHIDEventDriver", "IOHIDEventService",
                          "IOHIDResourceDeviceUserClient", "IOHIDDevice"};
  for (unsigned i = 0; i < 5; ++i) {
    const OSSymbol *name = OSSymbol::withCString(names[i]);
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (meta) native_hid_class_sizes[i] = meta->getClassSize();
    OSSafeReleaseNULL(name);
  }
  native_hid_stage = 2;
  OSDictionary *match = IOService::serviceMatching("IOHIDResource");
  IOService *resource = match ? IOService::waitForMatchingService(match, 5000000000ULL) : nullptr;
  OSSafeReleaseNULL(match);
  native_hid_resource_found = resource != nullptr;
  if (resource == nullptr) { native_hid_stage = 3; result = 2; return result; }
  // 208 is the IOHIDDevice instance size this build's headers describe. If it does not match,
  // the class layout and the original vtable disagree and starting it would corrupt memory.
  if (native_hid_class_sizes[4] != 208) { native_hid_stage = 4; result = 3; OSSafeReleaseNULL(resource); return result; }
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (handle == nullptr) { native_hid_stage = 5; result = 4; OSSafeReleaseNULL(resource); return result; }
  native_hid_static_init();
  native_hid_input_meta_status = OSMetaClass::postModLoad(handle);
  if (native_hid_input_meta_status != 0) { native_hid_stage = 6; result = 5; OSSafeReleaseNULL(resource); return result; }
  IOService *device = native_hid_input_allocate();
  native_hid_input_object = (uintptr_t) device;
  bool valid = device && device->metaCast("IOHIDDevice") && device->init();
  bool attached = valid && device->attach(resource);
  bool started = attached && device->start(resource);
  if (!started) {
    native_hid_stage = 7;
    if (attached) device->detach(resource);
    OSSafeReleaseNULL(device);
    native_hid_input_object = 0;
    result = 6;
    OSSafeReleaseNULL(resource);
    return result;
  }
  native_hid_stage = 8;
  // A second IOHIDDevice carrying only the digitizer. The combined device above publishes
  // whatever the touch core hands it, and on this board that has never reached the window
  // server: reports are attempted (hid rises) and handleReport refuses them (hres=1), while
  // edge keeps counting real touches. sheng publishes the digitizer as its own service when
  // touch-split=1, and the driver already carries both halves
  // (NativeHidInput.cpp:267 setTouchOnly / :320 native_hid_touch_allocate); only the call was
  // missing here. Gated on the same touch-hid=1 the reference requires.
  unsigned split = 0, touchHid = 0;
  if (PE_parse_boot_argn("touch-split", &split, sizeof split) && split == 1 &&
      PE_parse_boot_argn("touch-hid", &touchHid, sizeof touchHid) && touchHid == 1) {
    IOService *touch = native_hid_touch_allocate();
    native_hid_touch_object = (uintptr_t) touch;
    bool touchStarted = touch && touch->metaCast("IOHIDDevice") && touch->init() &&
                        touch->attach(resource) && touch->start(resource);
    if (!touchStarted && touch) {
      touch->detach(resource);
      OSSafeReleaseNULL(touch);
      native_hid_touch_object = 0;
    }
    native_hid_touch_status = touchStarted ? 1 : 2;
  }
  // gts7xlwifi: a third IOHIDDevice for the accelerometer (package/T970Sensors.cpp feeds it). Its own service, so it publishes
  // PrimaryUsagePage 0x20 / PrimaryUsage 0x73 (Accelerometer 3D) and carries no Built-In (native_hid_live_checkpoint leaves it alone).
  unsigned sensors = 0;
  if (PE_parse_boot_argn("t970-sensors", &sensors, sizeof sensors) && (sensors & 0x30)) {
    IOService *accel = native_hid_accel_allocate();
    native_hid_accel_object = (uintptr_t) accel;
    bool accelStarted = accel && accel->metaCast("IOHIDDevice") && accel->init() &&
                        accel->attach(resource) && accel->start(resource);
    if (!accelStarted && accel) {
      accel->detach(resource);
      OSSafeReleaseNULL(accel);
      native_hid_accel_object = 0;
    }
    native_hid_accel_status = accelStarted ? 1 : 2;
    // The device orientation device (Sensor page 0x20/0x8a, Apple motion orientation flags), fed with each orientation change.
    IOService *orient = native_hid_orient_allocate();
    bool orientStarted = orient && orient->metaCast("IOHIDDevice") && orient->init() &&
                         orient->attach(resource) && orient->start(resource);
    if (!orientStarted && orient) {
      orient->detach(resource);
      OSSafeReleaseNULL(orient);
    }
    native_hid_orient_status = orientStarted ? 1 : 2;
    // The gyro (Sensor page 0x20/0x7a, stamped 0xff00/9 like the accelerometer): locationd's orientation detector needs one.
    IOService *gyro = native_hid_gyro_allocate();
    bool gyroStarted = gyro && gyro->metaCast("IOHIDDevice") && gyro->init() && gyro->attach(resource) && gyro->start(resource);
    if (!gyroStarted && gyro) {
      gyro->detach(resource);
      OSSafeReleaseNULL(gyro);
    }
    native_hid_gyro_status = gyroStarted ? 1 : 2;
  }
  // gts7xlwifi S Pen (package/T970Pen.cpp, t970-pen bit 4): by default the touch device carries it in its own report 3 (NativeHidInput);
  // with bit 2048 a fourth IOHIDDevice, the stylus digitizer (or with bit 8 a one-contact touch screen), as in combo23/24b, whose
  // service backboardd never listed. It stamps its own event driver (NativeHidInput stampPen), not through native_hid_live_checkpoint.
  unsigned pen = 0;
  if (PE_parse_boot_argn("t970-pen", &pen, sizeof pen) && (pen & 4) && (pen & 2048)) {
    IOService *stylus = native_hid_pen_allocate();
    bool penStarted = stylus && stylus->metaCast("IOHIDDevice") && stylus->init() && stylus->attach(resource) && stylus->start(resource);
    if (!penStarted && stylus) {
      stylus->detach(resource);
      OSSafeReleaseNULL(stylus);
    }
    native_hid_pen_status = penStarted ? 1 : 2;
  }
  result = 0;
  OSSafeReleaseNULL(resource);
  return result;
}

// Called once a second by the publisher with the event driver it found. backboardd decides
// whether a digitizer event is accepted from the properties of the service that sent it:
// Built-In, DisplayIntegrated, and PrimaryUsagePage 0x0D with PrimaryUsage 1, 4 or 0x20. The
// combined device lists its keyboard collection first, so without this the services publish
// (1,6) and the event is ignored -- the reference board sets exactly these, on the driver and on
// its parent interface, once per driver rather than every tick.
// The IOHIDEventDriver below a device (umi HidInputProbe findEventDriver).
static IORegistryEntry *findEventDriver(IORegistryEntry *parent, unsigned depth) {
  if (!depth) return nullptr;
  OSIterator *it = parent->getChildIterator(gIOServicePlane);
  if (!it) return nullptr;
  IORegistryEntry *found = nullptr;
  while (OSObject *child = it->getNextObject()) {
    IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, child);
    if (!entry) continue;
    found = entry->metaCast("IOHIDEventDriver") ? entry : findEventDriver(entry, depth - 1);
    if (found) break;
  }
  it->release();
  return found;
}

// Called once a second by each NativeHidInput device with ITSELF (NativeHidInput.cpp tick), as umi's is. backboardd
// (-[BKHIDDirectTouchEventProcessor processEvent:sender:dispatcher:]) drops a digitizer event unless the sending event
// service has PrimaryUsagePage 0x0D and PrimaryUsage 1, 4 or 0x20, and reads Built-In/DisplayIntegrated from that service
// too. gts7xlwifi v36: the earlier version set them on the device and its parent (the IOHIDResource) and never on the
// IOHIDEventDriver, so 2571 accepted reports reached no window. Only the device that carries the digitizer has Built-In;
// the keyboard/buttons device is skipped. Set once per driver: each setProperty is a registry change notification.
extern "C" void native_hid_live_checkpoint(IORegistryEntry *device) {
  if (!device || !device->getProperty("Built-In")) return;
  IORegistryEntry *driver = findEventDriver(device, 4);
  if (!driver) return;
  static IORegistryEntry *publishedFor;
  if (publishedFor == driver) return;
  publishedFor = driver;
  native_hid_props_driver = (uintptr_t) driver;
  driver->setProperty("Built-In", kOSBooleanTrue);
  driver->setProperty("DisplayIntegrated", kOSBooleanTrue);
  driver->setProperty("PrimaryUsagePage", (unsigned long long) 0x0d, 32);
  driver->setProperty("PrimaryUsage", (unsigned long long) 4, 32);
  if (IORegistryEntry *interface = driver->getParentEntry(gIOServicePlane)) {
    interface->setProperty("Built-In", kOSBooleanTrue);
    interface->setProperty("DisplayIntegrated", kOSBooleanTrue);
    interface->setProperty("PrimaryUsagePage", (unsigned long long) 0x0d, 32);
    interface->setProperty("PrimaryUsage", (unsigned long long) 4, 32);
  }
}
