// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi camera stage C6: the front camera as a CoreMediaIO camera extension, so AVFoundation (mediaserverd's
// BWFigCaptureDeviceVendor, which discovers CMIO extension devices like iPadOS 17's USB cameras) lists it as an external camera.
//
// Same shape as Apple's own CMIO camera daemons (macOS appleh13camerad, UVCAssistant): a launchd job subscribed to the event stream
// com.apple.cmio.registerassistantservice.system-extensions.matching (published by mediaserverd on iPadOS 17) with
// CMIOExtensionBundleIdentifier, a Mach service of the same name, and the CMIOExtension/CMIOExtensionMachServiceName key in the
// embedded Info.plist; +[CMIOExtensionProvider startServiceWithProvider:] does the rest. Frames come from the T970Camera service
// (package/T970CameraService.cpp, T970CameraAbi.h): packed RAW10 Bayer (GRBG, combo11a site means), debayered here on the CPU into
// IOSurface-backed 420v pixel buffers (gray-world white balance, gamma 2.2), 1280x720 (centre crop) or 1640x1232 (full field).
//
//   t970camerad [-s slot] [-n name] [-b rggb|grbg|gbrg|bggr] [-p seconds]
//
// -p: AVFoundation probe (camera stage C6b) that many seconds after start: list the cameras AVFoundation offers (log + [17]), then
// capture 6 s from this provider's own device through AVCaptureSession + AVCaptureVideoDataOutput (so mediaserverd starts our
// stream exactly as an app would), count the frames ([18]) and write the first one as a 320-wide BMP to
// /private/var/log/t970camerad-avf.bmp. Probe stage bits ([16]): 1 started, 2 listed, 4 our device listed, 8 input made,
// 0x10 session running, 0x20 first frame, 0x40 BMP written, 0x80 done; | (our device's type: 1 external, 2 built-in) << 8.
// [19]: camera authorization status before | after a request << 4 (0 not determined, 1 restricted, 2 denied, 3 authorized) |
// listing rounds << 8 | devices via devicesWithMediaType: << 16 | systemPreferredCamera set << 24 (C6c, main7b listed nothing).
//
// -s: which status word this instance reports ([t970 camera c5] row, fields d0/d1; scripts/build-t970camerad.py builds one binary per
// extension identity). Stage bits: 1 started, 2 kernel service opened, 4 provider service started, 8 device registered, 0x10 host
// connected (mediaserverd discovered it), 0x20 stream start requested, 0x40 kernel streaming, 0x80 first frame sent, 0x100 stream
// stopped, 0x200 host disconnected; | slot << 28. d2 = frames sent, d3 = last error (code & 0xffff | stage << 16 | slot << 28).
// Build: scripts/build-t970camerad.py (CoreMediaIO headers from the macOS SDK, linked against the iOS SDK's CoreMediaIO.tbd).
#include <os/availability.h>
#undef API_UNAVAILABLE
#define API_UNAVAILABLE(...)
#import <Foundation/Foundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <IOKit/IOKitLib.h>
#import <CoreMediaIO/CMIOExtension.h>
#import <AVFoundation/AVFoundation.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "T970CameraAbi.h"

#define BLACK 16   /* the 10-bit pedestal 64 as an MSB byte */

static unsigned gSlot;
static uint32_t gStage, gFramesSent;
static io_connect_t gConn;
static T970CameraControl *gCtl;
static const uint8_t *gFrames;
static uint64_t gFrameBytes;
static unsigned gOrder = 1;   /* index into kOrders: GRBG */
static const char *const kOrders[4] = {"rggb", "grbg", "gbrg", "bggr"};
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "t970camerad[%u]: ", gSlot);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

static void report(unsigned slot, uint32_t value) {
    if (!gConn) return;
    const uint64_t in[2] = {slot, value};
    (void)IOConnectCallScalarMethod(gConn, T970CameraReport, in, 2, NULL, NULL);
}
static void stage(uint32_t bit) {
    pthread_mutex_lock(&gLock);
    gStage |= bit;
    const uint32_t v = gStage | gSlot << 28;
    pthread_mutex_unlock(&gLock);
    report(gSlot & 1, v);
}
static void fail(uint32_t at, int code) {
    logf_("error %d at stage %#x", code, at);
    report(3, ((uint32_t)code & 0xffff) | (at & 0xfff) << 16 | gSlot << 28);
}

// The kernel service: opened when it appears (published after the camera's boot run), kept open for the process lifetime.
static BOOL openService(void) {
    if (gConn) return YES;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("T970Camera"));
    if (!svc) return NO;
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr) { fail(2, kr); return NO; }
    mach_vm_address_t ctl = 0, frm = 0;
    mach_vm_size_t ctlSize = 0, frmSize = 0;
    kr = IOConnectMapMemory64(conn, T970CameraMemoryControl, mach_task_self(), &ctl, &ctlSize, kIOMapAnywhere);
    if (!kr) kr = IOConnectMapMemory64(conn, T970CameraMemoryFrames, mach_task_self(), &frm, &frmSize,
                                       kIOMapAnywhere | kIOMapReadOnly | kIOMapWriteCombineCache);
    T970CameraControl *k = (T970CameraControl *)(uintptr_t)ctl;
    if (kr || k->magic != T970_CAMERA_MAGIC || k->version != T970_CAMERA_VERSION) {
        fail(2, kr ? kr : -1);
        IOServiceClose(conn);
        return NO;
    }
    gConn = conn; gCtl = k; gFrames = (const uint8_t *)(uintptr_t)frm; gFrameBytes = frmSize;
    k->reading = T970_CAMERA_NONE;
    stage(2);
    stage(0);   // re-send the bits gathered before the service existed
    logf_("kernel service open, frames %llu bytes", (unsigned long long)frmSize);
    return YES;
}

// ---- debayer ----
typedef struct {uint32_t width, height;} OutFormat;
static const OutFormat kFormats[2] = {{1280, 720}, {1640, 1232}};

typedef struct {
    double sum[3];   // black-subtracted R, G, B of the previous frame (gray world)
    uint8_t lut[3][256];
    BOOL haveLut;
} Tone;

static void buildLut(Tone *t) {
    double gain[3] = {1, 1, 1};
    if (t->sum[0] > 1 && t->sum[1] > 1 && t->sum[2] > 1) {
        gain[0] = t->sum[1] / t->sum[0];
        gain[2] = t->sum[1] / t->sum[2];
        for (int i = 0; i < 3; i += 2) if (gain[i] > 4) gain[i] = 4;
    }
    for (int ch = 0; ch < 3; ++ch)
        for (int v = 0; v < 256; ++v) {
            double lin = (v > BLACK ? v - BLACK : 0) * gain[ch] / (255.0 - BLACK);
            if (lin > 1) lin = 1;
            t->lut[ch][v] = (uint8_t)lrint(pow(lin, 1 / 2.2) * 255);
        }
    t->haveLut = YES;
}

// One output pixel per Bayer cell picked by nearest neighbour from a centre crop of the sensor field with the output's aspect.
static void convert(const uint8_t *raw, uint32_t stride, uint32_t sw, uint32_t sh, CVPixelBufferRef pb, Tone *tone) {
    const uint32_t ow = (uint32_t)CVPixelBufferGetWidth(pb), oh = (uint32_t)CVPixelBufferGetHeight(pb);
    uint32_t cw = sw & ~1u, ch = (uint32_t)((uint64_t)cw * oh / ow) & ~1u;
    if (ch > sh) { ch = sh & ~1u; cw = (uint32_t)((uint64_t)ch * ow / oh) & ~1u; }
    const uint32_t x0 = ((sw - cw) / 2) & ~1u, y0 = ((sh - ch) / 2) & ~1u;
    static uint32_t col[2048];
    static uint8_t line[2][8192];
    if (ow > 2048 || stride > sizeof line[0]) return;
    for (uint32_t x = 0; x < ow; ++x) col[x] = (x0 + (uint32_t)((uint64_t)x * cw / ow)) & ~1u;
    static const unsigned rSite[4] = {0, 1, 2, 3}, bSite[4] = {3, 2, 1, 0};
    const unsigned rs = rSite[gOrder], bs = bSite[gOrder];
    if (!tone->haveLut) buildLut(tone);
    double sum[3] = {0, 0, 0};
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t *yp = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *cp = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), cs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    for (uint32_t y = 0; y < oh; ++y) {
        const uint32_t sy = (y0 + (uint32_t)((uint64_t)y * ch / oh)) & ~1u;
        // two sensor lines into cached memory with wide copies (the frame mapping is write-combined: no byte loads from it)
        memcpy(line[0], raw + (size_t)sy * stride, stride);
        memcpy(line[1], raw + (size_t)(sy + 1) * stride, stride);
        uint8_t *yo = yp + y * ys, *co = cp + (y / 2) * cs;
        for (uint32_t x = 0; x < ow; ++x) {
            const uint32_t sx = col[x], a = (sx / 4) * 5 + sx % 4, b = ((sx + 1) / 4) * 5 + (sx + 1) % 4;
            const uint32_t c[4] = {line[0][a], line[0][b], line[1][a], line[1][b]};
            const uint32_t r = c[rs], bl = c[bs], g = (c[0] + c[1] + c[2] + c[3] - r - bl) / 2;
            if (!(x & 7) && !(y & 7)) {
                sum[0] += r > BLACK ? r - BLACK : 0; sum[1] += g > BLACK ? g - BLACK : 0; sum[2] += bl > BLACK ? bl - BLACK : 0;
            }
            const int R = tone->lut[0][r], G = tone->lut[1][g], B = tone->lut[2][bl];
            yo[x] = (uint8_t)(16 + ((66 * R + 129 * G + 25 * B + 128) >> 8));
            if (!(y & 1) && !(x & 1)) {
                co[x] = (uint8_t)(128 + ((-38 * R - 74 * G + 112 * B + 128) >> 8));
                co[x + 1] = (uint8_t)(128 + ((112 * R - 94 * G - 18 * B + 128) >> 8));
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    memcpy(tone->sum, sum, sizeof sum);
    buildLut(tone);
}

// ---- CoreMediaIO sources ----
@interface T970StreamSource : NSObject <CMIOExtensionStreamSource>
@property(nonatomic, strong) CMIOExtensionStream *stream;
@property(atomic, strong) NSArray<CMIOExtensionStreamFormat *> *formats;
@property(nonatomic) NSUInteger activeFormatIndex;
@end

@interface T970DeviceSource : NSObject <CMIOExtensionDeviceSource>
@property(nonatomic, strong) CMIOExtensionDevice *device;
@property(nonatomic, strong) T970StreamSource *streamSource;
@end

@interface T970ProviderSource : NSObject <CMIOExtensionProviderSource>
@property(nonatomic, strong) CMIOExtensionProvider *provider;
@property(nonatomic, strong) T970DeviceSource *deviceSource;
@end

@implementation T970StreamSource {
    volatile BOOL _running;
    pthread_t _thread;
    CVPixelBufferPoolRef _pool;
    CMVideoFormatDescriptionRef _fd;
}

- (NSSet<CMIOExtensionProperty> *)availableProperties {
    return [NSSet setWithObjects:CMIOExtensionPropertyStreamActiveFormatIndex, CMIOExtensionPropertyStreamFrameDuration, nil];
}
- (CMIOExtensionStreamProperties *)streamPropertiesForProperties:(NSSet<CMIOExtensionProperty> *)properties error:(NSError **)error {
    CMIOExtensionStreamProperties *p = [CMIOExtensionStreamProperties streamPropertiesWithDictionary:@{}];
    if ([properties containsObject:CMIOExtensionPropertyStreamActiveFormatIndex]) p.activeFormatIndex = @(self.activeFormatIndex);
    if ([properties containsObject:CMIOExtensionPropertyStreamFrameDuration]) {
        CMTime d = CMTimeMake(1, 15);
        p.frameDuration = CFBridgingRelease(CMTimeCopyAsDictionary(d, kCFAllocatorDefault));
    }
    return p;
}
- (BOOL)setStreamProperties:(CMIOExtensionStreamProperties *)p error:(NSError **)error {
    if (p.activeFormatIndex && p.activeFormatIndex.unsignedIntegerValue < self.formats.count)
        self.activeFormatIndex = p.activeFormatIndex.unsignedIntegerValue;
    return YES;
}
- (BOOL)authorizedToStartStreamForClient:(CMIOExtensionClient *)client {
    return YES;
}

static void *captureThread(void *arg);

- (BOOL)startStreamAndReturnError:(NSError **)error {
    stage(0x20);
    if (!openService()) {
        if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain code:kIOReturnNotReady userInfo:nil];
        return NO;
    }
    if (_running) return YES;
    const OutFormat f = kFormats[self.activeFormatIndex < 2 ? self.activeFormatIndex : 0];
    if (_pool) { CVPixelBufferPoolRelease(_pool); _pool = NULL; }
    if (_fd) { CFRelease(_fd); _fd = NULL; }
    NSDictionary *attrs = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
                            (id)kCVPixelBufferWidthKey: @(f.width), (id)kCVPixelBufferHeightKey: @(f.height),
                            (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
    CVReturn cr = CVPixelBufferPoolCreate(kCFAllocatorDefault, (__bridge CFDictionaryRef)@{(id)kCVPixelBufferPoolMinimumBufferCountKey: @4},
                                          (__bridge CFDictionaryRef)attrs, &_pool);
    if (cr) { fail(0x20, cr); return NO; }
    const kern_return_t kr = IOConnectCallScalarMethod(gConn, T970CameraStart, NULL, 0, NULL, NULL);
    if (kr) {
        fail(0x20, kr);
        if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain code:kr userInfo:nil];
        return NO;
    }
    _running = YES;
    if (pthread_create(&_thread, NULL, captureThread, (__bridge void *)self)) { _running = NO; fail(0x20, -2); return NO; }
    logf_("stream start %ux%u", f.width, f.height);
    return YES;
}
- (BOOL)stopStreamAndReturnError:(NSError **)error {
    if (_running) {
        _running = NO;
        pthread_join(_thread, NULL);
    }
    if (gConn) (void)IOConnectCallScalarMethod(gConn, T970CameraStop, NULL, 0, NULL, NULL);
    stage(0x100);
    logf_("stream stop, %u frames sent", gFramesSent);
    return YES;
}

- (void)runCapture {
    T970CameraControl *k = gCtl;
    for (int i = 0; i < 1500 && _running && k->state != T970CameraStreaming; ++i) {
        if (k->state == T970CameraFailed) { fail(0x40, k->error); _running = NO; return; }
        usleep(10000);
    }
    if (k->state != T970CameraStreaming) { fail(0x40, -3); _running = NO; return; }
    stage(0x40);
    const uint32_t sw = k->width, sh = k->height, stride = k->stride;
    if ((uint64_t)k->buffer_offset[k->buffers - 1] + (uint64_t)stride * sh > gFrameBytes) { fail(0x40, -4); _running = NO; return; }
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    Tone tone = {0};
    uint32_t last = k->seq;
    while (_running) {
        if (k->state != T970CameraStreaming) { fail(0x40, -5); break; }
        if (k->seq == last) { usleep(3000); continue; }
        // take the newest frame (T970CameraAbi.h protocol): seq, then latest, mark reading, re-check
        const uint32_t s = k->seq;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        const uint32_t idx = k->latest;
        if (idx >= k->buffers) continue;
        k->reading = idx;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (k->seq != s || k->writing == idx) { k->reading = T970_CAMERA_NONE; continue; }
        last = s;
        const uint64_t when = k->timestamp;
        CVPixelBufferRef pb = NULL;
        if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, _pool, &pb) || !pb) {
            k->reading = T970_CAMERA_NONE;
            continue;
        }
        convert(gFrames + k->buffer_offset[idx], stride, sw, sh, pb, &tone);
        k->reading = T970_CAMERA_NONE;
        if (!_fd) CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pb, &_fd);
        CMSampleTimingInfo timing = {CMTimeMake(1, 13), CMClockMakeHostTimeFromSystemUnits(when), kCMTimeInvalid};
        CMSampleBufferRef sb = NULL;
        OSStatus st = CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, pb, true, NULL, NULL, _fd, &timing, &sb);
        CVPixelBufferRelease(pb);
        if (st || !sb) { fail(0x80, st); continue; }
        const uint64_t ns = when * tb.numer / tb.denom;
        [self.stream sendSampleBuffer:sb discontinuity:CMIOExtensionStreamDiscontinuityFlagNone hostTimeInNanoseconds:ns];
        CFRelease(sb);
        ++gFramesSent;
        if (gFramesSent == 1) stage(0x80);
        if (gFramesSent == 1 || !(gFramesSent % 64)) report(2, gFramesSent);
    }
    report(2, gFramesSent);
}
static void *captureThread(void *arg) {
    pthread_setname_np("t970camerad.capture");
    @autoreleasepool { [(__bridge T970StreamSource *)arg runCapture]; }
    return NULL;
}
@end

@implementation T970DeviceSource
- (NSSet<CMIOExtensionProperty> *)availableProperties {
    return [NSSet setWithObjects:CMIOExtensionPropertyDeviceTransportType, CMIOExtensionPropertyDeviceModel, nil];
}
- (CMIOExtensionDeviceProperties *)devicePropertiesForProperties:(NSSet<CMIOExtensionProperty> *)properties error:(NSError **)error {
    CMIOExtensionDeviceProperties *p = [CMIOExtensionDeviceProperties devicePropertiesWithDictionary:@{}];
    if ([properties containsObject:CMIOExtensionPropertyDeviceTransportType]) p.transportType = @(0x75736220);   // 'usb ', as a UVC camera
    if ([properties containsObject:CMIOExtensionPropertyDeviceModel]) p.model = @"T970 Front Camera";
    return p;
}
- (BOOL)setDeviceProperties:(CMIOExtensionDeviceProperties *)p error:(NSError **)error {
    return YES;
}
@end

@implementation T970ProviderSource
- (NSSet<CMIOExtensionProperty> *)availableProperties {
    return [NSSet setWithObjects:CMIOExtensionPropertyProviderManufacturer, nil];
}
- (CMIOExtensionProviderProperties *)providerPropertiesForProperties:(NSSet<CMIOExtensionProperty> *)properties error:(NSError **)error {
    CMIOExtensionProviderProperties *p = [CMIOExtensionProviderProperties providerPropertiesWithDictionary:@{}];
    if ([properties containsObject:CMIOExtensionPropertyProviderManufacturer]) p.manufacturer = @"UniXNU";
    return p;
}
- (BOOL)setProviderProperties:(CMIOExtensionProviderProperties *)p error:(NSError **)error {
    return YES;
}
- (BOOL)connectClient:(CMIOExtensionClient *)client error:(NSError **)error {
    logf_("client connected: pid %d", client.pid);
    stage(0x10);
    return YES;
}
- (void)disconnectClient:(CMIOExtensionClient *)client {
    logf_("client disconnected: pid %d", client.pid);
    stage(0x200);
}
@end

static CMIOExtensionStreamFormat *makeFormat(OutFormat f) {
    CMVideoFormatDescriptionRef fd = NULL;
    CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, (int32_t)f.width,
                                   (int32_t)f.height, NULL, &fd);
    CMIOExtensionStreamFormat *format = [[CMIOExtensionStreamFormat alloc] initWithFormatDescription:fd maxFrameDuration:CMTimeMake(1, 5)
                                                                                   minFrameDuration:CMTimeMake(1, 15) validFrameDurations:nil];
    CFRelease(fd);
    return format;
}

// ---- C6b: the AVFoundation side, from inside the provider's own process ----
static uint32_t gProbeStage;
static void probeStage(uint32_t bit) { gProbeStage |= bit; report(4, gProbeStage); }

static void writeBmpBGRA(CVPixelBufferRef pb, const char *path) {
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const uint8_t *base = CVPixelBufferGetBaseAddress(pb);
    const size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), bpr = CVPixelBufferGetBytesPerRow(pb);
    const uint32_t ow = 320, oh = (uint32_t)(h * 320 / (w ? w : 1));
    FILE *f = base && oh ? fopen(path, "wb") : NULL;
    if (f) {
        const uint32_t row = (ow * 3 + 3) & ~3u, size = 54 + row * oh;
        uint8_t hdr[54] = {'B', 'M'};
        const uint32_t v[] = {size, 54, 40, ow, oh};
        memcpy(hdr + 2, &v[0], 4); memcpy(hdr + 10, &v[1], 4); memcpy(hdr + 14, &v[2], 4); memcpy(hdr + 18, &v[3], 4);
        memcpy(hdr + 22, &v[4], 4); hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, sizeof hdr, f);
        uint8_t line[320 * 3 + 4] = {0};
        for (uint32_t y = 0; y < oh; ++y) {
            const uint8_t *src = base + (size_t)((oh - 1 - y) * h / oh) * bpr;
            for (uint32_t x = 0; x < ow; ++x) memcpy(line + x * 3, src + (size_t)(x * w / ow) * 4, 3);   // BGRA -> BGR
            fwrite(line, 1, row, f);
        }
        fclose(f);
        probeStage(0x40);
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
}

@interface T970Probe : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(atomic) uint32_t frames;
@end
@implementation T970Probe
- (void)captureOutput:(AVCaptureOutput *)output didOutputSampleBuffer:(CMSampleBufferRef)sb fromConnection:(AVCaptureConnection *)c {
    const uint32_t n = ++self.frames;
    if (n == 1) {
        probeStage(0x20);
        CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
        if (pb) {
            logf_("probe: first frame %zux%zu format %.4s", CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb),
                  (const char *)&(uint32_t){CFSwapInt32HostToBig(CVPixelBufferGetPixelFormatType(pb))});
            writeBmpBGRA(pb, "/private/var/log/t970camerad-avf.bmp");
        }
    }
    if (n == 1 || !(n % 16)) report(6, n);
}
@end

static const char *typeName(AVCaptureDevice *d) {return d.deviceType.UTF8String ?: "?";}

static void runProbe(NSString *ourName) {
    probeStage(1);
    // TCC first: mediaserverd asks tccd about this process (main7b log) and may hide devices from an unauthorized client.
    const AVAuthorizationStatus before = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    __block AVAuthorizationStatus after = before;
    if (before == AVAuthorizationStatusNotDetermined) {
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted) {
            logf_("probe: requestAccess granted %d", granted);
            dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
        after = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    }
    logf_("probe: camera authorization %ld -> %ld", (long)before, (long)after);
    [[NSNotificationCenter defaultCenter] addObserverForName:AVCaptureDeviceWasConnectedNotification object:nil queue:nil
                                                  usingBlock:^(NSNotification *n) {
        AVCaptureDevice *d = n.object;
        logf_("probe: connected notification \"%s\" type %s", d.localizedName.UTF8String, typeName(d));
    }];
    NSMutableArray<AVCaptureDeviceType> *types = [NSMutableArray arrayWithObjects:AVCaptureDeviceTypeBuiltInWideAngleCamera,
                                                  AVCaptureDeviceTypeBuiltInUltraWideCamera, AVCaptureDeviceTypeBuiltInTelephotoCamera,
                                                  AVCaptureDeviceTypeBuiltInDualCamera, AVCaptureDeviceTypeBuiltInTrueDepthCamera, nil];
    if (@available(iOS 17.0, *)) [types addObject:AVCaptureDeviceTypeExternal];
    uint32_t total = 0, builtin = 0, external = 0, ours = 0, ourType = 0, rounds = 0, legacy = 0, preferred = 0;
    AVCaptureDevice *mine = nil;
    // mediaserverd may publish the external sources late: look up to six times, 5 s apart.
    for (; rounds < 6 && !mine; ++rounds) {
        if (rounds) sleep(5);
        AVCaptureDeviceDiscoverySession *ds = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                                                                     mediaType:AVMediaTypeVideo
                                                                                                      position:AVCaptureDevicePositionUnspecified];
        total = builtin = external = ours = 0;
        for (AVCaptureDevice *d in ds.devices) {
            ++total;
            BOOL ext = NO;
            if (@available(iOS 17.0, *)) ext = [d.deviceType isEqualToString:AVCaptureDeviceTypeExternal];
            if (ext) ++external; else ++builtin;
            logf_("probe[%u]: device \"%s\" type %s position %ld id %s connected %d", rounds, d.localizedName.UTF8String, typeName(d),
                  (long)d.position, d.uniqueID.UTF8String, d.connected);
            if ([d.localizedName isEqualToString:ourName]) { mine = d; ++ours; ourType = ext ? 1 : 2; }
        }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSArray<AVCaptureDevice *> *all = [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
#pragma clang diagnostic pop
        legacy = (uint32_t)all.count;
        for (AVCaptureDevice *d in all) {
            logf_("probe[%u]: devicesWithMediaType \"%s\" type %s", rounds, d.localizedName.UTF8String, typeName(d));
            if (!mine && [d.localizedName isEqualToString:ourName]) { mine = d; ++ours; ourType = 1; }
        }
        AVCaptureDevice *pref = nil;
        if (@available(iOS 17.0, *)) pref = AVCaptureDevice.systemPreferredCamera;
        preferred = pref != nil;
        logf_("probe[%u]: %u listed (%u built-in, %u external), legacy %u, systemPreferredCamera \"%s\", default \"%s\"", rounds, total,
              builtin, external, legacy, pref ? pref.localizedName.UTF8String : "none",
              [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo].localizedName.UTF8String ?: "none");
        report(7, (uint32_t)(before & 0xf) | (uint32_t)(after & 0xf) << 4 | (rounds + 1) << 8 | (legacy & 0xff) << 16 | preferred << 24);
    }
    report(5, (total & 0xff) | (builtin & 0xff) << 8 | (external & 0xff) << 16 | (ours & 0xff) << 24);
    probeStage(2 | ourType << 8);
    if (!mine) { probeStage(0x80); return; }
    probeStage(4);
    NSError *error = nil;
    AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:mine error:&error];
    if (!input) { logf_("probe: input error %s", error.description.UTF8String); fail(0x408, (int)error.code); probeStage(0x80); return; }
    probeStage(8);
    AVCaptureSession *session = [AVCaptureSession new];
    AVCaptureVideoDataOutput *output = [AVCaptureVideoDataOutput new];
    output.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
    T970Probe *probe = [T970Probe new];
    dispatch_queue_t q = dispatch_queue_create("t970camerad.probe.frames", DISPATCH_QUEUE_SERIAL);
    [output setSampleBufferDelegate:probe queue:q];
    [session beginConfiguration];
    if ([session canAddInput:input]) [session addInput:input];
    if ([session canAddOutput:output]) [session addOutput:output];
    [session commitConfiguration];
    [session startRunning];
    logf_("probe: session running %d", session.running);
    if (session.running) probeStage(0x10);
    sleep(6);
    [session stopRunning];
    report(6, probe.frames);
    logf_("probe: %u frames in 6 s", probe.frames);
    probeStage(0x80);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        const char *name = "T970 Front Camera";
        int ch;
        unsigned probeAfter = 0;
        while ((ch = getopt(argc, argv, "s:n:b:p:")) != -1) {
            if (ch == 'p') probeAfter = (unsigned)strtoul(optarg, NULL, 0);
            else if (ch == 's') gSlot = (unsigned)strtoul(optarg, NULL, 0) & 1;
            else if (ch == 'n') name = optarg;
            else if (ch == 'b') for (unsigned i = 0; i < 4; ++i) if (!strcmp(optarg, kOrders[i])) gOrder = i;
        }
        logf_("start (%s, bundle %s)", name, NSBundle.mainBundle.bundleIdentifier.UTF8String ?: "none");
        stage(1);
        (void)openService();
        // The service appears once the camera's boot run is over (t970-camera bit 512): keep looking for a while.
        dispatch_source_t retry = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        __block int tries = 0;
        dispatch_source_set_timer(retry, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 4);
        dispatch_source_set_event_handler(retry, ^{
            if (openService() || ++tries > 600) dispatch_source_cancel(retry);
        });
        dispatch_resume(retry);

        T970ProviderSource *ps = [T970ProviderSource new];
        ps.provider = [[CMIOExtensionProvider alloc] initWithSource:ps clientQueue:nil];
        T970DeviceSource *ds = [T970DeviceSource new];
        // Stable IDs per identity, so a camera chosen in an app stays chosen across boots.
        NSUUID *deviceID = [[NSUUID alloc] initWithUUIDString:gSlot ? @"6A3F2C10-7B1E-4C55-9E0A-970C0FFEE001" : @"6A3F2C10-7B1E-4C55-9E0A-970C0FFEE000"];
        NSUUID *streamID = [[NSUUID alloc] initWithUUIDString:gSlot ? @"6A3F2C10-7B1E-4C55-9E0A-970C0FFEE101" : @"6A3F2C10-7B1E-4C55-9E0A-970C0FFEE100"];
        ds.device = [[CMIOExtensionDevice alloc] initWithLocalizedName:@(name) deviceID:deviceID legacyDeviceID:nil source:ds];
        T970StreamSource *ss = [T970StreamSource new];
        ss.formats = @[makeFormat(kFormats[0]), makeFormat(kFormats[1])];
        ss.stream = [[CMIOExtensionStream alloc] initWithLocalizedName:[@(name) stringByAppendingString:@".video"] streamID:streamID
                                                         direction:CMIOExtensionStreamDirectionSource clockType:CMIOExtensionStreamClockTypeHostTime
                                                            source:ss];
        ds.streamSource = ss;
        ps.deviceSource = ds;
        NSError *error = nil;
        if (![ds.device addStream:ss.stream error:&error]) fail(8, (int)error.code);
        else if (![ps.provider addDevice:ds.device error:&error]) fail(8, (int)error.code);
        else stage(8);
        [CMIOExtensionProvider startServiceWithProvider:ps.provider];
        stage(4);
        logf_("provider service started");
        if (probeAfter) {
            NSString *ourName = @(name);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)probeAfter * NSEC_PER_SEC),
                           dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ @autoreleasepool { runProbe(ourName); } });
        }
        CFRunLoopRun();
    }
    return 0;
}

