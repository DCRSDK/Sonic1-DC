// ---------------------------------------------------------------------
// RSDKv4 Dreamcast (KallistiOS) — native "Retro Engine" renderer backend
//
// The native menu/pause layer (NativeObjects/*) is Christian Whitehead's
// hardware UI. On desktop it renders through OpenGL: every RenderRect /
// RenderImage / RenderText / RenderMesh call fills a batch in Renderer.cpp's
// renderStateList[], and RenderScene() flushes it with glDrawElements. On the
// Dreamcast (RETRO_SOFTWARE_RENDER, no GL) that flush compiles to nothing, so
// the launch menu, title screen and pause menu are invisible.
//
// This backend re-implements the flush on the PVR: a single generic consumer
// of renderStateList[] that software-transforms each vertex and submits
// textured/flat triangles to the open TR list in DC_FlipScreen. See
// DCNative.hpp for the interface.
// ---------------------------------------------------------------------
#include "../RetroEngine.hpp"

#if RETRO_USING_KOS

#include <stdio.h> // fopen/fread/fclose — the .dtex loose-file loader
#include "DCNative.hpp"

#define DC_OUT_W (640.0f)
#define DC_OUT_H (480.0f)

// UNIFIED depth model: every batch — 2D and mesh alike — uses TRUE eye depth,
//
//   z = (1/ez) * PERSP_SCALE * (1 + batchIndex * BATCH_BIAS)
//
// Two things fall out of this and both are load-bearing:
//
// 1. The 1/ez term is exactly what the PVR needs: it treats vertex z as 1/w for
//    perspective-correcting textures, so a pure 1/w depth is the only thing that
//    keeps the rotating cartridge/box/Sonic-head from "swimming". Note that
//    perspective correction is invariant under SCALING z but NOT under adding a
//    constant — which is why the per-batch term is a MULTIPLIER, not an offset.
//    (The old model added BASE + batch*STEP and paid for it in affine warp.)
//
// 2. The whole native menu is authored on ONE plane at model z = 160: the white
//    fill, the watermark, the rotating BGCircle/BGLines meshes, each button's
//    yellow disc and its text label are all coplanar (see MenuBG.cpp,
//    StartGameButton.cpp, DialogPanel.cpp). Real depth cannot order coplanar
//    geometry, and the desktop GL path never asks it to: Drawing.cpp disables
//    GL_DEPTH_TEST for 2D, so 2D-vs-mesh is pure PAINTER'S / submission order and
//    the z-buffer only separates the solid models from each other. We get that
//    from submission order directly (autosort off, see below); the
//    (1 + b*BATCH_BIAS) factor survives only as a tie-break nudge between
//    depth-tested batches that would otherwise land on identical z.
//
// PERSP_SCALE puts the authoring plane (ez=160 → iw≈0.00625) at ~187, above the
// framebuffer quad (z=1) and gameplay sprites (z~2-6), and the near clip at
// ez=4 caps native z at 7500 (the DCGraphics fade sits above that).
//
// EVERYTHING goes on ONE list — TR — WITH THE AUTOSORTER DISABLED, so the list
// composites strictly in submission order. Per batch the depth test follows
// `state->depthTest` (RenderMesh's third argument): off for all 2D and for the
// panel meshes, on for the solid models. That is exactly what the reference does
// — Renderer.cpp:RenderScene() opens with glDisable(GL_DEPTH_TEST) and
// Renderer.cpp:479-490 re-enables it only when a batch asks.
//
// Two dead ends, both already tried on hardware; do not retry either:
//
//  a) Meshes on the PT list, 2D on TR. PT composites before TR, so a PT mesh
//     either always loses to the 2D above it (discs paint over their own icons)
//     or, if the 2D depth-tests against it, always wins — an opaque panel mesh
//     depth-kills the text submitted before it, which is what erased the SAVE
//     SELECT slot text. Neither is painter's order; only one list can be.
//
//  b) One TR list with autosort left ON, relying on the batch-bias multiplier to
//     encode order. The autosorter re-sorts by true depth, and a 1e-4-per-batch
//     bias cannot outrank the scene's real depth spread (a carousel icon at
//     ez=100 sorts a full 60% above the label drawn after it at ez=160). Order
//     has to come from the hardware honouring submission order, not from z.
#define DC_NATIVE_PERSP_SCALE (30000.0f)
#define DC_NATIVE_BATCH_BIAS  (0.0001f)

// The native menus are authored on a plane at model z = 160 (RenderRect /
// RenderImage / RenderRetroBuffer all pass z = 160). We use a clean pinhole
// perspective sized so that plane fills the display. This deliberately does
// NOT reproduce the engine's SetPerspectiveMatrix (which is wildly anisotropic
// and only resolves on desktop via GL matrix-stack state we lack here).
#define DC_NATIVE_ZDESIGN (160.0f)

// Compact on-screen diagnostic strip (top-left value bars). 0 = off.
// Bars (each length encodes the value in px): red = #batches x4,
// green = #textured-batches x4, blue = #textures-actually-in-VRAM x4,
// yellow = SCREEN_CENTERX_F, cyan = SCREEN_CENTERY_F, white = free VRAM / 32KB.
//
// OFF as of 2026-07-29. The expensive site is the per-batch scene dump near the
// bottom of this file: it fires every 300 native frames and printf+fflushes one
// line per batch, and those writes go through the PTY layer whether or not
// anything is listening — a burst of blocking I/O in the middle of a menu frame.
// It existed to chase the Box BACK texture binding dark (cache=0 in that dump),
// which is now closed. Set it back to 1 to debug the native renderer; the
// texture-load FAILURE line below is unguarded and reports either way.
#define DC_NATIVE_DIAG 0

static bool dcProjReady = false;

// Triangles with any vertex nearer than this (eye-space z) are dropped. The
// native menus are authored around z=160; the spinning 3D background meshes
// (MenuBG circles/lines, title screenshot panels) fly in from behind the camera
// and would otherwise project vertices to infinity, producing rainbow streaks.
#define DC_NATIVE_NEARZ (4.0f)

// Called from SetPerspectiveMatrix() at video init. We don't use the engine's
// matrix; this just flags that the display has been sized. We also reserve
// textureList[0] here, which the engine normally does in its GL init (compiled
// out on DC): without it the first menu texture lands in slot 0 and every
// RenderRetroBuffer call would draw that texture instead of the game.
//
// The slot is set up to behave like the desktop one rather than being disabled:
//
//   fileName '#'  — non-empty, so LoadTexture's linear scan walks past it.
//   format        — NON-ZERO, because RenderRetroBuffer bails on a zero format.
//                   That call is how the pause menu asks for its preview window;
//                   leaving it inert (as this used to) is why the preview was a
//                   plain black rectangle. DC_RenderNativeScene decides what to
//                   actually draw — see the retro-buffer note there, which is
//                   also what keeps the full-screen gameplay call free.
//   id            — DC_NATIVE_FBTEX_ID, a sentinel outside 0..TEXTURE_COUNT.
//                   On desktop this field holds a GL texture name, which is
//                   never 0; RenderRetroBuffer relies on that when it decides
//                   whether to open its own batch (it compares the id against
//                   currentRenderState.id, and PauseMenu.cpp hands it a
//                   just-opened untextured RenderRect batch whose id is 0). A
//                   0 here would make the preview quad inherit useTexture=false
//                   and draw as an untextured smear.
void DC_NativeSetProjection(const float *m16)
{
    (void)m16;
    if (textureList[0].fileName[0] == '\0') {
        textureList[0].fileName[0] = '#';
        textureList[0].fileName[1] = '\0';
        textureList[0].format      = TEXFMT_RETROBUFFER;
        textureList[0].id          = DC_NATIVE_FBTEX_ID;
    }
    dcProjReady = true;
}

// ---- texture cache (keyed by textureList[] index) ----
struct DCNativeTex {
    pvr_ptr_t ptr;
    int       potW, potH;
    float     uS, vS; // original / padded, to remap normalized texcoords
    int       pvrFmt; // base pixel format (PVR_TXRFMT_ARGB1555 / ARGB4444)
    bool      isVQ;   // true: VQ-compressed (twiddled codebook); false: linear non-twiddled
    bool      noReload; // a re-stream was tried and failed; don't retry every frame
};
static DCNativeTex dcNTex[TEXTURE_COUNT];

static int dcNextPow2(int n)
{
    int p = 8;
    while (p < n) p <<= 1;
    return p;
}

void DC_NativeUploadTexture(int texID, const void *rgba, int w, int h, int format)
{
    if (texID < 0 || texID >= TEXTURE_COUNT || !rgba || w <= 0 || h <= 0)
        return;

    DCNativeTex *t = &dcNTex[texID];
    if (t->ptr) {
        pvr_mem_free(t->ptr);
        t->ptr = NULL;
    }

    int potW = dcNextPow2(w);
    int potH = dcNextPow2(h);

    // Temp 16bpp POT buffer for convert+pad, malloc'd and freed immediately so
    // it never sits on the heap while the engine's next stbi_load runs (holding
    // a persistent buffer here starved the decoder and dropped most textures).
    size_t  need = (size_t)potW * potH * sizeof(ushort);
    ushort *tmp  = (ushort *)malloc(need);
    if (!tmp) {
        printf("[DCNative] tex %d (%dx%d->%dx%d) RAM malloc FAILED (%u B)\n", texID, w, h, potW, potH, (uint)need);
        fflush(stdout);
        return;
    }
    memset(tmp, 0, need);

    const byte *src    = (const byte *)rgba;
    bool        is1555 = (format == TEXFMT_RGBA5551);
    int         pvrFmt = is1555 ? PVR_TXRFMT_ARGB1555 : PVR_TXRFMT_ARGB4444;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const byte *p = &src[(x + y * w) * 4];
            int         r = p[0], g = p[1], b = p[2], a = p[3];
            if (is1555)
                tmp[x + y * potW] = ((a >= 128 ? 1 : 0) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
            else
                tmp[x + y * potW] = ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
        }
    }

    pvr_ptr_t vram = pvr_mem_malloc(need);
    if (vram) {
        pvr_txr_load(tmp, vram, (uint)need);
        t->ptr      = vram;
        t->potW     = potW;
        t->potH     = potH;
        t->uS       = (float)w / (float)potW;
        t->vS       = (float)h / (float)potH;
        t->pvrFmt   = pvrFmt;
        t->isVQ     = false;
        t->noReload = false;
#if DC_NATIVE_DIAG
        printf("[DCNative] tex %d OK %dx%d->%dx%d fmt=%d avail=%u\n", texID, w, h, potW, potH, format, (uint)pvr_mem_available());
        fflush(stdout);
#endif
    }
    else {
        printf("[DCNative] tex %d (%dx%d) VRAM malloc FAILED (%u B), avail=%u\n", texID, potW, potH, (uint)need, (uint)pvr_mem_available());
        fflush(stdout);
    }
    free(tmp);
}

// Load a pre-converted .dtex (see tools/dtex_convert.py) straight into VRAM,
// streaming the file in small chunks so it needs almost no main RAM — unlike
// stbi_load, which must allocate the whole decoded image and fails on the DC's
// fragmented heap for anything bigger than ~256x256 (why most menu/model PNGs
// were invisible). Populates dcNTex[texID] and returns the ORIGINAL image
// dimensions via outW/outH. Returns false if the file is missing/invalid so
// the caller can fall back to stbi.
bool DC_NativeTryLoadDtex(int texID, const char *pngPath, int *outW, int *outH)
{
    if (texID < 0 || texID >= TEXTURE_COUNT)
        return false;

    // Read the converted texture as a LOOSE file from the data root, so it can be
    // dropped onto the disc next to Data.rsdk WITHOUT repacking the data file.
    // The format is identified by the 'DTEX' magic header, not the extension, so
    // the file may be named either "X.dtex" or kept as "X.png" (holding .dtex
    // data — the same trick as the downsampled .ogg audio). We try the .dtex
    // sibling first, then the exact requested path.
    //
    // The prefix is BASE_PATH rather than a literal "/cd/". This loader
    // deliberately bypasses LoadFile (which MD5-resolves Data.rsdk first, so its
    // loose-file fallback never runs), and while the root was hardcoded it was
    // the ONE consumer of the loose Data/ tree that ignored the cmake data-root
    // swap: building with BASE_PATH="/pc/" for dcload moved the ELF, Data.rsdk,
    // settings.ini and the music onto /pc but left every menu and model texture
    // looking for a disc that isn't in the drive.
    //
    // BASE_PATH is a compile-time string literal (platforms/KallistiOS.cmake), so
    // on a disc build this expands to exactly the "/cd/" it has always been —
    // no probing, no per-file fallback, no fOpen redirect. That distinction
    // matters: the 2026-07-29 attempt at RUNTIME /cd+/pc resolution broke asset
    // loading on disc as well as dcload and is on the do-not-retry list.
    char cdPng[0x110], cdDtex[0x110];
    {
        int i = 0;
        const char *pfx = BASE_PATH;
        for (int k = 0; pfx[k] && i < (int)sizeof(cdPng) - 1; ++k) cdPng[i++] = pfx[k];
        for (int k = 0; pngPath[k] && i < (int)sizeof(cdPng) - 1; ++k) cdPng[i++] = pngPath[k];
        cdPng[i] = '\0';

        // cdDtex = cdPng with its extension replaced by ".dtex"
        int dot = -1;
        for (int k = i - 1; k >= 0 && cdPng[k] != '/'; --k) {
            if (cdPng[k] == '.') {
                dot = k;
                break;
            }
        }
        if (dot > 0) {
            int j = 0;
            for (; j < dot; ++j) cdDtex[j] = cdPng[j];
            const char *ext = ".dtex";
            for (int k = 0; ext[k] && j < (int)sizeof(cdDtex) - 1; ++k) cdDtex[j++] = ext[k];
            cdDtex[j] = '\0';
        }
        else {
            cdDtex[0] = '\0';
        }
    }

    FILE *f = NULL;
    if (cdDtex[0])
        f = fopen(cdDtex, "rb"); // prefer an explicit .dtex sibling
    if (!f)
        f = fopen(cdPng, "rb"); // else the requested path (may be a .png holding dtex data)
    if (!f)
        return false;

    byte hdr[16];
    if (fread(hdr, 1, 16, f) != 16 || hdr[0] != 'D' || hdr[1] != 'T' || hdr[2] != 'E' || hdr[3] != 'X') {
        fclose(f); // not a converted texture (e.g. a genuine PNG) — fall back to stbi
        return false;
    }
    int fmt   = hdr[5];
    int origW = hdr[6] | (hdr[7] << 8);
    int origH = hdr[8] | (hdr[9] << 8);
    int potW  = hdr[10] | (hdr[11] << 8);
    int potH  = hdr[12] | (hdr[13] << 8);

    if (potW < 8 || potH < 8 || potW > 1024 || potH > 1024 || origW <= 0 || origH <= 0) {
        fclose(f);
        return false;
    }

    // fmt 1/2 = plain 16bpp non-twiddled linear (potW*potH*2 bytes).
    // fmt 3/4 = VQ: a 2048-byte codebook + one index byte per 2x2 block,
    //           i.e. 2048 + (potW/2)*(potH/2) bytes, twiddled (see dtex_convert.py).
    bool   isVQ      = (fmt == 3 || fmt == 4);
    size_t dataBytes = isVQ ? (2048 + (size_t)(potW / 2) * (potH / 2))
                            : ((size_t)potW * potH * 2);
    pvr_ptr_t vram   = pvr_mem_malloc(dataBytes);
    if (!vram) {
        printf("[DCNative] dtex %d VRAM malloc FAILED (%u B) avail=%u\n", texID, (uint)dataBytes, (uint)pvr_mem_available());
        fflush(stdout);
        fclose(f);
        return false;
    }

    // Stream file -> VRAM in 32KB chunks. The payload (linear non-twiddled 16bpp,
    // or a VQ codebook followed by its twiddled index array) is contiguous, so it
    // maps to VRAM sequentially either way. Static buffer: no heap churn.
    static byte chunk[32768];
    size_t      off = 0;
    bool        ok  = true;
    while (off < dataBytes) {
        size_t n = dataBytes - off;
        if (n > sizeof(chunk))
            n = sizeof(chunk);
        if (fread(chunk, 1, n, f) != n) {
            ok = false;
            break;
        }
        pvr_txr_load(chunk, (pvr_ptr_t)((byte *)vram + off), (uint)n);
        off += n;
    }
    fclose(f);
    if (!ok) {
        pvr_mem_free(vram);
        printf("[DCNative] dtex %d short read\n", texID);
        fflush(stdout);
        return false;
    }

    DCNativeTex *t = &dcNTex[texID];
    if (t->ptr)
        pvr_mem_free(t->ptr);
    t->ptr      = vram;
    t->potW     = potW;
    t->potH     = potH;
    t->uS       = (float)origW / (float)potW;
    t->vS       = (float)origH / (float)potH;
    t->pvrFmt   = (fmt == 2 || fmt == 4) ? PVR_TXRFMT_ARGB1555 : PVR_TXRFMT_ARGB4444;
    t->isVQ     = isVQ;
    t->noReload = false;

    if (outW) *outW = origW;
    if (outH) *outH = origH;

#if DC_NATIVE_DIAG
    printf("[DCNative] dtex %d OK %dx%d->%dx%d fmt=%d%s avail=%u\n", texID, origW, origH, potW, potH, fmt, isVQ ? "(VQ)" : "", (uint)pvr_mem_available());
    fflush(stdout);
#endif
    return true;
}

void DC_NativeFreeTextures()
{
    for (int i = 0; i < TEXTURE_COUNT; ++i) {
        if (dcNTex[i].ptr) {
            pvr_mem_free(dcNTex[i].ptr);
            dcNTex[i].ptr = NULL;
        }
        dcNTex[i].noReload = false;
    }
}

// Evict native menu textures from VRAM. Called when a game STAGE loads: the menu
// textures (several MB of VRAM) are not needed during gameplay and would
// otherwise starve the stage's tileset + sprite sheets (which then fail to
// allocate and the game renders broken). Slot 0 is left alone (reserved
// retro-buffer slot).
//
// The engine-side textureList[] entries are deliberately KEPT. This used to
// clear them so that LoadTexture would re-stream the .dtex next time a menu
// object asked for it, and that works — but ONLY for objects that are created
// fresh. Coming back from a stage the engine does not create them: Object.cpp's
// RestoreNativeObjects() is a raw memcpy of the entity bank, so no Create event
// runs and LoadTexture is never called again. The restored entities still hold
// the texture SLOT INDICES they were given before the stage, and clearing the
// entries left those pointing at empty slots — the main menu came back with its
// labels but no carousel discs and no icon meshes. Worse, a cleared entry is a
// hole in a list LoadTexture scans linearly and stops at, so the next load would
// silently reuse a slot some restored entity was still referencing.
//
// So: free the pixels, keep the bookkeeping, and let DC_NativeReloadEvicted
// bring back exactly the textures the next frame actually draws.
void DC_NativeResetTextures()
{
    // The in-game PAUSE menu draws with the same bitmap fonts as the launch
    // menu, so the font glyph-atlas textures MUST survive a stage load — freeing
    // them (as we do the logos, box model, portraits and backgrounds, none of
    // which gameplay needs) left the pause menu with no text. Fonts are only a
    // few small atlases, so keeping them resident costs little VRAM. Collect the
    // texIDs every loaded font references and spare those.
    bool keep[TEXTURE_COUNT];
    for (int i = 0; i < TEXTURE_COUNT; ++i)
        keep[i] = false;
    for (int fi = 0; fi < FONTLIST_COUNT; ++fi) {
        BitmapFont *bf = &fontList[fi];
        int         nc = bf->count;
        if (nc > FONTLIST_CHAR_COUNT)
            nc = FONTLIST_CHAR_COUNT;
        for (int c = 0; c < nc; ++c) {
            int tid = bf->characters[c].textureID;
            if (tid > 0 && tid < TEXTURE_COUNT)
                keep[tid] = true;
        }
    }

    for (int i = 1; i < TEXTURE_COUNT; ++i) {
        if (keep[i])
            continue; // font atlas — keep resident so the pause menu has text
        if (dcNTex[i].ptr) {
            pvr_mem_free(dcNTex[i].ptr);
            dcNTex[i].ptr = NULL;
        }
        dcNTex[i].noReload = false; // a fresh stage cycle earns a fresh attempt
    }
}

// A .dtex re-stream is a CD read. A menu can reference a dozen textures, so cap
// how many come back per frame: spread over a handful of frames the cost is
// invisible, where doing them all at once is one long visible stall.
#define DC_NATIVE_RELOAD_PER_FRAME (4)

// Restore textures this frame's batches reference but that DC_NativeResetTextures
// evicted. Runs in DC_FlipScreen BEFORE pvr_scene_begin() — pvr_mem_malloc is not
// safe inside an open scene — and after the frame's render states are built, so a
// texture restored here is already visible on this frame.
//
// Note this costs nothing during gameplay: with no menu up, the native path
// produces no batches at all, so the loop below has nothing to walk.
void DC_NativeReloadEvicted()
{
    if (renderStateCount < 1 || !dcProjReady)
        return;

    int budget = DC_NATIVE_RELOAD_PER_FRAME;
    for (int b = 0; b < renderStateCount && budget > 0; ++b) {
        RenderState *s = &renderStateList[b];
        if (!s->indexCount || !s->vertPtr || !s->useTexture)
            continue;

        int id = s->id;
        if (id <= 0 || id >= TEXTURE_COUNT)
            continue; // untextured, or the retro-buffer sentinel (not ours to load)
        if (dcNTex[id].ptr || dcNTex[id].noReload)
            continue; // already resident, or known unrecoverable
        if (textureList[id].fileName[0] == '\0')
            continue; // slot never held anything

        --budget;
        int w = 0, h = 0;
        if (!DC_NativeTryLoadDtex(id, textureList[id].fileName, &w, &h)) {
            // No .dtex on the disc, or VRAM is full (a paused stage still owns
            // its tileset + sheets). We cannot rebuild it from the PNG here —
            // stbi needs the whole decoded image on a heap that is fragmented by
            // now — so latch the failure rather than re-reading the CD on every
            // single frame for the rest of the session.
            printf("[DCNative] reload FAILED tex %d '%s' avail=%u\n", id, textureList[id].fileName, (uint)pvr_mem_available());
            fflush(stdout);
            dcNTex[id].noReload = true;
        }
    }
}

bool DC_NativeSceneActive() { return renderStateCount >= 1; }

// "Is a native menu actually on screen?" — NOT the same question as
// DC_NativeSceneActive(), which is true during gameplay too.
//
// RetroEngine's main loop calls ProcessNativeObjects() every frame. That resets
// renderStateCount to -1, runs the native entities, then calls RenderScene(),
// which flushes the open batch. In a plain gameplay frame the only native entity
// is RetroGameLoop, and all it submits is RenderRetroBuffer's full-screen quad —
// so renderStateCount is 1 by flip time and DC_NativeSceneActive() reads true
// even though nothing native is visible. DC_RenderNativeScene drops that batch
// (dcResolveTexture sets *skip for an unmatrixed DC_NATIVE_FBTEX_ID: it is a
// pixel-for-pixel duplicate of the OP framebuffer quad). Apply the same test
// here so the two agree on what "visible" means.
//
// The pause preview draws the same texture THROUGH a render matrix, which is
// why the renderMatrix check and not the id alone decides it.
bool DC_NativeSceneVisible()
{
    if (renderStateCount < 1 || !dcProjReady)
        return false;

    for (int b = 0; b < renderStateCount; ++b) {
        RenderState *s = &renderStateList[b];
        if (!s->indexCount || !s->vertPtr)
            continue;
        if (s->useTexture && s->id == DC_NATIVE_FBTEX_ID && !s->renderMatrix)
            continue; // the self-duplicate DC_RenderNativeScene throws away
        return true;
    }
    return false;
}

// Transform one model-space vertex: apply the render matrix (modelview), then a
// pinhole perspective divide, then map to the display. Returns false if the
// vertex is at/behind the eye (z <= 0).
static inline bool dcXformVertex(const MatrixF *mv, float x, float y, float z, float *outX, float *outY, float *outIW)
{
    float ex, ey, ez;
    if (mv) {
        ex = mv->values[0][0] * x + mv->values[1][0] * y + mv->values[2][0] * z + mv->values[3][0];
        ey = mv->values[0][1] * x + mv->values[1][1] * y + mv->values[2][1] * z + mv->values[3][1];
        ez = mv->values[0][2] * x + mv->values[1][2] * y + mv->values[2][2] * z + mv->values[3][2];
    }
    else {
        ex = x;
        ey = y;
        ez = z;
    }

    if (ez < DC_NATIVE_NEARZ)
        return false;

    float fx   = DC_NATIVE_ZDESIGN / SCREEN_CENTERX_F;
    float fy   = DC_NATIVE_ZDESIGN / SCREEN_CENTERY_F;
    float invZ = 1.0f / ez;
    float ndcX = ex * invZ * fx;
    float ndcY = ey * invZ * fy;

    *outX  = (ndcX * 0.5f + 0.5f) * DC_OUT_W;
    *outY  = (0.5f - ndcY * 0.5f) * DC_OUT_H;
    *outIW = invZ;
    return true;
}

// ---- proper near-plane clipping ----
// The 3D menu elements (title, panels, icon carousel) swing in from BEHIND the
// camera. Culling a triangle whole when any vertex crosses the near plane makes
// them flicker (the tri blinks out for the frames a vertex is too close), and a
// vertex sitting just in front of the plane projects to an extreme coordinate,
// producing the stray stretched shards seen poking out of titles. Instead we
// clip each triangle against ez = NEARZ (Sutherland-Hodgman), so partial
// triangles are trimmed cleanly to the plane rather than dropped or stretched.
struct DCClipV {
    float ex, ey, ez; // eye space (pre-divide)
    float u, v;       // final texcoords (already tex-scaled)
    uint  argb;
};

static inline void dcEyeVertex(const MatrixF *mv, float x, float y, float z, float *ex, float *ey, float *ez)
{
    if (mv) {
        *ex = mv->values[0][0] * x + mv->values[1][0] * y + mv->values[2][0] * z + mv->values[3][0];
        *ey = mv->values[0][1] * x + mv->values[1][1] * y + mv->values[2][1] * z + mv->values[3][1];
        *ez = mv->values[0][2] * x + mv->values[1][2] * y + mv->values[2][2] * z + mv->values[3][2];
    }
    else {
        *ex = x;
        *ey = y;
        *ez = z;
    }
}

static inline uint dcLerpARGB(uint a, uint b, float t)
{
    int a0 = (a >> 24) & 0xFF, r0 = (a >> 16) & 0xFF, g0 = (a >> 8) & 0xFF, b0 = a & 0xFF;
    int a1 = (b >> 24) & 0xFF, r1 = (b >> 16) & 0xFF, g1 = (b >> 8) & 0xFF, b1 = b & 0xFF;
    int A = a0 + (int)((a1 - a0) * t + 0.5f);
    int R = r0 + (int)((r1 - r0) * t + 0.5f);
    int G = g0 + (int)((g1 - g0) * t + 0.5f);
    int B = b0 + (int)((b1 - b0) * t + 0.5f);
    return ((uint)A << 24) | ((uint)R << 16) | ((uint)G << 8) | (uint)B;
}

// Clip triangle `in[3]` to the half-space ez >= NEARZ. Writes 0, 3 or 4 output
// vertices to out[] and returns that count (a triangle cut by one plane yields
// at most a quad).
static int dcClipNear(const DCClipV in[3], DCClipV out[4])
{
    const float NZ = DC_NATIVE_NEARZ;
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        const DCClipV *A = &in[i];
        const DCClipV *B = &in[(i + 1) % 3];
        bool ain = A->ez >= NZ;
        bool bin = B->ez >= NZ;
        if (ain && n < 4)
            out[n++] = *A;
        if (ain != bin && n < 4) {
            float   t = (NZ - A->ez) / (B->ez - A->ez);
            DCClipV c;
            c.ex   = A->ex + (B->ex - A->ex) * t;
            c.ey   = A->ey + (B->ey - A->ey) * t;
            c.ez   = NZ;
            c.u    = A->u + (B->u - A->u) * t;
            c.v    = A->v + (B->v - A->v) * t;
            c.argb = dcLerpARGB(A->argb, B->argb, t);
            out[n++] = c;
        }
    }
    return n;
}

static inline void dcProjectEye(const DCClipV *e, float *sx, float *sy, float *iw)
{
    float fx   = DC_NATIVE_ZDESIGN / SCREEN_CENTERX_F;
    float fy   = DC_NATIVE_ZDESIGN / SCREEN_CENTERY_F;
    float invZ = 1.0f / e->ez;
    *sx = ((e->ex * invZ * fx) * 0.5f + 0.5f) * DC_OUT_W;
    *sy = (0.5f - (e->ey * invZ * fy) * 0.5f) * DC_OUT_H;
    *iw = invZ;
}

static void dcEnsureIndexList()
{
    // The engine only calls SetupDrawIndexList() inside its GL init path, which is
    // compiled out on the DC — so drawIndexList[] would otherwise be all zeros and
    // every 2D triangle would collapse to vertex 0. Initialise it once.
    static bool dcIndexInit = false;
    if (!dcIndexInit) {
        SetupDrawIndexList();
        dcIndexInit = true;
    }
}

// Per-batch depth scale: PERSP_SCALE nudged by submission order, so coplanar
// batches resolve painter's-style while true depth still dominates. A pure
// multiplier keeps vertex z proportional to 1/w, which is what the PVR needs for
// perspective-correct texturing.
static inline float dcBatchZScale(int batchIndex)
{
    return DC_NATIVE_PERSP_SCALE * (1.0f + (float)batchIndex * DC_NATIVE_BATCH_BIAS);
}

// Emit one batch's triangles: eye-space transform → near-plane clip → project →
// submit, with z = iw * zScale (see dcBatchZScale / the depth-model note above).
static void dcEmitBatch(RenderState *s, DCNativeTex *tex, float zScale)
{
    const ushort *idx      = s->indexPtr ? s->indexPtr : drawIndexList;
    int           nIndices = s->indexCount;
    for (int i = 0; i + 3 <= nIndices; i += 3) {
        DCClipV tri[3];
        for (int k = 0; k < 3; ++k) {
            DrawVertex *dv = &s->vertPtr[idx[i + k]];
            dcEyeVertex(s->renderMatrix, dv->vertX, dv->vertY, dv->vertZ, &tri[k].ex, &tri[k].ey, &tri[k].ez);
            tri[k].u    = tex ? dv->texCoordX * tex->uS : 0.0f;
            tri[k].v    = tex ? dv->texCoordY * tex->vS : 0.0f;
            tri[k].argb = ((uint)dv->a << 24) | ((uint)dv->r << 16) | ((uint)dv->g << 8) | (uint)dv->b;
        }

        DCClipV poly[4];
        int     n = dcClipNear(tri, poly);
        if (n < 3)
            continue;

        float sx[4], sy[4], iw[4];
        for (int k = 0; k < n; ++k)
            dcProjectEye(&poly[k], &sx[k], &sy[k], &iw[k]);

        for (int t = 2; t < n; ++t) {
            const int fan[3] = { 0, t - 1, t };
            for (int e = 0; e < 3; ++e) {
                int          k = fan[e];
                pvr_vertex_t vert;
                vert.flags = (e == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                vert.x     = sx[k];
                vert.y     = sy[k];
                vert.z     = iw[k] * zScale;
                vert.u     = poly[k].u;
                vert.v     = poly[k].v;
                vert.argb  = poly[k].argb;
                vert.oargb = 0;
                pvr_prim(&vert, sizeof(vert));
            }
        }
    }
}

// True if any batch this frame is a 3D mesh (so the flip opens the PT list).
bool DC_NativeHasMeshes()
{
    if (renderStateCount < 1 || !dcProjReady)
        return false;
    for (int b = 0; b < renderStateCount; ++b) {
        RenderState *s = &renderStateList[b];
        if (s->indexCount && s->vertPtr && s->indexPtr && s->indexPtr != drawIndexList)
            return true;
    }
    return false;
}

// Retired. The native scene is a SINGLE pass now (see DC_RenderNativeScene and
// the depth-model note at the top of this file). Kept as a no-op so the header
// and any stale call site still link; DC_FlipScreen no longer calls it.
void DC_RenderNativeOpaque() {}

// Resolve a batch's texture. Returns NULL for an untextured batch, for one whose
// texture is not (or not yet) resident, and for the full-screen retro-buffer
// duplicate we deliberately skip — `*skip` distinguishes that last case, which
// must drop the batch entirely rather than draw it flat.
//
// The retro-buffer slot has two very different callers:
//
//   RetroGameLoop_Main draws it FULL-SCREEN at z=160 with NO render matrix, on
//   every gameplay and dev-menu frame. On the DC that is a pixel-for-pixel
//   duplicate of the OP framebuffer quad DC_FlipScreen already submits (same
//   texture, same extent, and the alpha-64 blend of an image over itself is a
//   no-op), so drawing it would buy nothing and cost a second full 640x480
//   textured pass. Skip it. Same for PauseMenu's transition frame and
//   MultiplayerHandler's error screen, which also draw it unmatrixed at z=160.
//
//   PauseMenu_Main draws it at z=0 THROUGH a rotate+translate matrix: the pause
//   preview window, the little rotating panel showing the frozen game behind the
//   menu. THAT is the one we want, and the presence of a render matrix is what
//   picks it out.
//
// No UV rescale is needed (uS/vS = 1): Drawing.cpp's SetScreenDimensions() is
// platform-independent, so it already baked textureList[0].widthN = 1/512 and
// heightN = 1/256 into retroVertexList[] — exactly the padding DC_FlipScreen
// uses when it copies the 320x240 frame into the top-left of the 512x256 page.
static DCNativeTex *dcResolveTexture(RenderState *s, DCNativeTex *scratch, bool *skip)
{
    *skip = false;
    if (!s->useTexture)
        return NULL;

    if (s->id == DC_NATIVE_FBTEX_ID) {
        if (!s->renderMatrix) {
            *skip = true; // full-screen self-duplicate; the OP quad has it covered
            return NULL;
        }
        int   potW = 0, potH = 0;
        void *fb = DC_GetFramebufferTexture(&potW, &potH);
        if (!fb) {
            *skip = true;
            return NULL;
        }
        scratch->ptr      = (pvr_ptr_t)fb;
        scratch->potW     = potW;
        scratch->potH     = potH;
        scratch->uS       = 1.0f;
        scratch->vS       = 1.0f;
        scratch->pvrFmt   = PVR_TXRFMT_RGB565;
        scratch->isVQ     = false;
        scratch->noReload = false;
        return scratch;
    }

    if (s->id > 0 && s->id < TEXTURE_COUNT && dcNTex[s->id].ptr)
        return &dcNTex[s->id];

    return NULL;
}

// The whole native scene — backdrop, 3D models, panels, text, dashed lines — in
// ONE TR pass, in submission order, mirroring Renderer.cpp's GL path exactly
// (GL_CULL_FACE on; GL_DEPTH_TEST off except where state->depthTest asks).
//
// Submission order is real here only because the TR autosorter is switched OFF
// (pvr_init_params_t.autosort_disabled = 1, see DCGraphics.cpp). With autosort
// on, the hardware re-sorts TR polygons by true depth, which is the OPPOSITE of
// what a painter's-order UI needs: a carousel icon at eye-z 100 would sort above
// the text drawn after it at the z=160 design plane, no matter how the batches
// were biased. The per-batch bias is ~1e-4 and could never outrank a 2:1 depth
// spread; only disabling the sort makes order mean order.
//
// Vertex z is therefore no longer a sort key. It is still true 1/w, because the
// PVR reuses z as the perspective-correction term, and it still carries the
// batch multiplier so depth-tested batches break ties consistently.
void DC_RenderNativeScene()
{
    if (renderStateCount < 1 || !dcProjReady)
        return;
    dcEnsureIndexList();

    for (int b = 0; b < renderStateCount; ++b) {
        RenderState *s = &renderStateList[b];
        if (!s->indexCount || !s->vertPtr)
            continue;

        // (Only RenderMesh gives a batch its own index buffer; every 2D
        // primitive points at the shared quad list. Nothing below needs to tell
        // them apart any more — culling and depth are per-state, not per-kind.)

        DCNativeTex  fbScratch;
        bool         skip = false;
        DCNativeTex *tex  = dcResolveTexture(s, &fbScratch, &skip);
        if (skip)
            continue;

        pvr_poly_cxt_t cxt;
        if (tex) {
            int txrFmt = tex->pvrFmt | (tex->isVQ ? PVR_TXRFMT_VQ_ENABLE : PVR_TXRFMT_NONTWIDDLED);
            pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, txrFmt, tex->potW, tex->potH, tex->ptr, PVR_FILTER_BILINEAR);
            cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        }
        else
            pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);

        // Cull back faces for EVERYTHING, 2D included — Drawing.cpp:250 does
        // glEnable(GL_CULL_FACE) once at init and never turns it off, so on
        // desktop a back-facing quad is discarded whether it came from
        // RenderMesh or from RenderText. Leaving 2D uncalled here was not the
        // conservative choice it looked like; it made the DC draw geometry the
        // reference build has always thrown away.
        //
        // Which is exactly the credits bug. RenderTextClipped (Renderer.cpp:1591
        // and :1617) clips by CLAMPING vertices to the band y in [-76, +76] and
        // shifting texCoordY to compensate — it never rejects a glyph. A glyph
        // wholly ABOVE the band gets its top edge yanked down to +76 while its
        // bottom edge stays up at, say, +180: the quad is now inside out, and its
        // compensated texCoordY has run far past the glyph's cell into the rest
        // of the font atlas. Same story below -76. StaffCredits keeps every line
        // of the roll alive at once (StaffCredits.cpp:143 scrolls textY from
        // -128 up past +120), so there are always several of these, top and
        // bottom, which is precisely the garbled text.
        //
        // Inside out means reversed winding, so GL culls them and desktop never
        // sees them. Now the PVR does too. Fronts come out negative-area in this
        // screen space (y is down here, up in GL's window space), and
        // PVR_CULLING_CW is "cull if positive" — the same value the models
        // already use and render correctly with.
        cxt.gen.culling = PVR_CULLING_CW;

        // RENDER_BLEND_NONE is glDisable(GL_BLEND) on desktop, so it must write
        // the source unmodified — src/dst = ONE/ZERO. Blending a "none" batch
        // instead would make every solid model see-through wherever its texture
        // carries alpha. (This is also why the pause preview shows the frozen
        // game at full brightness rather than at its nominal alpha of 64: the
        // pause menu sets RENDER_BLEND_NONE around it, exactly as on desktop.)
        if (s->blendMode == RENDER_BLEND_NONE) {
            cxt.blend.src = PVR_BLEND_ONE;
            cxt.blend.dst = PVR_BLEND_ZERO;
        }
        else {
            cxt.blend.src = PVR_BLEND_SRCALPHA;
            cxt.blend.dst = (s->blendMode >= RENDER_BLEND_ALPHA2) ? PVR_BLEND_ONE : PVR_BLEND_INVSRCALPHA;
        }

        // Depth: mirror the GL path batch for batch. Renderer.cpp:479-490 toggles
        // GL_DEPTH_TEST from state->depthTest, which RenderMesh sets (true only for
        // the solid models: cartridge, box, intro mesh, carousel icons) and which is
        // forced false for every 2D batch and every panel mesh.
        //
        // GL also stops writing depth when the test is off (the spec ties the write
        // to the test), so the disabled case is ALWAYS + write-disable, not ALWAYS +
        // write-enable — otherwise a panel mesh would stamp depth over the models.
        if (s->depthTest) {
            cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
            cxt.depth.write      = PVR_DEPTHWRITE_ENABLE;
        }
        else {
            cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
            cxt.depth.write      = PVR_DEPTHWRITE_DISABLE;
        }

        pvr_poly_hdr_t hdr;
        pvr_poly_compile(&hdr, &cxt);
        pvr_prim(&hdr, sizeof(hdr));

        dcEmitBatch(s, tex, dcBatchZScale(b));

        // ---- pause preview: the other half of the game image ----------------
        // The batch we just drew is the retro buffer warped into the rotating
        // preview panel. On desktop that buffer IS the whole frame, so the panel
        // is complete at this point. On the DC only the scrolling background
        // layers are software-rendered into Engine.frameBuffer; every tile and
        // sprite is a PVR quad sitting in dcSpriteQueue. So the panel currently
        // shows sky, sea and hills with no level and no Sonic in it.
        //
        // Replay that frozen queue a second time, warped through the same panel.
        // DC_FlipScreen has already restored dcSpriteCount for the paused frame
        // and does not clear it until after this TR block, so the queue is live
        // right now. The four projected corners define a projective map (PVR z is
        // 1/w, so w = 1/z per corner); DC_DrawFrozenSpritesWarped clips each quad
        // to the game screen first, so half-offscreen objects can't spill outside
        // the panel.
        if (tex && s->id == DC_NATIVE_FBTEX_ID && s->renderMatrix && s->indexCount >= 6) {
            float cx[4], cy[4], cz[4];
            bool  ok = true;

            float uMin = s->vertPtr[0].texCoordX, uMax = uMin;
            float vMin = s->vertPtr[0].texCoordY, vMax = vMin;
            for (int k = 1; k < 4; ++k) {
                float u = s->vertPtr[k].texCoordX, v = s->vertPtr[k].texCoordY;
                if (u < uMin) uMin = u;
                if (u > uMax) uMax = u;
                if (v < vMin) vMin = v;
                if (v > vMax) vMax = v;
            }
            if (uMax - uMin < 0.000001f || vMax - vMin < 0.000001f)
                ok = false;

            int filled = 0;
            for (int k = 0; ok && k < 4; ++k) {
                DrawVertex *dv = &s->vertPtr[k];
                float       un = (dv->texCoordX - uMin) / (uMax - uMin);
                float       vn = (dv->texCoordY - vMin) / (vMax - vMin);
                // 0 = TL, 1 = TR, 2 = BL, 3 = BR (v grows downward)
                int slot = (un < 0.5f ? 0 : 1) + (vn < 0.5f ? 0 : 2);

                DCClipV e;
                dcEyeVertex(s->renderMatrix, dv->vertX, dv->vertY, dv->vertZ, &e.ex, &e.ey, &e.ez);
                if (e.ez < DC_NATIVE_NEARZ) {
                    ok = false; // panel edge-on or behind the eye this frame
                    break;
                }
                float sx, sy, iw;
                dcProjectEye(&e, &sx, &sy, &iw);
                cx[slot] = sx;
                cy[slot] = sy;
                cz[slot] = iw * dcBatchZScale(b);
                filled |= 1 << slot;
            }

            // Follow the panel's own facing. The batch above is culled when the
            // panel turns its back (2D is culled now, exactly as GL does), and
            // the replay is separate geometry that would otherwise keep drawing
            // after the panel it belongs to had vanished. Same signed area the
            // hardware computes, on the triangle the quad actually submits
            // (BL, TR, TL — see SetupDrawIndexList): negative is front-facing.
            if (ok && filled == 0xF) {
                float ax = cx[1] - cx[2], ay = cy[1] - cy[2]; // BL -> TR
                float bx = cx[0] - cx[1], by = cy[0] - cy[1]; // TR -> TL
                if ((ax * by - ay * bx) < 0.0f)
                    DC_DrawFrozenSpritesWarped(cx, cy, cz);
            }
        }
    }

#if DC_NATIVE_DIAG
    // Console-only per-batch dump (no on-screen overlay). One block every ~300
    // native frames. Per batch: textured?/texID/in-cache?/blend/#idx/matrix/
    // transformed screen bbox / first-vertex ARGB — to see which batches fail
    // to bind a texture (cache=0) or land off-screen. id=4096 is the reserved
    // retro-buffer slot (the pause preview window).
    {
        static int dcDumpTick = 0;
        if ((dcDumpTick++ % 300) == 0) {
            int nBatch = 0, nTextured = 0, nTexLoaded = 0;
            for (int b = 0; b < renderStateCount; ++b) {
                RenderState *s = &renderStateList[b];
                if (!s->indexCount || !s->vertPtr)
                    continue;
                nBatch++;
                if (s->useTexture) {
                    nTextured++;
                    if (s->id > 0 && s->id < TEXTURE_COUNT && dcNTex[s->id].ptr)
                        nTexLoaded++;
                }
            }
            printf("[DCNative] === scene: batches=%d textured=%d inVRAM=%d vram=%u ===\n", nBatch, nTextured, nTexLoaded, (uint)pvr_mem_available());
            for (int b = 0; b < renderStateCount; ++b) {
                RenderState *s = &renderStateList[b];
                if (!s->indexCount || !s->vertPtr)
                    continue;
                const ushort *idx = s->indexPtr ? s->indexPtr : drawIndexList;
                float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
                for (int i = 0; i + 3 <= s->indexCount; i += 3) {
                    for (int k = 0; k < 3; ++k) {
                        DrawVertex *dv = &s->vertPtr[idx[i + k]];
                        float px, py, pw;
                        if (dcXformVertex(s->renderMatrix, dv->vertX, dv->vertY, dv->vertZ, &px, &py, &pw)) {
                            if (px < minx) minx = px;
                            if (py < miny) miny = py;
                            if (px > maxx) maxx = px;
                            if (py > maxy) maxy = py;
                        }
                    }
                }
                bool cached = (s->useTexture
                               && ((s->id == DC_NATIVE_FBTEX_ID && s->renderMatrix) || (s->id > 0 && s->id < TEXTURE_COUNT && dcNTex[s->id].ptr)));
                printf("[DCNative] b%02d tex=%d id=%d cache=%d blend=%d idx=%d mtx=%d bbox=(%.0f,%.0f)-(%.0f,%.0f)\n", b, (int)s->useTexture, s->id,
                       cached ? 1 : 0, s->blendMode, s->indexCount, s->renderMatrix ? 1 : 0, (double)minx, (double)miny, (double)maxx, (double)maxy);
            }
            fflush(stdout);
        }
    }
#endif
}

#endif // RETRO_USING_KOS
