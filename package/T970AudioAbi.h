/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The interface between the gts7xlwifi audio service (T970Audio.cpp, IOKit class T970Audio) and the audio server plug-in that iOS loads
 * (hal/T970AudioPlugIn.c). The plug-in opens the service, maps three memory types and starts and stops each direction.
 *
 * Memory type 0 is the control page; 1 the playback ring; 2 the capture ring. Rings hold 16-bit little-endian interleaved samples,
 * ring_frames frames each. Sample time t lives at frame t % ring_frames. Sample time 0 of a direction is the frame the DSP plays (or
 * captures) at host time anchor_host of that direction; one frame lasts ticks_per_frame_q32 / 2^32 host ticks. The anchors are set
 * once, when the service starts, so one timeline holds for the whole boot; the service clears each playback period once it has taken it. The service consumes
 * playback frames lead_frames ahead of the play position and fills capture frames lead_frames behind it, so the plug-in writes at least
 * lead_frames ahead and reads at least lead_frames behind. */
#ifndef T970_AUDIO_ABI_H
#define T970_AUDIO_ABI_H
#include <stdint.h>

#define T970_AUDIO_CLASS "T970Audio"
#define T970_AUDIO_MAGIC 0x54394155u        /* 'T9AU' */
#define T970_AUDIO_VERSION 3u   /* 2: the volume moved to the amplifiers (out_volume_*); 3: capture (in_*), out_taken */
#define T970_AUDIO_RATE 48000u
#define T970_AUDIO_OUT_CHANNELS 2u
#define T970_AUDIO_IN_CHANNELS 1u          /* the microphone (mono); control in_channels is 0 while capture is off (t970-audio bit) */
#define T970_AUDIO_PERIOD_FRAMES 480u        /* 10 ms */
#define T970_AUDIO_RING_FRAMES 65536u        /* 1.37 s: room for the 300 ms lead and the audio server's late IO cycles */

enum {T970AudioMemoryControl = 0, T970AudioMemoryOutput = 1, T970AudioMemoryInput = 2};
enum {T970AudioStartOutput = 0, T970AudioStopOutput = 1, T970AudioStartInput = 2, T970AudioStopInput = 3};
/* Scalar input: the address of CoreAudio's host clock frequency constant in the caller (24 MHz, the timer of Apple's chips); the service
 * sets it to this board's timer frequency. */
enum {T970AudioSetHostClock = 4};
enum {T970AudioOutputRunning = 1u << 0, T970AudioInputRunning = 1u << 1, T970AudioOutputReady = 1u << 2, T970AudioInputReady = 1u << 3};

typedef struct {
    uint32_t magic, version;
    uint32_t rate, out_channels, in_channels, period_frames, ring_frames, lead_frames;
    volatile uint32_t state;                 /* T970Audio*Running / *Ready bits */
    volatile uint32_t test_flags;            /* gts7xlwifi: bit 0, the plug-in writes its own 1 kHz sine instead of the mix */
    uint64_t ticks_per_frame_q32;            /* host ticks per frame, 32.32 fixed point */
    volatile uint64_t out_anchor_host, in_anchor_host;
    volatile uint64_t out_frames, in_frames; /* frames the service consumed / produced so far */
    volatile uint32_t out_late, in_late;     /* periods the service had to send or deliver without fresh data */
    volatile uint32_t out_seq, in_seq;       /* incremented around anchor updates (odd while writing) */
    volatile uint64_t out_written;           /* the plug-in: the sample time after the last frame it wrote */
    volatile uint64_t in_written;            /* the service: the sample time after the last captured frame in the ring */
    volatile uint64_t out_written_host;      /* the plug-in: host time of its last write */
    volatile uint32_t out_holes;             /* the service: written periods that held a run of 64 or more silent frames */
    /* gts7xlwifi: the output volume, applied by the amplifiers' digital volume (0.125 dB steps in their own wide path) rather than by
     * scaling the 16-bit samples, which left quiet settings 3-4 bits deep (int15n: -57 dB, DSP peak 11). The plug-in writes
     * out_volume_mdb (milli-dB, T970_AUDIO_MUTE_MDB mutes), then increments out_volume_seq; the service applies each new seq. */
    volatile int32_t out_volume_mdb;
    volatile uint32_t out_volume_seq;
    volatile uint64_t out_taken;             /* the service: the playback frame it takes next (was in_written before capture) */
} T970AudioControl;
#define T970_AUDIO_MUTE_MDB (-200000)

/* Second half of the control page: the plug-in records what the audio server asked for and it does not answer (selector, object id
 * << 24 | scope fourcc's first byte), so the kernel log names them without the unified log. */
/* gts7xlwifi capture (test_flags bit 2, t970-audio bit 262144): the plug-in stores 256 frames of the audio server's raw mix
 * (L, R as x32767, rounded and clamped) from the first write at >= 20 s on the timeline whose peak reaches 0.1, starting 64 frames
 * before the peak, once the service's run has played 2 s (test_flags bit 8, set by the service); t is their sample time. The service
 * then records the same 256 timeline frames as it takes them from the ring. */
#define T970_AUDIO_CAP_OFFSET 960u
#define T970_AUDIO_CAP_FRAMES 256u
typedef struct {
    volatile uint32_t state;                 /* 0 armed, 1 stored */
    volatile uint32_t write_frames;          /* the write's frame count */
    volatile uint64_t t;
    volatile int32_t peak;                   /* x32767 */
    volatile uint32_t at;                    /* index of the first stored frame in the write */
    volatile int16_t s[2 * T970_AUDIO_CAP_FRAMES];
} T970AudioCapture;

#define T970_AUDIO_DIAG_OFFSET 2048u
#define T970_AUDIO_DIAG_SLOTS 32u
typedef struct {
    volatile uint32_t count, loads;
    volatile uint32_t selector[T970_AUDIO_DIAG_SLOTS], where[T970_AUDIO_DIAG_SLOTS];
    volatile uint32_t volume_milli, volume_sets;  /* the output volume (scalar x 1000) and how often the audio server set it */
    volatile uint32_t mix_peak;                   /* the largest output sample before the volume, x 32767 */
    volatile uint32_t in_misses;                  /* input reads that asked for frames the service had not captured yet */
    /* How far ahead of the play position the audio server writes (output time minus now, frames) and how far behind it reads, the
     * least and most of each, and the frames of the last write. */
    volatile int32_t ahead_min, ahead_max, behind_min, behind_max;
    volatile uint32_t io_frames, writes;
    /* Writes that began inside frames already written (another IO context of the audio server mixing into the same ring), and the most
     * frames such a write went back. */
    volatile uint32_t overlaps, overlap_max;
    /* Every frame written; writes that began past the end of the one before (a skipped stretch) and the frames skipped; the time from
     * one write to the next in buckets below 15, 25, 40, 80 and 160 ms and above (one IO cycle of 1024 frames is 21.3 ms). */
    volatile uint32_t frames_total, gaps, gap_frames;
    volatile uint32_t interval[6];
    volatile uint64_t last_end, last_host;
    volatile int32_t skew_min, skew_max;          /* the audio server's current sample time minus the timeline's, least and most */
    /* The audio server's IO thread as the plug-in sees it: current and base priority, scheduling policy, whether it has a time constraint
     * and that constraint's period, computation and constraint (host ticks); the lowest current priority seen. */
    volatile int32_t io_pri, io_base, io_policy, io_tc, io_pri_min;
    volatile uint32_t io_period, io_computation, io_constraint;
    /* CoreAudio's host clock frequency (Hz) after the plug-in asked the service to set it, and that request's result (0 done, -1 the
     * constant was not found, else the kernel's return). */
    volatile uint32_t host_clock;
    volatile int32_t host_clock_result;
    /* gts7xlwifi volume trace: the last set (kind 1 scalar, 2 decibels; its raw value x1000; the element), the gain the plug-in applies
     * now (linear x10000) and the least and most applied during IO, and the last 8 sets as kind << 30 | (raw x1000 + 2^29). */
    volatile int32_t vol_kind, vol_raw_milli, vol_element;
    volatile uint32_t gain_now, gain_min, gain_max;
    volatile uint32_t vol_history[8], vol_history_count;
    /* capture: input reads, reads that asked for frames the service had not captured yet (they read silence), the least and most
     * distance of the read's end behind the captured end (frames) */
    volatile uint32_t mic_reads, mic_short;
    volatile int32_t mic_behind_min, mic_behind_max;
} T970AudioDiag;
/* After it, every distinct property access in the order the audio server made it: selector, then object << 24 | scope's first byte << 16 |
 * kind (1 set, 0 get) | 2 if it failed. */
#define T970_AUDIO_TRACE_OFFSET 2560u
#define T970_AUDIO_TRACE_SLOTS 128u
typedef struct {
    volatile uint32_t count;
    volatile uint32_t entry[T970_AUDIO_TRACE_SLOTS][2];
} T970AudioTrace;

#endif
