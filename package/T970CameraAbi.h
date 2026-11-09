// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi camera stage C5: the interface between the T970Camera service (package/T970CameraService.cpp, kernel side in
// package/T970Camera.cpp) and its user-space clients (tools/t970cam/t970cam.c). Plain C, shared by both sides.
//
// A client opens the IOService named "T970Camera", maps memory type 0 (this control page, read/write) and type 1 (the frame
// buffers in the carve-out, read-only; pass kIOMapWriteCombineCache, the device writes them without the CPU caches), and calls
// T970CameraStart. Frames are packed RAW10 (4 pixels in 5 bytes: four MSB bytes, then one byte of 2-bit LSBs), `stride` bytes a line.
//
// Taking a frame without tearing (the kernel never re-arms the write master into the buffer named by `reading`):
//   s = seq; idx = latest; reading = idx; barrier; if (seq != s) retry;  ... use buffer idx ...;  reading = T970_CAMERA_NONE
#ifndef T970_CAMERA_ABI_H
#define T970_CAMERA_ABI_H
#include <stdint.h>

#define T970_CAMERA_MAGIC 0x54393743u   /* 'T97C' */
#define T970_CAMERA_VERSION 1u
#define T970_CAMERA_NONE 0xffffffffu
#define T970_CAMERA_MAX_BUFFERS 4u

enum {
    T970CameraStart = 0,        /* no arguments: power the front sensor and stream until Stop or close */
    T970CameraStop = 1,         /* no arguments */
    T970CameraSetExposure = 2,  /* scalars: coarse integration lines, analogue gain code (0x20 = 1x); 0, 0 = auto exposure */
};
enum {T970CameraMemoryControl = 0, T970CameraMemoryFrames = 1};
enum {
    T970CameraIdle = 0,
    T970CameraStarting = 1,
    T970CameraStreaming = 2,
    T970CameraStopped = 3,
    T970CameraFailed = 4,       /* `error`: the C4 stage (t970_camera[28]) or -1 too many frame timeouts, -2 no room for 2 buffers */
};

typedef struct {
    uint32_t magic, version;
    volatile uint32_t state;
    volatile int32_t error;
    volatile uint32_t width, height, stride, data_type;       /* data_type: CSI-2 DT, 0x2b RAW10 */
    volatile uint32_t buffers, buffer_bytes;
    volatile uint32_t buffer_offset[T970_CAMERA_MAX_BUFFERS]; /* byte offsets inside memory type 1 */
    volatile uint32_t seq;                                    /* frames completed this session */
    volatile uint32_t latest;                                 /* buffer holding frame `seq` */
    volatile uint32_t writing;                                /* buffer the write master fills now, or NONE */
    volatile uint32_t reading;                                /* written by the client: buffer it is reading, or NONE */
    volatile uint64_t timestamp;                              /* mach_absolute_time() when frame `seq` completed */
    volatile uint32_t exposure, gain, mean, timeouts;         /* coarse lines, gain code, mean MSB of the last AE sample */
    volatile uint32_t manual_exposure, manual_gain;           /* last T970CameraSetExposure values, 0 = auto */
    volatile uint32_t frame_bytes;                            /* size of memory type 1 */
    uint32_t reserved[7];
} T970CameraControl;

#endif
