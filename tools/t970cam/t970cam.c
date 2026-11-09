// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi camera stage C5c: a command-line client of the T970Camera service (package/T970CameraService.cpp, interface in
// package/T970CameraAbi.h). Starts the front camera, takes frames tear-free from the shared buffers, reports the frame rate and
// writes the last frame as a 24-bit BMP, debayered on the CPU at half (or smaller) resolution with gray-world white balance and a
// gamma of 2.2. The Bayer order of the S5K4HA is not known yet: -b all writes one file per order (the right one has natural colours).
//
//   t970cam [-n frames] [-o out.bmp] [-b rggb|grbg|gbrg|bggr|all] [-s 2|4|8] [-m] [-e lines -g gain]
//
// Build: scripts/build-t970cam.py (ad hoc signed, trust cache entry in its output). The kernel needs t970-camera=<..+512>.
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "T970CameraAbi.h"

#define BLACK 16   /* the 10-bit pedestal of 64 as an MSB byte */

static const char *const kOrders[4] = {"rggb", "grbg", "gbrg", "bggr"};

static void barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

// Takes the newest frame: returns its buffer index with `reading` set, or -1 when it moved under us (try again).
static int take(T970CameraControl *k) {
    const uint32_t s = k->seq;
    barrier();
    const uint32_t idx = k->latest;
    if (idx >= k->buffers) return -1;
    k->reading = idx;
    barrier();
    if (k->seq != s || k->writing == idx) { k->reading = T970_CAMERA_NONE; return -1; }
    return (int)idx;
}

static int writeBmp(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    const uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    uint8_t hdr[54] = {'B', 'M'};
    uint32_t v[] = {size, 0, 54, 40, w, h};
    memcpy(hdr + 2, &v[0], 4); memcpy(hdr + 10, &v[2], 4); memcpy(hdr + 14, &v[3], 4);
    memcpy(hdr + 18, &v[4], 4); memcpy(hdr + 22, &v[5], 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);
    uint8_t *line = calloc(1, row);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *src = rgb + (size_t)(h - 1 - y) * w * 3;
        for (uint32_t x = 0; x < w; ++x) { line[x * 3] = src[x * 3 + 2]; line[x * 3 + 1] = src[x * 3 + 1]; line[x * 3 + 2] = src[x * 3]; }
        fwrite(line, 1, row, f);
    }
    free(line);
    return fclose(f);
}

// MSB of pixel x on a packed RAW10 line.
static inline uint32_t px(const uint8_t *line, uint32_t x) { return line[(x / 4) * 5 + x % 4]; }

// One output pixel per (scale x scale) block, from the 2x2 Bayer cell at the block's top left. order: index into kOrders.
static void debayer(const uint8_t *raw, uint32_t stride, uint32_t w, uint32_t h, unsigned scale, unsigned order, int mirror,
                    uint8_t *out, uint32_t ow, uint32_t oh) {
    // Sites in a cell: 0 (y0 x0), 1 (y0 x1), 2 (y1 x0), 3 (y1 x1); which of them is R and B per order, the other two are G.
    static const unsigned rSite[4] = {0, 1, 2, 3}, bSite[4] = {3, 2, 1, 0};
    double sum[3] = {0, 0, 0};
    for (uint32_t oy = 0; oy < oh; oy += 4)
        for (uint32_t ox = 0; ox < ow; ox += 4) {
            const uint32_t y = (oy * scale) & ~1u, x = (ox * scale) & ~1u;
            const uint8_t *l0 = raw + (size_t)y * stride, *l1 = l0 + stride;
            const uint32_t c[4] = {px(l0, x), px(l0, x + 1), px(l1, x), px(l1, x + 1)};
            const double r = c[rSite[order]], b = c[bSite[order]], g = (c[0] + c[1] + c[2] + c[3] - r - b) / 2.0;
            sum[0] += r > BLACK ? r - BLACK : 0; sum[1] += g > BLACK ? g - BLACK : 0; sum[2] += b > BLACK ? b - BLACK : 0;
        }
    double gain[3];
    for (int i = 0; i < 3; ++i) gain[i] = (sum[1] > 1 && sum[i] > 1) ? sum[1] / sum[i] : 1.0;
    uint8_t lut[3][256];
    for (int ch = 0; ch < 3; ++ch)
        for (int v = 0; v < 256; ++v) {
            double lin = (v > BLACK ? v - BLACK : 0) * gain[ch] / (255.0 - BLACK);
            if (lin > 1) lin = 1;
            lut[ch][v] = (uint8_t)lrint(pow(lin, 1 / 2.2) * 255);
        }
    (void)h;
    for (uint32_t oy = 0; oy < oh; ++oy) {
        const uint32_t y = (oy * scale) & ~1u;
        const uint8_t *l0 = raw + (size_t)y * stride, *l1 = l0 + stride;
        for (uint32_t ox = 0; ox < ow; ++ox) {
            const uint32_t x = (ox * scale) & ~1u;
            if (x + 1 >= w) break;
            const uint32_t c[4] = {px(l0, x), px(l0, x + 1), px(l1, x), px(l1, x + 1)};
            const uint32_t r = c[rSite[order]], b = c[bSite[order]];
            const uint32_t g = (c[0] + c[1] + c[2] + c[3] - r - b) / 2;
            uint8_t *o = out + ((size_t)oy * ow + (mirror ? ow - 1 - ox : ox)) * 3;
            o[0] = lut[0][r]; o[1] = lut[1][g > 255 ? 255 : g]; o[2] = lut[2][b];
        }
    }
}

int main(int argc, char **argv) {
    unsigned frames = 30, scale = 2, order = 1, all = 0, exp = 0, gain = 0;
    int mirror = 0;
    const char *out = "t970cam.bmp";
    int ch;
    while ((ch = getopt(argc, argv, "n:o:b:s:me:g:")) != -1) {
        switch (ch) {
        case 'n': frames = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'o': out = optarg; break;
        case 'b':
            all = !strcmp(optarg, "all");
            for (unsigned i = 0; i < 4; ++i) if (!strcmp(optarg, kOrders[i])) order = i;
            break;
        case 's': scale = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'm': mirror = 1; break;
        case 'e': exp = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'g': gain = (unsigned)strtoul(optarg, NULL, 0); break;
        default:
            fprintf(stderr, "usage: t970cam [-n frames] [-o out.bmp] [-b rggb|grbg|gbrg|bggr|all] [-s 2|4|8] [-m] [-e lines -g gain]\n");
            return 2;
        }
    }
    if (scale != 2 && scale != 4 && scale != 8) scale = 2;

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("T970Camera"));
    if (!svc) { fprintf(stderr, "t970cam: no T970Camera service (boot with t970-camera bit 512)\n"); return 1; }
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr) { fprintf(stderr, "t970cam: IOServiceOpen 0x%x\n", kr); return 1; }
    mach_vm_address_t ctlAddr = 0, frmAddr = 0;
    mach_vm_size_t ctlSize = 0, frmSize = 0;
    kr = IOConnectMapMemory64(conn, T970CameraMemoryControl, mach_task_self(), &ctlAddr, &ctlSize, kIOMapAnywhere);
    if (!kr) kr = IOConnectMapMemory64(conn, T970CameraMemoryFrames, mach_task_self(), &frmAddr, &frmSize,
                                       kIOMapAnywhere | kIOMapReadOnly | kIOMapWriteCombineCache);
    if (kr) { fprintf(stderr, "t970cam: map 0x%x\n", kr); IOServiceClose(conn); return 1; }
    T970CameraControl *k = (T970CameraControl *)(uintptr_t)ctlAddr;
    const uint8_t *frm = (const uint8_t *)(uintptr_t)frmAddr;
    if (k->magic != T970_CAMERA_MAGIC || k->version != T970_CAMERA_VERSION) {
        fprintf(stderr, "t970cam: control page magic %08x version %u\n", k->magic, k->version);
        IOServiceClose(conn); return 1;
    }
    k->reading = T970_CAMERA_NONE;
    if (exp || gain) {
        const uint64_t in[2] = {exp, gain};
        kr = IOConnectCallScalarMethod(conn, T970CameraSetExposure, in, 2, NULL, NULL);
        if (kr) fprintf(stderr, "t970cam: set exposure 0x%x\n", kr);
    }
    kr = IOConnectCallScalarMethod(conn, T970CameraStart, NULL, 0, NULL, NULL);
    if (kr) { fprintf(stderr, "t970cam: start 0x%x\n", kr); IOServiceClose(conn); return 1; }

    // Power-up, C4 frame and auto exposure take about a second.
    for (int i = 0; i < 1500 && k->state != T970CameraStreaming; ++i) {
        if (k->state == T970CameraFailed) { fprintf(stderr, "t970cam: failed, error %d\n", k->error); IOServiceClose(conn); return 1; }
        usleep(10000);
    }
    if (k->state != T970CameraStreaming) { fprintf(stderr, "t970cam: not streaming (state %u)\n", k->state); IOServiceClose(conn); return 1; }
    const uint32_t w = k->width, h = k->height, stride = k->stride;
    printf("t970cam: %ux%u DT 0x%x stride %u, %u buffers of %u bytes, map %llu bytes\n", w, h, k->data_type, stride, k->buffers,
           k->buffer_bytes, (unsigned long long)frmSize);
    if ((uint64_t)k->buffer_offset[k->buffers - 1] + (uint64_t)stride * h > frmSize) {
        fprintf(stderr, "t970cam: buffers outside the mapping\n"); IOServiceClose(conn); return 1;
    }

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const uint32_t seq0 = k->seq;
    const uint64_t t0 = mach_absolute_time();
    uint32_t got = 0, retries = 0, last = k->seq;
    int idx = -1;
    const uint32_t ow = w / scale, oh = h / scale;
    uint8_t *rgb = malloc((size_t)ow * oh * 3);
    while (got < frames && k->state == T970CameraStreaming) {
        if (k->seq == last) { usleep(2000); continue; }
        idx = take(k);
        if (idx < 0) { ++retries; continue; }
        last = k->seq;
        ++got;
        if (got < frames) k->reading = T970_CAMERA_NONE;   // keep the last one held for the debayer
    }
    const double secs = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e9;
    printf("t970cam: %u frames taken, sensor seq %u -> %u in %.2f s (%.1f fps delivered), %u retries, %u timeouts, exposure %u gain 0x%x mean %u\n",
           got, seq0, k->seq, secs, secs > 0 ? (k->seq - seq0) / secs : 0, retries, k->timeouts, k->exposure, k->gain, k->mean);
    if (idx >= 0 && rgb) {
        const uint8_t *raw = frm + k->buffer_offset[idx];
        for (unsigned o = 0; o < 4; ++o) {
            if (!all && o != order) continue;
            char path[512];
            if (all) {
                const char *dot = strrchr(out, '.');
                const int stem = dot ? (int)(dot - out) : (int)strlen(out);
                snprintf(path, sizeof path, "%.*s-%s%s", stem, out, kOrders[o], dot ? dot : ".bmp");
            } else {
                snprintf(path, sizeof path, "%s", out);
            }
            debayer(raw, stride, w, h, scale, o, mirror, rgb, ow, oh);
            if (!writeBmp(path, rgb, ow, oh)) printf("t970cam: wrote %s (%ux%u, %s)\n", path, ow, oh, kOrders[o]);
        }
        k->reading = T970_CAMERA_NONE;
    }
    free(rgb);
    IOConnectCallScalarMethod(conn, T970CameraStop, NULL, 0, NULL, NULL);
    IOServiceClose(conn);
    return got ? 0 : 1;
}
