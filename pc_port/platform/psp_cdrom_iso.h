#ifndef PSP_CDROM_ISO_H
#define PSP_CDROM_ISO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PSP CD-ROM & ISO Stream Subsystem
 *
 * Provides asynchronous sector reading (256 KB ring buffer) from raw
 * BIN/CUE / ISO images on the PSP Memory Stick, dual-disc hot-swapping
 * between Disc 1 and Disc 2, and in-place MDEC / STR FMV video decoding
 * directly into sceGu texture buffers.
 */

#define PSP_CDROM_SECTOR_RAW     2352u  /* Raw MODE2/2352 sector size */
#define PSP_CDROM_SECTOR_USER    2048u  /* User payload per sector */
#define PSP_CDROM_USER_OFFSET    24u    /* MODE2 sync(12) + header(4) + subheader(8) */

/* 256 KB Asynchronous Sector Ring Buffer (110 raw sectors = 258,720 bytes) */
#define PSP_CDROM_RING_SECTORS   110u
#define PSP_CDROM_RING_BYTES     (PSP_CDROM_RING_SECTORS * PSP_CDROM_SECTOR_RAW)

/* Memory Stick Paths */
#define PSP_DISC_DIR             "ms0:/PSP/GAME/ParasiteEve/disc"
#define PSP_DISC1_DEFAULT_PATH   "ms0:/PSP/GAME/ParasiteEve/disc/Parasite Eve (USA) (Disc 1).bin"
#define PSP_DISC2_DEFAULT_PATH   "ms0:/PSP/GAME/ParasiteEve/disc/Parasite Eve (USA) (Disc 2).bin"

/* CD-ROM Status Flags */
#define PSP_CD_STAT_PLAYING      0x80u
#define PSP_CD_STAT_SEEKING      0x40u
#define PSP_CD_STAT_READING      0x20u
#define PSP_CD_STAT_SHELLOPEN    0x10u
#define PSP_CD_STAT_IDLE         0x02u
#define PSP_CD_STAT_ERROR        0x01u

/* CD-ROM Command Codes (Psy-Q Cdl*) */
#define CdlNop                   0x01
#define CdlSetloc                0x02
#define CdlPlay                  0x03
#define CdlForward               0x04
#define CdlBackward              0x05
#define CdlReadN                 0x06
#define CdlStandby               0x07
#define CdlStop                  0x08
#define CdlPause                 0x09
#define CdlMute                  0x0B
#define CdlDemute                0x0C
#define CdlSetfilter             0x0D
#define CdlSetmode               0x0E
#define CdlGetparam              0x0F
#define CdlGetlocL               0x10
#define CdlGetlocP               0x11
#define CdlReadS                 0x13
#define CdlSeekL                 0x15
#define CdlSeekP                 0x16
#define CdlReadToc               0x1E

/* Subsystem Lifecycle */
int  PspCdrom_Init(const char *disc1_path, const char *disc2_path);
void PspCdrom_Shutdown(void);
int  PspCdrom_IsInitialized(void);

/* Disc Hot-Swapping */
int  PspCdrom_SwapDisc(int disc_number); /* 1 = Disc 1, 2 = Disc 2 */
int  PspCdrom_GetActiveDisc(void);
void PspCdrom_CheckHotSwapChord(uint16_t buttons); /* Hotkey: L + R + Select */

/* Sector Navigation & Reading */
int  PspCdrom_SeekLba(uint32_t lba);
int  PspCdrom_ReadRawSector(uint32_t lba, uint8_t *out_raw);
int  PspCdrom_ReadUserSector(uint32_t lba, uint8_t *out_user);
int  PspCdrom_ReadUserData(uint32_t lba, uint32_t offset, void *out, uint32_t len);
uint32_t PspCdrom_GetSectorCount(void);

/* Psy-Q Cdl Compatibility Functions */
int  CdControl(unsigned char com, const unsigned char *param, unsigned char *result);
int  CdControlB(unsigned char com, const unsigned char *param, unsigned char *result);
int  CdRead(int sectors, unsigned long *buf, int mode);
int  CdReadSync(int mode, unsigned char *result);
int  CdReady(int mode, unsigned char *result);
int  CdSeekL(const unsigned char *msf);
int  CdGetSector(void *madr, int size);
int  CdPosToInt(const unsigned char *msf);
void CdIntToPos(int i, unsigned char *msf);

/*
 * PS1 MDEC & STR Movie Streaming (Lightweight Macroblock IDCT)
 */
typedef struct PspMdecState {
    int initialized;
    uint8_t quant_luma[64];
    uint8_t quant_chroma[64];
    int16_t idct_scale[64];
    uint32_t decoded_frames;
} PspMdecState;

int  PspMdec_Init(void);
void PspMdec_Shutdown(void);
void PspMdec_Reset(void);
void PspMdec_SetQuantTables(const uint8_t *luma_quant, const uint8_t *chroma_quant);

/*
 * Decode an entire STR video frame directly into an sceGu texture buffer.
 * In-place decoding with zero large dynamic heap allocations.
 */
int  PspStr_DecodeFrame(const uint8_t *str_chunk_stream, size_t total_bytes,
                        void *dst_texture, int tex_width, int tex_height, int is_16bit);

/*
 * Stream STR video sectors from the active disc beginning at start_lba
 * directly into dst_texture at 15 fps.
 */
int  PspStr_StreamVideo(uint32_t start_lba, uint32_t sector_count,
                        void *dst_texture, int tex_width, int tex_height, int is_16bit);

#ifdef __cplusplus
}
#endif

#endif /* PSP_CDROM_ISO_H */
