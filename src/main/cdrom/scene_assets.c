#define PE1_NO_INLINE_CDROM_POLL 1
#include "common.h"
#include "pe1/akao.h"
#include "pe1/akao/commands.h"
#include "pe1/akao/pan3d.h"
#include "pe1/asset_key_record.h"
#include "pe1/cd_poll.h"
#include "pe1/cdrom.h"
#include "pe1/cdrom_buffers.h"
#include "pe1/entity_frame_update.h"
#include "pe1/game_audio_state.h"
#include "pe1/game_state.h"
#include "pe1/game_state_types.h"
#include "pe1/geom_state.h"
#include "pe1/gpu_image.h"
#include "pe1/gte.h"
#include "pe1/pe_image.h"
#include "pe1/psyq_bios.h"
#include "pe1/psyq_cd.h"
#include "pe1/psyq_ds_queue.h"
#include "pe1/psyq_gpu.h"
#include "pe1/psyq_tim.h"
#include "pe1/render_buffers.h"
#include "pe1/render_camera.h"
#include "pe1/render_object.h"
#include "pe1/scene_assets.h"

/* Returns whether the TIM image of `type` placed at VRAM x = 832 sits off
 * row 256, or -1 when the scene has no such image. */
int Asset_SearchByKeyType(int type)
{
    void *blob;
    u32 key;
    TimPackedImage *image;
    u32 xMask, x832, yMask, y256;

    blob = g_LoadedSceneAssetBlock[0];
    key = 0x73DECD80;
    xMask = 0x7FF << 10;
    x832 = 832 << 10;
    yMask = 0x7FF << 21;
    y256 = 256 << 21;
    for (;;) {
        image = Asset_FindTable08ByU32Key(blob, key);
        if (image == 0) {
            return -1;
        }
        if (image->key.bytes.type == type && (image->geometry & xMask) == x832) {
            return (image->geometry & yMask) != y256;
        }
        key += 4;
    }
}

typedef struct AkaoPosPanWork {
    AkaoPackedRect3 rect;
    s32 out1;
    s32 out2;
} AkaoPosPanWork;

s32 Asset_Find08w(s32 arg0, s32 arg1, s16 arg2, s16 arg3, s16 arg4) {
    return Akao_SendPositionalCmd((void *)g_LoadedSceneAssetBlock[0], arg0, arg1, arg2, arg3, arg4);
}

s32 Akao_SetPos3D(s32 arg0, s32 arg1, u16 arg2, u16 arg3, u16 arg4) {
    AkaoPosPanWork local;
    s32 ret = 0;

    local.rect.x = arg2;
    local.rect.y = arg3;
    local.rect.z = arg4;
    Akao_Calc3DPan(&local.rect, &local.out1, &local.out2);

    {
        if (g_AkaoRuntimeState.reset_pending != 0) {
            u8 **table = &g_AkaoRuntimeState.voice_banks[3];

            ret = Akao_Cmd_24(table[arg0], arg1, local.out1, local.out2);
        }
    }

    return ret;
}

s32 Akao_SendPositionalCmdStereo(s32 arg0, s32 arg1, s16 arg2, s16 arg3, s16 arg4) {
    s32 ret;
    void **basep = g_AkaoBgmHandle;

    ret = Akao_SendPositionalCmd(*basep, arg0, arg1, arg2, arg3, arg4);
    Akao_SendPositionalCmd(*basep, arg0 + 1, arg1, arg2, arg3, arg4);
    return ret;
}

s32 Asset_Find08Alt(s32 arg0, s32 arg1, s16 arg2, s16 arg3, s16 arg4) {
    return Akao_SendPositionalCmd(g_AkaoBgmHandle[0], arg0, arg1, arg2, arg3, arg4);
}

s32 Akao_SendPositionalCmd(void *arg0, s32 arg1, s32 arg2, AkaoPosCoord arg3, AkaoPosCoord arg4, AkaoPosCoord arg5) {
    AkaoPackedRect3 rect;
    s32 out1;
    s32 out2;

    rect.x = arg3;
    rect.y = arg4;
    rect.z = arg5;

    Akao_Calc3DPan(&rect, &out1, &out2);
    return Akao_SendTableCommand(arg0, arg1, arg2, out1, out2);
}

int Akao_SendTableCommand(void *arg0, int arg1, int arg2, int arg3, int arg4) {
    void *entry = Asset_FindTable2CByU16Key(arg0, arg1);
    int ret;

    if (entry != 0) {
        ret = Akao_Cmd_24(entry, arg2, arg3, arg4);
    } else {
        ret = -1;
    }
    return ret;
}

int Akao_Calc3DPan(AkaoPackedRect3 *pos, int *pan, int *volume)
{
    Pe1GameState *state;
    long sxy;
    long x;
    long y;
    int depth;
    int far;
    int span;
    int low;

    Render_SetGteScreenOffset();
    state = &g_GameState;
    gte_ldrotmatrix(g_ViewMatrixSlot.value);
    gte_ldtransmatrix(g_ViewMatrixSlot.value);
    gte_ctc2_26(*g_GeomScroll.state.position.projectionDistance);
    depth = RotTransPers((GteShortVector *)pos, (int *)&sxy, (int *)&x, (int *)&y);
    x = (short)sxy;
    y = sxy >> 16;
    Render_ResetGteScreenOffset();

    *pan = ((x + 40) << 7) / 400 + 64;
    if ((unsigned int)*pan >= 256) {
        *pan = 255;
    }

    if (depth < g_GameState.bank_value_f8) {
        depth = g_GameState.bank_value_f8;
    } else if (g_GameState.bank_value_fa < depth) {
        depth = g_GameState.bank_value_fa;
    }

    far = state->bank_value_fa;
    depth = far - depth;
    depth = depth * depth / (far - state->bank_value_f8);
    span = state->bank_value_f7;
    low = state->bank_value_f6;
    span -= low;
    *volume = depth * span / (far - state->bank_value_f8) + low;
    if ((unsigned int)*volume >= 128) {
        *volume = 127;
    }
    return 0;
}

/* Contiguous scene-container helpers: TIM upload, key encoding and lookup. */

int Draw_LoadTimAsset(TimUploadRecord *asset, void *base) {
    RECT rect;
    int mask;
    u8 *saved_base;
    unsigned int packed;
    int h;
    unsigned int offset;
    int secondary;
    unsigned int image_offset;
    u8 *image_address;

    packed = asset->image_rectangle;
    rect.x = (packed >> 10) & 0x7FF;
    packed = asset->image_rectangle;
    rect.y = packed >> 21;
    packed = asset->image_rectangle;
    rect.w = packed & 0x3FF;
    saved_base = base;
    packed = asset->image_source.bytes.height;
    if (packed != 0) {
        h = packed & 0xFF;
    } else {
        h = 0x100;
    }

    mask = 0xFFFFFF;
    rect.h = h;
    LoadImage(&rect, saved_base + (asset->image_source.offsetAndHeight & mask));

    secondary = asset->secondary_source.offsetAndHeight & mask;
    if (secondary != 0) {
        packed = asset->secondary_rectangle;
        rect.x = (packed >> 10) & 0x7FF;
        packed = asset->secondary_rectangle;
        rect.y = packed >> 21;
        packed = asset->secondary_rectangle;
        rect.w = packed & 0x3FF;
        rect.h = asset->secondary_source.bytes.height;
        image_offset = asset->image_source.offsetAndHeight & mask;
        image_address = saved_base + image_offset;
        offset = asset->secondary_source.offsetAndHeight & mask;
        LoadImage(&rect, offset + image_address);
    }

    return 0;
}

int Text_EncodeBase32(char *out, unsigned int value) {
    int i;
    int last;
    char *dst;

    i = 0;
    last = 5;
    dst = out;
    while (i < 6) {
        int shift = ((last - i) * 5) + 2;
        u8 c = g_Base32CharTable[(value >> shift) & 0x1F];

        *dst = c;
        if (c - 0x61U < 0x1AU) {
            *dst = c - 0x20;
        }
        i++;
        dst++;
    }

    out[6] = 0;
    return 0;
}

int Text_DecodeBase32(unsigned int a0) {
    int total = 0;
    int i;

    for (i = 1; i < 5; i++) {
        int factor = 1;
        int j;
        s8 digit;

        for (j = 0; j < 4 - i; j++) {
            factor *= 10;
        }

        digit = g_Base32CharTable[(a0 >> ((5 * (5 - i)) + 2)) & 0x1F];
        total += (digit - '0') * factor;
    }

    return total;
}

int Text_ParseBase32Id(signed char *arg0) {
    unsigned int result = 0;
    int index = 0;

    while (index < 6) {
        signed char c = *arg0;
        int value = ((unsigned int)(c - '0') < 10U)
            ? (c - '0')
            : (c - 'W') - (c >= 'k') - (c >= 'r') - (c >= 'y');

        result |= (unsigned int)value << ((5 - index) * 5 + 2);
        index++;
        arg0++;
    }

    return result;
}

int Text_ParseMapNumber(signed char *arg0) {
    int hundreds;
    int tens;
    int result;

    hundreds = arg0[2] - 0x30;
    result = hundreds * 3;
    result <<= 3;
    result += hundreds;
    result <<= 2;

    tens = arg0[3] - 0x30;
    result += tens * 10;
    result -= 0x30;

    return result + arg0[4];
}

/* Keyed record tables: SceneAssetDirectory word = 22-bit byte offset of the
 * records from the blob plus a 10-bit record count; each record holds the
 * 24-bit byte offset of its payload. Retail reserves an 8-byte frame for an
 * unused local in each lookup. */
#define DEFINE_ASSET_SEARCHER(name, base_type, key_type, table_member, record_type, offset_member) \
void *name(base_type base, key_type key) { \
    SceneAssetDirectory *directory; \
    record_type *records; \
    u32 descriptor; \
    u32 count; \
    u32 i; \
    void *found; \
    char unused[1]; \
 \
    directory = SceneAsset_ResolveOffset(base, ((SceneAssetBlob *)base)->directoryOffset); \
    descriptor = directory->table_member; \
    records = SceneAsset_ResolveOffset(base, descriptor & 0x3FFFFF); \
    count = descriptor >> 22; \
    found = 0; \
    for (i = 0; i < count; i++) { \
        if (records[i].key == key) { \
            found = SceneAsset_ResolveOffset(base, records[i].offset_member & 0xFFFFFF); \
            return found; \
        } \
    } \
    return found; \
}

DEFINE_ASSET_SEARCHER(Asset_FindTable08ByU32Key, void *, int, keyEntries, AssetU32KeyRecord, packedOffset)
DEFINE_ASSET_SEARCHER(Asset_FindTable2CByU16Key, char *, int, streamEntries, SceneCdStreamRecord, offset)
DEFINE_ASSET_SEARCHER(Asset_FindTable30ByU16Key, char *, int, trackEntries, SceneTrackRecord, offset)

/* Scene display clearing and CD read flow share the scene read flags. */

void Draw_ClearOnFlag(void) {
    u32 *state = &g_GameState.flags;

    if (*state & 0x08000000) {
        RECT rect;
        unsigned int value;
        unsigned char byte;

        rect.x = 0;
        rect.y = 0;
        rect.w = 0x140;
        rect.h = 0x1C0;
        ClearImage(&rect, 0, 0, 1);
        DrawSync(0);
        Render_PrepareFrame();
        byte = g_DiscChangeFlags;
        value = *state;
        g_DiscChangeFlags = byte | 2;
        *state = value & 0xF7FFFDFF;
    }
}

int CdRom_ReadSectorsFromLba(u32 lba, void *destination, u32 size) {
    return CdRom_ReadSectors(lba, 0, destination, size);
}

int CdRom_ReadSectors(u32 lba, u32 offset, void *destination, u32 size) {
    Pe1GameState *state;
    CdlLOC loc;
    int ret;

    state = &g_GameState;

    if ((state->flags & 0x1000000) != 0) {
        return -1;
    }
    if (DsSystemStatus() != 1) {
        return -1;
    }
    if (DsQueueLen() != 0) {
        return -1;
    }
    if (DsShellOpen() != g_CdDiskType) {
        exit(1);
    }

    state->flags |= 0x1004000;
    lba += offset;
    DsIntToPos(lba, &loc);
    ret = DsRead(&loc, size, destination, 0x80);
    if (ret != 0) {
        return ret;
    }

    state->flags &= 0xFEFFBFFF;
    printf(g_CdReadErrorFormat, lba, size);
    return -1;
}
int CdRom_PollReady(void) {
    int scratch;
    int status;
    Pe1GameState *state;

    status = DsReadSync(&scratch);
    if (status + 1U < 2U) {
        state = &g_GameState;
        state->flags &= 0xFEFFBFFF;
    }
    return status;
}

s32 CdRom_LoadInitialImage(void) {
    s32 status;
    u16 *range;
    s32 result;
    DISPENV sp18;

    g_GameState.scene_audio.tracks.keys[0][1] = -1;
    g_GameState.scene_audio.tracks.keys[0][0] = -1;
    g_GameState.scene_audio.tracks.keys[1][1] = -1;
    g_GameState.scene_audio.tracks.keys[1][0] = -1;
    g_GameState.scene_audio.tracks.banks[1] = -1;
    g_GameState.scene_audio.tracks.banks[0] = -1;
    g_GameState.flags &= 0xFFFFFF0F;
    Akao_Cmd_F0();

restart:
    range = g_StrFileDirLba;
    do {
        status = CdRom_ReadSectors(g_PeImageBaseLba + range[0], 0, g_StrFileDirBuffer, range[1] - range[0]);
    } while (status == -1);

    while ((status = CdRom_PollReady()) != 0) {
        if (status == -1) {
            goto restart;
        }
    }

    EnterCriticalSection();
    FlushCache();
    ExitCriticalSection();
    VSync(0);
    SetDispMask(0);
    SetDefDispEnv(&sp18, 0, 0, 0x140, 0xF0);
    sp18.isrgb24 = 1;
    PutDispEnv(&sp18);
    return 0;
}
