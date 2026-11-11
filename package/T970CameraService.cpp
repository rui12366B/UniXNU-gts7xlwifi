// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi camera stage C5b: the T970Camera service and its user client (interface in T970CameraAbi.h). The service maps the
// control page and the frame carve-out that T970Camera.cpp owns and passes start, stop and exposure requests to it; the frames
// never pass through here. T970Camera.cpp registers this file's classes (static initializer taken out of __mod_init_func by the
// build, as T970AudioService.cpp) and publishes the service after its boot run (t970-camera bit 512).
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include "T970CameraAbi.h"

extern "C" IOMemoryDescriptor *t970_camera_memory(uint32_t type);        // retained; null when absent
extern "C" int t970_camera_request(uint32_t selector, uint32_t a, uint32_t b);   // 0 accepted
// [9] service state (1 published; negative: the step that failed), [10] opens | starts << 8 | maps << 16 | closes << 24
extern "C" {extern volatile int32_t t970_camera_c5[20];}

namespace {
void bump(unsigned shift) {
    const uint32_t v = uint32_t(t970_camera_c5[10]);
    const uint32_t field = ((v >> shift) + 1) & 0xff;
    t970_camera_c5[10] = int32_t((v & ~(0xffu << shift)) | field << shift);
}
}

class T970Camera : public IOService {
    OSDeclareDefaultStructors(T970Camera)
public:
    bool startCamera() {
        setProperty("NativeSandboxGrantProcess", "t970cam");
        setProperty("IOUserClientClass", "T970CameraUserClient");
        registerService();
        return true;
    }
};
OSDefineMetaClassAndStructors(T970Camera, IOService)

class T970CameraUserClient : public IOUserClient {
    OSDeclareDefaultStructors(T970CameraUserClient)
public:
    bool start(IOService *provider) override {
        if (!OSDynamicCast(T970Camera, provider) || !IOUserClient::start(provider)) return false;
        bump(0);
        return true;
    }
    IOReturn clientClose() override {
        // The client went away: nothing reads the frames any more, so the sensor stops.
        (void)t970_camera_request(T970CameraStop, 0, 0);
        bump(24);
        terminate();
        return kIOReturnSuccess;
    }
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override {
        IOMemoryDescriptor *m = t970_camera_memory(type);
        if (!m) return kIOReturnBadArgument;
        *options = type == T970CameraMemoryFrames ? kIOMapReadOnly : 0;
        *memory = m;   // the caller consumes the reference
        bump(16);
        return kIOReturnSuccess;
    }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override {
        if (args->structureInputSize) return kIOReturnBadArgument;
        uint32_t a = 0, b = 0;
        if (selector == T970CameraSetExposure || selector == T970CameraReport) {
            if (args->scalarInputCount != 2) return kIOReturnBadArgument;
            a = uint32_t(args->scalarInput[0]); b = uint32_t(args->scalarInput[1]);
        } else if (selector > T970CameraReport || args->scalarInputCount) {
            return kIOReturnBadArgument;
        }
        if (selector == T970CameraStart) bump(8);
        const int rc = t970_camera_request(selector, a, b);
        return rc == 0 ? kIOReturnSuccess : rc == 1 ? kIOReturnBusy : kIOReturnNotReady;
    }
};
OSDefineMetaClassAndStructors(T970CameraUserClient, IOUserClient)

// Called by T970Camera.cpp once its classes are registered.
extern "C" void t970_camera_service_start(void) {
    T970Camera *camera = OSTypeAlloc(T970Camera);
    if (!camera || !camera->init() || !camera->attach(IOService::getPlatform())) {
        if (camera) camera->release();
        t970_camera_c5[9] = -1;
        return;
    }
    if (!camera->start(IOService::getPlatform()) || !camera->startCamera()) {
        t970_camera_c5[9] = -2;
        camera->detach(IOService::getPlatform());
        camera->release();
        return;
    }
    t970_camera_c5[9] = 1;
    camera->release();   // the platform's attachment keeps it
}
