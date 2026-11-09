// SPDX-License-Identifier: BSD-2-Clause-Patent
// The gts7xlwifi built-in speakers for the iOS audio server. UniXNU-sheng's hal/ShengAudioPlugIn.c (same iPadOS 17.7.11 audio server)
// with the input stream removed (no capture path on this board yet): one device with a stereo output stream (the four CS35L41
// amplifiers, left and right) and its volume control. Samples move through a ring shared with the kernel service T970Audio
// (package/T970AudioAbi.h, package/T970Audio.cpp); the service feeds the audio DSP. The original comment, for the parts kept:
// The sheng built-in audio device for the iOS audio server: one device with a stereo output stream (the six speakers) and a mono input
// stream (the microphones). Samples move through rings shared with the kernel service T970Audio (package/T970AudioAbi.h); the service
// feeds the audio DSP. Objects: the plug-in (1), the device (2), the output stream (3), the input stream (4), the output volume (5).
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <math.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <os/log.h>
#include "T970AudioAbi.h"

enum {kPlugIn = kAudioObjectPlugInObject, kDevice = 2, kOutStream = 3, kInStream = 4, kVolume = 5};
#define kPlugInBundleID "org.unixnu.t970.audio"
#define kDeviceUID "T970AudioDevice"
#define kModelUID "T970AudioModel"
static const Float64 kRate = T970_AUDIO_RATE;
// The time stamp period. It must exceed the safety offset plus an IO cycle, or the audio server never runs IO (i543: 8192, no writes).
static const UInt32 kZeroPeriod = T970_AUDIO_RING_FRAMES;
// The time stamps start at the frame IO started on, as in Apple's sample driver, not on a grid from boot: the audio server took the first
// stamp, up to a period old, for the start, and wrote that much late (least write-ahead -734 frames with a 16384 period, i539; about
// -9700 with 65536, i540 to i542). Sample times stay those of the shared timeline. Playback is smooth this way (i544); i545 went back to
// the grid with a 32768 period and SimpleIIR, and both directions stuttered.
static uint64_t gZeroOrigin, gZeroSeed;

static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint gRefs = 1;
static io_connect_t gConnect;
static T970AudioControl *gControl;
static int16_t *gOut;
static int16_t *gIn;   // the capture ring (mono), mapped only when the service captures (control in_channels)
static UInt32 gIoUsers;
static Float32 gVolume = 1.0f;   // the volume control's scalar (slider position)
static void publishVolume(void);
static Float64 gHostTicksPerFrame;
static void setHostClock(void);

// ---- Kernel service ------------------------------------------------------------------------------------------------------------
static int mapType(uint32_t type, void **out) {
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    if (IOConnectMapMemory64(gConnect, type, mach_task_self(), &address, &size, kIOMapAnywhere) != kIOReturnSuccess) return -1;
    *out = (void *)(uintptr_t)address;
    return 0;
}
static int connectService(void) {
    if (gControl) return 0;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(T970_AUDIO_CLASS));
    if (!service) return -1;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &gConnect);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) return -1;
    void *control = NULL, *out = NULL;
    if (mapType(T970AudioMemoryControl, &control) || mapType(T970AudioMemoryOutput, &out)) {
        IOServiceClose(gConnect); gConnect = 0; return -1;
    }
    T970AudioControl *c = control;
    if (c->magic != T970_AUDIO_MAGIC || c->version != T970_AUDIO_VERSION) {IOServiceClose(gConnect); gConnect = 0; return -1;}
    void *in = NULL;
    if (c->in_channels && !mapType(T970AudioMemoryInput, &in)) gIn = in;
    gOut = out; gControl = c;
    gHostTicksPerFrame = (Float64)c->ticks_per_frame_q32 / 4294967296.0;
    setHostClock();
    publishVolume();
    return 0;
}
static kern_return_t call(uint32_t selector) {return gConnect ? IOConnectCallScalarMethod(gConnect, selector, NULL, 0, NULL, NULL) : KERN_FAILURE;}

// CoreAudio's host clock frequency is a constant of 24 MHz (Apple's timer) that its IO clock and IO thread read directly; this board's timer
// runs at 19.2 MHz, so the audio server's IO cycles came 25 % late (i546), and each recording fell silent from about 1.0 to 1.8 s until its
// clock caught up (i548 to i551; gone with the constant set, i552). AudioGetHostClockFrequency loads it with adrp and ldr d0: the address
// is taken from those two instructions and the service sets the constant (on the shared page it cannot be written from here).
static void setHostClock(void) {
    T970AudioDiag *diag = (T970AudioDiag *)((uint8_t *)gControl + T970_AUDIO_DIAG_OFFSET);
    // dlsym signs the pointer for an arm64e caller: the code is read through the stripped address and called through the signed one
    // (an arm64 build sees the signature bits only in an arm64e server, and neither reads nor calls through them).
    void *const signed_symbol = dlsym(RTLD_DEFAULT, "AudioGetHostClockFrequency");
#if __has_feature(ptrauth_calls)
    const uintptr_t symbol = (uintptr_t)ptrauth_strip(signed_symbol, ptrauth_key_function_pointer);
    Float64 (*frequency)(void) = (Float64 (*)(void))signed_symbol;
#else
    const uintptr_t symbol = (uintptr_t)signed_symbol & (((uintptr_t)1 << 47) - 1);
    Float64 (*frequency)(void) = (Float64 (*)(void))symbol;
#endif
    if (!symbol) {diag->host_clock_result = -1; return;}
    const uint32_t *code = (const uint32_t *)symbol, adrp = code[0], ldr = code[1];
    if ((adrp & 0x9f000000u) != 0x90000000u || (ldr & 0xffc00000u) != 0xfd400000u || ((ldr >> 5) & 31) != (adrp & 31)) {
        diag->host_clock_result = -1;
        return;
    }
    int64_t page = (int64_t)(((adrp >> 5) & 0x7ffffu) << 2 | ((adrp >> 29) & 3u));
    if (page & (1 << 20)) page -= 1 << 21;
    const uint64_t address = ((uintptr_t)code & ~(uintptr_t)0xfff) + (uint64_t)(page << 12) + ((ldr >> 10) & 0xfffu) * 8;
    const kern_return_t r = IOConnectCallScalarMethod(gConnect, T970AudioSetHostClock, &address, 1, NULL, NULL);
    diag->host_clock_result = r;
    diag->host_clock = (uint32_t)frequency();
}

// ---- Helpers ---------------------------------------------------------------------------------------------------------------------
static AudioStreamBasicDescription format(UInt32 channels) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * channels; f.mFramesPerPacket = 1; f.mBytesPerFrame = 4 * channels;
    f.mChannelsPerFrame = channels; f.mBitsPerChannel = 32;
    return f;
}
#define REPLY(type, value) do { if (inDataSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; *(type *)outData = (value); *outDataSize = sizeof(type); return 0; } while (0)
#define REPLY_STR(text) REPLY(CFStringRef, CFSTR(text))
static UInt32 latencyFrames(void) {return gControl ? gControl->lead_frames : 2 * T970_AUDIO_PERIOD_FRAMES;}

// ---- IUnknown --------------------------------------------------------------------------------------------------------------------
static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out);
static ULONG AddRef(void *self) {(void)self; return atomic_fetch_add(&gRefs, 1) + 1;}
static ULONG Release(void *self) {(void)self; UInt32 r = atomic_load(&gRefs); if (r) r = atomic_fetch_sub(&gRefs, 1) - 1; return r;}

// Connects at once, so the service sees the audio server load the plug-in; a failure here is retried at the first StartIO.
static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host) {
    (void)d;
    gHost = host;
    pthread_mutex_lock(&gLock);
    const int connected = connectService();
    pthread_mutex_unlock(&gLock);
    os_log_error(OS_LOG_DEFAULT, "T970Audio: loaded, service %{public}s", connected ? "not connected" : "connected");
    if (gControl) {
        T970AudioDiag *diag = (T970AudioDiag *)((uint8_t *)gControl + T970_AUDIO_DIAG_OFFSET);
        diag->loads += 1;
        diag->volume_milli = (UInt32)(gVolume * 1000.0f);
    }
    return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *c, AudioObjectID *o) {(void)d; (void)desc; (void)c; (void)o; return kAudioHardwareUnsupportedOperationError;}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o) {(void)d; (void)o; return kAudioHardwareUnsupportedOperationError;}
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {(void)d; (void)o; (void)c; return 0;}
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {(void)d; (void)o; (void)c; return 0;}
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) {(void)d; (void)o; (void)a; (void)i; return 0;}
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) {(void)d; (void)o; (void)a; (void)i; return 0;}

// ---- Properties ------------------------------------------------------------------------------------------------------------------
static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a);
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *size);
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData);

static void trace(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 kind, OSStatus r) {
    if (!gControl) return;
    T970AudioTrace *tr = (T970AudioTrace *)((uint8_t *)gControl + T970_AUDIO_TRACE_OFFSET);
    const UInt32 info = (UInt32)o << 24 | (a->mScope >> 24) << 16 | kind | (r ? 2u : 0u);
    const UInt32 n = tr->count < T970_AUDIO_TRACE_SLOTS ? tr->count : T970_AUDIO_TRACE_SLOTS;
    for (UInt32 i = 0; i < n; ++i) if (tr->entry[i][0] == a->mSelector && tr->entry[i][1] == info) return;
    if (n < T970_AUDIO_TRACE_SLOTS) {tr->entry[n][0] = a->mSelector; tr->entry[n][1] = info; tr->count = n + 1;}
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    (void)d; (void)pid;
    *out = (o == kVolume && (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue)) ||
           ((o == kOutStream || o == kInStream) && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat ||
                                                     a->mSelector == kAudioStreamPropertyIsActive)) ||
           (o == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate);
    return HasProperty(d, o, pid, a) ? 0 : kAudioHardwareUnknownPropertyError;
}

// The output volume spans kMinDb..0 dB on a squared curve over the scalar (loud in the middle, fine steps near the bottom); scalar 0
// mutes. The audio server maps its user volume onto this range: a -96 dB floor made a third of full volume -56 dB (i532), a linear
// -40 dB range still -26 dB (i533, quiet); now it is -16 dB.
// gts7xlwifi: -60 dB (sheng's -36 dB left iOS's lowest step loud on these four boosted amplifiers, int13).
static const Float32 kMinDb = -60.0f;
static Float32 toDb(Float32 scalar) {const Float32 r = 1.0f - (scalar < 0 ? 0 : scalar > 1 ? 1 : scalar); return kMinDb * r * r;}
static Float32 toScalar(Float32 db) {return db <= kMinDb ? 0.0f : db >= 0 ? 1.0f : 1.0f - sqrtf(db / kMinDb);}
static Float32 gain(void) {return gVolume <= 0.0f ? 0.0f : powf(10.0f, toDb(gVolume) / 20.0f);}
// gts7xlwifi: the samples go to the ring at full scale (0 when muted) and the kernel sets the amplifiers' digital volume to toDb.
static Float32 ringGain(void) {return gVolume <= 0.0f ? 0.0f : 1.0f;}
static void publishVolume(void) {
    if (!gControl) return;
    gControl->out_volume_mdb = gVolume <= 0.0f ? T970_AUDIO_MUTE_MDB : (int32_t)lrintf(toDb(gVolume) * 1000.0f);
    __atomic_add_fetch(&gControl->out_volume_seq, 1u, __ATOMIC_RELEASE);
}

static OSStatus setProperty(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 size, const void *data) {
    if (o == kVolume && size >= sizeof(Float32)) {
        Float32 v = *(const Float32 *)data;
        const Float32 raw = v;
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue) v = toScalar(v);
        else if (a->mSelector != kAudioLevelControlPropertyScalarValue) return kAudioHardwareUnknownPropertyError;
        gVolume = v < 0 ? 0 : v > 1 ? 1 : v;
        if (gControl) {
            T970AudioDiag *diag = (T970AudioDiag *)((uint8_t *)gControl + T970_AUDIO_DIAG_OFFSET);
            diag->volume_milli = (UInt32)(gVolume * 1000.0f);
            diag->volume_sets += 1;
            const int32_t kind = a->mSelector == kAudioLevelControlPropertyDecibelValue ? 2 : 1;
            const int32_t milli = (int32_t)lrintf(raw * 1000.0f);
            diag->vol_kind = kind; diag->vol_raw_milli = milli; diag->vol_element = (int32_t)a->mElement;
            diag->gain_now = (uint32_t)lrintf(gain() * 10000.0f);
            publishVolume();
            diag->vol_history[diag->vol_history_count % 8] = (uint32_t)kind << 30 | (uint32_t)((milli + (1 << 29)) & 0x3fffffff);
            diag->vol_history_count += 1;
        }
        AudioObjectPropertyAddress changed[2] = {{kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
                                                 {kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}};
        gHost->PropertiesChanged(gHost, kVolume, 2, changed);
        return 0;
    }
    if ((o == kOutStream || o == kInStream) && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        const AudioStreamBasicDescription *f = data;
        return size >= sizeof *f && f->mSampleRate == kRate && f->mChannelsPerFrame == (o == kOutStream ? 2u : 1u) ? 0 : kAudioDeviceUnsupportedFormatError;
    }
    if (o == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate)
        return size >= sizeof(Float64) && *(const Float64 *)data == kRate ? 0 : kAudioDeviceUnsupportedFormatError;
    // The audio server turns streams it does not use off and on (i530); both streams stay running, so this only acknowledges it.
    if ((o == kOutStream || o == kInStream) && a->mSelector == kAudioStreamPropertyIsActive) return size >= sizeof(UInt32) ? 0 : kAudioHardwareBadPropertySizeError;
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 size, const void *data) {
    (void)d; (void)pid; (void)qs; (void)q;
    const OSStatus r = setProperty(o, a, size, data);
    trace(o, a, 1, r);
    return r;
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a) {
    UInt32 size = 0;
    return GetPropertyDataSize(d, o, pid, a, 0, NULL, &size) == 0;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *size) {
    UInt8 scratch[1024];
    UInt32 written = 0;
    OSStatus r = GetPropertyData(d, o, pid, a, qs, q, sizeof scratch, &written, scratch);
    if (r == 0) *size = written;
    return r;
}

static UInt32 objects(AudioObjectID o, AudioObjectPropertyScope scope, AudioObjectID *list) {
    UInt32 n = 0;
    if (o == kPlugIn) list[n++] = kDevice;
    else if (o == kDevice) {
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kOutStream;
        if (scope != kAudioObjectPropertyScopeOutput && gIn) list[n++] = kInStream;   // the microphone, when the service captures
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kVolume;
    }
    return n;
}

static OSStatus propertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 inDataSize,
                             UInt32 *outDataSize, void *outData) {
    const AudioObjectPropertySelector s = a->mSelector;
    // Lists every object answers, empty here: custom properties ('cust'), and on the plug-in its boxes and clock devices. The audio
    // server's device manager reads them for each new device and drops a device whose answer is an unknown-property error (i525).
    if (o <= kVolume && (s == kAudioObjectPropertyCustomPropertyInfoList || ((s == kAudioPlugInPropertyBoxList ||
        s == kAudioPlugInPropertyClockDeviceList) && o == kPlugIn) || ((s == 'taps') && (o == kOutStream || o == kInStream)))) {
        *outDataSize = 0;
        return 0;
    }
    switch (o) {
    case kPlugIn:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            if (inDataSize < sizeof(AudioObjectID)) {*outDataSize = 0; return 0;}
            ((AudioObjectID *)outData)[0] = kDevice; *outDataSize = sizeof(AudioObjectID); return 0;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef uid = qs >= sizeof(CFStringRef) ? *(const CFStringRef *)q : NULL;
            REPLY(AudioObjectID, uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo ? kDevice : kAudioObjectUnknown);
        }
        case kAudioPlugInPropertyResourceBundle: REPLY_STR("");
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kDevice:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName: REPLY_STR("Speaker");
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioDevicePropertyDeviceUID: REPLY_STR(kDeviceUID);
        case kAudioDevicePropertyModelUID: REPLY_STR(kModelUID);
        case kAudioObjectPropertyModelName: REPLY_STR("Speaker");
        case kAudioObjectPropertySerialNumber: REPLY_STR("0001");
        case kAudioObjectPropertyFirmwareVersion: REPLY_STR("1.0");
        case kAudioObjectPropertyIdentify: REPLY(UInt32, 0);
        // Private device properties the audio server reads for each new device (i526: 'accs' and 'auto' were its last unanswered ones).
        case 'accs': REPLY(UInt32, 0);
        case 'auto': REPLY(UInt32, 0);
        case kAudioObjectPropertyElementName: REPLY_STR("");
        case kAudioDevicePropertyConfigurationApplication: REPLY_STR("com.apple.Preferences");
        // Thunderbolt: the audio server's routing layer makes a device of this transport a driver device with one output and one input
        // port of its own. Built-in devices are only used through Apple's port descriptions, and a USB device must carry a USB location
        // ('lcID') that the HAL does not take from a plug-in (i529).
        case kAudioDevicePropertyTransportType: REPLY(UInt32, kAudioDeviceTransportTypeThunderbolt);
        case kAudioDevicePropertyRelatedDevices: REPLY(AudioObjectID, kDevice);
        case kAudioDevicePropertyClockDomain: REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: REPLY(UInt32, gIoUsers > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REPLY(UInt32, 1);
        case kAudioDevicePropertyLatency: REPLY(UInt32, latencyFrames());
        // The service takes a period lead_frames before it plays (its own queue and the DSP's); the service measures and publishes it, so
        // the audio server writes each frame at least that early.
        case kAudioDevicePropertySafetyOffset: REPLY(UInt32, latencyFrames() + 2 * T970_AUDIO_PERIOD_FRAMES);
        case kAudioDevicePropertyNominalSampleRate: REPLY(Float64, kRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: REPLY(AudioValueRange, ((AudioValueRange){kRate, kRate}));
        case kAudioDevicePropertyIsHidden: REPLY(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: REPLY(UInt32, kZeroPeriod);
        case kAudioDevicePropertyClockIsStable: REPLY(UInt32, 1);
        // Raw: the zero time stamps are exact (one host clock), so smoothing only delays the audio server's view of them.
        case kAudioDevicePropertyClockAlgorithm: REPLY(UInt32, kAudioDeviceClockAlgorithmRaw);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)outData)[0] = 1; ((UInt32 *)outData)[1] = a->mScope == kAudioObjectPropertyScopeInput ? 1 : 2;
            *outDataSize = 2 * sizeof(UInt32); return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 channels = a->mScope == kAudioObjectPropertyScopeInput ? 1 : 2;
            const UInt32 bytes = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + channels * sizeof(AudioChannelDescription));
            if (inDataSize < bytes) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *l = outData;
            memset(l, 0, bytes);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = channels;
            l->mChannelDescriptions[0].mChannelLabel = channels == 1 ? kAudioChannelLabel_Mono : kAudioChannelLabel_Left;
            if (channels == 2) l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            *outDataSize = bytes; return 0;
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList: {
            AudioObjectID list[4];
            UInt32 n = objects(kDevice, a->mScope, list), out = 0;
            for (UInt32 i = 0; i < n; ++i) {
                if (s == kAudioDevicePropertyStreams && list[i] == kVolume) continue;
                if (s == kAudioObjectPropertyControlList && list[i] != kVolume) continue;
                if ((out + 1) * sizeof(AudioObjectID) > inDataSize) break;
                ((AudioObjectID *)outData)[out++] = list[i];
            }
            *outDataSize = out * (UInt32)sizeof(AudioObjectID); return 0;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kInStream:
        if (!gIn) return kAudioHardwareBadObjectError;
        /* fall through */
    case kOutStream: {
        const UInt32 channels = o == kOutStream ? 2 : 1;
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyName: if (o == kOutStream) REPLY_STR("Speaker"); REPLY_STR("Microphone");
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioStreamPropertyIsActive: REPLY(UInt32, 1);
        case kAudioStreamPropertyDirection: REPLY(UInt32, o == kInStream);
        case kAudioStreamPropertyTerminalType: REPLY(UInt32, o == kOutStream ? kAudioStreamTerminalTypeSpeaker : kAudioStreamTerminalTypeMicrophone);
        case kAudioStreamPropertyStartingChannel: REPLY(UInt32, 1);
        case kAudioStreamPropertyLatency: REPLY(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: REPLY(AudioStreamBasicDescription, format(channels));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r = {format(channels), {kRate, kRate}};
            REPLY(AudioStreamRangedDescription, r);
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    case kVolume:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioControlPropertyScope: REPLY(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: REPLY(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioLevelControlPropertyScalarValue: REPLY(Float32, gVolume);
        case kAudioLevelControlPropertyDecibelValue: REPLY(Float32, toDb(gVolume));
        case kAudioLevelControlPropertyDecibelRange: REPLY(AudioValueRange, ((AudioValueRange){kMinDb, 0.0}));
        case kAudioLevelControlPropertyConvertScalarToDecibels:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toDb(*(Float32 *)outData); *outDataSize = sizeof(Float32); return 0;
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toScalar(*(Float32 *)outData); *outDataSize = sizeof(Float32); return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    default:
        return kAudioHardwareBadObjectError;
    }
}

// Every property the audio server asks for and the plug-in does not answer is logged once (object, selector, scope), so a client that
// drops the device after an unknown-property error names what it wanted.
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    (void)d;
    const OSStatus r = propertyData(o, a, qs, q, inDataSize, outDataSize, outData);
    trace(o, a, 0, r);
    if (r == kAudioHardwareUnknownPropertyError) {
        static UInt64 seen[128];
        static atomic_uint count;
        const UInt64 key = (UInt64)o << 56 ^ (UInt64)a->mSelector << 24 ^ a->mScope;
        unsigned n = atomic_load(&count);
        for (unsigned i = 0; i < n && i < 128; ++i) if (seen[i] == key) return r;
        n = atomic_fetch_add(&count, 1);
        if (n < 128) seen[n] = key;
        const UInt32 v = a->mSelector, sc = a->mScope;
        if (gControl && n < T970_AUDIO_DIAG_SLOTS) {
            T970AudioDiag *diag = (T970AudioDiag *)((uint8_t *)gControl + T970_AUDIO_DIAG_OFFSET);
            diag->selector[n] = v;
            diag->where[n] = (UInt32)o << 24 | (sc >> 24);
            diag->count = n + 1;
        }
        os_log_error(OS_LOG_DEFAULT, "T970Audio: unknown property object %u selector '%c%c%c%c' scope '%c%c%c%c' element %u pid %d", o,
               (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v, (char)(sc >> 24), (char)(sc >> 16), (char)(sc >> 8), (char)sc,
               a->mElement, pid);
    }
    return r;
}

// ---- The audio server's view of the device ---------------------------------------------------------------------------------------
// The routing layer of the audio server (VirtualAudio.plugin, in the same process) drops the device after a 'who?' error that never
// reaches this plug-in (i528). Its HAL calls and C++ throws are rebound here through its symbol pointers: every failing call (object,
// selector, scope, status) and every throw (the thrower's offset in the image, the exception's first word) is logged once and added to the
// trace with kind 4 (call) or 8 (throw, the offset in place of the selector).
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <sys/mman.h>
static OSStatus (*gRealGet)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *, void *);
static OSStatus (*gRealGetSize)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
static Boolean (*gRealHas)(AudioObjectID, const AudioObjectPropertyAddress *);
static void (*gRealThrow)(void *, void *, void (*)(void *));
static uintptr_t gVaBase;

static void noteCall(AudioObjectID o, const AudioObjectPropertyAddress *a, OSStatus r, const char *what) {
    const AudioObjectPropertyAddress t = {a->mSelector, a->mScope, 0};
    if (!gControl) return;
    T970AudioTrace *tr = (T970AudioTrace *)((uint8_t *)gControl + T970_AUDIO_TRACE_OFFSET);
    const UInt32 before = tr->count;
    trace(o & 0xff, &t, 4, r ? r : 1);
    if (tr->count == before) return;
    const UInt32 v = a->mSelector, sc = a->mScope;
    os_log_error(OS_LOG_DEFAULT, "T970Audio: va %{public}s object %u '%c%c%c%c' scope '%c%c%c%c' element %u status %d", what, o,
                 (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v, (char)(sc >> 24), (char)(sc >> 16), (char)(sc >> 8), (char)sc,
                 a->mElement, (int)r);
}
static OSStatus hookGet(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size, void *data) {
    const OSStatus r = gRealGet(o, a, qs, q, size, data);
    if (r && o) noteCall(o, a, r, "get");
    return r;
}
static OSStatus hookGetSize(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size) {
    const OSStatus r = gRealGetSize(o, a, qs, q, size);
    if (r && o) noteCall(o, a, r, "size");
    return r;
}
static Boolean hookHas(AudioObjectID o, const AudioObjectPropertyAddress *a) {
    const Boolean r = gRealHas(o, a);
    if (!r && o) noteCall(o, a, 0, "has");
    return r;
}
static void hookThrow(void *thrown, void *type, void (*destroy)(void *)) {
    const uintptr_t at = (uintptr_t)__builtin_return_address(0) - gVaBase;
    const UInt32 first = thrown ? *(const UInt32 *)thrown : 0;
    const char *name = type ? ((const char *const *)type)[1] : "";
    if (gControl) {
        const AudioObjectPropertyAddress t = {(UInt32)at, kAudioObjectPropertyScopeGlobal, 0};
        trace(0, &t, 8, (OSStatus)(first | 1));
    }
    os_log_error(OS_LOG_DEFAULT, "T970Audio: va throw at 0x%lx type %{public}s first 0x%08x", (unsigned long)at, name, first);
    __attribute__((musttail)) return gRealThrow(thrown, type, destroy);
}

// Rebinds the named imports of one image: its non-lazy and lazy symbol pointer sections, through the indirect symbol table.
static void rebind(const struct mach_header_64 *h, intptr_t slide) {
    const struct load_command *lc = (const void *)(h + 1);
    const struct segment_command_64 *linkedit = NULL;
    const struct symtab_command *symtab = NULL;
    const struct dysymtab_command *dysymtab = NULL;
    for (uint32_t i = 0; i < h->ncmds; ++i, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, SEG_LINKEDIT)) linkedit = (const void *)lc;
        else if (lc->cmd == LC_SYMTAB) symtab = (const void *)lc;
        else if (lc->cmd == LC_DYSYMTAB) dysymtab = (const void *)lc;
    }
    if (!linkedit || !symtab || !dysymtab) return;
    const uintptr_t base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
    const struct nlist_64 *syms = (const void *)(base + symtab->symoff);
    const char *strs = (const char *)(base + symtab->stroff);
    const uint32_t *indirect = (const void *)(base + dysymtab->indirectsymoff);
    static const char *const names[] = {"_AudioObjectGetPropertyData", "_AudioObjectGetPropertyDataSize", "_AudioObjectHasProperty", "___cxa_throw"};
    void *const hooks[] = {(void *)hookGet, (void *)hookGetSize, (void *)hookHas, (void *)hookThrow};
    void **const reals[] = {(void **)&gRealGet, (void **)&gRealGetSize, (void **)&gRealHas, (void **)&gRealThrow};
    lc = (const void *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; ++i, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *seg = (const void *)lc;
        const struct section_64 *sect = (const void *)(seg + 1);
        for (uint32_t s = 0; s < seg->nsects; ++s, ++sect) {
            const uint32_t type = sect->flags & SECTION_TYPE;
            if (type != S_NON_LAZY_SYMBOL_POINTERS && type != S_LAZY_SYMBOL_POINTERS) continue;
            void **slots = (void **)((uintptr_t)slide + sect->addr);
            for (uint64_t k = 0; k < sect->size / sizeof(void *); ++k) {
                const uint32_t index = indirect[sect->reserved1 + k];
                if (index & (INDIRECT_SYMBOL_ABS | INDIRECT_SYMBOL_LOCAL)) continue;
                const char *name = strs + syms[index].n_un.n_strx;
                for (unsigned n = 0; n < 4; ++n) {
                    if (strcmp(name, names[n]) || slots[k] == hooks[n]) continue;
                    const uintptr_t page = (uintptr_t)&slots[k] & ~(uintptr_t)(getpagesize() - 1);
                    if (mprotect((void *)page, (size_t)getpagesize(), PROT_READ | PROT_WRITE) &&
                        vm_protect(mach_task_self(), page, (vm_size_t)getpagesize(), false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY)) {
                        os_log_error(OS_LOG_DEFAULT, "T970Audio: va rebind %{public}s failed", name);
                        continue;
                    }
                    if (!*reals[n]) *reals[n] = slots[k];
                    slots[k] = hooks[n];
                    os_log_error(OS_LOG_DEFAULT, "T970Audio: va rebind %{public}s", name);
                }
            }
        }
    }
}
__attribute__((unused)) static void imageAdded(const struct mach_header *h, intptr_t slide) {
    Dl_info info;
    if (h->magic != MH_MAGIC_64 || !dladdr(h, &info) || !info.dli_fname || !strstr(info.dli_fname, "VirtualAudio.plugin/")) return;
    gVaBase = (uintptr_t)h;
    rebind((const struct mach_header_64 *)h, slide);
}
// arm64e (iPadOS 27's server): the symbol pointers are authenticated with their slot address, so a plain rebind faults; the trace is off.
#if !__has_feature(ptrauth_calls)
__attribute__((constructor)) static void watchAudioServer(void) {_dyld_register_func_for_add_image(imageAdded);}
#endif

// ---- IO ----------------------------------------------------------------------------------------------------------------------------
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    OSStatus r = 0;
    if (gIoUsers == 0) {
        if (connectService()) r = kAudioHardwareNotRunningError;
        else if (!gControl) r = kAudioHardwareNotRunningError;
        else {
            setHostClock();   // again: the shared page may have been read back from disk since
            if (call(T970AudioStartOutput) != KERN_SUCCESS) r = kAudioHardwareNotRunningError;
            else {
                if (gIn) call(T970AudioStartInput);
                const uint64_t host = mach_absolute_time(), anchor = gControl->out_anchor_host;
                gZeroOrigin = host > anchor ? (uint64_t)((Float64)(host - anchor) / gHostTicksPerFrame) : 0;
                gZeroSeed++;
            }
        }
    }
    if (!r) gIoUsers++;
    pthread_mutex_unlock(&gLock);
    return r;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    if (gIoUsers && --gIoUsers == 0) {call(T970AudioStopOutput); if (gIn) call(T970AudioStopInput);}
    pthread_mutex_unlock(&gLock);
    return 0;
}
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, Float64 *sampleTime, UInt64 *hostTime, UInt64 *seed) {
    (void)d; (void)o; (void)client;
    if (!gControl) return kAudioHardwareNotRunningError;
    uint32_t seq;
    uint64_t anchor;
    do {seq = gControl->out_seq; anchor = gControl->out_anchor_host;} while ((seq & 1) || seq != gControl->out_seq);
    const uint64_t now = mach_absolute_time();
    const Float64 period = gHostTicksPerFrame * kZeroPeriod;
    const uint64_t origin = anchor + (uint64_t)((Float64)gZeroOrigin * gHostTicksPerFrame);
    const uint64_t wraps = now > origin ? (uint64_t)((Float64)(now - origin) / period) : 0;
    *sampleTime = (Float64)(gZeroOrigin + wraps * kZeroPeriod);
    *hostTime = origin + (uint64_t)((Float64)wraps * period);
    *seed = 1 + seq / 2 + gZeroSeed;
    return 0;
}
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace) {
    (void)d; (void)o; (void)client;
    *will = op == kAudioServerPlugInIOOperationWriteMix || (gIn && op == kAudioServerPlugInIOOperationReadInput);
    *inPlace = true;
    return 0;
}
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    (void)d; (void)o; (void)client; (void)op; (void)frames; (void)info; return 0;
}
// Records how the audio server schedules its IO thread (i545: 98 gaps of more than 160 ms between IO cycles).
static void noteThread(T970AudioDiag *diag) {
    const thread_act_t self = mach_thread_self();
    thread_extended_info_data_t info;
    mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
    if (thread_info(self, THREAD_EXTENDED_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS) {
        diag->io_pri = info.pth_curpri;
        diag->io_base = info.pth_priority;
        diag->io_policy = info.pth_policy;
        if (!diag->io_pri_min || info.pth_curpri < diag->io_pri_min) diag->io_pri_min = info.pth_curpri;
    }
    thread_time_constraint_policy_data_t tc;
    count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t def = false;
    if (thread_policy_get(self, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tc, &count, &def) == KERN_SUCCESS) {
        diag->io_tc = def ? 0 : 1;
        diag->io_period = tc.period;
        diag->io_computation = tc.computation;
        diag->io_constraint = tc.constraint;
    } else diag->io_tc = -1;
    mach_port_deallocate(mach_task_self(), self);
}
// Rounded to nearest (a plain cast truncates toward zero: a dead band around 0 that roughens quiet sound).
static inline int16_t toS16(Float32 v) {v *= 32767.0f; return (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : lrintf(v));}
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 frames,
                              const AudioServerPlugInIOCycleInfo *info, void *main, void *secondary) {
    (void)d; (void)o; (void)client; (void)secondary;
    if (!gControl) return 0;
    // The play position now, on the timeline GetZeroTimeStamp gives the audio server.
    const uint64_t host = mach_absolute_time(), anchor = gControl->out_anchor_host;
    const int64_t nowFrames = host > anchor ? (int64_t)((Float64)(host - anchor) / gHostTicksPerFrame) : 0;
    T970AudioDiag *diag = (T970AudioDiag *)((uint8_t *)gControl + T970_AUDIO_DIAG_OFFSET);
    if (op == kAudioServerPlugInIOOperationWriteMix && stream == kOutStream) {
        const int32_t ahead = (int32_t)((int64_t)info->mOutputTime.mSampleTime - nowFrames);
        // Where the audio server places now, against the timeline.
        const int32_t skew = (int32_t)((int64_t)info->mCurrentTime.mSampleTime - nowFrames);
        if (!diag->writes || skew < diag->skew_min) diag->skew_min = skew;
        if (!diag->writes || skew > diag->skew_max) diag->skew_max = skew;
        if (!diag->writes || ahead < diag->ahead_min) diag->ahead_min = ahead;
        if (!diag->writes || ahead > diag->ahead_max) diag->ahead_max = ahead;
        diag->io_frames = frames;
        if (diag->writes % 256 == 0) noteThread(diag);
        diag->writes += 1;
        const Float32 *src = main;
        uint64_t t = (uint64_t)info->mOutputTime.mSampleTime;
        Float32 g = ringGain();   // the volume itself is the amplifiers' (publishVolume)
        {
            const uint32_t gm = (uint32_t)lrintf(gain() * 10000.0f);   // the amplifiers' gain, for the log
            diag->gain_now = gm;
            if (!diag->gain_max || gm < diag->gain_min) diag->gain_min = gm;
            if (gm > diag->gain_max) diag->gain_max = gm;
        }
        Float32 peak = 0;
        for (UInt32 i = 0; i < 2 * frames; ++i) {const Float32 v = fabsf(src[i]); if (v > peak) peak = v;}
        const UInt32 p = (UInt32)(fminf(peak, 1.0f) * 32767.0f);
        if (p > diag->mix_peak) diag->mix_peak = p;
        // The audio server runs more than one IO context on the device (i539: 65291 writes of 1024 frames in a 600 s run, 2.3 times the
        // frames that played), each with its own mix. Each is added to what the ring holds, so a context writing silence does not replace
        // another one's sound; the service clears every period it takes.
        if (diag->last_host) {
            const Float64 ms = (Float64)(host - diag->last_host) / gHostTicksPerFrame / (kRate / 1000.0);
            diag->interval[ms < 15 ? 0 : ms < 25 ? 1 : ms < 40 ? 2 : ms < 80 ? 3 : ms < 160 ? 4 : 5] += 1;
            if (ms < 160 && t > diag->last_end) {diag->gaps += 1; diag->gap_frames += (uint32_t)(t - diag->last_end);}
        }
        diag->last_host = host;
        diag->last_end = t + frames;
        diag->frames_total += frames;
        // test_flags bit 2: one capture of the raw mix (T970AudioCapture), for the log.
        // Only once the service's run has played 2 s (test_flags bit 8): a capture at a run's start shows its start-up, not the steady path.
        if ((gControl->test_flags & 4) && (gControl->test_flags & (1u << 8)) && t >= (uint64_t)(20 * kRate)) {
            T970AudioCapture *cap = (T970AudioCapture *)((uint8_t *)gControl + T970_AUDIO_CAP_OFFSET);
            if (!cap->state && p >= 3277) {
                UInt32 at = 0; Float32 best = 0;
                for (UInt32 i = 0; i < frames; ++i) {
                    const Float32 v = fmaxf(fabsf(src[2 * i]), fabsf(src[2 * i + 1]));
                    if (v > best) {best = v; at = i;}
                }
                at = at >= 64 ? at - 64 : 0;
                if (frames >= T970_AUDIO_CAP_FRAMES && at + T970_AUDIO_CAP_FRAMES > frames) at = frames - T970_AUDIO_CAP_FRAMES;
                for (UInt32 i = 0; i < T970_AUDIO_CAP_FRAMES; ++i) {
                    const UInt32 k = at + i;
                    cap->s[2 * i] = k < frames ? toS16(src[2 * k]) : 0;
                    cap->s[2 * i + 1] = k < frames ? toS16(src[2 * k + 1]) : 0;
                }
                cap->write_frames = frames; cap->t = t + at; cap->peak = (int32_t)p; cap->at = at;
                __atomic_store_n(&cap->state, 1u, __ATOMIC_RELEASE);
            }
        }
        // gts7xlwifi: a write that starts before the frame the service takes next lost its beginning (the service took it already).
        if (t < gControl->out_taken) diag->in_misses += 1;
        // test_flags bit 0 (t970-audio bit 1024): the plug-in's own 1 kHz sine at -18 dBFS in place of the mix, on these sample times.
        // test_flags bit 1 (t970-audio bit 8192): the kernel's chirp (T970Audio.cpp chirpSample) generated here, through the ring:
        // 4 s log sweep 100 Hz -> 16 kHz, 0.5 s silence, 2 s 440 + 3520 Hz, at 2000/32767, starting at the first write after load.
        static uint64_t first = UINT64_MAX;   // the chirp's start; the normal mix plays again 6.5 s after it
        // From 75 s on the service's timeline (after the kernel's own chirp at 45 s), at the first write: play any sound to start it.
        if ((gControl->test_flags & 2) && first == UINT64_MAX && t >= (uint64_t)(75 * kRate)) first = t;
        if ((gControl->test_flags & 2) && first != UINT64_MAX && t - first < (uint64_t)(6.5 * kRate)) {
            static Float32 chirp[2 * 4096];
            static double phase, phase2;
            const UInt32 n = frames < 4096 ? frames : 4096;
            const double fs = kRate, sweep = 4 * fs, gapEnd = sweep + fs / 2, total = 6.5 * fs, ramp = fs / 100;
            for (UInt32 i = 0; i < n; ++i) {
                const double k = (double)(t + i - first);
                double v = 0, g = 2000.0 / 32767.0;
                if (k < sweep) {
                    phase += 100.0 * pow(160.0, k / sweep) / fs;
                    if (k < ramp) g *= k / ramp; else if (sweep - k < ramp) g *= (sweep - k) / ramp;
                    v = g * sin(2 * M_PI * phase);
                } else if (k >= gapEnd && k < total) {
                    phase += 440.0 / fs; phase2 += 3520.0 / fs;
                    const double at = k - gapEnd, len = total - gapEnd;
                    if (at < ramp) g *= at / ramp; else if (len - at < ramp) g *= (len - at) / ramp;
                    v = g * 0.5 * (sin(2 * M_PI * phase) + sin(2 * M_PI * phase2));
                }
                phase -= floor(phase); phase2 -= floor(phase2);
                chirp[2 * i] = chirp[2 * i + 1] = (Float32)v;
            }
            src = chirp;
            frames = n;
            g = 1.0f;
        } else if (gControl->test_flags & 1) {
            static Float32 sine[2 * 4096];
            const UInt32 n = frames < 4096 ? frames : 4096;
            for (UInt32 i = 0; i < n; ++i) sine[2 * i] = sine[2 * i + 1] = 0.125f * sinf((Float32)((t + i) % 48) * (2.0f * 3.14159265f / 48.0f));
            src = sine;
            frames = n;
            g = 1.0f;   // the test level into the ring (the amplifiers still apply the volume)
        }
        const uint64_t written = gControl->out_written;
        if (t < written) {
            diag->overlaps += 1;
            if (written - t > diag->overlap_max) diag->overlap_max = (uint32_t)(written - t);
        }
        for (UInt32 i = 0; i < frames; ++i, ++t) {
            int16_t *dst = gOut + 2 * (t % T970_AUDIO_RING_FRAMES);
            dst[0] = toS16(dst[0] / 32767.0f + src[2 * i] * g);
            dst[1] = toS16(dst[1] / 32767.0f + src[2 * i + 1] * g);
        }
        if (t > gControl->out_written) gControl->out_written = t;
        gControl->out_written_host = host;
    } else if (op == kAudioServerPlugInIOOperationReadInput && stream == kInStream && gIn) {
        // The microphone ring, mono 16-bit, on the same timeline: frames the service has not captured yet read as silence (counted).
        Float32 *dst = main;
        uint64_t t = (uint64_t)info->mInputTime.mSampleTime;
        const uint64_t written = __atomic_load_n(&gControl->in_written, __ATOMIC_ACQUIRE);
        const int32_t behind = (int32_t)((int64_t)written - (int64_t)(t + frames));
        if (!diag->mic_reads || behind < diag->mic_behind_min) diag->mic_behind_min = behind;
        if (!diag->mic_reads || behind > diag->mic_behind_max) diag->mic_behind_max = behind;
        diag->mic_reads += 1;
        if (t + frames > written) diag->mic_short += 1;
        for (UInt32 i = 0; i < frames; ++i, ++t)
            dst[i] = t < written && written - t <= T970_AUDIO_RING_FRAMES ? gIn[t % T970_AUDIO_RING_FRAMES] / 32768.0f : 0.0f;
    }
    return 0;
}
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    (void)d; (void)o; (void)client; (void)op; (void)frames; (void)info; return 0;
}

static AudioServerPlugInDriverInterface gInterface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformDeviceConfigurationChange, AbortDeviceConfigurationChange, HasProperty, IsPropertySettable, GetPropertyDataSize,
    GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;

static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out) {
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        AddRef(self);
        *out = &gInterfacePtr;
        r = S_OK;
    }
    CFRelease(requested);
    return r;
}

__attribute__((visibility("default"))) void *T970Audio_Create(CFAllocatorRef allocator, CFUUIDRef type) {
    (void)allocator;
    if (!CFEqual(type, kAudioServerPlugInTypeUUID)) return NULL;
    if (!gHostTicksPerFrame) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        gHostTicksPerFrame = 1e9 / kRate * tb.denom / tb.numer;
    }
    return &gInterfacePtr;
}
