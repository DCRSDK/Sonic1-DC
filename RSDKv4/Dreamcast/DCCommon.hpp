// ---------------------------------------------------------------------
// RSDKv4 Dreamcast (KallistiOS) backend
// Based on techniques from the RSDKv5 Dreamcast port by SF94 / Michael Fadely
// (RSDKv5-Decompilation, branch sf94/dreamcast-kallistios-pvr)
// ---------------------------------------------------------------------
#ifndef DC_COMMON_HPP
#define DC_COMMON_HPP

#if RETRO_USING_KOS

#include <kos.h>

// Logging that reaches the emulator/serial console. Empirically (from the
// working RSDKv5 DC port): plain printf works — but only with INIT_FS_PTY
// in KOS_INIT_FLAGS (see main.cpp). fflush guards against buffering.
#define DC_LOG(...)                                                                                                                                  \
    do {                                                                                                                                             \
        printf(__VA_ARGS__);                                                                                                                         \
        fflush(stdout);                                                                                                                              \
    } while (0)

// Visual boot breadcrumb: paints a colored 8px bar near the top of the
// screen for each boot phase (serial output is unreliable in emulators —
// the screen is the one channel proven to work). Bars stack downward;
// if the game dies, the frozen screen shows how far boot got.
//   0=blue ctors | 1=green main | 2=yellow settings | 3=cyan datapack
//   4=magenta gameconfig | 5=white video-init | 6=orange audio-init
//   7=red audio-done/stage
void DC_Probe(int phase);

// ---- Video (DCGraphics.cpp) ----
int DC_InitRenderDevice();
void DC_FlipScreen();
void DC_ReleaseRenderDevice();

// ---- Hardware sprite renderer: sheet texture cache (DCGraphics.cpp) ----
// Phase 1 foundation. Each engine sprite sheet (gfxSurface[sheetID], 8-bit
// palette indices in graphicData[]) is mirrored into PVR VRAM as a twiddled
// PAL8BPP texture, padded up to power-of-two dimensions. These are uploaded
// as sheets load and freed as they unload, so Phase 2 can draw sprites as
// hardware textured quads. Uploading is a no-op when DC_USE_PVR is off.
void DC_UploadSheet(int sheetID);      // (re)mirror one sheet into VRAM
void DC_InvalidateSheet(int sheetID);  // free one sheet's VRAM texture
void DC_InvalidateAllSheets();         // free every sheet texture (stage change)
// Upload the engine's palettes into the PVR's hardware palette banks
// (RGB565 -> ARGB1555, index 0 transparent). v4 has 8 banks; the PVR has
// only 4 for PAL8BPP, so banks are collapsed bank%4. Call once per frame at
// scene start, before any textured draw (the deferred HW samples palette RAM
// at flush time). Used by Phase 2; defined now.
void DC_UploadPalettes();
// Queue one sprite quad for this frame (flushed as a hardware TR-list batch in
// DC_FlipScreen). destX/destY/w/h are in 320x240 engine space; sprX/sprY are
// the source rect on gfxSurface[sheetID]; flip is a FlipFlags value. The
// palette bank is taken from gfxLineBuffer at the sprite's top row.
void DC_QueueSprite(int sheetID, int destX, int destY, int w, int h, int sprX, int sprY, int flip);
// Blend modes for DC_QueueSpriteInk.
#define DC_INK_OPAQUE (0) // color-keyed, fully opaque
#define DC_INK_ALPHA  (1) // alpha-blended (alpha 0-255)
#define DC_INK_ADD    (2) // additive
// Same as DC_QueueSprite but with a blend mode + alpha, for the engine's
// blended sprite draws (alpha/additive). Renders in the same TR batch so
// blended sprites layer correctly against opaque ones.
void DC_QueueSpriteInk(int sheetID, int destX, int destY, int w, int h, int sprX, int sprY, int flip, int ink, int alpha);
// LOCAL-VALIDATION decls (the device's current DCCommon.hpp already has these;
// reconstructed here only so the reverted sandbox tree syntax-checks — this
// file is NOT committed back).
void DC_QueueSpriteQuad(int sheetID, const float *xs, const float *ys, int sprX, int sprY, int w, int h, int ink, int alpha);
void DC_QueueTileQuad(int tileIndex, int direction, int destX, int destY, int srcY, int h);
void DC_MarkTilesetDirty();
void DC_UploadTilesetNow();
void DC_RemapTile(int dest, int src);
void DC_QueueRect(int x, int y, int w, int h, int r, int g, int b, int a);
void DC_ClearSpriteQueue();
void DC_VmuShowIcon();
// Per-frame VMU poll (DCSystem.cpp): puts the emblem up on the first frame of
// the boot logos instead of waiting for the first stage load, and re-asserts it
// when a VMU is plugged in later. Cheap — it only enumerates twice a second.
void DC_VmuTick();
// Set this frame's full-screen fade overlay (drawn over tiles AND sprites in
// DC_FlipScreen). The engine's software fade only covers the framebuffer, so
// on DC we route the full-screen fade here instead. Cleared each flip.
void DC_SetFadeOverlay(int r, int g, int b, int a);

// ---- Audio (DCAudio.cpp) ----
bool DC_InitAudioDevice();
void DC_ReleaseAudioDevice();
// Raw-PCM music streaming (22050 Hz stereo s8 from /cd/Data/Music/*.ogg).
// loopPoint and startFrame are both PCM frame indices in the ORIGINAL 44100 Hz
// ogg the loop tables were authored against (the same units ov_pcm_seek takes
// on desktop), NOT byte offsets into the raw file — DC_MusicOpen rescales.
// startFrame is 0 for a normal PlayMusic; SwapMusicTrack passes the scaled
// position of the outgoing track so the new one resumes in musical sync.
bool DC_MusicOpen(const char *path, bool loop, uint loopPoint, uint startFrame);
// Position already heard, in original-ogg PCM frames (the DC ov_pcm_tell).
uint DC_MusicTell();
void DC_MusicClose();
// Pulls decoded+format-converted music samples for the software mixer.
// Returns number of Sint16 samples written to `out` (interleaved stereo,
// device rate). Handles loop points and end-of-track state changes.
int DC_MusicReadSamples(Sint16 *out, size_t samplesWanted);
// Minimal WAV (PCM8/PCM16, mono/stereo) loader. Returns malloc'd S16
// samples in the file's NATIVE rate/channels (the mixer converts on the
// fly) — upconverting to device format would multiply memory cost.
Sint16 *DC_LoadWAVNative(const byte *data, size_t dataLen, size_t *outSamples, int *outRate, int *outChannels);

// Streaming version of the above, and the one LoadSfx uses. Reads through the
// engine's file reader (the file must already be open via LoadFile) so it never
// holds the whole file in RAM, and decimates straight into the final buffer
// instead of building a full-rate copy first. Peak heap is the returned buffer
// plus a 4KB static staging block, where DC_LoadWAVNative peaks at roughly five
// times what it ends up keeping. Leaves the file open; the caller closes it.
// `stageSfx` routes the buffer through the stage-SFX bump arena instead of
// malloc — see the arena block in DCAudio.cpp. Pass true for the sfx a stage
// loads from its StageConfig, false for the global set loaded at boot.
Sint16 *DC_LoadWAVStreamed(size_t fileSize, size_t *outSamples, int *outRate, int *outChannels, bool stageSfx);

// ---- loose raw-PCM sfx (DCAudio.cpp) ----
// Global sfx can ship as headerless raw PCM under BASE_PATH, keeping their
// .ogg filenames so GameConfig.bin still resolves them — the same trick the
// music and the .dtex textures use, and the step that lets libtremor go.
//
// `fullPath` is the engine-relative path ("Data/SoundFX/Global/Jump.ogg");
// BASE_PATH is prepended here. Opens the file DIRECTLY rather than through
// LoadFile, which MD5-resolves Data.rsdk first and whose loose-file fallback
// therefore never runs — a loose file next to Data.rsdk is invisible to it.
//
// Returns NULL when the file is not there (use the packed asset), when it is
// still real Vorbis (starts "OggS"), or on a read/alloc failure. Never
// half-loads: on any failure nothing is allocated and nothing is stored.
Sint16 *DC_LoadRawSfx(const char *fullPath, size_t *outSamples, int *outRate, int *outChannels);

// ---- stage-SFX arena (DCAudio.cpp) ----
// Stage sfx are reloaded wholesale on every stage change, which fragments the
// heap badly enough that Hidden Palace's 1.4MB across 23 allocations fails at
// the tail. A bump arena cannot fragment internally: layout after a reset is
// byte-identical to the first load.
// The arena is taken as up to six blocks, largest hole first, rather than one
// 1.5MB slab — a single slab is all-or-nothing and gets refused outright on a
// heap that has the space but not in one piece.
// DC_SfxAlloc falls back to malloc when stageSfx is false or the arena is
// full/unavailable, so DC_SfxFree — which must be used for EVERY sfx buffer —
// range-checks the pointer and does the right thing either way.
// DC_SfxArenaAcquire claims the blocks; call it at the top of a stage load, once
// the previous stage has been released and before anything new is read. It must
// NOT be called at audio init — that is before the native menu loads its models,
// and starving those is what broke the 2013 menu.
// DC_SfxArenaReset (via ReleaseStageSfx) hands the blocks back to the heap, so
// the menu's ~620KB of models can be re-read when the player quits to the title.
void  DC_SfxArenaAcquire();
void *DC_SfxAlloc(size_t bytes, bool stageSfx);
void  DC_SfxFree(void *ptr);
void  DC_SfxArenaReset();

// ---- stage SFX in AICA sound RAM (DCAudio.cpp) ----
// The arena above manages main heap, and at Hidden Palace there simply isn't
// enough of it: 23 sfx wanting 1,469,818 bytes against ~831KB free once the
// 2013 menu's models are resident. The AICA has its own 2MB that the main heap
// never touches, so stage sfx are uploaded there and played on hardware
// channels instead — costing the heap nothing and the SH4 no mixing time.
// Global sfx are untouched and stay on the software mixer.
//
// DC_AicaSfxLoad reads through the engine's already-open file (as
// DC_LoadWAVStreamed does), decimates to fit the AICA's 65534-sample channel
// limit and uploads. It returns false for anything it can't take — an
// unsupported format, a refused staging malloc, sound RAM full — and LoadSfx
// then falls back to the heap path, so no zone can end up worse off than it is
// without this.
//
// Sfx that live here have sfxList[].buffer == NULL, which is exactly what the
// software mixer already skips on, so playback routes cleanly: PlaySfx,
// SetSfxAttributes, StopSfx and StopAllSfx check DC_AicaSfxHas first and hand
// off, and ReleaseStageSfx calls DC_AicaSfxUnloadStage to give the sound RAM
// back at the same moment it drops the arena.
bool DC_AicaSfxLoad(size_t fileSize, int sfxID);
bool DC_AicaSfxHas(int sfxID);
bool DC_AicaSfxPlay(int sfxID, bool loop);
bool DC_AicaSfxSetAttr(int sfxID, int loopCount, sbyte pan);
void DC_AicaSfxStop(int sfxID);
void DC_AicaSfxStopAll();
void DC_AicaSfxUnloadStage();

// Last resort for an allocation that MUST succeed (LoadMesh): stops all sfx,
// drops the stage's sfx set and gives the arena's blocks back. Returns false if
// there was no arena to give back — nothing was gained, don't bother retrying.
// Stage sfx stay silent until the next stage load.
bool DC_SfxArenaEmergencyRelease();

// Total free heap / hole count / largest hole, to the log. Built from nothing
// but malloc and free (see DCAudio.cpp) — mallinfo() is not dependable here.
void DC_HeapReport(const char *tag);

// Largest block malloc will still hand out, found by binary search (~10 probes).
// Diagnostic only — for logging how close the heap is to the edge.
size_t DC_LargestFreeBlock();

// Halves the sample rate of S16 PCM (averaging pairs) — sfx are stored
// at 22kHz on DC to halve their heap cost; the mixer resamples anyway.
// Returns malloc'd buffer (NULL on OOM), sets *outSamples.
Sint16 *DC_HalfRateCopy(const Sint16 *src, size_t samples, int channels, size_t *outSamples);

// Reserves `bytes` of VRAM for the music buffers and returns a byte offset
// from the VRAM 64-bit base (0xA5000000). Implemented in DCGraphics.cpp so
// it can go through the PVR allocator when the PVR presenter is active.
uint DC_MusicVramAlloc(uint bytes);

// VRAM-backed storage helpers (music buffers live in PVR VRAM).
// Offsets are byte offsets into VRAM; accesses are performed with
// 32-bit reads/writes only, as VRAM requires.
void DC_VramRead(void *dst, uint vramOffset, size_t n);
void DC_VramWrite(uint vramOffset, const void *src, size_t n);

// ---- Input (DCInput.cpp) ----
void DC_ProcessInput();

// ---- Stage profiling (accumulators live in DCGraphics.cpp) ----
// DC_PERF_LOG instruments the renderer, but on Sonic 1's special stage the
// flush is only ~17% of the frame — on hardware, 3.8ms of 22.4ms. The other
// 83% is the script interpreter and it had no instrument at all, so every
// theory about it was guesswork. These accumulate nanoseconds per frame;
// DC_FlipScreen reports them alongside frame/flip/wait and clears them.
extern unsigned long long dcProfObjectsNs; // ProcessObjects  — ObjectUpdate scripts
extern unsigned long long dcProfDrawNs;    // DrawStageGFX    — tile passes + ObjectDraw scripts
extern unsigned long long dcProfObjDrawNs; // DrawObjectList  — ObjectDraw scripts alone; tiles = draw - objdraw
#define DC_PROF_T0(v)        const unsigned long long v = DC_GetTicks()
#define DC_PROF_ADD(acc, v)  ((acc) += DC_GetTicks() - (v))

// ---- Special-stage draw cull (implemented in DrawObjectList, Drawing.cpp) ----
// Margin in pixels added to the screen rectangle before its circumscribed
// radius is taken, so a sprite whose draw origin sits just outside the viewport
// is still drawn. The widest act-placed special-stage sprite is the Bumper's
// 32x32 frame, whose farthest corner is 23px from the origin; 32 leaves real
// headroom and costs only ~10% more disc area than a tight bound would.
#define DC_SS_CULL_PAD (32)
// Margin for the second, exact test. Covers the same 23px sprite reach plus the
// drift from using the PREVIOUS frame's stage rotation: the rotation moves at
// most one 1/512 turn per frame, which is 2*255*sin(pi/512) = 3.1px of arc at
// the cull radius. 23 + 3.1 leaves 14px of slack at 40.
#define DC_SS_ROT_PAD (40)
extern uint dcSSCullTested;  // PRIORITY_BOUNDS ObjectDraws considered this frame
extern uint dcSSCullSkipped; // ...of which provably off-screen, so skipped

// ---- Timing / system (DCSystem.cpp) ----
unsigned long long DC_GetTicks();          // monotonic, in DC_GetTicksPerSecond() units
unsigned long long DC_GetTicksPerSecond();

#endif // RETRO_USING_KOS
#endif // DC_COMMON_HPP
