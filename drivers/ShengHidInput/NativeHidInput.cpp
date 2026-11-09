// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit offline keyboard/consumer input source. Uses the unchanged original
// HID parser and event dispatch; no user-client entitlement is substituted.
#include "OriginalHidDeviceABI.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSIterator.h>
extern "C" void native_hid_input_observed(unsigned,int);
extern "C" void native_hid_live_checkpoint(IOService *);
extern "C" bool native_hid_input_requested(void);
extern "C" void native_touch_set_sink(void (*)(void *), void *) __attribute__((weak_import));
// Optional hardware buttons fed by the board button driver: a mask of report 2 bits, sent on every change.
extern "C" void native_buttons_set_sink(void (*)(void *, uint32_t), void *) __attribute__((weak_import));
// Optional touch digitizer fed by the board touch probe (kits/sheng/TouchProbe.cpp): finger set published with a seqlock.
extern "C" {
extern volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
}
// Keyboard Application collection ensures the original generic keyboard
// personality can match. Report 2 carries the Consumer buttons, bit 0 Menu (Home),
// bit 1 Power, bit 2 Volume Increment, bit 3 Volume Decrement. Report 1 is an
// ordinary boot keyboard report.
static const unsigned char descriptor[]={
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,
    0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,
    0x75,0x08,0x95,0x01,0x81,0x03,
    0x19,0x00,0x29,0x65,0x15,0x00,0x25,0x65,
    0x75,0x08,0x95,0x06,0x81,0x00,
    0x85,0x02,0x05,0x0c,0x09,0x40,0x09,0x30,0x09,0xe9,0x09,0xea,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x04,0x81,0x02,
    0x75,0x04,0x95,0x01,0x81,0x03,0xc0
};
// Report 3: ten-finger Touch Screen collection (Tip Switch, Touch Valid, Contact Identifier, X, Y per finger, then Contact Count).
// Touch Valid (0x47) is the finger confidence: IOHIDEventDriver turns a finger transducer whose value is 0 into a hand (palm), which
// multi-finger and system edge gestures ignore, so every finger, down or lifting, sends 1.
// Coordinates are normalised to 0..32767 on both axes by the producer.
#define FINGER_DESC \
    0x09,0x22,0xa1,0x02, 0x09,0x42,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x01,0x81,0x02, 0x09,0x47,0x81,0x02, 0x75,0x06,0x95,0x01,0x81,0x03, \
    0x09,0x51,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02, \
    0x05,0x01,0x09,0x30,0x15,0x00,0x26,0xff,0x7f,0x75,0x10,0x95,0x01,0x81,0x02, 0x09,0x31,0x81,0x02, 0x05,0x0d, 0xc0,
static const unsigned char touchDescriptor[]={
    0x05,0x0d,0x09,0x04,0xa1,0x01,0x85,0x03,
    FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
    FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
    0x09,0x54,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02,0xc0
};
#define TOUCH_FINGERS 10
// gts7xlwifi: the motion sensors (package/T970Sensors.cpp) as two devices of their own, in the form the original IOHIDEventDriver
// takes them. Its "HID SensorEvent Driver" personality (IOHIDFamily 2031, iPadOS 17) matches the Sensor page application collections
// 0x20/0x73 (Accelerometer 3D) and 0x20/0x8a (Device Orientation), among others; nothing matches Apple vendor page 0xff00/3, which is
// why the first version (combo1b) got no event driver and sent nothing.
// Accelerometer (report 4): Data Field Acceleration Axis X/Y/Z (0x453-0x455), int16 in mg: logical = physical -32768..32767 with unit
// exponent 0xd (-3), so getScaledFixedValue(kIOHIDValueScaleTypeExponent) in handleAccelReport gives g. The event driver turns each
// report into an accelerometer event.
static const unsigned char accelDescriptor[]={
    0x05,0x20,0x09,0x73,0xa1,0x01,0x85,0x04,
    0x0a,0x53,0x04,0x0a,0x54,0x04,0x0a,0x55,0x04,
    0x16,0x00,0x80,0x26,0xff,0x7f,0x36,0x00,0x80,0x46,0xff,0x7f,0x55,0x0d,
    0x75,0x10,0x95,0x03,0x81,0x02,0xc0
};
// Device orientation (report 5): the seven Apple motion orientation flags (page 0xff0c, usages 0x64 ambiguous, 0x65 portrait, 0x66
// portrait upside down, 0x67 landscape left, 0x68 landscape right, 0x69 face up, 0x6a face down; the range parseDeviceOrientationElement
// of the donor IOHIDEventDriver checks) as one bit each. handleDeviceOrientationReport sends an orientation event (CM usage type) for
// a flag that went from 0 to 1.
static const unsigned char orientDescriptor[]={
    0x05,0x20,0x09,0x8a,0xa1,0x01,0x85,0x05,
    0x06,0x0c,0xff,0x19,0x64,0x29,0x6a,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x07,0x81,0x02,
    0x75,0x01,0x95,0x01,0x81,0x03,0xc0
};
// Gyro (report 6): Data Field Angular Velocity about X/Y/Z (0x457-0x459) in the Gyrometer collection (0x20/0x7a, which the same
// personality matches), int16 in mrad/s with unit exponent -3, so handleGyroReport gives rad/s. locationd's orientation detector
// (CLOrientationNotifier::startDetection) gives up with "No gyro; not detecting orientation!" on a model that has one, as the j120 does.
static const unsigned char gyroDescriptor[]={
    0x05,0x20,0x09,0x7a,0xa1,0x01,0x85,0x06,
    0x0a,0x57,0x04,0x0a,0x58,0x04,0x0a,0x59,0x04,
    0x16,0x00,0x80,0x26,0xff,0x7f,0x36,0x00,0x80,0x46,0xff,0x7f,0x55,0x0d,
    0x75,0x10,0x95,0x03,0x81,0x02,0xc0
};
// The sensor driver hands each sample (iPadOS axes, mg) to the accel sink, x, y, z packed as three int16 in the low 48 bits, and each
// orientation change (bit 0..6 = usage 0x64..0x6a) to the orientation sink.
extern "C" void native_accel_set_sink(void (*)(void *, uint64_t), void *) __attribute__((weak_import));
extern "C" void native_orient_set_sink(void (*)(void *, uint64_t), void *) __attribute__((weak_import));
extern "C" void native_gyro_set_sink(void (*)(void *, uint64_t), void *) __attribute__((weak_import));   // mrad/s, packed as accel
// [0] 1 started (2 accel: event driver found and stamped 0xff00/3), [1] reports sent, [2] refused, [3] dropped: no event driver
// attached yet, [4] last result
extern "C" {volatile uint32_t native_accel_hid[5], native_orient_hid[5], native_gyro_hid[5];}
// gts7xlwifi S Pen, the old separate device (t970-pen bits 4 + 2048; the default now rides in the touch report, below). Report 7 is a Digitizer application collection (byte 3; Pen
// 0x02 with t970-pen bit 256) with one Stylus transducer: Tip Switch, Barrel Switch, Invert, Eraser, In Range, X/Y 0..32767
// (frame buffer orientation, like the touch screen), Tip Pressure 0..4095, X/Y Tilt -63..63 degrees. IOHIDEventDriver makes each report
// a stylus digitizer event (transducer type from the Stylus usage, pressure scaled by the logical range, tilt by the physical one).
// The application usage is the device's PrimaryUsage, which the HID event system reads once, when the service appears; backboardd takes
// digitizer events only from PrimaryUsage 1, 4 or 0x20. combo23 announced Pen (2): 673 reports accepted by handleReport, none reached the
// UI, and the later stamp to 0x20 came after the service had been created. Digitizer (1) is accepted as published.
static const unsigned char penDescriptor[]={
    0x05,0x0d,0x09,0x01,0xa1,0x01,0x85,0x07,
    0x09,0x20,0xa1,0x00,
    0x09,0x42,0x09,0x44,0x09,0x3c,0x09,0x45,0x09,0x32,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x05,0x81,0x02,
    0x95,0x03,0x81,0x03,
    0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x00,0x26,0xff,0x7f,0x75,0x10,0x95,0x02,0x81,0x02,
    0x05,0x0d,0x09,0x30,0x15,0x00,0x26,0xff,0x0f,0x75,0x10,0x95,0x01,0x81,0x02,
    0x09,0x3d,0x09,0x3e,0x15,0xc1,0x25,0x3f,0x35,0xc1,0x45,0x3f,0x65,0x14,0x55,0x00,0x75,0x08,0x95,0x02,0x81,0x02,
    0x65,0x00,0x35,0x00,0x45,0x00,
    0xc0,0xc0
};
// The fallback (t970-pen bit 8): report 8, a one-contact Touch Screen, the pen tip as a finger.
static const unsigned char penTouchDescriptor[]={
    0x05,0x0d,0x09,0x04,0xa1,0x01,0x85,0x08,
    FINGER_DESC
    0x09,0x54,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02,0xc0
};
// gts7xlwifi S Pen, default (t970-pen bit 4 without 2048): no device of its own. combo23 and combo24b sent 673 / 2733 reports to the
// separate pen device and backboardd never listed its service: its "primaryPage 0xD, display builtin" set held only this touch device
// (transport Virtual) both times, so nothing from the pen service reached a window whatever its usages. The pen goes into the touch
// device's report 3 instead, the one service backboardd takes digitizer events from, as an iPad's digitizer carries the Pencil:
// - stylus: this Stylus physical collection inside the Touch Screen application collection, after Contact Count (10 more bytes in
//   report 3). IOHIDEventDriver makes it a transducer of its own (type from the Stylus usage), dispatched as a child of the same
//   collection event as the fingers. It sits in the same report rather than a report of its own: in collection dispatch every
//   transducer without an element in the report logs "generates null event", which at ~225 pen packets/s would be ten log lines
//   each. Out of range and not touching it makes no event (the driver drops repeated untouch), so touch reports carry it for free.
//   Contact Identifier kPenContact (10, below backboardd's path index limit 31) keeps it apart from the fingers' 0..9.
// - fallback (bit 8): the tip as one more finger (contact kPenContact = 10) in report 3; the descriptor is unchanged.
static const unsigned char stylusCollection[]={
    0x09,0x20,0xa1,0x00,
    0x09,0x42,0x09,0x44,0x09,0x3c,0x09,0x45,0x09,0x32,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x05,0x81,0x02,
    0x95,0x03,0x81,0x03,
    0x09,0x51,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02,
    0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x00,0x26,0xff,0x7f,0x75,0x10,0x95,0x02,0x81,0x02,
    0x05,0x0d,0x09,0x30,0x15,0x00,0x26,0xff,0x0f,0x75,0x10,0x95,0x01,0x81,0x02,
    0x09,0x3d,0x09,0x3e,0x15,0xc1,0x25,0x3f,0x35,0xc1,0x45,0x3f,0x65,0x14,0x55,0x00,0x75,0x08,0x95,0x02,0x81,0x02,
    0x65,0x00,0x35,0x00,0x45,0x00,
    0xc0
};
// kPenContact: the pen's contact identifier, which becomes the event's index and backboardd's touch path index. backboardd
// (-[... addContact:]) drops any path index >= 31 ("addContact: pathIndex out of bounds"), so combo25's 0xfe made both the stylus
// (t970-pen=23) and the pen finger (t970-pen=31) vanish. The touch controller's fingers are 0..9 (T970FtsTouch kFingers), so 10.
enum {kPenContact=10,kTouchReport=1+TOUCH_FINGERS*6+1,kStylusBytes=10,kTouchReportStylus=kTouchReport+kStylusBytes};
// The pen driver hands every coordinate packet over packed (T970Pen.cpp pack()): x 0..14, y 15..29, pressure 30..41, tilt x 42..48,
// tilt y 49..55 (7-bit signed), 56 tip, 57 barrel, 58 in range, 59 eraser end.
extern "C" void native_pen_set_sink(void (*)(void *, uint64_t), void *) __attribute__((weak_import));
// [0] 1 separate device started (2 its event driver found and stamped), 3 in the touch device's report 3 as a stylus, 4 as a finger;
// [1] reports sent, [2] refused, [3] dropped (no event driver yet, or ten fingers down for the finger), [4] last result
extern "C" {volatile uint32_t native_pen_hid[5];}
// Edge contacts (int-163). A contact that starts within ~3 mm of a panel edge carries kIOHIDDigitizerEventFromEdgeTip (0x800) in its
// finger events' event mask for its whole life, as Apple's multitouch stack reports it (MultitouchHID). UIKit derives UITouch's edge
// type from that bit alone (_UIEventHIDTouchEdgeTypeForHIDEvent: 0x800/0x1000/0x2000/0x40000 -> 1..4), and every screen-edge
// recognizer of SpringBoard fails at once on edge type 0: int-162 logged edge=0 for touches starting 2 pt above the bottom edge and
// SBHomeGesturePanGestureRecognizer / SBFluidSwitcherScreenEdgePanGestureRecognizer going 0->5, so no home or switcher gesture.
// The original IOHIDEventDriver (IOHIDFamily-2031.120.4) never sets the bit. Its dispatchEvent (IOHIDEventService slot 198, the slot
// 37 of IOHIDFamily's indirect calls use) is wrapped for this device's driver only, in a copy of its vtable; the wrapper adds the bit
// to the finger children of each digitizer collection and calls the original. IOHIDEvent virtuals of that version: getType 18,
// getIntegerValue 23, setIntegerValue 24, getFixedValue 25 (setIntegerValue/getIntegerValue at +0xc0/+0xb8 in the driver's own
// calls); _children at +24 (IOHIDEvent.h; appendChild stores into it and free releases it). _parent is never set (appendChild only
// stores the child, int-163 skipped every finger by comparing it), so each child is an IOHIDEvent by its class instead.
namespace edge {
enum {kSlots=256,kDispatch=198,kGetType=18,kGetInteger=23,kSetInteger=24,kGetFixed=25,kContacts=256};
enum : uint32_t {kDigitizer=11,kX=0xB0000,kY=0xB0001,kType=0xB0004,kStylus=0,kIndex=0xB0005,kMask=0xB0007,kTouch=0xB0009,kMaskTouch=2,kFromEdgeTip=0x800};
// 48 panel pixels of 3048 x 2032 (the sweet reference uses 48 px), in the driver's normalised IOFixed coordinates.
static const int32_t kEdgeX=48*65536/3048,kEdgeY=48*65536/2032;
static uintptr_t originalDispatch;
static bool fromEdge[kContacts];
extern "C" {volatile uint32_t native_touch_edge_stats[4];}   // [0] installed, [1] contacts begun, [2] of them at an edge, [3] skipped events
// [0] reports with two or more fingers down, [1] the most fingers down in one report, [2] tip-up entries sent for vanished fingers
extern "C" {volatile uint32_t native_touch_hid_multi[3];}
template <typename R,typename... A> static R call(void *object,unsigned slot,A... args) {
    return reinterpret_cast<R (*)(void *,A...)>((*reinterpret_cast<uintptr_t **>(object))[slot])(object,args...);
}
// One finger event; true when it now carries the bit.
static bool mark(void *child) {
    if(call<uint32_t,IOOptionBits>(child,kGetType,0)!=kDigitizer)return false;   // the scan-time vendor child
    if((uint32_t)call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kType,0)==kStylus)return false;   // the S Pen: no edge bit
    const uint32_t index=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kIndex,0);
    if(index>=kContacts) {native_touch_edge_stats[3]=native_touch_edge_stats[3]+1;return false;}
    const uint32_t mask=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kMask,0);
    const bool touch=call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kTouch,0)!=0;
    if((mask&kMaskTouch) && touch) {   // the contact begins: its position now decides
        const int32_t x=call<int32_t,uint32_t,IOOptionBits>(child,kGetFixed,kX,0),y=call<int32_t,uint32_t,IOOptionBits>(child,kGetFixed,kY,0);
        fromEdge[index]=x<kEdgeX || x>65536-kEdgeX || y<kEdgeY || y>65536-kEdgeY;
        native_touch_edge_stats[1]=native_touch_edge_stats[1]+1;
        if(fromEdge[index])native_touch_edge_stats[2]=native_touch_edge_stats[2]+1;
    }
    const bool edged=fromEdge[index];
    if(edged)call<void,uint32_t,int32_t,IOOptionBits>(child,kSetInteger,kMask,(int32_t)(mask|kFromEdgeTip),0);
    if(!touch && (mask&kMaskTouch))fromEdge[index]=false;   // the lift keeps the bit; the next contact with this index starts fresh
    return edged;
}
static void dispatch(IOService *self,OSObject *event,IOOptionBits options) {
    if(event && call<uint32_t,IOOptionBits>(event,kGetType,0)==kDigitizer) {
        OSArray *children=*reinterpret_cast<OSArray **>(reinterpret_cast<uint8_t *>(event)+24);
        if(children && OSDynamicCast(OSArray,children) && children->getCount()) {
            // A collection (the driver's touch-screen dispatch): its mask is the union of its fingers', so it takes the bit as well.
            bool any=false;
            for(unsigned i=0;i<children->getCount();++i) {
                OSObject *child=children->getObject(i);
                if(child && child->metaCast("IOHIDEvent"))any|=mark(child);
                else native_touch_edge_stats[3]=native_touch_edge_stats[3]+1;
            }
            if(any) {
                const uint32_t mask=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(event,kGetInteger,kMask,0);
                call<void,uint32_t,int32_t,IOOptionBits>(event,kSetInteger,kMask,(int32_t)(mask|kFromEdgeTip),0);
            }
        } else if(!children) mark(event);   // one transducer dispatched on its own
    }
    reinterpret_cast<void (*)(IOService *,OSObject *,IOOptionBits)>(originalDispatch)(self,event,options);
}
// This device's own event driver gets the wrapped copy; any other driver keeps the original table.
static void install(IORegistryEntry *driver) {
    if(originalDispatch || !driver)return;
    uintptr_t *table=*reinterpret_cast<uintptr_t **>(driver);
    uintptr_t *copy=static_cast<uintptr_t *>(IOMalloc(kSlots*sizeof(uintptr_t)));
    if(!copy)return;
    memcpy(copy,table,kSlots*sizeof(uintptr_t));
    originalDispatch=copy[kDispatch];copy[kDispatch]=(uintptr_t)&dispatch;
    __asm__ volatile("dmb ish":::"memory");
    *reinterpret_cast<uintptr_t **>(driver)=copy;
    native_touch_edge_stats[0]=1;
}
}  // namespace edge
class NativeHidInput : public IOHIDDevice {
    OSDeclareDefaultStructors(NativeHidInput)
    IOWorkLoop *inputLoop=nullptr;
    IOTimerEventSource *timer=nullptr;
    bool down=false;
    uint8_t buttons=0;   // report 2 bits held by the hardware buttons
    bool touchEnabled=false;
    // The digitizer is a separate service from the keyboard: backboardd matches digitizers by primary usage page/usage
    // (Digitizer/Touch Screen) and the Built-In flag, which a combined keyboard-first descriptor would not provide.
    bool touchOnly=false;
    bool accelOnly=false;
    bool orientOnly=false;
    bool gyroOnly=false;
    // gts7xlwifi S Pen device; penBits is t970-pen (8 touch fallback, 256 Pen application collection, 512 stamp usage 4, 1024 no
    // collection dispatch). penTip: the fallback's contact was down in the last report.
    bool penOnly=false,penTip=false;
    unsigned penBits=0;
    // The touch device carrying the S Pen in its report 3 (t970-pen bit 4 without 2048): penAsFinger with bit 8, else the stylus
    // collection. penState: the pen's last packet (T970Pen.cpp packing); penRange: the stylus was in range in the last report.
    bool penMerged=false,penAsFinger=false,penRange=false;
    uint64_t penState=0;
    bool penStylus() const {return penMerged && !penAsFinger;}
    // The pen as one more finger, at its last position: down while the tip is, then one tip-up entry.
    void putPenFinger(unsigned char *f,bool pressed) const {
        const unsigned x=unsigned(penState&0x7fff),y=unsigned((penState>>15)&0x7fff);
        f[0]=pressed?3:2;f[1]=kPenContact;f[2]=(unsigned char)x;f[3]=(unsigned char)(x>>8);f[4]=(unsigned char)y;f[5]=(unsigned char)(y>>8);
    }
    // The stylus fields (report 7 of the separate device and the stylus collection of report 3 alike, after the flags and with the
    // contact identifier only in report 3). The eraser end: Invert while in range, Eraser instead of Tip Switch while pressed.
    static unsigned char penFlags(uint64_t v) {
        const bool tip=(v>>56)&1,barrel=(v>>57)&1,range=(v>>58)&1,eraser=(v>>59)&1;
        return (unsigned char)((tip && !eraser?1:0)|(barrel?2:0)|(eraser && range?4:0)|(tip && eraser?8:0)|(range?16:0));
    }
    static void putPenAxes(unsigned char *a,uint64_t v) {
        const unsigned x=unsigned(v&0x7fff),y=unsigned((v>>15)&0x7fff),p=unsigned((v>>30)&0xfff);
        const int tx=int((v>>42)&0x7f)<<25>>25,ty=int((v>>49)&0x7f)<<25>>25;
        a[0]=(unsigned char)x;a[1]=(unsigned char)(x>>8);a[2]=(unsigned char)y;a[3]=(unsigned char)(y>>8);
        a[4]=(unsigned char)p;a[5]=(unsigned char)(p>>8);a[6]=(unsigned char)(signed char)tx;a[7]=(unsigned char)(signed char)ty;
    }
    void putStylus(unsigned char *s) const {s[0]=penFlags(penState);s[1]=kPenContact;putPenAxes(s+2,penState);}
    // The orientation last asked for, and whether it reached an event driver: the drivers attach late (IOMatchDefer, ~47 s into
    // combo2), so the state decided before then is sent once the driver is there.
    uint64_t lastOrient=0;
    bool orientDelivered=false;
    bool eventDriverFound=false;
    uint32_t lastSeq=0;
    // Fingers sent as down in the previous report; a vanished identifier gets one explicit tip-up report.
    int prevIds[TOUCH_FINGERS]={-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
    int prevX[TOUCH_FINGERS]={},prevY[TOUCH_FINGERS]={};
    static unsigned transformCoordinate(int value,int maximum) {
        if(value<0)value=0;if(value>=maximum)value=maximum-1;
        return (unsigned)((long long)value*32767/(maximum-1));
    }
    // One finger set to the event driver. The touch thread (kits/sheng/TouchProbe.cpp) calls touchNotify after it publishes a frame; there is no timer
    // watching the seqlock any more (it woke the work loop every 8 ms for nothing).
    static void touchNotify(void *context) {
        NativeHidInput *self=static_cast<NativeHidInput *>(context);
        if(self && self->inputLoop)self->inputLoop->runAction(&NativeHidInput::touchAction,self);
    }
    static IOReturn touchAction(OSObject *owner,void *,void *,void *,void *) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return kIOReturnSuccess;
        self->touchSend();
        return kIOReturnSuccess;
    }
    void touchSend() {
        NativeHidInput *self=this;
        if(!self->hasEventDriverCached())return;
        uint32_t before=native_touch_seq;
        if((before&1) || before==self->lastSeq)return;
        __asm__ volatile("dmb ish":::"memory");
        unsigned count=native_touch_count;if(count>TOUCH_FINGERS)count=TOUCH_FINGERS;
        int ids[TOUCH_FINGERS],xs[TOUCH_FINGERS],ys[TOUCH_FINGERS];
        for(unsigned i=0;i<count;++i){ids[i]=native_touch_ids[i];xs[i]=native_touch_xs[i];ys[i]=native_touch_ys[i];}
        __asm__ volatile("dmb ish":::"memory");
        if(native_touch_seq!=before)return;      // torn read: retry on the next tick
        self->lastSeq=before;
        // Sizes follow the axis maxima of the touch core: 30480 long, 20320 short, mapped to the same 0..32767 range.
        unsigned char report[kTouchReportStylus]={3};
        unsigned slot=0;
        int nextIds[TOUCH_FINGERS],nextX[TOUCH_FINGERS],nextY[TOUCH_FINGERS];
        for(unsigned i=0;i<count && slot<TOUCH_FINGERS;++i,++slot) {
            unsigned x=transformCoordinate(xs[i],30480),y=transformCoordinate(ys[i],20320);
            unsigned char *f=report+1+slot*6;
            f[0]=3;f[1]=(unsigned char)(ids[i]&0xff);f[2]=(unsigned char)x;f[3]=(unsigned char)(x>>8);f[4]=(unsigned char)y;f[5]=(unsigned char)(y>>8);   // tip, valid
            nextIds[slot]=ids[i];nextX[slot]=(int)x;nextY[slot]=(int)y;
        }
        const unsigned downCount=slot;
        if(downCount>=2)edge::native_touch_hid_multi[0]=edge::native_touch_hid_multi[0]+1;
        if(downCount>edge::native_touch_hid_multi[1])edge::native_touch_hid_multi[1]=downCount;
        // Identifiers that were down and are gone: tip up at the last known position. They are matched by identifier, not by slot:
        // the producer packs its fingers, so when one lifts the others move down a slot and a slot match would lift a finger still
        // down and leave the lifted one stuck.
        for(unsigned j=0;j<TOUCH_FINGERS;++j) {
            const int id=self->prevIds[j];
            if(id<0)continue;
            bool kept=false;
            for(unsigned i=0;i<downCount && !kept;++i)kept=(ids[i]&0xff)==(id&0xff);
            if(kept || slot>=TOUCH_FINGERS)continue;
            unsigned char *f=report+1+slot*6;
            f[0]=2;f[1]=(unsigned char)(id&0xff);   // tip up, valid
            f[2]=(unsigned char)self->prevX[j];f[3]=(unsigned char)(self->prevX[j]>>8);
            f[4]=(unsigned char)self->prevY[j];f[5]=(unsigned char)(self->prevY[j]>>8);
            ++slot;edge::native_touch_hid_multi[2]=edge::native_touch_hid_multi[2]+1;
        }
        for(unsigned j=0;j<TOUCH_FINGERS;++j) {
            self->prevIds[j]=j<downCount?nextIds[j]:-1;
            if(j<downCount){self->prevX[j]=nextX[j];self->prevY[j]=nextY[j];}
        }
        if(!slot)return;
        // The S Pen in the same report: as a finger while its tip is down, or its stylus fields (no event while it is out of range).
        if(self->penAsFinger && self->penTip && slot<TOUCH_FINGERS){self->putPenFinger(report+1+slot*6,true);++slot;}
        if(self->penStylus())self->putStylus(report+kTouchReport);
        report[1+TOUCH_FINGERS*6]=(unsigned char)slot;
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,self->penStylus()?kTouchReportStylus:kTouchReport,kIODirectionNone);
        IOReturn result=buffer?self->handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(6,result);
        native_touch_hid_reports=native_touch_hid_reports+1;
    }
    // The button driver's thread calls buttonsNotify on every change of the pressed set.
    static void buttonsNotify(void *context,uint32_t mask) {
        NativeHidInput *self=static_cast<NativeHidInput *>(context);
        if(self && self->inputLoop)self->inputLoop->runAction(&NativeHidInput::buttonsAction,self,reinterpret_cast<void *>(uintptr_t(mask)));
    }
    static IOReturn buttonsAction(OSObject *owner,void *mask,void *,void *,void *) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return kIOReturnSuccess;
        self->buttons=static_cast<uint8_t>(uintptr_t(mask)&0xe);
        if(!self->hasEventDriverCached())return kIOReturnSuccess;
        unsigned char report[]={2,static_cast<unsigned char>((self->down?1:0)|self->buttons)};
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=buffer?self->handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(4,result);
        return kIOReturnSuccess;
    }
    // The sensor thread calls accelNotify for every sample; the report goes out from this device's work loop.
    static void accelNotify(void *context,uint64_t packed) {
        NativeHidInput *self=static_cast<NativeHidInput *>(context);
        if(self && self->inputLoop)self->inputLoop->runAction(&NativeHidInput::accelAction,self,reinterpret_cast<void *>(uintptr_t(packed)));
    }
    static IOReturn accelAction(OSObject *owner,void *packed,void *,void *,void *) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return kIOReturnSuccess;
        volatile uint32_t *stats=self->orientOnly?native_orient_hid:self->gyroOnly?native_gyro_hid:native_accel_hid;
        const uint64_t v=uintptr_t(packed);
        if(self->orientOnly){self->lastOrient=v;self->orientDelivered=false;}
        if(!self->hasEventDriverCached()){stats[3]=stats[3]+1;return kIOReturnSuccess;}
        unsigned char report[7]={static_cast<unsigned char>(self->orientOnly?5:self->gyroOnly?6:4)};
        const unsigned length=self->orientOnly?2:7;
        for(unsigned i=1;i<length;++i)report[i]=static_cast<unsigned char>(v>>(8*(i-1)));
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,length,kIODirectionNone);
        IOReturn result=buffer?self->handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(7,result);
        if(result==kIOReturnSuccess){stats[1]=stats[1]+1;if(self->orientOnly)self->orientDelivered=true;}else stats[2]=stats[2]+1;
        stats[4]=(uint32_t)result;
        return kIOReturnSuccess;
    }
    // The pen thread calls penNotify for every coordinate packet; the report goes out from this device's work loop.
    static void penNotify(void *context,uint64_t packed) {
        NativeHidInput *self=static_cast<NativeHidInput *>(context);
        if(self && self->inputLoop)self->inputLoop->runAction(&NativeHidInput::penAction,self,reinterpret_cast<void *>(uintptr_t(packed)));
    }
    static IOReturn penAction(OSObject *owner,void *packed,void *,void *,void *) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return kIOReturnSuccess;
        if(!self->hasEventDriverCached()){native_pen_hid[3]=native_pen_hid[3]+1;return kIOReturnSuccess;}
        const uint64_t v=uintptr_t(packed);
        if(self->penMerged)return self->penMergedSend(v);
        const unsigned x=unsigned(v&0x7fff),y=unsigned((v>>15)&0x7fff);
        const bool tip=(v>>56)&1;
        unsigned char report[10];
        unsigned length;
        if(self->penBits&8) {
            // One finger: down while the tip is, one tip-up report when it lifts, nothing while it hovers.
            if(!tip && !self->penTip)return kIOReturnSuccess;
            report[0]=8;report[1]=tip?3:2;report[2]=0;
            report[3]=(unsigned char)x;report[4]=(unsigned char)(x>>8);report[5]=(unsigned char)y;report[6]=(unsigned char)(y>>8);
            report[7]=1;length=8;
            self->penTip=tip;
        } else {
            report[0]=7;report[1]=penFlags(v);putPenAxes(report+2,v);
            length=10;
        }
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,length,kIODirectionNone);
        IOReturn result=buffer?self->handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(8,result);
        if(result==kIOReturnSuccess)native_pen_hid[1]=native_pen_hid[1]+1;else native_pen_hid[2]=native_pen_hid[2]+1;
        native_pen_hid[4]=(uint32_t)result;
        return kIOReturnSuccess;
    }
    // One pen packet into the touch device's report 3, with the fingers last sent as down (still down: touchSend lifts them), so the
    // event driver sees the whole contact set, as it does in every touch report. Nothing while the pen stays away (fallback: tip up;
    // stylus: out of range), one report when it leaves.
    IOReturn penMergedSend(uint64_t v) {
        penState=v;
        const bool tip=(v>>56)&1,range=(v>>58)&1;
        if(penAsFinger ? (!tip && !penTip) : (!range && !penRange))return kIOReturnSuccess;
        unsigned char report[kTouchReportStylus]={3};
        unsigned slot=0;
        for(unsigned j=0;j<TOUCH_FINGERS && slot<TOUCH_FINGERS;++j) {
            if(prevIds[j]<0)continue;
            unsigned char *f=report+1+slot*6;
            f[0]=3;f[1]=(unsigned char)(prevIds[j]&0xff);
            f[2]=(unsigned char)prevX[j];f[3]=(unsigned char)(prevX[j]>>8);f[4]=(unsigned char)prevY[j];f[5]=(unsigned char)(prevY[j]>>8);
            ++slot;
        }
        if(penAsFinger) {
            if(slot>=TOUCH_FINGERS){penTip=false;native_pen_hid[3]=native_pen_hid[3]+1;return kIOReturnSuccess;}   // ten fingers down
            putPenFinger(report+1+slot*6,tip);++slot;
            penTip=tip;
        } else {
            putStylus(report+kTouchReport);
            penRange=range;
        }
        report[1+TOUCH_FINGERS*6]=(unsigned char)slot;
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,penAsFinger?kTouchReport:kTouchReportStylus,kIODirectionNone);
        IOReturn result=buffer?handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(8,result);
        if(result==kIOReturnSuccess)native_pen_hid[1]=native_pen_hid[1]+1;else native_pen_hid[2]=native_pen_hid[2]+1;
        native_pen_hid[4]=(uint32_t)result;
        return kIOReturnSuccess;
    }
    // backboardd takes a digitizer event only from a Built-In, DisplayIntegrated service whose PrimaryUsagePage is 0x0D and PrimaryUsage
    // 1, 4 or 0x20 (native_hid_live_checkpoint). The descriptor already announces 1 (stylus) or 4 (fallback), so the driver and its interface
    // get Built-In and DisplayIntegrated once, as stampMotion does, and PrimaryUsage only with bit 512 (4). native_hid_live_checkpoint
    // itself is not used: it remembers a single driver, and the touch device's would then be stamped again every second.
    void stampPen(IORegistryEntry *driver) {
        IORegistryEntry *targets[2]={driver,driver->getParentEntry(gIOServicePlane)};
        for(IORegistryEntry *t:targets) {
            if(!t)continue;
            t->setProperty("Built-In",kOSBooleanTrue);
            t->setProperty("DisplayIntegrated",kOSBooleanTrue);
            if(penBits&512){t->setProperty("PrimaryUsagePage",(unsigned long long)0x0d,32);t->setProperty("PrimaryUsage",(unsigned long long)4,32);}
        }
        native_pen_hid[0]=2;
    }
    bool hasEventDriverCached() {
        if(!eventDriverFound) {
            IORegistryEntry *driver=eventDriver(this,4);
            eventDriverFound=driver!=nullptr;
            if(driver && (touchEnabled || touchOnly))edge::install(driver);
            if(driver && accelOnly)stampMotion(driver,3,0x73,native_accel_hid);
            if(driver && gyroOnly)stampMotion(driver,9,0x7a,native_gyro_hid);
            if(driver && orientOnly)stampBuiltIn(driver,native_orient_hid);
            if(driver && penOnly)stampPen(driver);
        }
        return eventDriverFound;
    }
    // The clients look the accelerometer up as the Apple SPU publishes it, not as the event driver matched it: backboardd's matcher
    // is initWithUsagePage:0xff00 usage:3 builtIn:YES (BKAccelerometerInterface), locationd's CLAccelerometer service lookup is the
    // pair {0xff00, 3}. IOHIDEventDriver only matches the Sensor page collection (0x20/0x73) and publishes that as its primary usage,
    // so the driver and its interface are stamped with the SPU identity (the event system's conformsTo reads DeviceUsagePairs, then
    // PrimaryUsagePage/PrimaryUsage), keeping 0x20/0x73 in the pairs as well. Same approach as native_hid_live_checkpoint for the
    // digitizer. Once, when the driver is first seen.
    // The gyro the same way: backboardd's gyro matcher is 0xff00/9 builtIn:YES, locationd's CLGyro looks up {0xff00, 9}.
    // The device orientation service keeps its Sensor page identity (0x20/0x8a is the pair locationd's CLOrientationNotifier looks up),
    // but locationd's CLIoHidInterface also requires its matching properties, {Built-In = true} for this device (a constant dictionary
    // in the donor locationd). Without it every lookup logged "Service for device with usage pair {32, 138} is not ready" (combo8b).
    static void stampBuiltIn(IORegistryEntry *driver,volatile uint32_t *stats) {
        IORegistryEntry *targets[2]={driver,driver->getParentEntry(gIOServicePlane)};
        for(IORegistryEntry *t:targets)if(t)t->setProperty("Built-In",kOSBooleanTrue);
        stats[0]=2;   // 2: event driver found and stamped
    }
    static void stampMotion(IORegistryEntry *driver,unsigned appleUsage,unsigned sensorUsage,volatile uint32_t *stats) {
        OSArray *pairs=OSArray::withCapacity(2);
        const unsigned values[2][2]={{0xff00,appleUsage},{0x20,sensorUsage}};
        for(unsigned i=0;pairs && i<2;++i) {
            OSDictionary *pair=OSDictionary::withCapacity(2);
            OSNumber *page=OSNumber::withNumber(values[i][0],32),*usage=OSNumber::withNumber(values[i][1],32);
            if(pair && page && usage){pair->setObject("DeviceUsagePage",page);pair->setObject("DeviceUsage",usage);pairs->setObject(pair);}
            OSSafeReleaseNULL(page);OSSafeReleaseNULL(usage);OSSafeReleaseNULL(pair);
        }
        IORegistryEntry *targets[2]={driver,driver->getParentEntry(gIOServicePlane)};
        for(IORegistryEntry *t:targets) {
            if(!t)continue;
            t->setProperty("Built-In",kOSBooleanTrue);
            t->setProperty("PrimaryUsagePage",(unsigned long long)0xff00,32);
            t->setProperty("PrimaryUsage",(unsigned long long)appleUsage,32);
            if(pairs)t->setProperty("DeviceUsagePairs",pairs);
        }
        OSSafeReleaseNULL(pairs);
        stats[0]=2;   // 2: event driver found and stamped
    }
    static bool hasEventDriver(IORegistryEntry *parent,unsigned depth) {return eventDriver(parent,depth)!=nullptr;}
    // The IOHIDEventDriver below this device, if one has attached.
    static IORegistryEntry *eventDriver(IORegistryEntry *parent,unsigned depth) {
        if(!depth)return nullptr;
        OSIterator *it=parent->getChildIterator(gIOServicePlane);if(!it)return nullptr;
        IORegistryEntry *found=nullptr;
        while(OSObject *child=it->getNextObject()) {
            IORegistryEntry *entry=OSDynamicCast(IORegistryEntry,child);
            if(!entry)continue;
            found=entry->metaCast("IOHIDEventDriver")?entry:eventDriver(entry,depth-1);
            if(found)break;
        }
        it->release();return found;
    }
    static void tick(OSObject *owner,IOTimerEventSource *source) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return;
        if(self->accelOnly||self->orientOnly||self->gyroOnly||self->penOnly){
            // The separate pen device: how many user clients its event driver has (bits 8..15 of native_pen_hid[0]). The HID event
            // system opens one per service it takes; none means no user-space process ever saw the service (combo24b: backboardd's
            // digitizer matcher never listed it).
            if(self->penOnly && self->hasEventDriverCached()) {
                unsigned clients=0;
                if(IORegistryEntry *driver=eventDriver(self,4)) {
                    if(OSIterator *it=driver->getChildIterator(gIOServicePlane)) {
                        while(OSObject *child=it->getNextObject())if(OSDynamicCast(IORegistryEntry,child))++clients;
                        it->release();
                    }
                }
                native_pen_hid[0]=(native_pen_hid[0]&0xff)|((clients>0xff?0xff:clients)<<8);
            }
            if(self->hasEventDriverCached() && self->orientOnly && self->lastOrient && !self->orientDelivered)
                accelAction(self,reinterpret_cast<void *>(uintptr_t(self->lastOrient)),nullptr,nullptr,nullptr);
            source->setTimeoutMS(1000);return;
        }
        native_hid_live_checkpoint(self);
        if(self->touchOnly){source->setTimeoutMS(1000);return;}
        if(!self->down && !hasEventDriver(self,4)) {
            native_hid_input_observed(3,0);
            source->setTimeoutMS(1000);
            return;
        }
        native_hid_input_observed(3,1);
        if(!self->down && !native_hid_input_requested()) {source->setTimeoutMS(1000);return;}
        unsigned char report[]={2,static_cast<unsigned char>((self->down?0:1)|self->buttons)};
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=buffer?self->handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(self->down?5:4,result);
        // Always attempt release, even if down returned an error: input must
        // never be left pressed by an ambiguous report completion.
        if(!self->down) {self->down=true;source->setTimeoutMS(100);}
        else {self->down=false;source->setTimeoutMS(1000);}
    }
public:
    void setTouchOnly() {touchOnly=true;}
    void setAccelOnly() {accelOnly=true;}
    void setOrientOnly() {orientOnly=true;}
    void setGyroOnly() {gyroOnly=true;}
    void setPenOnly() {penOnly=true;}
    IOWorkLoop *getWorkLoop() const override {return inputLoop;}
    OSString *newTransportString() const override {return OSString::withCString(touchOnly?"SPI":(accelOnly||orientOnly||gyroOnly)?"SPU":penOnly?"I2C":"Virtual");}
    OSString *newProductString() const override {return OSString::withCString(touchOnly?"Offline touch digitizer":accelOnly?"Accelerometer":orientOnly?"Device orientation":gyroOnly?"Gyro":penOnly?"Stylus digitizer":"Offline HID input probe");}
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        if(!out)return kIOReturnBadArgument;
        unsigned char combined[sizeof(descriptor)+sizeof(touchDescriptor)+sizeof(stylusCollection)];
        size_t size;
        if(penOnly && (penBits&8)){*out=IOBufferMemoryDescriptor::withBytes(penTouchDescriptor,sizeof(penTouchDescriptor),kIODirectionNone);return *out?kIOReturnSuccess:kIOReturnNoMemory;}
        if(penOnly){
            unsigned char pen[sizeof(penDescriptor)];
            memcpy(pen,penDescriptor,sizeof pen);
            if(penBits&256)pen[3]=0x02;   // Pen application collection instead of Digitizer (combo23's layout)
            // backboardd (-[BKHIDDirectTouchEventProcessor matcher:servicesDidMatch:]) files a digitizer service by its usage pair:
            // 0x0D/4 main display, 0x0D/1 "AOP", 0x0D/0x20 the pencil digitizer (mainDisplayPencilDigitizerService). Bit 4096: the
            // application collection is Stylus (0x20), so the service's primary usage is the pencil pair from the start. The Stylus
            // physical collection inside still makes the transducer (IOHIDEventDriver walks up to the first digitizer collection).
            if(penBits&4096)pen[3]=0x20;
            *out=IOBufferMemoryDescriptor::withBytes(pen,sizeof pen,kIODirectionNone);return *out?kIOReturnSuccess:kIOReturnNoMemory;
        }
        if(accelOnly){*out=IOBufferMemoryDescriptor::withBytes(accelDescriptor,sizeof(accelDescriptor),kIODirectionNone);return *out?kIOReturnSuccess:kIOReturnNoMemory;}
        if(gyroOnly){*out=IOBufferMemoryDescriptor::withBytes(gyroDescriptor,sizeof(gyroDescriptor),kIODirectionNone);return *out?kIOReturnSuccess:kIOReturnNoMemory;}
        if(orientOnly){*out=IOBufferMemoryDescriptor::withBytes(orientDescriptor,sizeof(orientDescriptor),kIODirectionNone);return *out?kIOReturnSuccess:kIOReturnNoMemory;}
        if(touchOnly){size=sizeof(touchDescriptor);memcpy(combined,touchDescriptor,size);}
        else{
            size=sizeof(descriptor);memcpy(combined,descriptor,size);
            // Default: one device, keyboard collection first, digitizer second (the arrangement whose event driver
            // and user client attach on hardware); the separate digitizer device is the opt-in sheng-touch-split.
            if(touchEnabled){memcpy(combined+size,touchDescriptor,sizeof(touchDescriptor));size+=sizeof(touchDescriptor);}
        }
        // The S Pen's stylus collection goes inside the Touch Screen application collection, before its closing 0xc0.
        if((touchOnly||touchEnabled) && penStylus()) {
            --size;
            memcpy(combined+size,stylusCollection,sizeof(stylusCollection));size+=sizeof(stylusCollection);
            combined[size++]=0xc0;
        }
        *out=IOBufferMemoryDescriptor::withBytes(combined,size,kIODirectionNone);
        return *out?kIOReturnSuccess:kIOReturnNoMemory;
    }
    bool start(IOService *provider) override {
        inputLoop=IOWorkLoop::workLoop();if(!inputLoop)return false;
        // Request the normal generic HID event personality for this owned
        // keyboard. This is a matching property, not a client entitlement.
        if(!setProperty("HIDDefaultBehavior",true))return false;
        unsigned touch=0;
        touchEnabled=PE_parse_boot_argn("sheng-touch-hid",&touch,sizeof(touch)) && touch==1;
        // gts7xlwifi: with sheng-touch-split=1 the digitizer is only the separate touch-only device. The combined device keeps
        // the keyboard and buttons and declares no second, dead Touch Screen the window server could bind to instead.
        unsigned split=0;
        if(!touchOnly && PE_parse_boot_argn("sheng-touch-split",&split,sizeof(split)) && split==1)touchEnabled=false;
        if(accelOnly||orientOnly||gyroOnly||penOnly)touchEnabled=false;
        // The pen device is Built-In and DisplayIntegrated from the start (the interface copies them when it starts, as for the motion
        // devices). Both dispatch as a collection with the transducer as its child, as the touch screen and as an iPad's own digitizer
        // sends the Pencil; t970-pen bit 1024 sends the stylus event on its own instead.
        if(penOnly){
            PE_parse_boot_argn("t970-pen",&penBits,sizeof(penBits));
            setProperty("Built-In",true);setProperty("DisplayIntegrated",true);
            if((penBits&8) || !(penBits&1024))setProperty("DigitizerCollectionDispatch",true);
        }
        // DigitizerCollectionDispatch: IOHIDEventDriver (setDigitizerProperties) reads it up the service plane and then sends each
        // report's fingers as one digitizer collection with a child per finger. Without it a touch screen gets one event per finger,
        // and backboardd takes each as the whole touch set, so no two fingers are ever down together for UIKit (no pinch, no
        // multi-finger or system gestures).
        if(touchOnly||touchEnabled){setProperty("Built-In",true);setProperty("DisplayIntegrated",true);setProperty("DigitizerCollectionDispatch",true);}
        // The S Pen rides in this device's report 3 (t970-pen bit 4; bit 2048 keeps the old separate device instead). Read before
        // IOHIDDevice::start, which asks for the report descriptor.
        if(touchOnly||touchEnabled) {
            unsigned bits=0;
            if(PE_parse_boot_argn("t970-pen",&bits,sizeof(bits)) && (bits&4) && !(bits&2048) && &native_pen_set_sink) {
                penBits=bits;penMerged=true;penAsFinger=(bits&8)!=0;
            }
        }
        // The motion devices are Built-In from the start: IOHIDInterface copies Built-In from the device and IOHIDEventService from the
        // interface when they start, so the event driver already carries it when it registers. locationd's CLIoHidInterface matches a
        // service once, when the new-service notification arrives; the later stamp (stampBuiltIn/stampMotion, ~1 s after) came too late
        // and {32, 138} stayed "not ready" for the whole of combo9.
        if(accelOnly||orientOnly||gyroOnly)setProperty("Built-In",true);
        if(!IOHIDDevice::start(provider))return false;
        timer=IOTimerEventSource::timerEventSource(this,tick);
        if(!timer || inputLoop->addEventSource(timer)!=kIOReturnSuccess) {
            OSSafeReleaseNULL(timer);IOHIDDevice::stop(provider);return false;
        }
        if(touchOnly||touchEnabled) {
            if(&native_touch_set_sink)native_touch_set_sink(&NativeHidInput::touchNotify,this);
            if(penMerged){native_pen_set_sink(&NativeHidInput::penNotify,this);native_pen_hid[0]=penAsFinger?4:3;}
        }
        if(accelOnly){if(&native_accel_set_sink)native_accel_set_sink(&NativeHidInput::accelNotify,this);native_accel_hid[0]=1;}
        else if(orientOnly){if(&native_orient_set_sink)native_orient_set_sink(&NativeHidInput::accelNotify,this);native_orient_hid[0]=1;}
        else if(gyroOnly){if(&native_gyro_set_sink)native_gyro_set_sink(&NativeHidInput::accelNotify,this);native_gyro_hid[0]=1;}
        else if(penOnly){if(&native_pen_set_sink)native_pen_set_sink(&NativeHidInput::penNotify,this);native_pen_hid[0]=1;}
        else if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(&NativeHidInput::buttonsNotify,this);
        setProperty("OfflineInputProbe",true);
        native_hid_input_observed(2,0);
        timer->setTimeoutMS(1000);return true;
    }
    void stop(IOService *provider) override {
        if((touchOnly||touchEnabled) && &native_touch_set_sink)native_touch_set_sink(nullptr,nullptr);
        if(penMerged && &native_pen_set_sink)native_pen_set_sink(nullptr,nullptr);
        if(accelOnly){if(&native_accel_set_sink)native_accel_set_sink(nullptr,nullptr);}
        else if(orientOnly){if(&native_orient_set_sink)native_orient_set_sink(nullptr,nullptr);}
        else if(gyroOnly){if(&native_gyro_set_sink)native_gyro_set_sink(nullptr,nullptr);}
        else if(penOnly){if(&native_pen_set_sink)native_pen_set_sink(nullptr,nullptr);}
        else if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(nullptr,nullptr);
        if(timer){timer->cancelTimeout();inputLoop->removeEventSource(timer);OSSafeReleaseNULL(timer);}
        IOHIDDevice::stop(provider);
    }
    void free() override {OSSafeReleaseNULL(inputLoop);IOHIDDevice::free();}
};
OSDefineMetaClassAndStructors(NativeHidInput,IOHIDDevice)
extern "C" IOService *native_hid_input_allocate(void) {return new NativeHidInput;}
extern "C" IOService *native_hid_accel_allocate(void) {NativeHidInput *device=new NativeHidInput;if(device)device->setAccelOnly();return device;}
extern "C" IOService *native_hid_gyro_allocate(void) {NativeHidInput *device=new NativeHidInput;if(device)device->setGyroOnly();return device;}
extern "C" IOService *native_hid_orient_allocate(void) {NativeHidInput *device=new NativeHidInput;if(device)device->setOrientOnly();return device;}
extern "C" IOService *native_hid_pen_allocate(void) {NativeHidInput *device=new NativeHidInput;if(device)device->setPenOnly();return device;}
extern "C" IOService *native_hid_touch_allocate(void) {NativeHidInput *device=new NativeHidInput;if(device)device->setTouchOnly();return device;}
