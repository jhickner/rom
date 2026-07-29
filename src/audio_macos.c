#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "rom.h"

// CoreAudio output backend for macOS.
#define RING_FRAMES 32768u          // power of two; ~1s at 32kHz
#define RING_MASK   (RING_FRAMES - 1u)

struct AudioCtx {
    AudioUnit unit;
    int16_t  *ring;                 // interleaved stereo
    atomic_uint rd, wr;             // frame counters (free-running)
    atomic_int  volume;             // 0..100
    bool running;
};

static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *flags,
                          const AudioTimeStamp *ts, UInt32 bus,
                          UInt32 nframes, AudioBufferList *io) {
    (void)flags; (void)ts; (void)bus;
    AudioCtx *a = (AudioCtx *)ref;
    int16_t *out = (int16_t *)io->mBuffers[0].mData;

    unsigned rd = atomic_load_explicit(&a->rd, memory_order_relaxed);
    unsigned wr = atomic_load_explicit(&a->wr, memory_order_acquire);
    unsigned avail = wr - rd;
    unsigned n = nframes < avail ? nframes : avail;
    int vol = atomic_load_explicit(&a->volume, memory_order_relaxed);

    for (unsigned i = 0; i < n; i++) {
        unsigned idx = (rd + i) & RING_MASK;
        int l = a->ring[idx * 2], r = a->ring[idx * 2 + 1];
        if (vol != 100) { l = l * vol / 100; r = r * vol / 100; }
        out[i * 2] = (int16_t)l;
        out[i * 2 + 1] = (int16_t)r;
    }
    // Underrun: pad with silence rather than glitching.
    for (unsigned i = n; i < nframes; i++) {
        out[i * 2] = 0;
        out[i * 2 + 1] = 0;
    }
    unsigned next = rd + n;
    atomic_compare_exchange_strong_explicit(
        &a->rd, &rd, next, memory_order_release, memory_order_relaxed);
    return noErr;
}

int audio_start(AudioCtx **out, double sample_rate) {
    AudioCtx *a = calloc(1, sizeof *a);
    if (!a) return -1;
    a->ring = calloc(RING_FRAMES * 2, sizeof(int16_t));
    if (!a->ring) { free(a); return -1; }
    atomic_init(&a->rd, 0);
    atomic_init(&a->wr, 0);
    atomic_init(&a->volume, 100);

    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) goto fail;
    if (AudioComponentInstanceNew(comp, &a->unit) != noErr) goto fail;

    AudioStreamBasicDescription asbd;
    memset(&asbd, 0, sizeof asbd);
    asbd.mSampleRate = sample_rate > 0 ? sample_rate : 44100.0;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mChannelsPerFrame = 2;
    asbd.mBitsPerChannel = 16;
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = 4;
    asbd.mBytesPerPacket = 4;
    if (AudioUnitSetProperty(a->unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &asbd, sizeof asbd) != noErr)
        goto fail_unit;

    AURenderCallbackStruct cb = { .inputProc = render_cb, .inputProcRefCon = a };
    if (AudioUnitSetProperty(a->unit, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr)
        goto fail_unit;
    if (AudioUnitInitialize(a->unit) != noErr) goto fail_unit;
    if (AudioOutputUnitStart(a->unit) != noErr) {
        AudioUnitUninitialize(a->unit);
        goto fail_unit;
    }
    a->running = true;
    *out = a;
    return 0;

fail_unit:
    AudioComponentInstanceDispose(a->unit);
fail:
    free(a->ring);
    free(a);
    return -1;
}

void audio_stop(AudioCtx *a) {
    if (!a) return;
    if (a->running) {
        AudioOutputUnitStop(a->unit);
        AudioUnitUninitialize(a->unit);
        AudioComponentInstanceDispose(a->unit);
    }
    free(a->ring);
    free(a);
}

size_t audio_write(AudioCtx *a, const int16_t *pcm, size_t frames) {
    if (!a) return frames;
    unsigned wr = atomic_load_explicit(&a->wr, memory_order_relaxed);
    unsigned rd = atomic_load_explicit(&a->rd, memory_order_acquire);
    unsigned space = RING_FRAMES - (wr - rd);
    if (frames > space) frames = space;
    for (size_t i = 0; i < frames; i++) {
        unsigned idx = (wr + (unsigned)i) & RING_MASK;
        a->ring[idx * 2] = pcm[i * 2];
        a->ring[idx * 2 + 1] = pcm[i * 2 + 1];
    }
    atomic_store_explicit(&a->wr, wr + (unsigned)frames, memory_order_release);
    return frames;
}

size_t audio_queued_frames(AudioCtx *a) {
    if (!a) return 0;
    unsigned wr = atomic_load_explicit(&a->wr, memory_order_acquire);
    unsigned rd = atomic_load_explicit(&a->rd, memory_order_acquire);
    return wr - rd;
}

size_t audio_free_frames(AudioCtx *a) {
    if (!a) return RING_FRAMES;
    return RING_FRAMES - audio_queued_frames(a);
}

void audio_set_volume(AudioCtx *a, int vol) {
    if (!a) return;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    atomic_store_explicit(&a->volume, vol, memory_order_relaxed);
}

void audio_flush(AudioCtx *a) {
    if (!a) return;
    atomic_store_explicit(&a->rd, atomic_load_explicit(&a->wr, memory_order_acquire),
                          memory_order_release);
}
