#ifndef PSP_SDL_AUDIO_H
#define PSP_SDL_AUDIO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSP_AUDIO_RATE           44100
#define PSP_AUDIO_CHANNELS       2
#define PSP_AUDIO_BUFFER_SAMPLES 1024
#define PSP_SPU_VOICES           24

/* Subsystem lifecycle */
int  PspAudio_Init(void);
void PspAudio_Shutdown(void);
int  PspAudio_IsActive(void);

/* Audio Submission (Pushing 44.1 kHz stereo int16 samples into playback queue) */
void PspAudio_Submit(const int16_t *stereo, int frames);
void PspAudio_SetFastForward(int on);
int  PspAudio_QueueFrames(void);

/* CD-XA / STR / CD-DA streaming audio input */
void PspAudio_PushCdAudio(const int16_t *stereo, int frames);
void PspAudio_PushCdAudioSector(const uint8_t *sector_raw);
void PspAudio_CdAudioStop(void);

/*
 * Direct SPU Voice Synthesizer Hook:
 * Decodes 4-bit ADPCM blocks with Gauss interpolation, pitch modulation,
 * ADSR envelopes, and CD audio mixing into the output buffer.
 */
void PspAudio_MixVoices(int16_t *out, int frames);

/* Direct drop-in compatibility symbols for pc_port host_audio.h */
int  HostAudio_Open(int want_device);
void HostAudio_Submit(const int16_t *stereo, int frames);
void HostAudio_Close(void);
int  HostAudio_DeviceOpen(void);
void HostAudio_SetFastForward(int on);
int  HostAudio_QueueFrames(void);

/* SPU DMA non-blocking completion poll */
long SpuIsTransferCompleted(long flag);

#ifdef __cplusplus
}
#endif

#endif /* PSP_SDL_AUDIO_H */
