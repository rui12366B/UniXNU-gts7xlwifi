// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi touch: STM fts1ba90a at I2C 0x49 (live FDT /soc/i2c@994000/touchscreen@49, "stm,fts_touch").
//
//   bus        QUP SE5 pins GPIO 12 (SDA) and 13 (SCL), TLMM NORTH (pinctrl-kona.c PINGROUP(12/13, NORTH, qup5)), driven here
//              as a GPIO I2C master at about 100 kHz, as UniXNU-umi's fallback transport: no QUP clock, GPI or SMMU is touched
//   attention  GPIO 15, NORTH, active low (stm,irq_gpio <&tlmm 15 0x2008>), polled
//   power      AVDD PM8150 L13 3.3 V (high power mode), then DVDD L14 1.8 V (fts_power_ctrl), voted on RPMh ("ldoa13",
//              "ldoa14") through T970Rpmh.inc from UniXNU-umi-latest
//   protocol   Samsung's driver, pascua28/android_kernel_samsung_sm8250 159176c drivers/input/touchscreen/stm/fts1ba90a:
//              system reset FA 20 00 00 24 81, READY status event, 16-byte events read with 0x60 (one) and 0x61 (the rest),
//              coordinates x = b1 << 4 | b3 >> 4, y = b2 << 4 | (b3 & 15) on a 1751 x 2799 sensor (stm,max_coords)
//
// The finger set is published through the native_touch_* seqlock that ShengHidInput reads (x 0..30480 along the panel's long
// side, y 0..20320 along the short one). t970-touch-xform=<bits>: 1 swap the sensor axes (the default, the sensor is portrait
// and the GOP frame buffer landscape), 2 mirror x, 4 mirror y; the result is in frame buffer orientation.
// Started from native_legacy_display_late_probe on native-touch=1.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

// ---- platform shim for the RPMh code (UniXNU-umi package/touch/UmiTouchPlatform.h) ----------------------------------
#define STATIC static
#define CONST const
#define VOID void
#define BIT0 (1U<<0)
#define BIT1 (1U<<1)
#define BIT2 (1U<<2)
#define BIT3 (1U<<3)
#define BIT8 (1U<<8)
#define BIT16 (1U<<16)
#define BIT24 (1U<<24)
#define MAX_UINT32 UINT32_MAX
typedef uint8_t UINT8;typedef uint16_t UINT16;typedef uint32_t UINT32;typedef uint64_t UINT64;
typedef uintptr_t UINTN;typedef bool BOOLEAN;typedef int EFI_STATUS;
enum {EFI_SUCCESS=0,EFI_TIMEOUT=1,EFI_DEVICE_ERROR=2,EFI_INVALID_PARAMETER=3,EFI_UNSUPPORTED=4,
 EFI_NOT_FOUND=5,EFI_ACCESS_DENIED=6,EFI_COMPROMISED_DATA=7,EFI_NOT_READY=8,EFI_ALREADY_STARTED=9};
#define EFI_ERROR(x) ((x)!=EFI_SUCCESS)
#define DEBUG_WARN 0
static void touchDebug(unsigned,const char *format,...) __printflike(2,3);
#define DEBUG(x) touchDebug x
static uint32_t MmioRead32(uintptr_t);
static uint16_t MmioRead16(uintptr_t);
static void MmioWrite32(uintptr_t,uint32_t);
static uint64_t frequency;
static inline uint64_t GetPerformanceCounter(void) {uint64_t n;__asm__ volatile("isb\n\tmrs %0,cntpct_el0":"=r"(n));return n;}
static inline uint64_t GetTimeInNanoSecond(uint64_t delta) {return delta/frequency*1000000000ULL+(delta%frequency)*1000000000ULL/frequency;}
static inline void MicroSecondDelay(unsigned us) {
    if(us>50 || !frequency){IODelay(us);return;}
    uint64_t start=GetPerformanceCounter(),ticks=(frequency*us+999999)/1000000;
    while(GetPerformanceCounter()-start<ticks){}
}
static inline void MemoryFence(void) {__asm__ volatile("dsb sy":::"memory");}
static inline bool SaveAndDisableInterrupts(void) {return ml_set_interrupts_enabled(false);}
static inline void SetInterruptState(bool state) {ml_set_interrupts_enabled(state);}

extern "C" {
// The interface ShengHidInput and LegacyDisplayProbe read (strong definitions; LinkStubs.c keeps weak zeros for builds
// without this file).
volatile uint32_t native_touch_seq, native_touch_count;
volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
volatile uint32_t native_touch_fail_step;
volatile int native_touch_stage_result;
volatile unsigned native_touch_stage_value, native_touch_stage_total, native_touch_ready, native_touch_ready_ms;
char native_touch_stage_name[32];
volatile unsigned native_touch_irq_low, native_touch_irq_level;
volatile uint32_t native_touch_frames, native_touch_valid;
volatile int native_touch_last_x, native_touch_last_y;
volatile unsigned native_touch_app_id, native_touch_hdr0, native_touch_hdr1, native_touch_hdr2, native_touch_hdr3;
volatile unsigned native_touch_data0, native_touch_data1, native_touch_data2;
}

namespace {
struct Region {uintptr_t physical;unsigned length;IOMemoryMap *mapping;};
// TLMM NORTH GPIO 12..15, the AOSS IMEM word that points at the command DB, the command DB, APPS RSC.
Region regions[]={{0x0f90c000,0x4000,nullptr},{0x0c3f0000,0x1000,nullptr},{0x80860000,0x20000,nullptr},
                  {0x18220000,0x4000,nullptr}};
void (*gSink)(void *)=nullptr;
void *gSinkContext=nullptr;
}

static uintptr_t virtualAddress(uintptr_t physical,unsigned bytes) {
    for(auto &r:regions)if(r.mapping && physical>=r.physical && physical-r.physical<=r.length-bytes)
        return r.mapping->getVirtualAddress()+physical-r.physical;
    panic("T970 touch: unmapped MMIO %lx",(unsigned long)physical);
}
static uint32_t MmioRead32(uintptr_t address) {
    uint32_t value;uintptr_t mapped=virtualAddress(address,4);
    __asm__ volatile("ldr %w0,[%1]":"=&r"(value):"r"(mapped):"memory");return value;
}
static uint16_t MmioRead16(uintptr_t address) {return (uint16_t)(MmioRead32(address&~(uintptr_t)3)>>((address&2)*8));}
static void MmioWrite32(uintptr_t address,uint32_t value) {
    uintptr_t mapped=virtualAddress(address,4);
    __asm__ volatile("dsb sy\n\tstr %w0,[%1]\n\tdsb sy"::"r"(value),"r"(mapped):"memory");
}
// IOLog can sleep and asserts on an IRQ-masked caller, and the RPMh vote runs with interrupts masked (v34 hung in the
// first vote's first DEBUG line). Lines are kept here and printed once interrupts are on again, as umi's UmiTouchLog does.
static char gDebugLines[16][160];
static unsigned gDebugCount,gDebugDropped;
static void touchFlush(void) {
    if(!ml_get_interrupts_enabled() || ml_at_interrupt_context())return;
    for(unsigned i=0;i<gDebugCount;++i)IOLog("%s",gDebugLines[i]);
    if(gDebugDropped)IOLog("T970 touch: %u debug lines dropped\n",gDebugDropped);
    gDebugCount=0;gDebugDropped=0;
}
static void touchDebug(unsigned,const char *format,...) {
    if(gDebugCount==16){++gDebugDropped;return;}
    va_list a;va_start(a,format);vsnprintf(gDebugLines[gDebugCount++],sizeof gDebugLines[0],format,a);va_end(a);
    touchFlush();
}

namespace rpmh {
#include "T970Rpmh.inc"
}

// gts7xlwifi 2026-11-05: one lock for every writer of APPS RSC DRV2 (0x18220000): this file's votes, t970_rpmh_write (Wi-Fi, USB) and
// GpuProbe.cpp's DDR vote. Each picks an idle TCS on its own, so two at once could fill the same TCS; periph9 died at 18 s twice with
// AOP_ERR_FATAL / TZBSP_ERR_FATAL_RPM_ERR, the GPU's DDR vote and the touch/Wi-Fi rail votes falling in the same seconds.
static volatile uint32_t gRpmhBusy;
extern "C" void t970_rpmh_lock(void) {
    for (uint32_t idle = 0; !__atomic_compare_exchange_n(&gRpmhBusy, &idle, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED); idle = 0) IOSleep(1);
}
extern "C" void t970_rpmh_unlock(void) {__atomic_store_n(&gRpmhBusy, 0u, __ATOMIC_RELEASE);}
static int lockedRpmhWrite(const char *name, uint32_t word, uint32_t value) {
    t970_rpmh_lock();
    const int rc = rpmh::T970RpmhWrite(name, word, value);
    t970_rpmh_unlock();
    return rc;
}

namespace {
// ---- GPIO I2C ---------------------------------------------------------------------------------------------------------
constexpr uintptr_t kTlmmNorth=0x0f900000;
constexpr unsigned kSda=12,kScl=13,kAttn=15,kAddr=0x49;
constexpr uint32_t kCtlPullMask=3,kCtlPullUp=3,kCtlFuncMask=0x3c,kCtlOe=1U<<9,kIoIn=1U<<0,kIoOut=1U<<1;
uintptr_t ctl(unsigned pin){return kTlmmNorth+pin*0x1000;}
uintptr_t io(unsigned pin){return ctl(pin)+4;}
// Released: input with the pad's pull-up (the board also pulls the bus up); driven: output low.
void release(unsigned pin){MmioWrite32(ctl(pin),MmioRead32(ctl(pin))&~kCtlOe);}
void drive(unsigned pin){MmioWrite32(io(pin),MmioRead32(io(pin))&~kIoOut);MmioWrite32(ctl(pin),MmioRead32(ctl(pin))|kCtlOe);}
bool high(unsigned pin){return MmioRead32(io(pin))&kIoIn;}
constexpr unsigned kHalf=5;
unsigned gBusErrors,gNacks,gStretchTimeouts;
bool sclUp(){release(kScl);for(unsigned i=0;i<2000;++i){if(high(kScl))return true;MicroSecondDelay(1);}gStretchTimeouts++;return false;}
void configurePins(){
    const unsigned bus[2]={kSda,kScl};
    for(unsigned pin:bus){
        uint32_t c=MmioRead32(ctl(pin));
        c=(c&~(kCtlFuncMask|kCtlPullMask|kCtlOe))|kCtlPullUp;   // function 0 (GPIO), pull-up, input
        MmioWrite32(io(pin),MmioRead32(io(pin))&~kIoOut);
        MmioWrite32(ctl(pin),c);
    }
    uint32_t a=MmioRead32(ctl(kAttn));
    MmioWrite32(ctl(kAttn),(a&~(kCtlFuncMask|kCtlPullMask|kCtlOe))|kCtlPullUp);
}
void recover(){
    // A slave holding SDA low mid-byte releases it after at most nine clocks.
    for(unsigned i=0;i<9 && !high(kSda);++i){drive(kScl);MicroSecondDelay(kHalf);sclUp();MicroSecondDelay(kHalf);}
}
bool start(){
    release(kSda);if(!sclUp())return false;MicroSecondDelay(kHalf);
    if(!high(kSda)){recover();if(!high(kSda)){gBusErrors++;return false;}}
    drive(kSda);MicroSecondDelay(kHalf);drive(kScl);MicroSecondDelay(kHalf);return true;
}
void stop(){drive(kSda);MicroSecondDelay(kHalf);sclUp();MicroSecondDelay(kHalf);release(kSda);MicroSecondDelay(kHalf);}
bool writeByte(uint8_t b){
    for(int bit=7;bit>=0;--bit){
        if(b&(1U<<bit))release(kSda);else drive(kSda);
        MicroSecondDelay(kHalf);if(!sclUp())return false;MicroSecondDelay(kHalf);drive(kScl);
    }
    release(kSda);MicroSecondDelay(kHalf);if(!sclUp())return false;
    bool ack=!high(kSda);MicroSecondDelay(kHalf);drive(kScl);
    if(!ack)gNacks++;
    return ack;
}
uint8_t readByte(bool ack){
    uint8_t b=0;release(kSda);
    for(int bit=7;bit>=0;--bit){MicroSecondDelay(kHalf);sclUp();if(high(kSda))b|=1U<<bit;MicroSecondDelay(kHalf);drive(kScl);}
    if(ack)drive(kSda);else release(kSda);
    MicroSecondDelay(kHalf);sclUp();MicroSecondDelay(kHalf);drive(kScl);release(kSda);
    return b;
}
bool i2cWrite(const uint8_t *w,unsigned wn){
    bool ok=start() && writeByte(kAddr<<1);
    for(unsigned i=0;ok && i<wn;++i)ok=writeByte(w[i]);
    stop();return ok;
}
// Write then read with a repeated start, as fts_read_reg's two-message transfer.
bool i2cWriteRead(const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn){
    bool ok=start() && writeByte(kAddr<<1);
    for(unsigned i=0;ok && i<wn;++i)ok=writeByte(w[i]);
    if(ok){release(kSda);MicroSecondDelay(kHalf);ok=sclUp();MicroSecondDelay(kHalf);}
    ok=ok && start() && writeByte((kAddr<<1)|1);
    for(unsigned i=0;ok && i<rn;++i)r[i]=readByte(i+1<rn);
    stop();return ok;
}

// ---- fts1ba90a --------------------------------------------------------------------------------------------------------
constexpr unsigned kEvent=16,kFifoMax=32,kFingers=10,kSensorX=1752,kSensorY=2800;
uint8_t gEvents[kFifoMax*kEvent];
struct Finger {bool down;int x,y;};
Finger gFinger[kFingers];
unsigned gXform=1;
uint64_t gEpoch;
unsigned msSince(uint64_t t){return (unsigned)(GetTimeInNanoSecond(GetPerformanceCounter()-t)/1000000);}

void stage(const char *name,unsigned value,int result){
    strlcpy(native_touch_stage_name,name,sizeof native_touch_stage_name);
    native_touch_stage_value=value;native_touch_stage_result=result;native_touch_stage_total=native_touch_stage_total+1;
    touchFlush();
    IOLog("T970 touch: %s value=%x result=%d nacks=%u bus=%u stretch=%u t=%ums\n",name,value,result,gNacks,gBusErrors,
          gStretchTimeouts,msSince(gEpoch));
}
bool readEvent(uint8_t cmd,uint8_t *out,unsigned n){return i2cWriteRead(&cmd,1,out,n);}
// fts_wait_for_ready: a status event of type INFORMATION (2) with status id READY (0).
bool waitReady(unsigned tries){
    for(unsigned i=0;i<tries;++i){
        uint8_t e[kEvent]={};
        if(!readEvent(0x60,e,kEvent)){IOSleep(5);continue;}
        native_touch_hdr0=e[0];native_touch_hdr1=e[1];native_touch_hdr2=e[2];native_touch_hdr3=e[3];
        if((e[0]&3)==1 && ((e[0]>>2)&15)==2 && e[1]==0)return true;
        if(e[0]==0xf3)IOLog("T970 touch: error event %02x %02x %02x %02x\n",e[0],e[1],e[2],e[3]);
        if(!e[0])IOSleep(5);
    }
    return false;
}
// fts_fw_wait_for_echo_event: the command, then a STATUS_REPORT (0x43) echo 01 <cmd...>.
bool command(const uint8_t *c,unsigned n){
    if(!i2cWrite(c,n))return false;
    for(unsigned i=0;i<100;++i){
        uint8_t e[kEvent]={};
        if(readEvent(0x60,e,kEvent) && e[0]==0x43 && e[1]==0x01){
            bool match=true;for(unsigned k=0;k<n && k<4;++k)match=match && e[2+k]==c[k];
            if(match)return true;
        }
        IOSleep(2);
    }
    return false;
}

void publish(){
    int ids[kFingers],xs[kFingers],ys[kFingers];unsigned count=0;
    for(unsigned id=0;id<kFingers;++id){
        if(!gFinger[id].down)continue;
        // Frame buffer orientation: the sensor is portrait (x 0..1751, y 0..2799), the GOP mode 2800 x 1752.
        int sx=gFinger[id].x,sy=gFinger[id].y,fx,fy;
        if(gXform&1){fx=sy;fy=sx;}else{fx=sx*2800/kSensorX;fy=sy*1752/kSensorY;}
        if(gXform&2)fx=2799-fx;
        if(gXform&4)fy=1751-fy;
        if(fx<0)fx=0;if(fx>2799)fx=2799;if(fy<0)fy=0;if(fy>1751)fy=1751;
        ids[count]=(int)id;xs[count]=fx*30480/2800;ys[count]=fy*20320/1752;
        native_touch_last_x=fx;native_touch_last_y=fy;
        ++count;
    }
    native_touch_seq=native_touch_seq+1;__asm__ volatile("dmb ish":::"memory");
    for(unsigned i=0;i<count;++i){native_touch_ids[i]=ids[i];native_touch_xs[i]=xs[i];native_touch_ys[i]=ys[i];}
    native_touch_count=count;
    __asm__ volatile("dmb ish":::"memory");native_touch_seq=native_touch_seq+1;
    native_touch_frames=native_touch_frames+1;
    void (*sink)(void *)=__atomic_load_n(&gSink,__ATOMIC_ACQUIRE);
    if(sink)sink(gSinkContext);
}

void handleEvents(){
    if(!readEvent(0x60,gEvents,kEvent))return;
    unsigned left=gEvents[7]&0x3f;if(left>=kFifoMax)left=kFifoMax-1;
    if(left && !readEvent(0x61,gEvents+kEvent,kEvent*left))left=0;
    bool changed=false;
    for(unsigned n=0;n<=left;++n){
        const uint8_t *e=gEvents+n*kEvent;
        if((e[0]&3)!=0)continue;                     // not a coordinate event
        unsigned id=(e[0]>>2)&15,action=e[0]>>6,type=((e[6]>>6)<<2)|(e[7]>>6);
        if(id>=kFingers)continue;
        if(type!=0 && type!=5 && type!=6 && type!=3)continue;   // normal, palm, wet, glove (fts_event_handler_type_b)
        native_touch_valid=native_touch_valid+1;
        native_touch_data0=e[1];native_touch_data1=e[2];native_touch_data2=e[3];
        if(action==3){gFinger[id].down=false;}
        else if(action==1 || action==2){
            gFinger[id].down=true;
            gFinger[id].x=(e[1]<<4)|(e[3]>>4);
            gFinger[id].y=(e[2]<<4)|(e[3]&15);
        }
        changed=true;
    }
    if(changed)publish();
}

bool bringUp(){
    gEpoch=GetPerformanceCounter();
    stage("worker",0,0);
    for(auto &r:regions){
        IODeviceMemory *mem=IODeviceMemory::withRange(r.physical,r.length);
        r.mapping=mem?mem->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        if(mem)mem->release();
        if(!r.mapping){native_touch_fail_step=1;stage("map",(unsigned)r.physical,-1);return false;}
    }
    stage("mapped",0,0);
    // fts_power_ctrl: AVDD (L13, 100 mA load: high power mode), 1 ms, DVDD (L14), 5 ms.
    // Each vote is marked before it starts, so a vote that never returns names itself in the log.
    stage("avdd-volt",3300,1);
    int rc=lockedRpmhWrite("ldoa13",0,3300);
    if(!rc){stage("avdd-mode",7,1);rc=lockedRpmhWrite("ldoa13",8,7);}
    if(!rc){stage("avdd-on",1,1);rc=lockedRpmhWrite("ldoa13",4,1);}
    stage("avdd",3300,rc);
    if(rc){native_touch_fail_step=2;return false;}
    IOSleep(1);
    stage("dvdd-volt",1800,1);
    rc=lockedRpmhWrite("ldoa14",0,1800);
    if(!rc)rc=lockedRpmhWrite("ldoa14",4,1);
    stage("dvdd",1800,rc);
    if(rc){native_touch_fail_step=3;return false;}
    stage("pins-begin",0,1);
    configurePins();
    IOSleep(20);
    stage("pins",(high(kSda)?1:0)|(high(kScl)?2:0)|(high(kAttn)?4:0),0);
    // fts_systemreset.
    stage("reset-begin",0,1);
    const uint8_t reset[6]={0xfa,0x20,0x00,0x00,0x24,0x81};
    bool ok=i2cWrite(reset,6);
    stage("reset",ok,ok?0:-1);
    if(!ok){native_touch_fail_step=4;return false;}
    IOSleep(30);
    ok=waitReady(200);
    stage("ready",native_touch_hdr0|(native_touch_hdr1<<8),ok?0:-1);
    if(!ok){native_touch_fail_step=5;return false;}
    native_touch_ready=1;native_touch_ready_ms=msSince(gEpoch);
    uint8_t chip[kEvent]={};
    const uint8_t id[1]={0x22};
    if(i2cWriteRead(id,1,chip,3)){native_touch_app_id=(chip[0]<<16)|(chip[1]<<8)|chip[2];}
    stage("chip-id",native_touch_app_id,0);
    // fts_init tail: touch functions (touch|palm|wet), force calibration, clear events, scan mode MS/SS.
    const uint8_t functions[3]={0x30,0x61,0x00};i2cWrite(functions,3);IOSleep(10);
    const uint8_t cal[1]={0x13};ok=command(cal,1);stage("calibrate",ok,ok?0:-1);
    const uint8_t clear[1]={0x62};command(clear,1);
    const uint8_t scan[3]={0xa0,0x00,0x01};ok=command(scan,3);stage("scan",ok,ok?0:-1);
    if(!ok){native_touch_fail_step=6;return false;}
    return true;
}

void worker(void *,wait_result_t){
    thread_set_thread_name(current_thread(),"t970-touch");
    if(!bringUp())return;
    unsigned idle=0;
    for(;;){
        bool low=!high(kAttn);
        native_touch_irq_level=!low;
        if(low){native_touch_irq_low=native_touch_irq_low+1;handleEvents();idle=0;continue;}
        // Poll faster while a finger is down, slower when idle.
        IOSleep(++idle<200?4:16);
    }
}
}  // namespace

extern "C" void native_touch_set_sink(void (*sink)(void *),void *context) {
    gSinkContext=context;__atomic_store_n(&gSink,sink,__ATOMIC_RELEASE);
}

bool touchProbeStart(void) {
    static bool started;
    if(started)return true;
    uint64_t f;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(f));frequency=f;
    unsigned x=1;if(PE_parse_boot_argn("t970-touch-xform",&x,sizeof x))gXform=x&7;
    stage("probe",x,0);
    thread_t thread=nullptr;
    if(kernel_thread_start(worker,nullptr,&thread)!=KERN_SUCCESS){native_touch_fail_step=9;return false;}
    thread_deallocate(thread);started=true;return true;
}

// One RPMh VRM vote for other board probes (T970WifiProbe.cpp): word 0 voltage mV, 4 enable, 8 mode. The touch worker maps the command DB
// and APPS RSC DRV2 windows and keeps them; until it has, the call answers -200. Callers are serialised here; the touch worker's own votes
// run once at start-up, before any other caller (the Wi-Fi probe waits 20 s).
extern "C" int t970_rpmh_write(const char *name, uint32_t word, uint32_t value) {
    for (unsigned i = 1; i < sizeof regions / sizeof regions[0]; ++i) if (!regions[i].mapping) return -200;
    return lockedRpmhWrite(name, word, value);
}
