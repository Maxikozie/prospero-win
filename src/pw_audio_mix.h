/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_AUDIO_MIX_H
#define PW_AUDIO_MIX_H
#include "../include/prospero_win.h"

/* The mixer of Wine's PS5 audio driver (wine/wineps5): it turns the streams
 * Windows programs play, in whatever PCM format each one chose, into the
 * console's one output format, 16-bit stereo at 48 kHz.
 *
 * Each stream reads from a ring of frames its driver fills (mmdevapi's
 * local buffer). Mixing converts every sample to float, applies the
 * stream's per-channel gain, folds the channels down to stereo by speaker
 * position, resamples linearly to 48 kHz and adds the result to a float
 * accumulator; store then clamps the sum to 16 bits. A stream that runs out
 * of frames adds silence and resumes where it stopped.
 *
 * Portable: no allocation and no locking. The driver owns the buffers,
 * serialises access to a stream and runs the output thread. */

enum {
    PW_AUDIO_MIX_RATE=48000,
    PW_AUDIO_MIX_MAX_CHANNELS=8,
    PW_AUDIO_MIX_MIN_RATE=1000,PW_AUDIO_MIX_MAX_RATE=384000,
    /* PCM: 8-bit unsigned, 16/24/32-bit signed little-endian; FLOAT: 32-bit */
    PW_AUDIO_MIX_PCM=1,PW_AUDIO_MIX_FLOAT=3,
};

typedef struct PwAudioMixFormat {
    uint32_t rate;              /* frames per second */
    uint16_t channels;          /* 1..PW_AUDIO_MIX_MAX_CHANNELS */
    uint16_t bits;              /* container bits per sample */
    uint16_t block_align;       /* bytes per frame: channels * bits / 8 */
    uint16_t encoding;          /* PW_AUDIO_MIX_PCM or PW_AUDIO_MIX_FLOAT */
    uint32_t channel_mask;      /* WAVEFORMATEXTENSIBLE speaker bits; 0 takes the usual layout */
} PwAudioMixFormat;

typedef struct PwAudioMixStream {
    PwAudioMixFormat format;
    const uint8_t *frames;      /* ring of capacity frames, block_align bytes each */
    uint32_t capacity;
    uint32_t offset,held;       /* the oldest unplayed frame, and how many there are */
    uint64_t consumed;          /* frames taken from the ring since init or reset */
    float gain[PW_AUDIO_MIX_MAX_CHANNELS];
    float left[PW_AUDIO_MIX_MAX_CHANNELS],right[PW_AUDIO_MIX_MAX_CHANNELS];
    uint64_t step,phase;        /* input frames per output frame, and position: 32.32 */
    float previous[2],current[2];
} PwAudioMixStream;

/* PW_OK when the mixer can play the format; PW_ERR_UNSUPPORTED otherwise. */
int pw_audio_mix_format_check(const PwAudioMixFormat *format);
/* A stream of format reading the ring frames[capacity], empty, at unity gain. */
int pw_audio_mix_stream_init(PwAudioMixStream *stream,const PwAudioMixFormat *format,
                             const uint8_t *frames,uint32_t capacity);
/* Forget the held frames and the resampler's history (a stopped stream's Reset). */
void pw_audio_mix_stream_reset(PwAudioMixStream *stream);
/* Play the same frames at another rate (IAudioClockAdjustment). */
int pw_audio_mix_set_rate(PwAudioMixStream *stream,uint32_t rate);
/* One gain per channel of the stream's format, 0 (silent) to 1 (unity). */
int pw_audio_mix_set_gain(PwAudioMixStream *stream,const float *gain,uint32_t count);

/* Add frames 48 kHz stereo frames of the stream to accumulator (left,right
 * pairs). Returns how many of them had data: fewer than frames when the
 * ring ran out. */
uint32_t pw_audio_mix_add(PwAudioMixStream *stream,float *accumulator,uint32_t frames);
/* Clamp frames accumulated stereo frames into 16-bit output. */
void pw_audio_mix_store(const float *accumulator,int16_t *output,uint32_t frames);

/* Silence inside a sound. A started stream whose ring runs out during a
 * grain leaves part of it silent. A stretch of such grains ends either with
 * more data while the stream is still started (an underrun: the program was
 * late and the silence fell inside its sound) or with a stop or reset (a
 * drain: the sound's own end, before the program stopped the stream; winmm
 * stops its client only once the last block has played). Between two sounds
 * no stream is started and nothing is counted. The driver keeps one
 * PwAudioStarve per stream and one PwAudioStarveStats, under its lock. */
typedef struct PwAudioStarve {
    uint32_t grains;            /* short grains in the current stretch, 0 when there is none */
    uint32_t frames;            /* frames missing in them */
    uint64_t first;             /* the grain number the stretch began at */
} PwAudioStarve;

typedef struct PwAudioStarveStats {
    uint64_t grains;            /* grains mixed: the driver counts them */
    uint64_t starved;           /* stream grains that came up short */
    uint64_t underruns,underrun_grains;
    uint64_t drains,drain_grains;
} PwAudioStarveStats;

/* got of frames had data in grain number grain of a started stream. */
void pw_audio_starve_grain(PwAudioStarve *starve,PwAudioStarveStats *stats,uint32_t got,uint32_t frames,
                           uint64_t grain);
/* More data for a started stream: 1 when it ended a stretch as an underrun
 * (the stretch copied to *ended when ended is not NULL), else 0. */
int pw_audio_starve_data(PwAudioStarve *starve,PwAudioStarveStats *stats,PwAudioStarve *ended);
/* The stream stopped, was reset or released: a stretch ends as a drain. */
void pw_audio_starve_stop(PwAudioStarve *starve,PwAudioStarveStats *stats);

#endif
