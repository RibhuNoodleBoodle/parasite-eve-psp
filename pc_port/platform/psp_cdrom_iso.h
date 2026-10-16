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
#ifndef PSP_DISC_DIR
#define PSP_DISC_DIR             "ms0:/PSP/GAME/ParasiteEve/disc"
#endif

#ifndef PSP_DISC1_PATH
#define PSP_DISC1_PATH           "ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC1.bin"
#endif

#ifndef PSP_DISC1_FALLBACK
#define PSP_DISC1_FALLBACK       "./disc/PE_DISC1.bin"
#endif

#ifndef PSP_DISC2_PATH
#define PSP_DISC2_PATH           "ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC2.bin"
#endif

#ifndef PSP_DISC2_FALLBACK
#define PSP_DISC2_FALLBACK       "./disc/PE_DISC2.bin"
#endif

#define PSP_DISC1_DEFAULT_PATH   PSP_DISC1_PATH
#define PSP_DISC2_DEFAULT_PATH   PSP_DISC2_PATH

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
int  psp_verify_disc_files(void);
int  PspCdrom_Init(const char *disc1_path, const char *disc2_path);
void PspCdrom_Shutdown(void);
int  PspCdrom_IsInitialized(void);
int  PspCdrom_CheckDiscExists(int disc_number);

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
int  CdControlF(unsigned char com, const unsigned char *param);
int  CdControlB(unsigned char com, const unsigned char *param, unsigned char *result);
int  CdRead(int sectors, unsigned long *buf, int mode);
int  CdSync(int mode, unsigned char *result);
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

#ifndef CDLLOC_DEFINED
#define CDLLOC_DEFINED
typedef struct CdlLOC {
    uint8_t minute;
    uint8_t second;
    uint8_t sector;
    uint8_t track;
} CdlLOC;
#endif

#ifndef DSLFILE_DEFINED
#define DSLFILE_DEFINED
typedef struct DslFILE {
    CdlLOC pos;
    uint32_t size;
    char name[16];
} DslFILE;
#endif

/* PS1 Psy-Q libds & cdrom prototypes */
int DsSystemStatus(void);
int DsShellOpen(void);
int DsQueueLen(void);
int DsGetDiskType(void);
int DsPosToInt(CdlLOC *position);
CdlLOC *DsIntToPos(int i, CdlLOC *p);
DslFILE *DsSearchFile(DslFILE *file, const char *name);
int CdRom_ReadSectors(uint32_t lba, uint32_t offset, void *destination, uint32_t size);
int CdRom_ReadSectorsFromLba(uint32_t lba, void *destination, uint32_t size);
int CdRom_PollReady(void);
int DsReadSync(void *argument);
int DsReady(uint8_t *destination);
int DsRead(CdlLOC *position, int sectors, void *destination, int mode);
int DsControl(unsigned char command, void *param, void *result);
int DsControlB(unsigned char command, void *param, void *result);
void DS_restart(void);
int DS_system_active(void);
int DsReset(void);
int ER_retry(void);
int DS_lastcom(void);
int DS_lastmode(void);
CdlLOC *DS_lastpos(void);
CdlLOC *DsLastPos(CdlLOC *destination);

#ifdef __cplusplus
}
#endif

#endif /* PSP_CDROM_ISO_H */
