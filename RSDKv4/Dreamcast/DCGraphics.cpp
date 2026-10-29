// RSDKv4 Dreamcast video backend
//
// Two presentation paths, chosen at build time by DC_USE_PVR:
//
//   DC_USE_PVR = 1 (default): the engine still software-renders RGB565 into
//     Engine.frameBuffer (320x240); each frame we upload that into a PVR
//     texture and draw it as a hardware textured quad scaled to 640x480.
//     This puts the PVR in charge of scan-out/scaling and is the foundation
//     for drawing native UI (pause menu etc.) as hardware polys and, later,
//     moving sprite rasterisation onto the PVR.
//
//   DC_USE_PVR = 0: the original path — sq_cpy the framebuffer straight to
//     VRAM at 320x240. Kept as a known-good fallback: if the PVR path shows
//     a black screen, rebuild with -DDC_USE_PVR=OFF to confirm the rest of
//     the engine still runs.

#include "../RetroEngine.hpp"

#if RETRO_USING_KOS

#include <kos.h>
#include <dc/video.h>
#include <dc/sq.h>
#include <dc/pvr.h>

#include "DCCommon.hpp"
#include "DCNative.hpp"
#include "DCSave.hpp" // DC_FlushSaves - deferred VMU writes land at our flush points

#ifndef DC_USE_PVR
#define DC_USE_PVR 1
#endif

// Diagnostic: palette-bank usage + per-bank fade animation logging. Confirmed
// the title emblem is static in the palette (not a renderer issue), so off.
#ifndef DC_PAL_DEBUG
#define DC_PAL_DEBUG 0
#endif

// Per-frame renderer timing + counters: serial log every 120 frames plus
// on-screen bars (green=frame period, yellow=flip, red=GPU wait; 1px=100us).
// This is how the HTZ slowdown was diagnosed (CPU-bound software tiles) and
// the P3 fix verified (frame ~15.4ms, locked 60). OFF for normal builds now
// that performance is confirmed good — set to 1 to profile again.
#ifndef DC_PERF_LOG
#define DC_PERF_LOG 0 // on-screen perf bars + serial timing; 1 to profile, 0 to ship
#endif

// ---------------------------------------------------------------------
// Store-queue vertex submission
// ---------------------------------------------------------------------
// Profiling Sonic 1's special stage put the entire per-frame regression in the
// flush rather than the GPU: flip 2159us for 749 quads with wait=0us on EVERY
// frame, i.e. ~2.9us of pure CPU per quad. Game logic was actually cheaper than
// Green Hill's (15157us vs 15710us) and the queue peaked at 889 of 3072, so
// neither the scripts nor the sprite cap were involved.
//
// That 2.9us is four pvr_prim() calls per quad. Each is a function call plus an
// sq_cpy() of one 32-byte vertex — a store-queue setup per vertex, ~144 SH4
// cycles apiece at 200MHz. The direct-render path sets the queue up once per
// batch and then writes each primitive straight into it, so a vertex is built
// in place instead of being built on the stack and copied.
//
// Headers go through the same path even though there are only 4-5 per frame:
// the point is not their cost, it is that pvr_prim's sq_cpy reprograms QACR,
// and interleaving that with direct rendering inside one batch is exactly the
// kind of thing that produces garbage geometry.
//
// Set to 0 to revert to pvr_prim() exactly. Both paths are kept side by side on
// purpose — this cannot be validated in an emulator (redream's SH4 timing is
// approximate, and a store-queue mistake shows up as corrupt polygons on real
// hardware, not as a compile error).
#ifndef DC_USE_SQ_VERTS
#define DC_USE_SQ_VERTS 1
#endif

#if DC_USE_PVR
// The framebuffer texture: 320x240 content stored in a 512x256 power-of-two
// non-twiddled RGB565 texture (PVR requires PO2 dims for non-stride textures).
#define DC_TEX_W  (512)
#define DC_TEX_H  (256)
#define DC_DISP_W (640)
#define DC_DISP_H (480)

static pvr_ptr_t dcFbTexture = NULL;
static pvr_poly_hdr_t dcFbHdr;
static bool dcPvrReady = false;
#endif

// ---------------------------------------------------------------------
// Music VRAM allocation (shared with DCAudio.cpp)
// ---------------------------------------------------------------------
// When the PVR owns VRAM its allocator must not hand our music buffers'
// memory back out as textures. Route the music region through the PVR
// allocator so it is reserved. Returns a byte offset from the VRAM 64-bit
// base (0xA5000000), matching what DC_VramWord() in DCAudio.cpp expects.
uint DC_MusicVramAlloc(uint bytes)
{
#if DC_USE_PVR
    pvr_ptr_t p = pvr_mem_malloc(bytes);
    if (!p) {
        DC_LOG("[DC] pvr_mem_malloc(%u) for music FAILED\n", bytes);
        return 0x800000u - bytes; // last-ditch fallback (top of VRAM)
    }
    uint off = (uint)((uintptr_t)p - 0xA5000000u);
    DC_LOG("[DC] sfx-stage VRAM: %u bytes at offset 0x%x (pvr), avail=%u\n", bytes, off, (uint)pvr_mem_available());
    return off;
#else
    return 0x800000u - bytes; // top of VRAM, raw
#endif
}

// ---------------------------------------------------------------------
// Hardware sprite renderer — sheet texture cache (Phase 1)
// ---------------------------------------------------------------------
// Mirror each engine sprite sheet into PVR VRAM as a twiddled PAL8BPP
// texture. The engine keeps its 8-bit-indexed sheets in graphicData[]
// (one byte per pixel, index 0 = transparent); that format maps directly
// onto a PVR paletted texture, so no pixel conversion is needed — only a
// power-of-two pad (PVR textures must be PO2, twiddled).

#if DC_USE_PVR

static pvr_ptr_t dcSheetTex[SURFACE_COUNT] = { NULL };
static int       dcSheetTexW[SURFACE_COUNT] = { 0 };
static int       dcSheetTexH[SURFACE_COUNT] = { 0 };

// Static staging buffer for texture uploads (sheet PO2 padding + tileset
// cell rearrange). This used to be malloc'd per upload — and a 256KB
// contiguous malloc FAILED after a few stage loads (heap fragmentation),
// which left Hidden Palace silently showing the previous stage's tiles.
// 256KB covers the tileset exactly and every real sheet (biggest seen is
// 512x256 = 128KB); anything larger falls back to malloc.
#define DC_TEX_STAGING_SIZE (256 * 1024)
static byte dcTexStaging[DC_TEX_STAGING_SIZE];

// Frozen-frame support (pause screen): quad count of the last frame that
// actually drew, re-presented when a frame queues nothing (the native pause
// menu doesn't render on DC, and a black pause screen is worse than a
// freeze-frame). Invalidated whenever the frozen quads could lie:
//  - a script ClearScreen means "blank IS this frame's content"
//    (dcQueueCleared — set by DC_ClearSpriteQueue, checked in the flip);
//  - freeing/reuploading textures leaves the frozen quads pointing at
//    recycled VRAM (stage loads flashed a raw sprite sheet for a frame).
static int  dcLastCount    = 0;
static bool dcQueueCleared = false;

// Stage-load frame detection (the "Act 1 flashes back during the act
// transition" bug). ProcessStage()'s STAGEMODE_LOAD case resets the scene,
// calls LoadStageFiles() and breaks — it never runs a draw pass — but the
// main loop still calls FlipScreen() for that frame. On the desktop that is
// harmless: sprites and the fade are composited into the software framebuffer,
// which nothing touched, so the previous (already black) image is re-presented.
// On DC nothing is composited: sprites live in dcSpriteQueue and the fade is a
// per-frame overlay that DC_FlipScreen clears every flip. So the load frame
// presented the stale framebuffer texture plus (via the frozen-frame path
// below) the stale sprite queue, with no fade over either — one full-brightness
// frame of the act you just left, right in the middle of the black.
//
// STAGEMODE_LOAD is consumed inside ProcessStage() (it sets STAGEMODE_NORMAL
// before doing the load), so by flip time stageMode already reads NORMAL.
// Instead sample stageMode at the END of each flip — that is exactly what the
// next frame's ProcessStage() will see — and call this a load frame when it
// was LOAD then and is no longer LOAD now. That "was LOAD, now isn't" edge is
// what makes it precise: on the native launch menu ProcessStage() never runs,
// so stageMode sits at its STAGEMODE_LOAD initial value on both samples and no
// menu frame is ever mistaken for a stage load.
static int dcPrevStageMode = -1;

// ...and a short window AFTER it. Covering only the load frame itself was not
// enough on hardware: the flash moved but did not go away. The load frame is
// not the only frame in the transition that composites nothing. LoadStageFiles()
// finishes inside ProcessStage(), so the NEXT frame is already STAGEMODE_NORMAL
// and runs DrawStageGFX — but the stage's objects have only just been created,
// and until TitleCard's ObjectDraw runs (it is what paints the black; there is
// no SetScreenFade anywhere in its SETUP state) a frame can go out with the new
// act's scrolling layers in the framebuffer and NOTHING in the hardware queue.
// Same symptom, one frame later.
//
// So instead of naming the guilty frame, describe it: for a few frames after a
// load, any frame that queues no hardware quads at all has nothing of the new
// stage on screen and must be covered. On DC a real gameplay frame ALWAYS
// queues quads — every tile is one — so an empty queue in that window is a
// reliable marker of a transition artifact rather than of legitimate content.
// The window ends the instant a frame does queue something (TitleCard's
// full-screen black DrawRect is itself a quad, so in practice that is the very
// next frame).
#define DC_POSTLOAD_COVER_FRAMES (8)
static int dcPostLoadFrames = 0;

static inline int DC_NextPOT(int v)
{
    int p = 8; // PVR minimum texture dimension
    while (p < v) p <<= 1;
    return p;
}

// The live game framebuffer as a PVR texture. DC_FlipScreen refreshes it from
// Engine.frameBuffer every frame (see the sq_cpy loop below), so a caller gets
// the current — or, while the game is paused and nothing redraws, the last —
// rendered frame. The native pause menu's preview window samples it; the UVs in
// the engine's retroVertexList[] already assume exactly this padding, because
// Drawing.cpp's SetScreenDimensions() runs on the DC and derives
// textureList[0].widthN/heightN as 1/512 and 1/256.
void *DC_GetFramebufferTexture(int *outPotW, int *outPotH)
{
    if (outPotW)
        *outPotW = DC_TEX_W;
    if (outPotH)
        *outPotH = DC_TEX_H;
    return (void *)dcFbTexture;
}

void DC_InvalidateSheet(int sheetID)
{
    if (sheetID < 0 || sheetID >= SURFACE_COUNT)
        return;
    if (dcSheetTex[sheetID]) {
        pvr_mem_free(dcSheetTex[sheetID]);
        dcSheetTex[sheetID]  = NULL;
        dcSheetTexW[sheetID] = 0;
        dcSheetTexH[sheetID] = 0;
        dcLastCount = 0; // frozen frame may reference this texture — drop it
    }
}

void DC_InvalidateAllSheets()
{
    for (int i = 0; i < SURFACE_COUNT; ++i) DC_InvalidateSheet(i);
}

void DC_UploadSheet(int sheetID)
{
    if (sheetID < 0 || sheetID >= SURFACE_COUNT)
        return;

    DC_InvalidateSheet(sheetID); // drop any previous texture in this slot

    GFXSurface *s = &gfxSurface[sheetID];
    const int w = s->width, h = s->height;
    if (w <= 0 || h <= 0)
        return; // empty / failed slot

    const int tw = DC_NextPOT(w), th = DC_NextPOT(h);
    if (tw > 1024 || th > 1024) {
        DC_LOG("[DC] sheet %d too large (%dx%d) — not uploaded\n", sheetID, w, h);
        return;
    }

    const size_t texBytes = (size_t)tw * th;
    pvr_ptr_t tex         = pvr_mem_malloc(texBytes);
    if (!tex) {
        DC_LOG("[DC] sheet %d: VRAM %dx%d (%u B) FAILED, avail=%u\n", sheetID, tw, th, (uint)texBytes, (uint)pvr_mem_available());
        return;
    }

    if (tw == w && th == h) {
        // Already power-of-two: upload straight from the sheet pool
        // (pvr_txr_load_ex twiddles) — no temp buffer, saves scarce heap.
        pvr_txr_load_ex(&graphicData[s->dataPosition], tex, tw, th, PVR_TXRLOAD_8BPP);
    }
    else {
        // Pad up to PO2 in the static staging buffer, pad filled with index 0
        // (transparent). Fall back to malloc only for oversized sheets (none
        // in practice — and heap allocs here failed mid-session before).
        uint8_t *tmp     = dcTexStaging;
        bool tmpIsHeap = false;
        if (texBytes > DC_TEX_STAGING_SIZE) {
            tmp       = (uint8_t *)malloc(texBytes);
            tmpIsHeap = true;
            if (!tmp) {
                DC_LOG("[DC] sheet %d: no RAM for %dx%d stage\n", sheetID, tw, th);
                pvr_mem_free(tex);
                return;
            }
        }
        memset(tmp, 0, texBytes);
        const byte *srcRow = &graphicData[s->dataPosition];
        for (int y = 0; y < h; ++y) {
            memcpy(tmp + (size_t)y * tw, srcRow, w);
            srcRow += w; // source stride is the sheet's full width
        }
        pvr_txr_load_ex(tmp, tex, tw, th, PVR_TXRLOAD_8BPP);
        if (tmpIsHeap)
            free(tmp);
    }

    dcSheetTex[sheetID]  = tex;
    dcSheetTexW[sheetID] = tw;
    dcSheetTexH[sheetID] = th;
    DC_LOG("[DC] sheet %d: %dx%d -> %dx%d PAL8 (%u B), avail=%u\n", sheetID, w, h, tw, th, (uint)texBytes, (uint)pvr_mem_available());
}

void DC_UploadPalettes()
{
#if DC_PAL_DEBUG
    // Diagnostic: per-bank palette signature (XOR of all 256 colors) + fade
    // state, every 15 frames. A bank whose signature changes over time is
    // animating (fading). Correlate with which raw banks sprites use.
    {
        static int f = 0;
        if ((++f % 15) == 0) {
            ushort s[8];
            for (int b = 0; b < 8; ++b) {
                ushort x = 0;
                for (int i = 0; i < 256; ++i) x ^= fullPalette[b][i];
                s[b] = x;
            }
            DC_LOG("[DC] palsig %04x %04x %04x %04x %04x %04x %04x %04x fadeMode=%d fadeA=%d\n", s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
                   fadeMode, (int)fadeA);
        }
    }
#endif
    // v4 stores 8 RGB565 palette banks; the PVR has 4 hardware banks of 256
    // for PAL8BPP. Collapse 8 -> 4 (bank%4). Index 0 is the transparent
    // color key (alpha bit clear); all others get the opaque bit.
    pvr_set_pal_format(PVR_PAL_ARGB1555);
    for (int bank = 0; bank < 4; ++bank) {
        const int base = bank * 256;
        for (int i = 0; i < 256; ++i) {
            const ushort c = fullPalette[bank][i];
            const uint r5  = (c >> 11) & 0x1F;
            const uint g6  = (c >> 5) & 0x3F;
            const uint b5  = c & 0x1F;
            const uint argb1555 = (r5 << 10) | ((g6 >> 1) << 5) | b5;
            pvr_set_pal_entry(base + i, (i == 0) ? 0u : (0x8000u | argb1555));
        }
    }
}

// ---------------------------------------------------------------------
// Sprite queue (Phase 2)
// ---------------------------------------------------------------------
// The engine's sprite draws (DrawSprite/DrawSpriteFlipped on KOS) record a
// quad here instead of blitting into the software framebuffer. The queue is
// flushed as a batch of hardware textured quads in DC_FlipScreen — this keeps
// the PVR scene self-contained in the flip and preserves draw order via an
// incrementing z (the TR list autosorts by z).

#define DC_SPRITE_MAX (3072)

// Max palette bands one axis-aligned sprite may be split into (see
// DC_QueueSpriteInk). The water line needs 2; anything beyond this merges
// into the final band, degrading to the old one-bank-per-sprite behaviour
// for that remainder rather than costing unbounded queue slots.
#define DC_SPRITE_PAL_BANDS (4)

struct DCSpriteCmd {
    pvr_ptr_t tex;
    int       texW, texH;
    int       palBank;
    int       ink;  // DC_INK_OPAQUE / _ALPHA / _ADD
    uint      argb; // vertex colour (alpha in the high byte)
    // Corner positions in display space, strip order TL, TR, BL, BR —
    // where "TL" is the corner showing texel (u0,v0), "BR" texel (u1,v1).
    // Axis-aligned sprites fill these from their rect; rotated/scaled
    // sprites pass arbitrary corners.
    float     xs[4], ys[4];
    float     u0, v0, u1, v1;
    float     z;
};

// Stage profiling accumulators (declared in DCCommon.hpp, written by Scene.cpp).
// Always defined and always cleared per frame so they cannot run away when
// DC_PERF_LOG is off; two 64-bit adds a frame is not worth a second toggle.
unsigned long long dcProfObjectsNs = 0;
unsigned long long dcProfDrawNs    = 0;
unsigned long long dcProfObjDrawNs = 0;

// Quads queued that lie entirely outside the 320x240 engine viewport.
// The profile says the special stage spends 54% of its frame in ObjectDraw
// scripts — ~266 blocks at ~35us each — so the only fix worth the effort is
// running fewer of them. This counts how many are wasted, i.e. the ceiling on
// what block culling could buy. Skipping them here also trims the flush, but
// that is the small half: the script has already run by this point.
static uint dcQuadOffscreen = 0;

// Special-stage draw cull tallies (declared in DCCommon.hpp, written by
// Drawing.cpp). Cleared per frame next to dcQuadOffscreen below, whether or not
// DC_PERF_LOG is on, so they cannot run away the way the prof counters once did.
uint dcSSCullTested  = 0;
uint dcSSCullSkipped = 0;

static DCSpriteCmd dcSpriteQueue[DC_SPRITE_MAX];
static int         dcSpriteCount = 0;
static float       dcSpriteZ     = 2.0f;

#if DC_USE_SQ_VERTS
// Copy an already-compiled 32-byte header into the store queue. Compiled on the
// stack first rather than in place: pvr_poly_compile is a write-only builder in
// current KOS, but the store-queue area must never be read back, and a future
// read-modify-write in there would fail in a way that is very hard to see.
static inline void DC_SQEmitHdr(pvr_dr_state_t *st, const pvr_poly_hdr_t *hd)
{
    uint32       *d = (uint32 *)pvr_dr_target(*st);
    const uint32 *s = (const uint32 *)hd;
    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
    d[4] = s[4]; d[5] = s[5]; d[6] = s[6]; d[7] = s[7];
    pvr_dr_commit(d);
}

// One quad as a 4-vertex strip, built directly in the queue. Corner and texel
// order is identical to the pvr_prim path below: TL(u0,v0) TR(u1,v0)
// BL(u0,v1) BR(u1,v1), with EOL on the last.
static inline void DC_SQEmitQuad(pvr_dr_state_t *st, const DCSpriteCmd *c)
{
    const float us[4] = { c->u0, c->u1, c->u0, c->u1 };
    const float vs[4] = { c->v0, c->v0, c->v1, c->v1 };
    for (int k = 0; k < 4; ++k) {
        pvr_vertex_t *vp = (pvr_vertex_t *)pvr_dr_target(*st);
        vp->flags = (k == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        vp->x     = c->xs[k];
        vp->y     = c->ys[k];
        vp->z     = c->z;
        vp->u     = us[k];
        vp->v     = vs[k];
        vp->argb  = c->argb;
        vp->oargb = 0;
        pvr_dr_commit(vp);
    }
}
#endif

void DC_QueueSpriteInk(int sheetID, int destX, int destY, int w, int h, int sprX, int sprY, int flip, int ink, int alpha)
{
    if (sheetID < 0 || sheetID >= SURFACE_COUNT || w <= 0 || h <= 0)
        return;
    pvr_ptr_t tex = dcSheetTex[sheetID];
    if (!tex || dcSpriteCount >= DC_SPRITE_MAX)
        return;

    if (destX + w <= 0 || destX >= SCREEN_XSIZE || destY + h <= 0 || destY >= SCREEN_YSIZE) {
        ++dcQuadOffscreen; // fully outside the viewport
        return;
    }

    const int tw = dcSheetTexW[sheetID], th = dcSheetTexH[sheetID];

    float u0 = (float)sprX / tw;
    float u1 = (float)(sprX + w) / tw;
    if (flip == FLIP_X || flip == FLIP_XY) { float t = u0; u0 = u1; u1 = t; }
    const bool flipY = (flip == FLIP_Y || flip == FLIP_XY);

    // ---- Palette bands ----------------------------------------------------
    // The software renderer picks a bank per SCANLINE out of gfxLineBuffer.
    // Taking one bank for the whole sprite (its top row) is wrong wherever a
    // sprite straddles a bank change, and the water line is exactly that: a
    // zone's setup calls SetActivePalette(waterBank, waterDrawPos,
    // SCREEN_YSIZE), so every line at or below the surface is a different
    // bank. Sonic standing in shallow water with his quills above the surface
    // took the DRY bank for ALL of him -- a bright sprite against correctly
    // darkened tiles -- and dropping a few pixels put his top row under the
    // line and flipped the WHOLE sprite to the water bank in a single frame.
    // It is a step function by construction; there is no in-between state.
    //
    // So split the quad at each bank change instead: one sub-quad per run of
    // constant bank, with v interpolated to the split. Source and screen rows
    // are 1:1 on this path, so every boundary lands on an exact texel edge and
    // the bands reassemble into the original image with no seam and no
    // resampling. A sprite that straddles nothing still emits ONE quad with
    // bit-identical numbers to before.
    int yTop = destY; // first row of the sprite that is actually on screen
    if (yTop < 0)
        yTop = 0;
    int yBot = destY + h; // one past its last on-screen row
    if (yBot > SCREEN_YSIZE)
        yBot = SCREEN_YSIZE;
    // The early reject above guarantees destY + h > 0 and destY < SCREEN_YSIZE,
    // so yTop < yBot here and both index gfxLineBuffer in range.

#if DC_PAL_DEBUG
    // Log each distinct raw palette bank (0-7) a sprite actually uses, so we
    // can tell if anything is on banks 4-7 (which bank%4 collapses).
    {
        static uint seen = 0;
        const int rb = gfxLineBuffer[yTop] & 7;
        if (!(seen & (1u << rb))) {
            seen |= (1u << rb);
            DC_LOG("[DC] sprite uses raw bank %d (sheet %d)\n", rb, sheetID);
        }
    }
#endif

    if (alpha < 0) alpha = 0;
    if (alpha > 0xFF) alpha = 0xFF;

    // 320x240 engine space -> 640x480 display (the PVR clips off-screen parts).
    const float x0 = destX * 2.0f;
    const float x1 = (destX + w) * 2.0f;
    // Every band of one sprite shares its z, and dcSpriteZ advances once, so
    // splitting cannot reorder a sprite against its neighbours.
    const float zs = dcSpriteZ;

    int bandStart = yTop;
    int bandsLeft = DC_SPRITE_PAL_BANDS;
    while (bandStart < yBot) {
        int bandEnd = bandStart + 1;
        if (--bandsLeft <= 0) {
            bandEnd = yBot; // out of band budget: the rest takes this bank
        }
        else {
            const int bank = gfxLineBuffer[bandStart] & 3;
            while (bandEnd < yBot && (gfxLineBuffer[bandEnd] & 3) == bank) ++bandEnd;
        }

        // The first band carries whatever was clipped off the top and the last
        // whatever was clipped off the bottom, so the quads still span the
        // whole sprite and the PVR clips precisely as it did before.
        const int rowA = (bandStart == yTop) ? destY : bandStart;
        const int rowB = (bandEnd == yBot) ? destY + h : bandEnd;
        const int dyA  = rowA - destY;
        const int dyB  = rowB - destY;

        if (dcSpriteCount >= DC_SPRITE_MAX)
            break;
        DCSpriteCmd *c = &dcSpriteQueue[dcSpriteCount++];
        c->tex     = tex;
        c->texW    = tw;
        c->texH    = th;
        c->palBank = gfxLineBuffer[bandStart] & 3;
        c->ink     = ink;
        c->argb    = ((uint)alpha << 24) | 0x00FFFFFFu; // white tint, sprite alpha

        const float yA = rowA * 2.0f;
        const float yB = rowB * 2.0f;
        c->xs[0] = x0; c->ys[0] = yA; // TL
        c->xs[1] = x1; c->ys[1] = yA; // TR
        c->xs[2] = x0; c->ys[2] = yB; // BL
        c->xs[3] = x1; c->ys[3] = yB; // BR
        c->u0 = u0;
        c->u1 = u1;
        // Flipped vertically the band still shows the same screen rows, but
        // reads them from the mirrored end of the source rows.
        c->v0 = flipY ? (float)(sprY + h - dyA) / th : (float)(sprY + dyA) / th;
        c->v1 = flipY ? (float)(sprY + h - dyB) / th : (float)(sprY + dyB) / th;
        c->z  = zs;

        bandStart = bandEnd;
    }
    dcSpriteZ += 0.001f;
}

// Arbitrarily transformed sprite quad (rotation / scaling). Corners are in
// 320x240 engine space, order TL, TR, BL, BR *in texture terms*: xs[0]/ys[0]
// is wherever texel (sprX, sprY) should land on screen, xs[3]/ys[3] is texel
// (sprX+w, sprY+h) — so a mirrored sprite simply passes mirrored geometry.
// The caller (Drawing.cpp) does the engine's fixed-point rotate/scale math;
// this just maps it onto the PVR (which clips off-screen parts itself).
void DC_QueueSpriteQuad(int sheetID, const float *xs, const float *ys, int sprX, int sprY, int w, int h, int ink, int alpha)
{
    if (sheetID < 0 || sheetID >= SURFACE_COUNT || w <= 0 || h <= 0)
        return;
    pvr_ptr_t tex = dcSheetTex[sheetID];
    if (!tex || dcSpriteCount >= DC_SPRITE_MAX)
        return;

    // Cheap reject: fully outside the viewport. The PVR would clip it anyway,
    // but it still costs a queue slot and four vertex submissions in the flush.
    {
        float minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
        for (int i = 1; i < 4; ++i) {
            if (xs[i] < minx) minx = xs[i];
            if (xs[i] > maxx) maxx = xs[i];
            if (ys[i] < miny) miny = ys[i];
            if (ys[i] > maxy) maxy = ys[i];
        }
        if (maxx <= 0.0f || minx >= (float)SCREEN_XSIZE || maxy <= 0.0f || miny >= (float)SCREEN_YSIZE) {
            ++dcQuadOffscreen;
            return;
        }
    }

    const int tw = dcSheetTexW[sheetID], th = dcSheetTexH[sheetID];

    // An arbitrary rotated quad cannot be band-split the way the axis-aligned
    // path is (a bank boundary is a horizontal line, not an edge of the quad),
    // so this keeps one bank per sprite -- but samples it at the quad's
    // VERTICAL CENTRE rather than its top edge. Centre halves the worst-case
    // error and, more importantly, makes it symmetric: a sprite crossing the
    // water line switches bank when it is half under, instead of the instant
    // its topmost pixel dips below.
    float topY = ys[0], botY = ys[0];
    for (int i = 1; i < 4; ++i) {
        if (ys[i] < topY) topY = ys[i];
        if (ys[i] > botY) botY = ys[i];
    }
    int by = (int)((topY + botY) * 0.5f);
    if (by < 0) by = 0;
    if (by >= SCREEN_YSIZE) by = SCREEN_YSIZE - 1;

    if (alpha < 0) alpha = 0;
    if (alpha > 0xFF) alpha = 0xFF;

    DCSpriteCmd *c = &dcSpriteQueue[dcSpriteCount++];
    c->tex     = tex;
    c->texW    = tw;
    c->texH    = th;
    c->palBank = gfxLineBuffer[by] & 3;
    c->ink     = ink;
    c->argb    = ((uint)alpha << 24) | 0x00FFFFFFu;
    for (int i = 0; i < 4; ++i) {
        c->xs[i] = xs[i] * 2.0f; // engine space -> 640x480 display
        c->ys[i] = ys[i] * 2.0f;
    }
    c->u0 = (float)sprX / tw;
    c->v0 = (float)sprY / th;
    c->u1 = (float)(sprX + w) / tw;
    c->v1 = (float)(sprY + h) / th;
    c->z  = dcSpriteZ;
    dcSpriteZ += 0.001f;
}

void DC_QueueSprite(int sheetID, int destX, int destY, int w, int h, int sprX, int sprY, int flip)
{
    DC_QueueSpriteInk(sheetID, destX, destY, w, h, sprX, sprY, flip, DC_INK_OPAQUE, 0xFF);
}

// Script ClearScreen support: drop everything queued so far this frame (the
// software path erases the framebuffer; the queued quads are the hardware
// half of "everything drawn so far").
void DC_ClearSpriteQueue()
{
    dcSpriteCount  = 0;
    dcSpriteZ      = 2.0f;
    dcQueueCleared = true; // a blank screen is this frame's real content
}

// Arbitrary-corner flat-colour quad (scene3D COLORED/FADED faces — the special
// stage half-pipe). Corners are 4 points in 320x240 engine space in PVR strip
// order (TL,TR,BL,BR — i.e. the diagonal runs xs[1]->xs[2]); the caller maps
// the face's a,b,c,d winding onto that. argb is the straight vertex colour
// (fog already baked in by the caller). Uses the running dcSpriteZ so it
// interleaves with sprites/3D-sprites by draw order. Untextured => TR list.
void DC_Queue3DColorQuad(const float *xs, const float *ys, uint argb)
{
    if (dcSpriteCount >= DC_SPRITE_MAX)
        return;
    DCSpriteCmd *c = &dcSpriteQueue[dcSpriteCount++];
    c->tex     = NULL; // colour quad
    c->texW    = 0;
    c->texH    = 0;
    c->palBank = 0;
    c->ink     = DC_INK_ALPHA; // TR pass (alpha 255 => opaque result)
    c->argb    = argb;
    for (int i = 0; i < 4; ++i) {
        c->xs[i] = xs[i] * 2.0f; // engine space -> 640x480 display
        c->ys[i] = ys[i] * 2.0f;
    }
    c->u0 = c->v0 = c->u1 = c->v1 = 0.0f;
    c->z = dcSpriteZ;
    dcSpriteZ += 0.001f;
}

// Full-screen fade overlay. The engine fades by drawing a translucent rect
// into the software framebuffer, which only darkens the tiles — the hardware
// sprites sit on top and are missed. On DC we skip that software rect and
// instead paint one PVR quad over the whole composited scene (tiles AND
// sprites) at the nearest z, so everything fades together.
static bool dcFadeActive = false;
static int  dcFadeR = 0, dcFadeG = 0, dcFadeB = 0, dcFadeA = 0;

void DC_SetFadeOverlay(int r, int g, int b, int a)
{
    dcFadeActive = true;
    dcFadeR = r; dcFadeG = g; dcFadeB = b; dcFadeA = a;
}

// Script-driven rectangle (DrawRect / DrawRectangle). The software renderer
// blends these into Engine.frameBuffer, which on DC is only the BACKGROUND
// layer — so a rect meant to veil the hardware sprites (e.g. the title
// emblem's fade-in-from-black: DrawSprite emblem, then a full-screen black
// rect whose alpha ramps 255->0) never touches them and the sprites appear
// instantly. Route it to a flat-colour hardware quad queued at the CURRENT
// draw-order z, so it layers exactly where the script drew it: above sprites
// queued before it, below sprites queued after. tex==NULL marks a colour
// quad in the flush. Always goes in the TR list (blend covers opaque too).
void DC_QueueRect(int x, int y, int w, int h, int r, int g, int b, int a)
{
    if (a <= 0 || w <= 0 || h <= 0 || dcSpriteCount >= DC_SPRITE_MAX)
        return;
    if (a > 0xFF) a = 0xFF;

    DCSpriteCmd *c = &dcSpriteQueue[dcSpriteCount++];
    c->tex     = NULL; // colour quad
    c->texW    = 0;
    c->texH    = 0;
    c->palBank = 0;
    c->ink     = DC_INK_ALPHA; // force into the TR pass
    c->argb    = ((uint)a << 24) | ((uint)(r & 0xFF) << 16) | ((uint)(g & 0xFF) << 8) | (uint)(b & 0xFF);
    const float x0 = x * 2.0f, y0 = y * 2.0f, x1 = (x + w) * 2.0f, y1 = (y + h) * 2.0f;
    c->xs[0] = x0; c->ys[0] = y0;
    c->xs[1] = x1; c->ys[1] = y0;
    c->xs[2] = x0; c->ys[2] = y1;
    c->xs[3] = x1; c->ys[3] = y1;
    c->u0 = c->v0 = c->u1 = c->v1 = 0.0f;
    c->z = dcSpriteZ;
    dcSpriteZ += 0.001f;
}

// ---------------------------------------------------------------------
// Tileset texture (P2b: hardware high-plane tiles)
// ---------------------------------------------------------------------
// The stage tileset (tilesetGFXData: 1024 tiles of 16x16 8-bit indices,
// stored linearly, 256 bytes per tile) is mirrored into one 256x1024
// PAL8BPP twiddled texture, arranged as a 16x64 grid of tile cells: tile t
// lives at cell ((t & 15) * 16, (t >> 4) * 16). Uploaded lazily at the next
// flip after stage load (and after runtime Copy16x16Tile edits), so tile
// quads can be queued from the layer drawers like any other sprite.

#define DC_TILESET_TEX_W (256)
#define DC_TILESET_TEX_H (1024)

static pvr_ptr_t dcTilesetTex   = NULL;
static bool      dcTilesetDirty = false;
// True when tile 0 is fully transparent (all index 0) — the empty tile that
// fills most of every sky/blank region. Skipping its quads saves hundreds of
// fully-invisible submissions per frame now that whole BG layers are queued.
static bool      dcTile0Blank   = false;

// Animated tiles: stage scripts animate tiles by calling Copy16x16Tile every
// frame (EHZ's flowers etc). Re-twiddling and re-uploading the whole 256KB
// texture per call stutters HARD — and it's unnecessary: a tile copy only
// ever moves ORIGINAL tileset pixels around, so we keep an indirection table
// ("tile N currently holds tile M's original pixels") and let the quads pick
// the remapped UV cell instead. Tile animation costs nothing; the full
// upload only happens on stage load.
static ushort dcTileRemap[TILE_COUNT];

void DC_MarkTilesetDirty() { dcTilesetDirty = true; }

void DC_RemapTile(int dest, int src)
{
    if (dest < 0 || dest >= TILE_COUNT || src < 0 || src >= TILE_COUNT)
        return;
    dcTileRemap[dest] = dcTileRemap[src];
}

static void DC_UploadTileset()
{
    dcTilesetDirty = false;
    dcLastCount    = 0; // frozen frame's tile quads now sample a new tileset

    // The texture mirrors tilesetGFXData as it is RIGHT NOW (including any
    // copies already applied), so the remap resets to identity.
    for (int t = 0; t < TILE_COUNT; ++t) dcTileRemap[t] = (ushort)t;

    if (!dcTilesetTex) {
        dcTilesetTex = pvr_mem_malloc(DC_TILESET_TEX_W * DC_TILESET_TEX_H);
        if (!dcTilesetTex) {
            DC_LOG("[DC] tileset: VRAM alloc FAILED\n");
            return;
        }
    }

    // Rearrange the linear tile blocks into the 16x64 cell grid, in the
    // static staging buffer (a 256KB malloc here FAILED after a few stage
    // loads — heap fragmentation — leaving the texture stale: the Hidden
    // Palace previous-stage-tiles bug).
    byte *staging = dcTexStaging; // DC_TEX_STAGING_SIZE == tileset size exactly
    for (int t = 0; t < TILE_COUNT; ++t) {
        const byte *src = &tilesetGFXData[t * TILE_DATASIZE];
        byte *dst       = &staging[((t >> 4) * TILE_SIZE) * DC_TILESET_TEX_W + (t & 15) * TILE_SIZE];
        for (int r = 0; r < TILE_SIZE; ++r) {
            memcpy(dst, src, TILE_SIZE);
            src += TILE_SIZE;
            dst += DC_TILESET_TEX_W;
        }
    }

    pvr_txr_load_ex(staging, dcTilesetTex, DC_TILESET_TEX_W, DC_TILESET_TEX_H, PVR_TXRLOAD_8BPP);

    // Verify the "blank tile" assumption instead of trusting convention.
    dcTile0Blank = true;
    for (int i = 0; i < TILE_DATASIZE; ++i) {
        if (tilesetGFXData[i]) {
            dcTile0Blank = false;
            break;
        }
    }

    // Signature of the source data (XOR of the first 16KB) — lets the serial
    // log prove whether a stage's tileset actually CHANGED between uploads
    // (diagnosing "stage shows the previous stage's tiles").
    uint sig = 0;
    const uint *sp = (const uint *)tilesetGFXData;
    for (int i = 0; i < 0x1000; ++i) sig ^= sp[i];
    DC_LOG("[DC] tileset: uploaded (256KB) sig=%08x\n", sig);
}

// Upload immediately (called from LoadStageGIFFile, while the loading screen
// is up) — deferring to the first flip put the 256KB twiddle+upload on the
// stage's first visible frame, a visible hitch right as the title card
// appears. Falls back to the dirty flag if the PVR isn't up yet.
void DC_UploadTilesetNow()
{
    // A game stage is loading — reclaim the native menu textures' VRAM (several
    // MB) so the stage's tileset + sprite sheets can allocate. Without this the
    // menu textures stay resident and the game renders broken (tileset/sheets
    // fail to allocate). The menu re-streams its .dtex textures if shown again.
    DC_NativeResetTextures();

    // Primary flush point for deferred VMU writes (DCSave.cpp). The loading
    // screen is up and a 256KB tileset twiddle+upload is about to happen right
    // below, so a ~300ms memory-card write here costs nothing visible - as
    // opposed to the signpost frame, where the engine originally asked for it.
    DC_FlushSaves();

    // Fresh stage: clear the software framebuffer too. With every tile plane
    // on the PVR (P3) the framebuffer only shows through transparent
    // (index 0) background texels — without this, stale loading-screen
    // pixels would peek through those gaps.
    if (Engine.frameBuffer)
        memset(Engine.frameBuffer, 0, (size_t)GFX_LINESIZE * SCREEN_YSIZE * sizeof(ushort));

    if (dcPvrReady)
        DC_UploadTileset();
    else
        dcTilesetDirty = true;
}

// Queue one 16-wide tile strip (srcY..srcY+h of a 16x16 tile) at destX/destY
// in engine space. Called from the high-plane tile pass in Drawing.cpp, so
// queue order (and therefore z) matches the engine's draw order exactly.
void DC_QueueTileQuad(int tileIndex, int direction, int destX, int destY, int srcY, int h)
{
    if (!dcTilesetTex || dcSpriteCount >= DC_SPRITE_MAX || h <= 0)
        return;
    if (tileIndex < 0 || tileIndex >= TILE_COUNT)
        return;
    tileIndex = dcTileRemap[tileIndex]; // animated-tile indirection
    if (tileIndex == 0 && dcTile0Blank)
        return; // fully transparent tile: nothing to draw

    int by = destY;
    if (by < 0) by = 0;
    if (by >= SCREEN_YSIZE) by = SCREEN_YSIZE - 1;

    const float cellX = (float)((tileIndex & 15) * TILE_SIZE);
    const float cellY = (float)((tileIndex >> 4) * TILE_SIZE);
    const bool fx = (direction == FLIP_X || direction == FLIP_XY);
    const bool fy = (direction == FLIP_Y || direction == FLIP_XY);

    float u0, u1, v0, v1;
    if (fx) { u0 = cellX + TILE_SIZE; u1 = cellX; }
    else    { u0 = cellX;             u1 = cellX + TILE_SIZE; }
    // Vertical flips mirror the band inside the tile: source edge e -> 16-e.
    if (fy) { v0 = cellY + (TILE_SIZE - srcY); v1 = cellY + (TILE_SIZE - srcY - h); }
    else    { v0 = cellY + srcY;               v1 = cellY + srcY + h; }

    DCSpriteCmd *c = &dcSpriteQueue[dcSpriteCount++];
    c->tex     = dcTilesetTex;
    c->texW    = DC_TILESET_TEX_W;
    c->texH    = DC_TILESET_TEX_H;
    c->palBank = gfxLineBuffer[by] & 3;
    c->ink     = DC_INK_OPAQUE;
    c->argb    = 0xFFFFFFFFu;
    const float x0 = destX * 2.0f, x1 = (destX + TILE_SIZE) * 2.0f;
    const float y0 = destY * 2.0f, y1 = (destY + h) * 2.0f;
    c->xs[0] = x0; c->ys[0] = y0;
    c->xs[1] = x1; c->ys[1] = y0;
    c->xs[2] = x0; c->ys[2] = y1;
    c->xs[3] = x1; c->ys[3] = y1;
    c->u0 = u0 / DC_TILESET_TEX_W; c->v0 = v0 / DC_TILESET_TEX_H;
    c->u1 = u1 / DC_TILESET_TEX_W; c->v1 = v1 / DC_TILESET_TEX_H;
    c->z  = dcSpriteZ;
    dcSpriteZ += 0.001f;
}

// ---------------------------------------------------------------------
// Pause-menu preview window: replay the frozen gameplay quads through the
// preview panel's projection.
//
// Why this is needed at all: on the DC only the SOFTWARE-rendered part of a
// frame lives in Engine.frameBuffer (the scrolling background layers). Every
// tile and sprite is a PVR quad in dcSpriteQueue instead, so the retro-buffer
// texture that PauseMenu's RenderRetroBuffer() samples holds the sky and the
// parallax hills and nothing else — no foreground tiles, no Sonic, no HUD.
// Desktop has no such split (its frameBuffer IS the whole frame), which is why
// the reference renderer needs nothing more than the one textured quad.
//
// The fix is to draw the queue a second time, warped into the preview panel.
// DC_FlipScreen has already re-armed dcSpriteCount to the frozen frame's quad
// count for the pause backdrop (see the dcLastCount restore), so the very same
// quads that paint the backdrop paint the preview — they cannot disagree.
//
// The panel is a rotated 3D quad, so the mapping from display space into it is
// a projective one. It is exact and cheap here: the four projected corners come
// in with their PVR z, and PVR z is 1/w by construction, so bilinearly
// interpolating (x*w, y*w, w) over the panel and dividing at the end reproduces
// the perspective map of the plane with no matrix work at all.
//
// Corners arrive in strip order TL, TR, BL, BR.
struct DCWV {
    float x, y, u, v;
};

// Sutherland-Hodgman against one display-space half-plane, d = sign*coord + k >= 0.
// Texture coords interpolate linearly, which is exact: (u,v) is affine over a
// sprite quad in display space (the projective part happens afterwards).
static int dcClipHalf(const DCWV *in, int n, DCWV *out, int axis, float sign, float k)
{
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const DCWV *a = &in[i];
        const DCWV *b = &in[(i + 1) % n];
        float       da = sign * (axis ? a->y : a->x) + k;
        float       db = sign * (axis ? b->y : b->x) + k;
        if (da >= 0.0f)
            out[m++] = *a;
        if ((da >= 0.0f) != (db >= 0.0f)) {
            float t  = da / (da - db);
            DCWV *o  = &out[m++];
            o->x     = a->x + (b->x - a->x) * t;
            o->y     = a->y + (b->y - a->y) * t;
            o->u     = a->u + (b->u - a->u) * t;
            o->v     = a->v + (b->v - a->v) * t;
        }
    }
    return m;
}

void DC_DrawFrozenSpritesWarped(const float *cornerX, const float *cornerY, const float *cornerZ)
{
    if (dcSpriteCount <= 0)
        return;

    // Homogeneous corners. PVR z is proportional to 1/w, so w = 1/z up to a
    // scale that cancels in the divide below.
    float Xh[4], Yh[4], Wh[4];
    for (int i = 0; i < 4; ++i) {
        if (cornerZ[i] <= 0.0001f)
            return; // corner behind the eye — the panel is degenerate this frame
        float w = 1.0f / cornerZ[i];
        Xh[i]   = cornerX[i] * w;
        Yh[i]   = cornerY[i] * w;
        Wh[i]   = w;
    }

    pvr_ptr_t curTex = (pvr_ptr_t)-1;
    int       curBank = -1, curInk = -1;

    for (int i = 0; i < dcSpriteCount; ++i) {
        const DCSpriteCmd *c = &dcSpriteQueue[i];
        if (c->z > 9000.0f)
            continue; // perf bars / overlays, not part of the game image

        // Ring order TL, TR, BR, BL (the queue stores a strip: TL, TR, BL, BR).
        DCWV ring[12], tmp[12];
        ring[0].x = c->xs[0]; ring[0].y = c->ys[0]; ring[0].u = c->u0; ring[0].v = c->v0;
        ring[1].x = c->xs[1]; ring[1].y = c->ys[1]; ring[1].u = c->u1; ring[1].v = c->v0;
        ring[2].x = c->xs[3]; ring[2].y = c->ys[3]; ring[2].u = c->u1; ring[2].v = c->v1;
        ring[3].x = c->xs[2]; ring[3].y = c->ys[2]; ring[3].u = c->u0; ring[3].v = c->v1;

        // Clip to the game screen so quads that ran off the edge during play
        // (edge tiles, half-offscreen objects) stay inside the little panel.
        int n = dcClipHalf(ring, 4, tmp, 0, 1.0f, 0.0f);
        if (n < 3) continue;
        n = dcClipHalf(tmp, n, ring, 0, -1.0f, (float)DC_DISP_W);
        if (n < 3) continue;
        n = dcClipHalf(ring, n, tmp, 1, 1.0f, 0.0f);
        if (n < 3) continue;
        n = dcClipHalf(tmp, n, ring, 1, -1.0f, (float)DC_DISP_H);
        if (n < 3) continue;

        if (c->tex != curTex || c->palBank != curBank || c->ink != curInk) {
            pvr_poly_cxt_t cx;
            if (!c->tex) {
                pvr_poly_cxt_col(&cx, PVR_LIST_TR_POLY);
            }
            else {
                pvr_poly_cxt_txr(&cx, PVR_LIST_TR_POLY, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(c->palBank), c->texW, c->texH, c->tex,
                                 PVR_FILTER_NONE);
                cx.txr.env = PVR_TXRENV_MODULATEALPHA;
                if (c->ink == DC_INK_ADD)
                    cx.blend.dst = PVR_BLEND_ONE;
            }
            cx.gen.culling = PVR_CULLING_NONE;
            // Opaque quads normally ride the PT list and lean on the alpha test;
            // here everything shares the menu's TR pass, so they alpha-blend
            // instead. Same result — the colour-key index is alpha 0 either way.
            // Depth is ALWAYS/no-write: the preview is painter's order, and it
            // must not stamp depth over the menu geometry drawn after it.
            cx.depth.comparison = PVR_DEPTHCMP_ALWAYS;
            cx.depth.write      = PVR_DEPTHWRITE_DISABLE;
            pvr_poly_hdr_t hd;
            pvr_poly_compile(&hd, &cx);
            pvr_prim(&hd, sizeof(hd));
            curTex  = c->tex;
            curBank = c->palBank;
            curInk  = c->ink;
        }

        float px[12], py[12], pz[12];
        for (int k = 0; k < n; ++k) {
            float un = ring[k].x * (1.0f / (float)DC_DISP_W);
            float vn = ring[k].y * (1.0f / (float)DC_DISP_H);
            float t0 = Xh[0] + (Xh[1] - Xh[0]) * un;
            float t1 = Xh[2] + (Xh[3] - Xh[2]) * un;
            float X  = t0 + (t1 - t0) * vn;
            t0       = Yh[0] + (Yh[1] - Yh[0]) * un;
            t1       = Yh[2] + (Yh[3] - Yh[2]) * un;
            float Y  = t0 + (t1 - t0) * vn;
            t0       = Wh[0] + (Wh[1] - Wh[0]) * un;
            t1       = Wh[2] + (Wh[3] - Wh[2]) * un;
            float W  = t0 + (t1 - t0) * vn;
            if (W <= 0.000001f)
                W = 0.000001f;
            px[k] = X / W;
            py[k] = Y / W;
            pz[k] = 1.0f / W;
        }

        pvr_vertex_t vv;
        vv.argb  = c->argb;
        vv.oargb = 0;

        if (n == 4) {
            // A convex ring emits as one strip once reordered TL, TR, BL, BR.
            static const int so[4] = { 0, 1, 3, 2 };
            for (int e = 0; e < 4; ++e) {
                int k    = so[e];
                vv.flags = (e == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                vv.x = px[k]; vv.y = py[k]; vv.z = pz[k];
                vv.u = ring[k].u; vv.v = ring[k].v;
                pvr_prim(&vv, sizeof(vv));
            }
        }
        else {
            for (int t = 2; t < n; ++t) {
                const int fan[3] = { 0, t - 1, t };
                for (int e = 0; e < 3; ++e) {
                    int k    = fan[e];
                    vv.flags = (e == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                    vv.x = px[k]; vv.y = py[k]; vv.z = pz[k];
                    vv.u = ring[k].u; vv.v = ring[k].v;
                    pvr_prim(&vv, sizeof(vv));
                }
            }
        }
    }
}

#else // !DC_USE_PVR — software presenter only, no texture cache

void DC_DrawFrozenSpritesWarped(const float *, const float *, const float *) {}
void DC_UploadSheet(int) {}
void DC_InvalidateSheet(int) {}
void DC_InvalidateAllSheets() {}
void DC_UploadPalettes() {}
void DC_QueueSprite(int, int, int, int, int, int, int, int) {}
void DC_QueueSpriteInk(int, int, int, int, int, int, int, int, int, int) {}
void DC_QueueSpriteQuad(int, const float *, const float *, int, int, int, int, int, int) {}
void DC_QueueTileQuad(int, int, int, int, int, int) {}
void DC_MarkTilesetDirty() {}
void DC_UploadTilesetNow() {}
void DC_RemapTile(int, int) {}
void DC_QueueRect(int, int, int, int, int, int, int, int) {}
void DC_ClearSpriteQueue() {}
void DC_Queue3DColorQuad(const float *, const float *, uint) {}
void DC_SetFadeOverlay(int, int, int, int) {}

#endif

int DC_InitRenderDevice()
{
    DC_LOG("[DC] video: init (DC_USE_PVR=%d)\n", DC_USE_PVR);

    SCREEN_XSIZE   = 320;
    SCREEN_CENTERX = SCREEN_XSIZE / 2;
    SetScreenDimensions(SCREEN_XSIZE, SCREEN_YSIZE);

    // The engine always renders into this 320x240 (GFX_LINESIZE stride) buffer.
    Engine.frameBuffer = new ushort[GFX_LINESIZE * SCREEN_YSIZE];
    memset(Engine.frameBuffer, 0, (GFX_LINESIZE * SCREEN_YSIZE) * sizeof(ushort));
    Engine.frameBuffer2x = NULL; // HQ mode unused on DC
    Engine.texBuffer     = NULL; // GL upload buffer unused on DC

    Engine.useHQModes   = false;
    Engine.borderless   = false;
    Engine.isFullScreen = true;

    OBJECT_BORDER_X2 = SCREEN_XSIZE + 0x80;
    OBJECT_BORDER_X4 = SCREEN_XSIZE + 0x20;

#if DC_USE_PVR
    vid_set_mode(DM_640x480, PM_RGB565);

    // pvr_init_defaults() enables only the OP and TR bins. We want the PT
    // (punch-through) bin too: opaque color-keyed sprites/tiles render there
    // depth-tested like opaque geometry, which is far cheaper per pixel than
    // the TR list's blending — TR is kept only for genuinely translucent draws
    // (alpha/additive sprites, the native menu layer, fade overlay).
    //
    // TR AUTOSORT IS DISABLED. The TR list then composites in submission order,
    // which is what both TR users actually want: sprites are already queued in
    // the engine's own draw order (with monotonically increasing z), and the
    // native menu renderer is a painter's-order UI whose coplanar batches no
    // depth sort could ever separate — see the header of DCNative.cpp. It is
    // also cheaper: no per-polygon sort pass.
    static const pvr_init_params_t pvrParams = {
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16 },
        512 * 1024, // vertex buffer
        0,          // no DMA
        0,          // no FSAA
        1,          // autosort DISABLED — TR draws in submission order
        3           // extra OPBs for overflow
    };
    pvr_init(&pvrParams);

    dcFbTexture = pvr_mem_malloc(DC_TEX_W * DC_TEX_H * 2);
    if (!dcFbTexture) {
        DC_LOG("[DC] video: framebuffer texture alloc FAILED\n");
        return 0;
    }

    // Compile the textured-quad header once (opaque list, point-sampled).
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, DC_TEX_W, DC_TEX_H, dcFbTexture, PVR_FILTER_NONE);
    cxt.gen.culling = PVR_CULLING_NONE; // a fullscreen quad must never be culled
    pvr_poly_compile(&dcFbHdr, &cxt);

    dcPvrReady = true;
#else
    vid_set_mode(DM_320x240, PM_RGB565);
    vid_empty();
#endif

    InitInputDevices();

    DC_LOG("[DC] video: init ok (linesize=%d)\n", GFX_LINESIZE);
    return 1;
}

void DC_FlipScreen()
{
    static bool firstFlip = true;
    if (firstFlip) {
        DC_LOG("[DC] video: first frame presented\n");
        firstFlip = false;
    }

    if (!Engine.frameBuffer)
        return;

#if DC_USE_PVR
    if (!dcPvrReady)
        return;

#if DC_PERF_LOG
    static unsigned long long pfLast = 0, pfFrameAcc = 0, pfFlipAcc = 0, pfWaitAcc = 0;
    static uint pfFrames = 0, pfQuadAcc = 0, pfTRAcc = 0, pfHdrAcc = 0;
    // Peak, not just mean: DC_SPRITE_MAX overruns silently DROP quads, and a
    // single spike over the cap loses geometry while barely moving an average.
    static uint pfQuadMax = 0;
    static unsigned long long pfObjAcc = 0, pfDrawAcc = 0, pfObjDrawAcc = 0;
    static uint pfOffAcc = 0;
    static uint pfSSSkipAcc = 0, pfSSTestAcc = 0;
    static uint pbFrame = 0, pbFlip = 0, pbWait = 0; // last frame's times, us
    const unsigned long long pfT0 = timer_ns_gettime64();
    if (pfLast) {
        pfFrameAcc += pfT0 - pfLast;
        pbFrame = (uint)((pfT0 - pfLast) / 1000u);
    }
    pfLast = pfT0;
    pfObjAcc += dcProfObjectsNs;
    pfDrawAcc += dcProfDrawNs;
    pfObjDrawAcc += dcProfObjDrawNs;
#endif
    // MUST clear, and OUTSIDE the guard. Without this the counters stay
    // cumulative-since-boot and the reported average grows without bound —
    // exactly what the first instrumented build did (obj climbed past 10
    // SECONDS per frame). Outside, because Scene.cpp keeps writing them
    // whatever DC_PERF_LOG is set to; inside the guard they would run away
    // silently in a shipping build.
    dcProfObjectsNs = dcProfDrawNs = dcProfObjDrawNs = 0;


    // Wait for the previous scene to finish sampling the texture BEFORE we
    // overwrite it (single-buffered texture), then upload this frame.
#if DC_PERF_LOG
    const unsigned long long pfW0 = timer_ns_gettime64();
#endif
    pvr_wait_ready();
#if DC_PERF_LOG
    {
        const unsigned long long waitNs = timer_ns_gettime64() - pfW0;
        pfWaitAcc += waitNs;
        pbWait = (uint)(waitNs / 1000u);
    }
#endif

    // Upload the 320x240 software frame into the top-left of the 512x256
    // texture (row-by-row: src stride GFX_LINESIZE, dst stride DC_TEX_W).
    // 320 px * 2 B = 640 B/row (32-byte multiple) — store-queue friendly.
    const ushort *src = Engine.frameBuffer;
    ushort *dst       = (ushort *)dcFbTexture;
    for (int y = 0; y < SCREEN_YSIZE; ++y) {
        sq_cpy(dst, src, SCREEN_XSIZE * sizeof(ushort));
        src += GFX_LINESIZE;
        dst += DC_TEX_W;
    }

    const float uMax = (float)SCREEN_XSIZE / DC_TEX_W;  // 320/512
    const float vMax = (float)SCREEN_YSIZE / DC_TEX_H;  // 240/256

    // Re-mirror the tileset after stage load / runtime tile edits (outside
    // the scene — texture loads must not happen mid-scene).
    if (dcTilesetDirty)
        DC_UploadTileset();

    // Re-stream any native menu texture this frame's batches reference but that
    // a stage load evicted from VRAM. Must run HERE: it calls pvr_mem_malloc,
    // which is not safe once the scene is open, and the render states for this
    // frame are already built, so a texture restored now is visible this frame.
    DC_NativeReloadEvicted();

    // The same job on the mesh side, one step removed: a stage load evicts the
    // menu's model geometry too (Renderer.cpp's EvictMeshData), and RenderMesh
    // reads a few slots back per frame as the menu asks to draw them. It cannot
    // be driven from here the way textures are - a mesh with no geometry
    // produces no batch at all, so there would be nothing in renderStateList to
    // notice - so all this does is hand RenderMesh its allowance for the frame
    // that is about to be built.
    ResetEvictedMeshBudget();

    pvr_scene_begin();

    // Palettes must be uploaded before any paletted draw is rendered (the
    // deferred HW samples palette RAM at scene flush, so never mid-list).
    DC_UploadPalettes();

    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&dcFbHdr, sizeof(dcFbHdr));

    pvr_vertex_t v;
    v.flags = PVR_CMD_VERTEX;
    v.argb  = 0xFFFFFFFF;
    v.oargb = 0;
    v.z     = 1.0f;

    // triangle strip: TL, TR, BL, BR
    v.x = 0.0f;        v.y = 0.0f;        v.u = 0.0f; v.v = 0.0f; pvr_prim(&v, sizeof(v));
    v.x = DC_DISP_W;   v.y = 0.0f;        v.u = uMax; v.v = 0.0f; pvr_prim(&v, sizeof(v));
    v.x = 0.0f;        v.y = DC_DISP_H;   v.u = 0.0f; v.v = vMax; pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.x = DC_DISP_W;   v.y = DC_DISP_H;   v.u = uMax; v.v = vMax; pvr_prim(&v, sizeof(v));

    pvr_list_finish();

    // Frozen frame (pause): an empty queue normally means a native-only mode
    // (pause menu) where nothing ran — re-present the last drawn frame. But
    // NOT when this frame explicitly cleared the screen (fade/transition
    // frames that ClearScreen and draw nothing: the blank IS the content —
    // re-presenting flashed the old intro text back after the title fade).
    // Texture frees/reuploads reset dcLastCount elsewhere (stage loads
    // recycled VRAM under the frozen quads: one frame of raw sprite sheet).
    // There used to be an `activeStageList != STAGELIST_SPECIAL` term on the
    // re-present below, from when special stages were software-only: Draw3DScene
    // rasterised Sonic 2's half-pipe into the framebuffer and sprites were forced
    // down the software path, so the hardware queue was legitimately empty every
    // frame and a freeze-frame would have painted the pre-stage hardware frame
    // over it. That stopped being true once DC_UseHWSprites() became
    // unconditional and DrawFace/DrawFadedFace/DrawTexturedFace grew their own
    // KOS branches — special stages queue quads like any other stage now.
    //
    // What the term still did was suppress the freeze-frame in the one case that
    // matters: an empty queue in a special stage IS the pause menu, and killing
    // dcLastCount there left the pause preview window black. Sonic 1's special
    // stage is the rotating maze — every block is a DrawSpriteFX(FX_ROTATE) that
    // lands in DC_QueueRotatedQuad — so there is a perfectly good frozen frame to
    // show, and the software rationale cannot apply to it at all.
    //
    // A stage-load frame (see dcPrevStageMode above) ran no draw pass at all,
    // and for a few frames after it the new stage may still have nothing on the
    // hardware queue — see the DC_POSTLOAD_COVER_FRAMES note. Any such frame
    // must not freeze-frame the outgoing act and must not present the stale
    // framebuffer either; it is always in the middle of a fade to black.
    if (dcPrevStageMode == STAGEMODE_LOAD && stageMode != STAGEMODE_LOAD)
        dcPostLoadFrames = DC_POSTLOAD_COVER_FRAMES;

    // No stage-list exemption. This used to carry the same
    // `activeStageList != STAGELIST_SPECIAL` term as the freeze-frame above, and
    // for the same obsolete reason — that special stages rendered in software
    // and an empty queue was normal there. It is not dead code: with the term,
    // entering a special stage fell through to the freeze-frame branch and
    // re-presented the act being left behind for the few frames before the maze
    // queued anything. That is precisely what this window exists to prevent, so
    // special stages now blank like every other stage list. The pause preview is
    // unaffected: dcPostLoadFrames is zeroed the moment anything draws, so it is
    // always 0 by the time the player can reach the pause button.
    const bool postLoadBlank = dcPostLoadFrames > 0 && dcSpriteCount == 0;

    if (dcPostLoadFrames > 0) {
        if (dcSpriteCount > 0)
            dcPostLoadFrames = 0; // the new stage is drawing; window over
        else
            --dcPostLoadFrames;
    }

    if (postLoadBlank) {
        dcSpriteCount = 0;
        dcLastCount   = 0; // never freeze-frame the act we are leaving
        // Cover the stale framebuffer quad — but not on top of a native menu.
        // The very first load of a session comes straight out of the launch
        // menu, whose geometry sits at z <= 7500, below the z-9000 fade overlay,
        // so a blanket cover there would black the menu out mid-transition.
        // The test is DC_NativeSceneVisible(), NOT DC_NativeSceneActive(): the
        // latter is true on EVERY gameplay frame (RetroGameLoop always submits
        // the retro-buffer batch, so renderStateCount is 1 by flip time), which
        // is precisely why the previous attempt at this fix cleared the sprite
        // queue — visibly, the flash lost its sprites and HUD — but never once
        // painted the black overlay that was supposed to hide the rest.
        if (!DC_NativeSceneVisible())
            DC_SetFadeOverlay(0, 0, 0, 0xFF);
    }
    else if (dcSpriteCount == 0 && !dcQueueCleared)
        dcSpriteCount = dcLastCount;
    else if (dcSpriteCount > 0)
        dcLastCount = dcSpriteCount;
    else
        dcLastCount = 0; // cleared-and-empty: the blank IS the content
    dcQueueCleared  = false;
    // Pending VMU writes land here once the pause menu has settled: the scene
    // is frozen, so the ~300ms write is invisible, and the half-second delay
    // lets the menu finish sliding in first. Fires once per pause (nothing can
    // become pending while paused). The other flush point is the stage load in
    // DC_UploadTilesetNow(); see DCSave.cpp for why writes are deferred at all.
    static int dcPausedFrames = 0;
    if (stageMode == STAGEMODE_PAUSED) {
        if (dcPausedFrames < 64)
            ++dcPausedFrames;
        if (dcPausedFrames == 30)
            DC_FlushSaves();
    }
    else
        dcPausedFrames = 0;

    dcPrevStageMode = stageMode; // what the NEXT frame's ProcessStage() will see

#if DC_PERF_LOG
    // On-screen perf bars — serial logs aren't practical on real hardware,
    // but any video/photo of the screen captures these. Queued LAST (and at
    // a near-top z) so they never count as "game drew something" above and
    // always sit over the scene. Three 2px rows at the top-left, 1 pixel
    // per 100us of ENGINE-space width:
    //   green  = whole frame period (83px = 16.7ms = locked 60fps)
    //   yellow = time inside DC_FlipScreen (renderer CPU + GPU wait)
    //   red    = time blocked on the PVR (GPU-bound when large)
    // Scale is 200us per ENGINE pixel, so the 320px screen spans 64ms. It used
    // to be 100us/px, which capped at 32ms — and on real hardware the special
    // stage exceeded that on 3.3% of frames, making the worst cases read as a
    // flat 32ms whatever they really were.
    // Reading it: green way past yellow => game logic (scripts) is the cost;
    // yellow mostly red => the GPU is; all short => no bottleneck.
    {
        dcSpriteZ = 9500.0f; // above gameplay (~2-6), native menus (<=7500) and the fade (9000)
        int w;
        w = (int)(pbFrame / 200u); if (w > 320) w = 320;
        DC_QueueRect(0, 0, w, 2, 0, 255, 0, 255);
        w = (int)(pbFlip / 200u); if (w > 320) w = 320;
        DC_QueueRect(0, 2, w, 2, 255, 255, 0, 255);
        w = (int)(pbWait / 200u); if (w > 320) w = 320;
        DC_QueueRect(0, 4, w, 2, 255, 0, 0, 255);
    }
#endif

    // Flush the frame's queued quads. Two hardware lists:
    //
    //   PT (punch-through): all DC_INK_OPAQUE sprites/tiles. PT is depth-
    //   tested like opaque geometry — the per-draw z alone decides layering,
    //   so submission order is free and we group by texture+bank to minimize
    //   header recompiles. Far cheaper per pixel than TR (no autosort, no
    //   blending), which matters when the screen is full of sprites.
    //
    //   TR (translucent): alpha/additive sprites + the fade overlay, in
    //   queue order (autosort handles z). TR renders after PT, and PT wrote
    //   depth, so translucent draws correctly sit between/behind opaque ones.
    static byte dcEmitted[DC_SPRITE_MAX];
    // The native menu/pause geometry (Christian Whitehead's UI layer) is flushed
    // in ONE pass on the TR list (DC_RenderNativeScene) — models included, depth
    // test off, submission order only, exactly like the GL reference. It used to
    // put meshes on the PT list; that made painter's order impossible to express
    // and is documented as a dead end in DCNative.cpp. Open TR if the menu drew
    // anything, even when no sprites/fade do.
    bool anyPT = false, anyTR = dcFadeActive || DC_NativeSceneActive();
    int  trQuads = 0, hdrCount = 0;
    for (int i = 0; i < dcSpriteCount; ++i) {
        const bool opaque = (dcSpriteQueue[i].ink == DC_INK_OPAQUE);
        dcEmitted[i]      = !opaque; // pre-mark translucent as "done" for the PT pass
        if (opaque) anyPT = true;
        else { anyTR = true; ++trQuads; }
    }
#if DC_PERF_LOG
    pfQuadAcc += (uint)dcSpriteCount;
    pfOffAcc += dcQuadOffscreen;
    pfSSSkipAcc += dcSSCullSkipped;
    pfSSTestAcc += dcSSCullTested;
    if ((uint)dcSpriteCount > pfQuadMax)
        pfQuadMax = (uint)dcSpriteCount;
    pfTRAcc += (uint)trQuads;
#endif

    if (anyPT) {
        pvr_list_begin(PVR_LIST_PT_POLY);
#if DC_USE_SQ_VERTS
        pvr_dr_state_t dcDR;
        pvr_dr_init(&dcDR);
#endif
        for (int i = 0; i < dcSpriteCount; ++i) {
            if (dcEmitted[i])
                continue;
            const DCSpriteCmd *k = &dcSpriteQueue[i];

            pvr_poly_cxt_t cx;
            pvr_poly_cxt_txr(&cx, PVR_LIST_PT_POLY, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(k->palBank), k->texW, k->texH, k->tex, PVR_FILTER_NONE);
            cx.gen.culling = PVR_CULLING_NONE;
            cx.txr.env     = PVR_TXRENV_MODULATEALPHA;
            pvr_poly_hdr_t hd;
            pvr_poly_compile(&hd, &cx);
#if DC_USE_SQ_VERTS
            DC_SQEmitHdr(&dcDR, &hd);
#else
            pvr_prim(&hd, sizeof(hd));
#endif
            ++hdrCount;

            // Emit every remaining opaque quad sharing this texture+bank.
            for (int j = i; j < dcSpriteCount; ++j) {
                if (dcEmitted[j])
                    continue;
                const DCSpriteCmd *c = &dcSpriteQueue[j];
                if (c->tex != k->tex || c->palBank != k->palBank)
                    continue;
                dcEmitted[j] = 1;

#if DC_USE_SQ_VERTS
                DC_SQEmitQuad(&dcDR, c);
#else
                pvr_vertex_t vv;
                vv.flags = PVR_CMD_VERTEX;
                vv.argb  = c->argb;
                vv.oargb = 0;
                vv.z     = c->z;
                vv.x = c->xs[0]; vv.y = c->ys[0]; vv.u = c->u0; vv.v = c->v0; pvr_prim(&vv, sizeof(vv));
                vv.x = c->xs[1]; vv.y = c->ys[1]; vv.u = c->u1; vv.v = c->v0; pvr_prim(&vv, sizeof(vv));
                vv.x = c->xs[2]; vv.y = c->ys[2]; vv.u = c->u0; vv.v = c->v1; pvr_prim(&vv, sizeof(vv));
                vv.flags = PVR_CMD_VERTEX_EOL;
                vv.x = c->xs[3]; vv.y = c->ys[3]; vv.u = c->u1; vv.v = c->v1; pvr_prim(&vv, sizeof(vv));
#endif
            }
        }

#if DC_USE_SQ_VERTS
        pvr_dr_finish();
#endif
        pvr_list_finish();
    }

    if (anyTR) {
        pvr_list_begin(PVR_LIST_TR_POLY);
#if DC_USE_SQ_VERTS
        pvr_dr_state_t dcDR;
        pvr_dr_init(&dcDR);
#endif

        pvr_ptr_t curTex = (pvr_ptr_t)-1; // sentinel: force first header compile
        int       curBank = -1, curInk = -1;
        for (int i = 0; i < dcSpriteCount; ++i) {
            const DCSpriteCmd *c = &dcSpriteQueue[i];
            if (c->ink == DC_INK_OPAQUE)
                continue; // rendered in the PT pass
            if (c->tex != curTex || c->palBank != curBank || c->ink != curInk) {
                pvr_poly_hdr_t hd;
                if (!c->tex) {
                    // Flat-colour quad (DrawRect): no texture, vertex ARGB is
                    // the rect colour; default TR blend (SRCALPHA/INVSRCALPHA)
                    // gives the script's alpha compositing.
                    pvr_poly_cxt_t cx;
                    pvr_poly_cxt_col(&cx, PVR_LIST_TR_POLY);
                    cx.gen.culling = PVR_CULLING_NONE;
                    pvr_poly_compile(&hd, &cx);
                }
                else {
                    pvr_poly_cxt_t cx;
                    pvr_poly_cxt_txr(&cx, PVR_LIST_TR_POLY, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(c->palBank), c->texW, c->texH, c->tex, PVR_FILTER_NONE);
                    cx.gen.culling = PVR_CULLING_NONE;
                    // MODULATEALPHA so the per-sprite vertex alpha actually scales
                    // the texture's alpha (the default MODULATE/REPLACE env ignores
                    // vertex alpha, which made alpha-blended sprites draw opaque —
                    // no fade-in).
                    cx.txr.env = PVR_TXRENV_MODULATEALPHA;
                    // Alpha uses the TR default (SRCALPHA/INVSRCALPHA); the
                    // texture's 1-bit alpha is the colour key and the vertex alpha
                    // scales it. Additive keeps SRCALPHA but adds onto the frame.
                    if (c->ink == DC_INK_ADD)
                        cx.blend.dst = PVR_BLEND_ONE;
                    pvr_poly_compile(&hd, &cx);
                }
#if DC_USE_SQ_VERTS
                DC_SQEmitHdr(&dcDR, &hd);
#else
                pvr_prim(&hd, sizeof(hd));
#endif
                ++hdrCount;
                curTex  = c->tex;
                curBank = c->palBank;
                curInk  = c->ink;
            }

#if DC_USE_SQ_VERTS
            DC_SQEmitQuad(&dcDR, c);
#else
            pvr_vertex_t vv;
            vv.flags = PVR_CMD_VERTEX;
            vv.argb  = c->argb;
            vv.oargb = 0;
            vv.z     = c->z;
            vv.x = c->xs[0]; vv.y = c->ys[0]; vv.u = c->u0; vv.v = c->v0; pvr_prim(&vv, sizeof(vv));
            vv.x = c->xs[1]; vv.y = c->ys[1]; vv.u = c->u1; vv.v = c->v0; pvr_prim(&vv, sizeof(vv));
            vv.x = c->xs[2]; vv.y = c->ys[2]; vv.u = c->u0; vv.v = c->v1; pvr_prim(&vv, sizeof(vv));
            vv.flags = PVR_CMD_VERTEX_EOL;
            vv.x = c->xs[3]; vv.y = c->ys[3]; vv.u = c->u1; vv.v = c->v1; pvr_prim(&vv, sizeof(vv));
#endif
        }

#if DC_USE_SQ_VERTS
        // Close the direct-render batch BEFORE anything goes back through
        // pvr_prim: DC_RenderNativeScene and the fade overlay below both use it,
        // and its sq_cpy reprograms QACR out from under the queue.
        pvr_dr_finish();
#endif

        // Christian Whitehead's native menus / pause screen (launch menu, title,
        // options, pause). Renders on top of the framebuffer quad and any frozen
        // gameplay behind it. No-op during normal play (renderStateCount < 1).
        // Submitted BEFORE the fade: TR autosort is off, so the fade only covers
        // the menu if it is queued after it.
        DC_RenderNativeScene();

        // Fade overlay: a solid quad over the whole scene, submitted last so it
        // paints over everything on this list. Its z still has to clear the
        // native menu's range — that renderer emits z = (1/w) * 30000 and its
        // near-plane clip at ez >= 4 caps it at 30000/4 = 7500 — because the
        // fade quad itself is depth-tested against whatever wrote depth.
        if (dcFadeActive) {
            pvr_poly_cxt_t fcx;
            pvr_poly_cxt_col(&fcx, PVR_LIST_TR_POLY);
            fcx.gen.culling = PVR_CULLING_NONE;
            pvr_poly_hdr_t fhd;
            pvr_poly_compile(&fhd, &fcx);
            pvr_prim(&fhd, sizeof(fhd));

            const uint fargb = ((uint)(dcFadeA & 0xFF) << 24) | ((uint)(dcFadeR & 0xFF) << 16) | ((uint)(dcFadeG & 0xFF) << 8) | (uint)(dcFadeB & 0xFF);
            pvr_vertex_t fv;
            fv.flags = PVR_CMD_VERTEX;
            fv.argb  = fargb;
            fv.oargb = 0;
            fv.z     = 9000.0f;
            fv.x = 0.0f;      fv.y = 0.0f;      fv.u = 0.0f; fv.v = 0.0f; pvr_prim(&fv, sizeof(fv));
            fv.x = DC_DISP_W; fv.y = 0.0f;      fv.u = 0.0f; fv.v = 0.0f; pvr_prim(&fv, sizeof(fv));
            fv.x = 0.0f;      fv.y = DC_DISP_H; fv.u = 0.0f; fv.v = 0.0f; pvr_prim(&fv, sizeof(fv));
            fv.flags = PVR_CMD_VERTEX_EOL;
            fv.x = DC_DISP_W; fv.y = DC_DISP_H; fv.u = 0.0f; fv.v = 0.0f; pvr_prim(&fv, sizeof(fv));
        }

        pvr_list_finish();
    }
    dcSpriteCount   = 0;
    dcSpriteZ       = 2.0f;
    dcFadeActive    = false;
    dcQuadOffscreen = 0;
    dcSSCullTested  = 0;
    dcSSCullSkipped = 0;

    pvr_scene_finish();

#if DC_PERF_LOG
    pfHdrAcc += (uint)hdrCount;
    {
        const unsigned long long flipNs = timer_ns_gettime64() - pfT0;
        pfFlipAcc += flipNs;
        pbFlip = (uint)(flipNs / 1000u);
    }
    if (++pfFrames >= 120) {
        // obj  = ProcessObjects  (ObjectUpdate scripts)
        // draw = DrawStageGFX     (tile passes + ObjectDraw scripts)
        // frame - flip - obj - draw is whatever is left: input, audio thread
        // preemption, the frame limiter.
        // sscull=skipped/considered — the special-stage radial draw cull. Zero
        // outside the special stage; there is nothing to cull in a normal zone.
        DC_LOG("[DC] perf: frame=%uus flip=%uus wait=%uus obj=%uus draw=%uus (tiles=%uus objdraw=%uus) quads=%u offscr=%u sscull=%u/%u peak=%u/%u tr=%u hdrs=%u (avg over %u frames)\n",
               (uint)(pfFrameAcc / pfFrames / 1000u), (uint)(pfFlipAcc / pfFrames / 1000u), (uint)(pfWaitAcc / pfFrames / 1000u),
               (uint)(pfObjAcc / pfFrames / 1000u), (uint)(pfDrawAcc / pfFrames / 1000u),
               (uint)((pfDrawAcc > pfObjDrawAcc ? pfDrawAcc - pfObjDrawAcc : 0) / pfFrames / 1000u), (uint)(pfObjDrawAcc / pfFrames / 1000u),
               pfQuadAcc / pfFrames, pfOffAcc / pfFrames, pfSSSkipAcc / pfFrames, pfSSTestAcc / pfFrames,
               pfQuadMax, (uint)DC_SPRITE_MAX, pfTRAcc / pfFrames, pfHdrAcc / pfFrames, pfFrames);
        pfFrameAcc = pfFlipAcc = pfWaitAcc = 0;
        pfQuadAcc = pfTRAcc = pfHdrAcc = 0;
        pfQuadMax = 0;
        pfObjAcc = pfDrawAcc = pfObjDrawAcc = 0;
        pfOffAcc = 0;
        pfSSSkipAcc = pfSSTestAcc = 0;
        pfFrames = 0;
    }
#endif
#else
    vid_waitvbl();
    const ushort *src = Engine.frameBuffer;
    ushort *dst       = vram_s;
    for (int y = 0; y < SCREEN_YSIZE; ++y) {
        sq_cpy(dst, src, SCREEN_XSIZE * sizeof(ushort));
        src += GFX_LINESIZE;
        dst += SCREEN_XSIZE;
    }
#endif
}

void DC_ReleaseRenderDevice()
{
    if (Engine.frameBuffer) {
        delete[] Engine.frameBuffer;
        Engine.frameBuffer = NULL;
    }
#if DC_USE_PVR
    if (dcPvrReady) {
        pvr_shutdown();
        dcPvrReady = false;
    }
#endif
}

#endif // RETRO_USING_KOS
