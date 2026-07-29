#include <alsa/asoundlib.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rom.h"

// ALSA output backend for Linux. The emulator writes into a lock-free ring;
// a dedicated thread drains it into ALSA so a slow device never blocks a core.
#define RING_FRAMES 32768u
#define RING_MASK   (RING_FRAMES - 1u)
#define CHUNK_FRAMES 512u

struct AudioCtx {
    snd_pcm_t *pcm;
    pthread_t thread;
    int16_t  *ring;                 // interleaved stereo
    atomic_uint rd, wr;             // frame counters (free-running)
    atomic_int volume;              // 0..100
    atomic_bool running;
};

static void *playback_thread(void *ref) {
    AudioCtx *a = ref;
    int16_t out[CHUNK_FRAMES * 2];

    while (atomic_load_explicit(&a->running, memory_order_acquire)) {
        unsigned rd = atomic_load_explicit(&a->rd, memory_order_relaxed);
        unsigned wr = atomic_load_explicit(&a->wr, memory_order_acquire);
        unsigned n = wr - rd;
        if (n == 0) {
            usleep(1000);
            continue;
        }
        if (n > CHUNK_FRAMES) n = CHUNK_FRAMES;

        int vol = atomic_load_explicit(&a->volume, memory_order_relaxed);
        for (unsigned i = 0; i < n; i++) {
            unsigned idx = (rd + i) & RING_MASK;
            int l = a->ring[idx * 2], r = a->ring[idx * 2 + 1];
            if (vol != 100) { l = l * vol / 100; r = r * vol / 100; }
            out[i * 2] = (int16_t)l;
            out[i * 2 + 1] = (int16_t)r;
        }

        snd_pcm_sframes_t wrote = snd_pcm_writei(a->pcm, out, n);
        if (wrote < 0) {
            if (snd_pcm_recover(a->pcm, (int)wrote, 1) < 0) usleep(5000);
            continue;
        }
        if (wrote > 0) {
            unsigned next = rd + (unsigned)wrote;
            atomic_compare_exchange_strong_explicit(
                &a->rd, &rd, next, memory_order_release, memory_order_relaxed);
        }
    }
    return NULL;
}

int audio_start(AudioCtx **out, double sample_rate) {
    AudioCtx *a = calloc(1, sizeof *a);
    if (!a) return -1;
    a->ring = calloc(RING_FRAMES * 2, sizeof(int16_t));
    if (!a->ring) { free(a); return -1; }
    atomic_init(&a->rd, 0);
    atomic_init(&a->wr, 0);
    atomic_init(&a->volume, 100);
    atomic_init(&a->running, false);

    unsigned rate = sample_rate > 0 ? (unsigned)(sample_rate + 0.5) : 44100u;
    if (snd_pcm_open(&a->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0)
        goto fail;
    if (snd_pcm_set_params(a->pcm, SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1, 50000) < 0)
        goto fail_pcm;

    atomic_store_explicit(&a->running, true, memory_order_release);
    if (pthread_create(&a->thread, NULL, playback_thread, a) != 0) {
        atomic_store_explicit(&a->running, false, memory_order_release);
        goto fail_pcm;
    }
    *out = a;
    return 0;

fail_pcm:
    snd_pcm_close(a->pcm);
fail:
    free(a->ring);
    free(a);
    return -1;
}

void audio_stop(AudioCtx *a) {
    if (!a) return;
    atomic_store_explicit(&a->running, false, memory_order_release);
    pthread_join(a->thread, NULL);
    snd_pcm_drop(a->pcm);
    snd_pcm_close(a->pcm);
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
    atomic_store_explicit(&a->rd,
                          atomic_load_explicit(&a->wr, memory_order_acquire),
                          memory_order_release);
}
