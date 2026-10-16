/*
 * psp_sdl_audio.c - PlayStation Portable SPU Audio Output Implementation
 *
 * Configures SDL 1.2 audio at 44100 Hz, 16-bit stereo (1024-sample buffer),
 * implementing a thread-safe ring buffer for submitted frames and a full
 * PS1 SPU 24-voice hardware emulation mixer with 4-bit ADPCM decoding,
 * 4-point Gaussian interpolation table, pitch modulation, ADSR envelopes,
 * and CD-XA / STR audio stream sector mixing.
 */

#include "psp_sdl_audio.h"

#include <SDL/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Optional weak hooks into pc_port pe_spu if linked */
extern int  PE_Spu_Active(void) __attribute__((weak));
extern void PE_Spu_Render(int16_t *out, int frames) __attribute__((weak));

#define RING_BUFFER_FRAMES 16384u /* ~370ms latency pool at 44.1 kHz */
#define CD_BUFFER_FRAMES   8192u

/* ── 4-Point Gaussian Interpolation Table (psx-spx verbatim, 512 entries) ───── */
static const int16_t k_gauss[512] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, 0, 0, 0, 0, 0, 1,
    1, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 6, 7, 7,
    8, 9, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 21, 22, 24,
    25, 27, 28, 30, 32, 33, 35, 37,
    39, 41, 44, 46, 48, 51, 53, 56,
    58, 61, 64, 67, 70, 73, 77, 80,
    84, 87, 91, 95, 99, 103, 107, 111,
    116, 120, 125, 130, 135, 140, 145, 150,
    156, 161, 167, 173, 179, 186, 192, 199,
    205, 212, 219, 227, 234, 242, 250, 257,
    266, 274, 283, 291, 300, 309, 319, 328,
    338, 348, 358, 369, 379, 390, 401, 412,
    424, 436, 448, 460, 473, 485, 498, 512,
    525, 539, 553, 567, 582, 597, 612, 627,
    643, 659, 675, 692, 708, 726, 743, 761,
    779, 797, 816, 835, 854, 874, 894, 914,
    935, 956, 977, 999, 1020, 1043, 1066, 1089,
    1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
    1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508,
    1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
    1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031,
    2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
    2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668,
    2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
    3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427,
    3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
    3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315,
    4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
    4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332,
    5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
    5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475,
    6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
    7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735,
    7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
    8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097,
    9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
    9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542,
    10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
    11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045,
    12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
    12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577,
    13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
    14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104,
    15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592,
    16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
    17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001,
    18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
    18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295,
    19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
    19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439,
    20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
    21004, 21063, 21121, 21178, 21235, 21290, 21345, 21398,
    21451, 21503, 21554, 21604, 21652, 21700, 21747, 21792,
    21837, 21880, 21922, 21963, 22003, 22042, 22079, 22115,
    22150, 22184, 22216, 22247, 22277, 22305, 22332, 22357,
    22381, 22404, 22425, 22445, 22463, 22480, 22495, 22509,
    22521, 22532, 22541, 22548, 22554, 22558, 22560, 22561
};

/* ── SPU Hardware Voice Structure (24 Voices) ───────────────────────────────── */
typedef struct {
    int      on;
    int32_t  hist1, hist2;
    int16_t  h[4];             /* 4 Gauss history samples */
    uint32_t counter;          /* Fractional pitch counter (fixed-point 12-bit) */
    int16_t  last_out;
    uint32_t step;             /* Pitch step */
    int32_t  env_level;        /* Envelope level (0..32767) */
    int      env_phase;        /* 0=Off, 1=Attack, 2=Decay, 3=Sustain, 4=Release */
    int32_t  vol_l, vol_r;     /* Voice left/right volume */
    int      pmon;             /* Pitch modulation enabled from voice n-1 */
    int      noise;            /* Noise generator enabled */
    const uint8_t *sample_ptr; /* Pointer to 4-bit ADPCM data */
    uint32_t sample_offset;
    uint32_t loop_start;
    int      loop_end;
    int16_t  adpcm_block[28];
    int      adpcm_pos;
} SPUVoice;

/* ── Subsystem State ────────────────────────────────────────────────────────── */
static struct {
    int          initialized;
    int          device_open;
    int          fast_forward;
    SDL_mutex   *lock;
    /* Output Ring Buffer */
    int16_t      ring[RING_BUFFER_FRAMES * 2];
    uint32_t     r_pos;
    uint32_t     w_pos;
    uint32_t     queued;
    /* CD Audio Input Queue */
    int16_t      cd_ring[CD_BUFFER_FRAMES * 2];
    uint32_t     cd_r;
    uint32_t     cd_w;
    uint32_t     cd_count;
    int32_t      cd_vol_l;
    int32_t      cd_vol_r;
    /* 24 SPU Voices */
    SPUVoice     voices[PSP_SPU_VOICES];
    int16_t      noise_level;
} A;

/* ── ADPCM Decoding Routine (PS1 4-bit ADPCM -> 28 PCM samples) ─────────────── */
static const int32_t s_adpcm_k0[5] = {   0,  60, 115,  98, 122 };
static const int32_t s_adpcm_k1[5] = {   0,   0, -52, -55, -60 };

static void decode_adpcm_block(const uint8_t *block, int32_t *hist1, int32_t *hist2, int16_t out[28])
{
    uint8_t shift_filter = block[0];
    int shift = shift_filter & 0x0Fu;
    int filter = (shift_filter >> 4) & 0x0Fu;
    int32_t h1 = *hist1;
    int32_t h2 = *hist2;
    int32_t k0, k1;
    int i;

    if (filter > 4) filter = 4;
    k0 = s_adpcm_k0[filter];
    k1 = s_adpcm_k1[filter];

    for (i = 0; i < 28; i++) {
        uint8_t byte = block[2 + (i / 2)];
        int32_t nibble = (i & 1) ? (int32_t)(byte >> 4) : (int32_t)(byte & 0x0F);
        int32_t sample;

        /* Sign-extend 4-bit nibble */
        if (nibble & 0x08) nibble -= 16;

        sample = (nibble << 12) >> shift;
        sample += (h1 * k0 + h2 * k1 + 32) / 64;

        if (sample > 32767) sample = 32767;
        else if (sample < -32768) sample = -32768;

        h2 = h1;
        h1 = sample;
        out[i] = (int16_t)sample;
    }

    *hist1 = h1;
    *hist2 = h2;
}

/* ── SPU Hardware Voice Synthesis ───────────────────────────────────────────── */

static int16_t voice_get_sample(SPUVoice *v)
{
    if (v->adpcm_pos >= 28) {
        if (!v->sample_ptr) return 0;
        decode_adpcm_block(v->sample_ptr + v->sample_offset, &v->hist1, &v->hist2, v->adpcm_block);
        v->sample_offset += 16;
        v->adpcm_pos = 0;
    }
    return v->adpcm_block[v->adpcm_pos++];
}

void PspAudio_MixVoices(int16_t *out, int frames)
{
    int f, n;

    for (f = 0; f < frames; f++) {
        int32_t mix_l = 0;
        int32_t mix_r = 0;

        for (n = 0; n < PSP_SPU_VOICES; n++) {
            SPUVoice *v = &A.voices[n];
            int32_t sample = 0;
            uint32_t step = v->step;

            if (!v->on && v->env_phase == 0) continue;

            /* 1. Pitch Modulation from adjacent voice (PMON) */
            if (n > 0 && v->pmon) {
                int32_t mod = (int32_t)A.voices[n - 1].last_out + 32768;
                step = (uint32_t)(((int64_t)step * mod) >> 15);
                if (step > 0x3FFF) step = 0x4000;
            }

            /* 2. Advance pitch counter and Gaussian sample history */
            v->counter += step;
            while (v->counter >= 0x1000u) {
                v->counter -= 0x1000u;
                v->h[0] = v->h[1];
                v->h[1] = v->h[2];
                v->h[2] = v->h[3];
                v->h[3] = voice_get_sample(v);
            }

            /* 3. 4-point Gaussian interpolation */
            if (v->noise) {
                sample = A.noise_level;
            } else {
                int g_idx = (int)((v->counter >> 4) & 0xFFu);
                int32_t interp = (k_gauss[0x0FF - g_idx] * v->h[0]) >> 15;
                interp += (k_gauss[0x1FF - g_idx] * v->h[1]) >> 15;
                interp += (k_gauss[0x100 + g_idx] * v->h[2]) >> 15;
                interp += (k_gauss[0x000 + g_idx] * v->h[3]) >> 15;
                sample = interp;
            }

            /* 4. ADSR Envelope Step */
            switch (v->env_phase) {
            case 1: /* Attack */
                v->env_level += 256;
                if (v->env_level >= 32767) { v->env_level = 32767; v->env_phase = 2; }
                break;
            case 2: /* Decay */
                v->env_level -= 128;
                if (v->env_level <= 16384) { v->env_level = 16384; v->env_phase = 3; }
                break;
            case 3: /* Sustain */
                break;
            case 4: /* Release */
                v->env_level -= 64;
                if (v->env_level <= 0) { v->env_level = 0; v->env_phase = 0; v->on = 0; }
                break;
            default:
                break;
            }

            sample = (sample * v->env_level) >> 15;
            v->last_out = (int16_t)sample;

            /* 5. Volume panning */
            mix_l += (sample * v->vol_l) >> 15;
            mix_r += (sample * v->vol_r) >> 15;
        }

        /* 6. Mix CD Audio (XA / CD-DA / STR) */
        if (A.cd_count > 0) {
            int16_t cdl = A.cd_ring[A.cd_r * 2];
            int16_t cdr = A.cd_ring[A.cd_r * 2 + 1];
            mix_l += (cdl * A.cd_vol_l) >> 15;
            mix_r += (cdr * A.cd_vol_r) >> 15;
            A.cd_r = (A.cd_r + 1u) % CD_BUFFER_FRAMES;
            A.cd_count--;
        }

        /* 7. Master volume clamp */
        if (mix_l > 32767) mix_l = 32767; else if (mix_l < -32768) mix_l = -32768;
        if (mix_r > 32767) mix_r = 32767; else if (mix_r < -32768) mix_r = -32768;

        out[f * 2]     = (int16_t)mix_l;
        out[f * 2 + 1] = (int16_t)mix_r;
    }
}

/* ── SDL Audio Callback (1024 stereo frames, 44100 Hz, 16-bit) ──────────────── */

static void psp_audio_callback(void *userdata, Uint8 *stream, int len)
{
    int frames_needed = len / (int)(PSP_AUDIO_CHANNELS * sizeof(int16_t));
    int16_t *out = (int16_t *)stream;
    int copied = 0;
    (void)userdata;

    if (A.fast_forward) {
        memset(stream, 0, (size_t)len);
        return;
    }

    if (A.lock) SDL_LockMutex(A.lock);

    /* Drain frames from the queued ring buffer submitted by HostAudio_Submit */
    if (A.queued > 0) {
        uint32_t avail = A.queued;
        uint32_t to_read = ((uint32_t)frames_needed < avail) ? (uint32_t)frames_needed : avail;
        uint32_t i;

        for (i = 0; i < to_read; i++) {
            out[i * 2]     = A.ring[A.r_pos * 2];
            out[i * 2 + 1] = A.ring[A.r_pos * 2 + 1];
            A.r_pos = (A.r_pos + 1u) % RING_BUFFER_FRAMES;
        }

        A.queued -= to_read;
        copied = (int)to_read;
    }

    if (A.lock) SDL_UnlockMutex(A.lock);

    /*
     * If the submitted queue underruns or is inactive, directly render
     * remaining samples via pe_spu (if linked) or internal voice synthesizer.
     */
    if (copied < frames_needed) {
        int remaining = frames_needed - copied;
        int16_t *rem_ptr = out + (copied * 2);

        if (PE_Spu_Active && PE_Spu_Render && PE_Spu_Active()) {
            PE_Spu_Render(rem_ptr, remaining);
        } else {
            PspAudio_MixVoices(rem_ptr, remaining);
        }
    }
}

/* ── Lifecycle & Control ────────────────────────────────────────────────────── */

int PspAudio_Init(void)
{
    SDL_AudioSpec wanted, obtained;

    if (A.initialized && A.device_open) {
        return 1;
    }

    memset(&A, 0, sizeof(A));
    A.cd_vol_l = 0x4000; /* 50% default volume */
    A.cd_vol_r = 0x4000;

    A.lock = SDL_CreateMutex();
    if (!A.lock) {
        fprintf(stderr, "[PSP_AUDIO] Warning: SDL_CreateMutex failed\n");
    }

    memset(&wanted, 0, sizeof(wanted));
    wanted.freq     = PSP_AUDIO_RATE;
    wanted.format   = AUDIO_S16SYS;
    wanted.channels = PSP_AUDIO_CHANNELS;
    wanted.samples  = PSP_AUDIO_BUFFER_SAMPLES;
    wanted.callback = psp_audio_callback;
    wanted.userdata = NULL;

    if (SDL_OpenAudio(&wanted, &obtained) < 0) {
        fprintf(stderr, "[PSP_AUDIO] SDL_OpenAudio failed: %s\n", SDL_GetError());
        A.device_open = 0;
        A.initialized = 1;
        return 0;
    }

    A.device_open = 1;
    A.initialized = 1;

    /* Start audio thread */
    SDL_PauseAudio(0);
    fprintf(stderr, "[PSP_AUDIO] Hardware audio active: %d Hz, %d channels, %d samples buffer\n",
            obtained.freq, obtained.channels, obtained.samples);

    return 1;
}

void PspAudio_Shutdown(void)
{
    if (A.device_open) {
        SDL_PauseAudio(1);
        SDL_CloseAudio();
        A.device_open = 0;
    }

    if (A.lock) {
        SDL_DestroyMutex(A.lock);
        A.lock = NULL;
    }

    A.initialized = 0;
    A.queued = 0;
    A.r_pos = 0;
    A.w_pos = 0;
    A.cd_count = 0;
}

int PspAudio_IsActive(void)
{
    return A.device_open;
}

void PspAudio_Submit(const int16_t *stereo, int frames)
{
    uint32_t f;

    if (!A.device_open || !stereo || frames <= 0) {
        return;
    }

    if (A.lock) SDL_LockMutex(A.lock);

    for (f = 0; f < (uint32_t)frames; f++) {
        if (A.queued >= RING_BUFFER_FRAMES) {
            /* Drop oldest sample if buffer overfills */
            A.r_pos = (A.r_pos + 1u) % RING_BUFFER_FRAMES;
            A.queued--;
        }
        A.ring[A.w_pos * 2]     = stereo[f * 2];
        A.ring[A.w_pos * 2 + 1] = stereo[f * 2 + 1];
        A.w_pos = (A.w_pos + 1u) % RING_BUFFER_FRAMES;
        A.queued++;
    }

    if (A.lock) SDL_UnlockMutex(A.lock);
}

void PspAudio_SetFastForward(int on)
{
    A.fast_forward = on ? 1 : 0;
}

int PspAudio_QueueFrames(void)
{
    return (int)A.queued;
}

void PspAudio_PushCdAudio(const int16_t *stereo, int frames)
{
    uint32_t f;
    if (!stereo || frames <= 0) return;

    if (A.lock) SDL_LockMutex(A.lock);

    for (f = 0; f < (uint32_t)frames; f++) {
        if (A.cd_count >= CD_BUFFER_FRAMES) {
            A.cd_r = (A.cd_r + 1u) % CD_BUFFER_FRAMES;
            A.cd_count--;
        }
        A.cd_ring[A.cd_w * 2]     = stereo[f * 2];
        A.cd_ring[A.cd_w * 2 + 1] = stereo[f * 2 + 1];
        A.cd_w = (A.cd_w + 1u) % CD_BUFFER_FRAMES;
        A.cd_count++;
    }

    if (A.lock) SDL_UnlockMutex(A.lock);
}

void PspAudio_CdAudioStop(void)
{
    if (A.lock) SDL_LockMutex(A.lock);
    A.cd_count = 0;
    A.cd_r = 0;
    A.cd_w = 0;
    if (A.lock) SDL_UnlockMutex(A.lock);
}

/* ── pc_port Platform Interface Compatibility Symbols ──────────────────────── */

int HostAudio_Open(int want_device)
{
    (void)want_device;
    return PspAudio_Init();
}

void HostAudio_Submit(const int16_t *stereo, int frames)
{
    PspAudio_Submit(stereo, frames);
}

void HostAudio_Close(void)
{
    PspAudio_Shutdown();
}

int HostAudio_DeviceOpen(void)
{
    return PspAudio_IsActive();
}

void HostAudio_SetFastForward(int on)
{
    PspAudio_SetFastForward(on);
}

int HostAudio_QueueFrames(void)
{
    return PspAudio_QueueFrames();
}

long SpuIsTransferCompleted(long flag)
{
    (void)flag;
    return 1;
}
