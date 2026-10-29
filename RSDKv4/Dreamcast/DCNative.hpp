// ---------------------------------------------------------------------
// RSDKv4 Dreamcast (KallistiOS) — native "Retro Engine" renderer backend
// See DCNative.cpp for the design overview.
// ---------------------------------------------------------------------
#ifndef DC_NATIVE_HPP
#define DC_NATIVE_HPP

#if RETRO_USING_KOS

// Called from SetPerspectiveMatrix() (Renderer.cpp) — flags that the display
// has been sized so the native scene may render. The matrix arg is ignored
// (DCNative uses its own pinhole projection).
void DC_NativeSetProjection(const float *m16);

// Upload a menu texture into PVR VRAM, keyed by its textureList[] index.
void DC_NativeUploadTexture(int texID, const void *rgba, int w, int h, int format);

// Try to load a pre-converted native texture for `pngPath` straight into VRAM
// (no stbi decode; streamed from a loose file on /cd). Returns false if absent.
bool DC_NativeTryLoadDtex(int texID, const char *pngPath, int *outW, int *outH);

// Free every cached native texture.
void DC_NativeFreeTextures();

// Evict native menu textures from VRAM when a game stage loads. Their
// textureList[] entries are KEPT, so texture-slot indices already held by live
// (or later restored) menu entities stay valid; the pixels come back on demand
// via DC_NativeReloadEvicted. Spares font atlases (the pause menu needs them)
// and leaves slot 0 (the reserved retro-buffer slot) alone.
void DC_NativeResetTextures();

// Re-stream any texture this frame's native batches reference but that is no
// longer resident (evicted by DC_NativeResetTextures at stage load). MUST be
// called from DC_FlipScreen BEFORE pvr_scene_begin() — it calls pvr_mem_malloc,
// which is not safe inside an open scene.
void DC_NativeReloadEvicted();

// ---- retro-buffer slot ------------------------------------------------------
// textureList[0] is the engine's reserved "retro buffer" (the game framebuffer
// presented as a native quad). On desktop its .id is a GL texture name, which is
// never 0; we mirror that with a sentinel outside the 0..TEXTURE_COUNT range so
// that (a) RenderRetroBuffer still opens its OWN batch instead of merging into a
// preceding untextured one — it decides by comparing currentRenderState.id
// against textureList[0].id, and with a 0 id both would match, which is exactly
// what happens in PauseMenu.cpp where a RenderRect precedes it — and (b)
// DC_RenderNativeScene can recognise the batch and bind the live framebuffer
// texture for it.
#define DC_NATIVE_FBTEX_ID (0x1000)

// The live game framebuffer as a PVR texture (RGB565, non-twiddled, the 320x240
// image padded into the top-left of a potW x potH page). Defined in
// DCGraphics.cpp. Returns NULL before video init.
void *DC_GetFramebufferTexture(int *outPotW, int *outPotH);

// Replay the frozen gameplay sprite/tile quads (dcSpriteQueue) warped into the
// pause preview panel, on the currently-open TR list. Needed because the DC
// render model is a hybrid: only the scrolling background layers reach
// Engine.frameBuffer, so the retro-buffer texture alone is an incomplete image.
// Corners are in display space, ordered TL, TR, BL, BR, each with its PVR z
// (which is 1/w, so the four of them define the projective map). Defined in
// DCGraphics.cpp.
void DC_DrawFrozenSpritesWarped(const float *cornerX, const float *cornerY, const float *cornerZ);

// ---- single-pass native flush (see DCNative.cpp / DCGraphics DC_FlipScreen) --

// True when the native path produced ANY geometry this frame (open the TR list).
//
// NOTE: this is true on EVERY gameplay frame, not just in menus. The main loop
// calls ProcessNativeObjects() unconditionally (RetroEngine.cpp), and during
// gameplay the RetroGameLoop native entity submits one batch — the full-screen
// retro-buffer quad — so renderStateCount lands on 1 before the flip. Use it
// only to decide whether to open the TR list. To ask "is a native menu ON
// SCREEN?", use DC_NativeSceneVisible() below.
bool DC_NativeSceneActive();

// True when the native scene would actually put something on screen this frame:
// same as DC_NativeSceneActive(), minus the full-screen retro-buffer batch that
// DC_RenderNativeScene deliberately skips as a duplicate of the framebuffer quad
// (see dcResolveTexture). False during ordinary gameplay; true while a native
// menu, splash or pause overlay is drawing.
bool DC_NativeSceneVisible();

// True when any native batch this frame is a 3D mesh. Informational only now
// that models and 2D share one list; kept for diagnostics.
bool DC_NativeHasMeshes();

// Retired no-op. The native scene is one pass; do not call.
void DC_RenderNativeOpaque();

// Draw the ENTIRE native scene — models and 2D UI — on the currently-open TR
// list, in submission order (TR autosort is disabled at pvr_init).
void DC_RenderNativeScene();

#endif // RETRO_USING_KOS
#endif // DC_NATIVE_HPP
