#include "RetroEngine.hpp"

#if RETRO_USING_KOS
#include "Dreamcast/DCCommon.hpp"
// Defined in DCGraphics.cpp (declared here rather than DCCommon.hpp to keep the
// change to the two committed files). Arbitrary-corner flat-colour PVR quad for
// scene3D COLORED/FADED faces — the special stage half-pipe.
extern void DC_Queue3DColorQuad(const float *xs, const float *ys, uint argb);
// Sprites always render as hardware quads now, INCLUDING in special stages.
// (Earlier the special stage half-pipe was software-only because Draw3DScene
// rasterised the pipe into the framebuffer — the backmost layer — so hardware
// sprites buried it. Draw3DScene now queues its own PVR quads too (see the
// KOS branches in DrawFace/DrawFadedFace/DrawTexturedFace), so the pipe, its
// 3D-sprite rings, the diamond background and the HUD all interleave by the
// running draw-order z. Nothing needs the software framebuffer path.)
static inline bool DC_UseHWSprites() { return true; }
#endif

ushort blendLookupTable[0x20 * 0x100];
ushort subtractLookupTable[0x20 * 0x100];
ushort tintLookupTable[0x10000];

// Extras used in blending
#define maxVal(a, b) (a >= b ? a : b)
#define minVal(a, b) (a <= b ? a : b)

bool windowCreated = false;

int SCREEN_XSIZE_CONFIG = 424;
int SCREEN_XSIZE        = 424;
int SCREEN_CENTERX      = 424 / 2;

float SCREEN_XSIZE_F   = 424;
float SCREEN_CENTERX_F = 424 / 2;

float SCREEN_YSIZE_F   = SCREEN_YSIZE;
float SCREEN_CENTERY_F = SCREEN_YSIZE / 2;

int touchWidth     = SCREEN_XSIZE;
int touchHeight    = SCREEN_YSIZE;
float touchWidthF  = SCREEN_XSIZE;
float touchHeightF = SCREEN_YSIZE;

DrawListEntry drawListEntries[DRAWLAYER_COUNT];

int gfxDataPosition = 0;
GFXSurface gfxSurface[SURFACE_COUNT];
byte graphicData[GFXDATA_SIZE];

DisplaySettings displaySettings;
bool convertTo32Bit     = false;
bool mixFiltersOnJekyll = false;

#if RETRO_USING_OPENGL
GLint defaultFramebuffer = -1;
GLuint framebufferHiRes  = -1;
GLuint renderbufferHiRes = -1;
#endif

#if !RETRO_USE_ORIGINAL_CODE
// enable integer scaling, which is a modification of enhanced scaling
bool integerScaling = false;
// allows me to disable it to prevent blur on resolutions that match only on 1 axis
bool disableEnhancedScaling = false;
// enable bilinear scaling, which just disables the fancy upscaling that enhanced scaling does.
bool bilinearScaling = false;
#endif

int InitRenderDevice()
{
    char gameTitle[0x40];

    sprintf(gameTitle, "%s%s", Engine.gameWindowText, Engine.usingDataFile_Config ? "" : " (Using Data Folder)");

#if RETRO_USING_KOS
    (void)gameTitle;
    return DC_InitRenderDevice();
#endif

#if !RETRO_USE_ORIGINAL_CODE
#if RETRO_USING_SDL2

#if RETRO_PLATFORM == RETRO_ANDROID
    setenv("SDL_AUDIODRIVER", "openslES", 1);   // This is a workaround to eliminate audio delay, since we use SDL 2.28 (as of this commit this is coming from.)
                                                // This could be resolved by properly updating SDL to 2.32.10, but that'd involve updating a lot of app related files.
#endif
    SDL_Init(SDL_INIT_EVERYTHING);

    SDL_DisableScreenSaver();

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, Engine.vsync ? "1" : "0");

    byte flags = 0;
#if RETRO_USING_OPENGL
    flags |= SDL_WINDOW_OPENGL;

#if RETRO_PLATFORM != RETRO_OSX // dude idk either you just gotta trust that this works
#if RETRO_PLATFORM != RETRO_ANDROID
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
#endif
#endif
#if RETRO_DEVICETYPE == RETRO_MOBILE
    Engine.startFullScreen = true;

    SDL_DisplayMode dm;
    SDL_GetDesktopDisplayMode(0, &dm);

    bool landscape = dm.h < dm.w;
    int h          = landscape ? dm.w : dm.h;
    int w          = landscape ? dm.h : dm.w;

    SCREEN_XSIZE = ((float)SCREEN_YSIZE * h / w);
    if (SCREEN_XSIZE % 2)
        ++SCREEN_XSIZE;
#endif

    SCREEN_CENTERX = SCREEN_XSIZE / 2;
    Engine.window  = SDL_CreateWindow(gameTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_XSIZE * Engine.windowScale,
                                     SCREEN_YSIZE * Engine.windowScale, SDL_WINDOW_ALLOW_HIGHDPI | flags);

    if (!Engine.window) {
        PrintLog("ERROR: failed to create window!");
        return 0;
    }

#if !RETRO_USING_OPENGL
    Engine.renderer = SDL_CreateRenderer(Engine.window, -1, SDL_RENDERER_ACCELERATED);

    if (!Engine.renderer) {
        PrintLog("ERROR: failed to create renderer!");
        return 0;
    }

    SDL_RenderSetLogicalSize(Engine.renderer, SCREEN_XSIZE, SCREEN_YSIZE);
    SDL_SetRenderDrawBlendMode(Engine.renderer, SDL_BLENDMODE_BLEND);

#if RETRO_SOFTWARE_RENDER
    Engine.screenBuffer = SDL_CreateTexture(Engine.renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, SCREEN_XSIZE, SCREEN_YSIZE);

    if (!Engine.screenBuffer) {
        PrintLog("ERROR: failed to create screen buffer!\nerror msg: %s", SDL_GetError());
        return 0;
    }

    Engine.screenBuffer2x =
        SDL_CreateTexture(Engine.renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, SCREEN_XSIZE * 2, SCREEN_YSIZE * 2);

    if (!Engine.screenBuffer2x) {
        PrintLog("ERROR: failed to create screen buffer HQ!\nerror msg: %s", SDL_GetError());
        return 0;
    }
#endif
#endif

    if (Engine.borderless) {
        SDL_RestoreWindow(Engine.window);
        SDL_SetWindowBordered(Engine.window, SDL_FALSE);
    }

    SDL_DisplayMode disp;
    if (SDL_GetDisplayMode(0, 0, &disp) == 0) {
        Engine.screenRefreshRate = disp.refresh_rate;
    }

#endif

#if RETRO_USING_SDL1
    SDL_Init(SDL_INIT_EVERYTHING);

    byte flags = 0;
#if RETRO_USING_OPENGL
    flags |= SDL_OPENGL;
#endif

    Engine.windowSurface = SDL_SetVideoMode(SCREEN_XSIZE * Engine.windowScale, SCREEN_YSIZE * Engine.windowScale, 32, SDL_SWSURFACE | flags);
    if (!Engine.windowSurface) {
        PrintLog("ERROR: failed to create window!\nerror msg: %s", SDL_GetError());
        return 0;
    }
    // Set the window caption
    SDL_WM_SetCaption(gameTitle, NULL);

    Engine.screenBuffer =
        SDL_CreateRGBSurface(0, SCREEN_XSIZE * Engine.windowScale, SCREEN_YSIZE * Engine.windowScale, 16, 0xF800, 0x7E0, 0x1F, 0x00);

    if (!Engine.screenBuffer) {
        PrintLog("ERROR: failed to create screen buffer!\nerror msg: %s", SDL_GetError());
        return 0;
    }

    /*Engine.screenBuffer2x = SDL_SetVideoMode(SCREEN_XSIZE * 2, SCREEN_YSIZE * 2, 16, SDL_SWSURFACE | flags);
    if (!Engine.screenBuffer2x) {
        PrintLog("ERROR: failed to create screen buffer HQ!\nerror msg: %s", SDL_GetError());
        return 0;
    }*/

    if (Engine.startFullScreen) {
        Engine.windowSurface =
            SDL_SetVideoMode(SCREEN_XSIZE * Engine.windowScale, SCREEN_YSIZE * Engine.windowScale, 16, SDL_SWSURFACE | SDL_FULLSCREEN | flags);
        SDL_ShowCursor(SDL_FALSE);
        Engine.isFullScreen = true;
    }

    // TODO: not supported in 1.2?
    if (Engine.borderless) {
        // SDL_RestoreWindow(Engine.window);
        // SDL_SetWindowBordered(Engine.window, SDL_FALSE);
    }

    // SDL_SetWindowPosition(Engine.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    Engine.useHQModes = false; // disabled
    Engine.borderless = false; // disabled
#endif

#if RETRO_USING_OPENGL

    // Init GL
    Engine.glContext = SDL_GL_CreateContext(Engine.window);

    SDL_GL_SetSwapInterval(Engine.vsync ? 1 : 0);

#if RETRO_PLATFORM != RETRO_ANDROID && RETRO_PLATFORM != RETRO_OSX
    GLenum err = glewInit();
    if (err != GLEW_OK && err != GLEW_ERROR_NO_GLX_DISPLAY) {
        PrintLog("glew init error:");
        PrintLog((const char *)glewGetErrorString(err));
        return false;
    }
#endif

    displaySettings.unknown2 = 0;

    glClearColor(0.0, 0.0, 0.0, 1.0);

    glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_DITHER);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

#if RETRO_PLATFORM == RETRO_ANDROID
    Engine.windowScale     = 1;
    displaySettings.width  = SCREEN_XSIZE;
    displaySettings.height = SCREEN_YSIZE;
#else
    displaySettings.width  = SCREEN_XSIZE_CONFIG * Engine.windowScale;
    displaySettings.height = SCREEN_YSIZE * Engine.windowScale;
#endif

    textureList[0].id = -1;
    SetupViewport();

    ResetRenderStates();
    SetupDrawIndexList();

    for (int c = 0; c < 0x10000; ++c) {
        int r               = (c & 0b1111100000000000) >> 8;
        int g               = (c & 0b0000011111100000) >> 3;
        int b               = (c & 0b0000000000011111) << 3;
        gfxPalette16to32[c] = (0xFF << 24) | (b << 16) | (g << 8) | (r << 0);
    }

    float lightAmbient[4] = { 2.0, 2.0, 2.0, 1.0 };
    float lightDiffuse[4] = { 1.0, 1.0, 1.0, 1.0 };
    float lightPos[4]     = { 0.0, 0.0, 0.0, 1.0 };

    glLightfv(GL_LIGHT0, GL_AMBIENT, lightAmbient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, lightDiffuse);
    glLightfv(GL_LIGHT0, GL_POSITION, lightPos);
    glEnable(GL_LIGHT0);

#if RETRO_PLATFORM == RETRO_ANDROID
    Engine.startFullScreen = true;
#endif
#endif

#if RETRO_PLATFORM != RETRO_ANDROID
    SetScreenDimensions(SCREEN_XSIZE_CONFIG * Engine.windowScale, SCREEN_YSIZE * Engine.windowScale);
#else
    SetScreenDimensions(SCREEN_XSIZE, SCREEN_YSIZE);
#endif

#if RETRO_SOFTWARE_RENDER
    Engine.frameBuffer   = new ushort[GFX_LINESIZE * SCREEN_YSIZE];
    Engine.frameBuffer2x = new ushort[GFX_LINESIZE_DOUBLE * (SCREEN_YSIZE * 2)];
    memset(Engine.frameBuffer, 0, (GFX_LINESIZE * SCREEN_YSIZE) * sizeof(ushort));
    memset(Engine.frameBuffer2x, 0, GFX_LINESIZE_DOUBLE * (SCREEN_YSIZE * 2) * sizeof(ushort));
#endif
    Engine.texBuffer = new uint[GFX_LINESIZE * SCREEN_YSIZE];
    memset(Engine.texBuffer, 0, (GFX_LINESIZE * SCREEN_YSIZE) * sizeof(uint));

#endif

    if (Engine.startFullScreen) {
        SetFullScreen(true);
    }

    OBJECT_BORDER_X2 = SCREEN_XSIZE + 0x80;
    // OBJECT_BORDER_Y2 = SCREEN_YSIZE + 0x100;
    OBJECT_BORDER_X4 = SCREEN_XSIZE + 0x20;
    // OBJECT_BORDER_Y4 = SCREEN_YSIZE + 0x80;

    InitInputDevices();

    return 1;
}
void FlipScreen()
{
#if RETRO_USING_KOS
    DC_FlipScreen();
    return;
#endif
#if !RETRO_USE_ORIGINAL_CODE
    float dimAmount = 1.0;
    if ((!Engine.masterPaused || Engine.frameStep) && !drawStageGFXHQ) {
        if (Engine.dimTimer < Engine.dimLimit) {
            if (Engine.dimPercent < 1.0) {
                Engine.dimPercent += 0.05;
                if (Engine.dimPercent > 1.0)
                    Engine.dimPercent = 1.0;
            }
        }
        else if (Engine.dimPercent > 0.25 && Engine.dimLimit >= 0) {
            Engine.dimPercent *= 0.9;
        }

        dimAmount = Engine.dimMax * Engine.dimPercent;
    }

#if RETRO_SOFTWARE_RENDER && !RETRO_USING_OPENGL
#if RETRO_USING_SDL2
    SDL_Rect destScreenPos_scaled;
    SDL_Texture *texTarget = NULL;

    switch (Engine.scalingMode) {
        // reset to default if value is invalid.
        default: Engine.scalingMode = 0; break;
        case 0: break;                         // nearest
        case 1: integerScaling = true; break;  // integer scaling
        case 2: break;                         // sharp bilinear
        case 3: bilinearScaling = true; break; // regular old bilinear
    }

    SDL_GetWindowSize(Engine.window, &Engine.windowXSize, &Engine.windowYSize);
    float screenxsize = SCREEN_XSIZE;
    float screenysize = SCREEN_YSIZE;

    // check if enhanced scaling is even necessary to be calculated by checking if the screen size is close enough on one axis
    // unfortunately it has to be "close enough" because of floating point precision errors. dang it
    if (Engine.scalingMode == 2) {
        bool cond1 = std::round((Engine.windowXSize / screenxsize) * 24) / 24 == std::floor(Engine.windowXSize / screenxsize);
        bool cond2 = std::round((Engine.windowYSize / screenysize) * 24) / 24 == std::floor(Engine.windowYSize / screenysize);
        if (cond1 || cond2)
            disableEnhancedScaling = true;
    }

    // get 2x resolution if HQ is enabled.
    if (drawStageGFXHQ) {
        screenxsize *= 2;
        screenysize *= 2;
    }

    if (Engine.scalingMode != 0 && !disableEnhancedScaling) {
        // set up integer scaled texture, which is scaled to the largest integer scale of the screen buffer
        // before you make a texture that's larger than the window itself. This texture will then be scaled
        // up to the actual screen size using linear interpolation. This makes even window/screen scales
        // nice and sharp, and uneven scales as sharp as possible without creating wonky pixel scales,
        // creating a nice image.

        // get integer scale
        float scale = 1;
        if (!bilinearScaling) {
            scale =
                std::fminf(std::floor((float)Engine.windowXSize / (float)SCREEN_XSIZE), std::floor((float)Engine.windowYSize / (float)SCREEN_YSIZE));
        }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear"); // set interpolation to linear
        // create texture that's integer scaled.
        texTarget = SDL_CreateTexture(Engine.renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_TARGET, SCREEN_XSIZE * scale, SCREEN_YSIZE * scale);

        // keep aspect
        float aspectScale = std::fminf(Engine.windowYSize / screenysize, Engine.windowXSize / screenxsize);
        if (integerScaling) {
            aspectScale = std::floor(aspectScale);
        }
        float xoffset          = (Engine.windowXSize - (screenxsize * aspectScale)) / 2;
        float yoffset          = (Engine.windowYSize - (screenysize * aspectScale)) / 2;
        destScreenPos_scaled.x = std::round(xoffset);
        destScreenPos_scaled.y = std::round(yoffset);
        destScreenPos_scaled.w = std::round(screenxsize * aspectScale);
        destScreenPos_scaled.h = std::round(screenysize * aspectScale);
        // fill the screen with the texture, making lerp work.
        SDL_RenderSetLogicalSize(Engine.renderer, Engine.windowXSize, Engine.windowYSize);
    }

    int pitch = 0;
    SDL_SetRenderTarget(Engine.renderer, texTarget);

    // Clear the screen. This is needed to keep the
    // pillarboxes in fullscreen from displaying garbage data.
    SDL_RenderClear(Engine.renderer);

    ushort *pixels = NULL;
    if (!drawStageGFXHQ) {
        SDL_LockTexture(Engine.screenBuffer, NULL, (void **)&pixels, &pitch);
        ushort *frameBufferPtr = Engine.frameBuffer;
        for (int y = 0; y < SCREEN_YSIZE; ++y) {
            memcpy(pixels, frameBufferPtr, SCREEN_XSIZE * sizeof(ushort));
            frameBufferPtr += GFX_LINESIZE;
            pixels += pitch / sizeof(ushort);
        }
        // memcpy(pixels, Engine.frameBuffer, pitch * SCREEN_YSIZE); //faster but produces issues with odd numbered screen sizes
        SDL_UnlockTexture(Engine.screenBuffer);

        SDL_RenderCopy(Engine.renderer, Engine.screenBuffer, NULL, NULL);
    }
    else {
        int w = 0, h = 0;
        SDL_QueryTexture(Engine.screenBuffer2x, NULL, NULL, &w, &h);
        SDL_LockTexture(Engine.screenBuffer2x, NULL, (void **)&pixels, &pitch);

        ushort *framebufferPtr = Engine.frameBuffer;
        for (int y = 0; y < (SCREEN_YSIZE / 2) + 12; ++y) {
            for (int x = 0; x < GFX_LINESIZE; ++x) {
                *pixels = *framebufferPtr;
                pixels++;
                *pixels = *framebufferPtr;
                pixels++;
                framebufferPtr++;
            }

            framebufferPtr -= GFX_LINESIZE;
            for (int x = 0; x < GFX_LINESIZE; ++x) {
                *pixels = *framebufferPtr;
                pixels++;
                *pixels = *framebufferPtr;
                pixels++;
                framebufferPtr++;
            }
        }

        framebufferPtr = Engine.frameBuffer2x;
        for (int y = 0; y < ((SCREEN_YSIZE / 2) - 12) * 2; ++y) {
            for (int x = 0; x < GFX_LINESIZE; ++x) {
                *pixels = *framebufferPtr;
                framebufferPtr++;
                pixels++;

                *pixels = *framebufferPtr;
                framebufferPtr++;
                pixels++;
            }
        }
        SDL_UnlockTexture(Engine.screenBuffer2x);
        SDL_RenderCopy(Engine.renderer, Engine.screenBuffer2x, NULL, NULL);
    }

    if (Engine.scalingMode != 0 && !disableEnhancedScaling) {
        // set render target back to the screen.
        SDL_SetRenderTarget(Engine.renderer, NULL);
        // clear the screen itself now, for same reason as above
        SDL_RenderClear(Engine.renderer);
        // copy texture to screen with lerp
        SDL_RenderCopy(Engine.renderer, texTarget, NULL, &destScreenPos_scaled);
        // Apply dimming
        SDL_SetRenderDrawColor(Engine.renderer, 0, 0, 0, 0xFF - (dimAmount * 0xFF));
        if (dimAmount < 1.0)
            SDL_RenderFillRect(Engine.renderer, NULL);
        // finally present it
        SDL_RenderPresent(Engine.renderer);
        // reset everything just in case
        SDL_RenderSetLogicalSize(Engine.renderer, SCREEN_XSIZE, SCREEN_YSIZE);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
        // putting some FLEX TAPE� on that memory leak
        SDL_DestroyTexture(texTarget);
    }
    else {
        // Apply dimming
        SDL_SetRenderDrawColor(Engine.renderer, 0, 0, 0, 0xFF - (dimAmount * 0xFF));
        if (dimAmount < 1.0)
            SDL_RenderFillRect(Engine.renderer, NULL);
        // no change here
        SDL_RenderPresent(Engine.renderer);
    }
    SDL_ShowWindow(Engine.window);
#endif

#if RETRO_USING_SDL1
    ushort *px = (ushort *)Engine.screenBuffer->pixels;
    int w      = SCREEN_XSIZE * Engine.windowScale;
    int h      = SCREEN_YSIZE * Engine.windowScale;

    if (Engine.windowScale == 1) {
        ushort *frameBufferPtr = Engine.frameBuffer;
        for (int y = 0; y < SCREEN_YSIZE; ++y) {
            for (int x = 0; x < SCREEN_XSIZE; ++x) {
                pixels[x] = frameBufferPtr[x];
            }
            frameBufferPtr += GFX_LINESIZE;
            px += Engine.screenBuffer->pitch / sizeof(ushort);
        }
        // memcpy(Engine.screenBuffer->pixels, Engine.frameBuffer, Engine.screenBuffer->pitch * SCREEN_YSIZE);
    }
    else {
        // TODO: this better, I really dont know how to use SDL1.2 well lol
        int dx = 0, dy = 0;
        do {
            do {
                int x = (int)(dx * (1.0f / Engine.windowScale));
                int y = (int)(dy * (1.0f / Engine.windowScale));

                px[dx + (dy * w)] = Engine.frameBuffer[x + (y * GFX_LINESIZE)];

                dx++;
            } while (dx < w);
            dy++;
            dx = 0;
        } while (dy < h);
    }

    // Apply image to screen
    SDL_BlitSurface(Engine.screenBuffer, NULL, Engine.windowSurface, NULL);

    // Update Screen
    SDL_Flip(Engine.windowSurface);
#endif

#endif // !RETRO_SOFTWARE_RENDER

#endif
}
void ReleaseRenderDevice(bool refresh)
{
    if (!refresh) {
        ClearMeshData();
        ClearTextures(false);
    }

#if RETRO_USING_KOS
    DC_ReleaseRenderDevice();
    return;
#endif

#if !RETRO_USE_ORIGINAL_CODE
#if RETRO_SOFTWARE_RENDER
    if (Engine.frameBuffer)
        delete[] Engine.frameBuffer;
    if (Engine.frameBuffer2x)
        delete[] Engine.frameBuffer2x;
#if RETRO_USING_SDL2 && !RETRO_USING_OPENGL
    SDL_DestroyTexture(Engine.screenBuffer);
    Engine.screenBuffer = NULL;
#endif
    if (Engine.texBuffer)
        delete[] Engine.texBuffer;

#if RETRO_USING_SDL1
    SDL_FreeSurface(Engine.screenBuffer);
#endif
#endif

#if RETRO_USING_OPENGL
    if (Engine.glContext)
        SDL_GL_DeleteContext(Engine.glContext);
#endif

#if RETRO_USING_SDL2
#if !RETRO_USING_OPENGL
    SDL_DestroyRenderer(Engine.renderer);
#endif
    SDL_DestroyWindow(Engine.window);
#endif
#endif
}

void GenerateBlendLookupTable(void)
{
    for (int y = 0; y < 0x100; y++) {
        for (int x = 0; x < 0x20; x++) {
            blendLookupTable[x + (0x20 * y)]    = y * x >> 8;
            subtractLookupTable[x + (0x20 * y)] = y * (0x1F - x) >> 8;
        }
    }

    for (int i = 0; i < 0x10000; i++) {
        int tintValue      = ((i & 0x1F) + ((i & 0x7E0) >> 6) + ((i & 0xF800) >> 11)) / 3 + 6;
        tintLookupTable[i] = 0x841 * minVal(tintValue, 0x1F);
    }
}

void ClearScreen(byte index)
{
#if RETRO_USING_KOS
    // A script ClearScreen erases everything drawn so far this frame — and
    // the hardware quad queue is part of "drawn so far". Without this, tile
    // layers queued before the clear survive it (the title screen's BG text
    // peeked out from behind the emblem: STLogo hides the BG by clearing
    // every frame). The framebuffer fill below still runs, so the cleared
    // colour shows through as the backdrop.
    DC_ClearSpriteQueue();
#endif
#if RETRO_SOFTWARE_RENDER
    ushort color        = activePalette[index];
    ushort *framebuffer = Engine.frameBuffer;
    int cnt             = GFX_LINESIZE * SCREEN_YSIZE;
    while (cnt--) {
        *framebuffer = color;
        ++framebuffer;
    }
#endif
}

void SetScreenDimensions(int width, int height)
{
    touchWidth               = width;
    touchHeight              = height;
    displaySettings.width    = width;
    displaySettings.height   = height;
    touchWidthF              = width;
    displaySettings.unknown1 = 16;
    touchHeightF             = height;
    // displaySettings.maxWidth = 424;
    double aspect    = (((width >> 16) * 65536.0) + width) / (((height >> 16) * 65536.0) + height);
    SCREEN_XSIZE_F   = SCREEN_YSIZE * aspect;
    SCREEN_CENTERX_F = aspect * SCREEN_CENTERY;
    SetPerspectiveMatrix(SCREEN_YSIZE * aspect, SCREEN_YSIZE_F, 0.0, 1000.0);
#if RETRO_USING_OPENGL
    glViewport(0, 0, displaySettings.width, displaySettings.height);
#endif

    Engine.useHighResAssets = displaySettings.height > (SCREEN_YSIZE * 2);
    int displayWidth        = aspect * SCREEN_YSIZE;
    // if (val > displaySettings.maxWidth)
    //    val = displaySettings.maxWidth;
#if !RETRO_USE_ORIGINAL_CODE
    SetScreenSize(displayWidth, (displayWidth + 9) & -0x8);
#else
    SetScreenSize(displayWidth, (displayWidth + 9) & -0x10);
#endif

    int width2 = 0;
    int wBuf   = GFX_LINESIZE - 1;
    while (wBuf > 0) {
        width2++;
        wBuf >>= 1;
    }
    int height2 = 0;
    int hBuf    = SCREEN_YSIZE - 1;
    while (hBuf > 0) {
        height2++;
        hBuf >>= 1;
    }
    int texWidth  = 1 << width2;
    int texHeight = 1 << height2;

    textureList[0].widthN  = 1.0f / texWidth;
    textureList[0].heightN = 1.0f / texHeight;

    float w  = (SCREEN_XSIZE * textureList[0].widthN);
    float w2 = (GFX_LINESIZE * textureList[0].widthN);
    float h  = (SCREEN_YSIZE * textureList[0].heightN);

    retroVertexList[0] = -SCREEN_CENTERX_F;
    retroVertexList[1] = SCREEN_CENTERY_F;
    retroVertexList[2] = 160.0;
    retroVertexList[6] = 0.0;
    retroVertexList[7] = 0.0;

    retroVertexList[9]  = SCREEN_CENTERX_F;
    retroVertexList[10] = SCREEN_CENTERY_F;
    retroVertexList[11] = 160.0;
    retroVertexList[15] = w;
    retroVertexList[16] = 0.0;

    retroVertexList[18] = -SCREEN_CENTERX_F;
    retroVertexList[19] = -SCREEN_CENTERY_F;
    retroVertexList[20] = 160.0;
    retroVertexList[24] = 0.0;
    retroVertexList[25] = h;

    retroVertexList[27] = SCREEN_CENTERX_F;
    retroVertexList[28] = -SCREEN_CENTERY_F;
    retroVertexList[29] = 160.0;
    retroVertexList[33] = w;
    retroVertexList[34] = h;

    screenBufferVertexList[0] = -1.0;
    screenBufferVertexList[1] = 1.0;
    screenBufferVertexList[2] = 1.0;
    screenBufferVertexList[6] = 0.0;
    screenBufferVertexList[7] = h;

    screenBufferVertexList[9]  = 1.0;
    screenBufferVertexList[10] = 1.0;
    screenBufferVertexList[11] = 1.0;
    screenBufferVertexList[15] = w2;
    screenBufferVertexList[16] = h;

    screenBufferVertexList[18] = -1.0;
    screenBufferVertexList[19] = -1.0;
    screenBufferVertexList[20] = 1.0;
    screenBufferVertexList[24] = 0.0;
    screenBufferVertexList[25] = 0.0;

    screenBufferVertexList[27] = 1.0;
    screenBufferVertexList[28] = -1.0;
    screenBufferVertexList[29] = 1.0;
    screenBufferVertexList[33] = w2;
    screenBufferVertexList[34] = 0.0;
}

void SetScreenSize(int width, int lineSize)
{
    SCREEN_XSIZE        = width;
    SCREEN_CENTERX      = width / 2;
    SCREEN_SCROLL_LEFT  = SCREEN_CENTERX - 8;
    SCREEN_SCROLL_RIGHT = SCREEN_CENTERX + 8;
    OBJECT_BORDER_X2    = width + 0x80;
    OBJECT_BORDER_X4    = width + 0x20;

    GFX_LINESIZE          = lineSize;
    GFX_LINESIZE_MINUSONE = lineSize - 1;
    GFX_LINESIZE_DOUBLE   = 2 * lineSize;
    GFX_FRAMEBUFFERSIZE   = SCREEN_YSIZE * lineSize;
    GFX_FBUFFERMINUSONE   = SCREEN_YSIZE * lineSize - 1;
}

#if RETRO_SOFTWARE_RENDER
void CopyFrameOverlay2x()
{
    ushort *frameBuffer   = &Engine.frameBuffer[((SCREEN_YSIZE / 2) + 12) * GFX_LINESIZE];
    ushort *frameBuffer2x = Engine.frameBuffer2x;

    for (int y = 0; y < (SCREEN_YSIZE / 2) - 12; ++y) {
        for (int x = 0; x < GFX_LINESIZE; ++x) {
            if (*frameBuffer == 0xF81F) { // magenta
                frameBuffer2x += 2;
            }
            else {
                *frameBuffer2x = *frameBuffer;
                frameBuffer2x++;
                *frameBuffer2x = *frameBuffer;
                frameBuffer2x++;
            }
            ++frameBuffer;
        }

        frameBuffer -= GFX_LINESIZE;
        for (int x = 0; x < GFX_LINESIZE; ++x) {
            if (*frameBuffer == 0xF81F) { // magenta
                frameBuffer2x += 2;
            }
            else {
                *frameBuffer2x = *frameBuffer;
                frameBuffer2x++;
                *frameBuffer2x = *frameBuffer;
                frameBuffer2x++;
            }
            ++frameBuffer;
        }
    }
}
#endif

void SetupViewport()
{
    double aspect    = displaySettings.width / (double)displaySettings.height;
    SCREEN_XSIZE_F   = SCREEN_YSIZE * aspect;
    SCREEN_CENTERX_F = aspect * SCREEN_CENTERY;

#if RETRO_USING_OPENGL
    glScalef(320.0f / (SCREEN_YSIZE * aspect), 1.0, 1.0);
#endif

    SetPerspectiveMatrix(90.0, 0.75, 1.0, 5000.0);

#if RETRO_USING_OPENGL
    glViewport(displaySettings.offsetX, 0, displaySettings.width, displaySettings.height);
#endif
    int displayWidth = aspect * SCREEN_YSIZE;
#if !RETRO_USE_ORIGINAL_CODE
    SetScreenSize(displayWidth, (displayWidth + 9) & -0x8);
#else
    SetScreenSize(displayWidth, (displayWidth + 9) & -0x10);
#endif

    Engine.useHighResAssets = displaySettings.height > (SCREEN_YSIZE * 2);

#if RETRO_USING_OPENGL
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
#endif

    int width2 = 0;
    int wBuf   = GFX_LINESIZE - 1;
    while (wBuf > 0) {
        width2++;
        wBuf >>= 1;
    }
    int height2 = 0;
    int hBuf    = SCREEN_YSIZE - 1;
    while (hBuf > 0) {
        height2++;
        hBuf >>= 1;
    }
    int texWidth  = 1 << width2;
    int texHeight = 1 << height2;

    float w  = (SCREEN_XSIZE * textureList[0].widthN);
    float w2 = (GFX_LINESIZE * textureList[0].widthN);
    float h  = (SCREEN_YSIZE * textureList[0].heightN);

    retroVertexList[0] = -SCREEN_CENTERX_F;
    retroVertexList[1] = SCREEN_CENTERY_F;
    retroVertexList[2] = 160.0;
    retroVertexList[6] = 0.0;
    retroVertexList[7] = 0.0;

    retroVertexList[9]  = SCREEN_CENTERX_F;
    retroVertexList[10] = SCREEN_CENTERY_F;
    retroVertexList[11] = 160.0;
    retroVertexList[15] = w;
    retroVertexList[16] = 0.0;

    retroVertexList[18] = -SCREEN_CENTERX_F;
    retroVertexList[19] = -SCREEN_CENTERY_F;
    retroVertexList[20] = 160.0;
    retroVertexList[24] = 0.0;
    retroVertexList[25] = h;

    retroVertexList[27] = SCREEN_CENTERX_F;
    retroVertexList[28] = -SCREEN_CENTERY_F;
    retroVertexList[29] = 160.0;
    retroVertexList[33] = w;
    retroVertexList[34] = h;

    screenBufferVertexList[0] = -1.0;
    screenBufferVertexList[1] = 1.0;
    screenBufferVertexList[2] = 1.0;
    screenBufferVertexList[6] = 0.0;
    screenBufferVertexList[7] = h;

    screenBufferVertexList[9]  = 1.0;
    screenBufferVertexList[10] = 1.0;
    screenBufferVertexList[11] = 1.0;
    screenBufferVertexList[15] = w2;
    screenBufferVertexList[16] = h;

    screenBufferVertexList[18] = -1.0;
    screenBufferVertexList[19] = -1.0;
    screenBufferVertexList[20] = 1.0;
    screenBufferVertexList[24] = 0.0;
    screenBufferVertexList[25] = 0.0;

    screenBufferVertexList[27] = 1.0;
    screenBufferVertexList[28] = -1.0;
    screenBufferVertexList[29] = 1.0;
    screenBufferVertexList[33] = w2;
    screenBufferVertexList[34] = 0.0;

    StrCopy(textureList[0].fileName, "RetroBuffer");
    textureList[0].width   = texWidth;
    textureList[0].height  = texHeight;
    textureList[0].format  = TEXFMT_RETROBUFFER;
    textureList[0].widthN  = 1.0f / texWidth;
    textureList[0].heightN = 1.0f / texHeight;

    if (Engine.useHighResAssets) {
#if RETRO_USING_OPENGL
        if (framebufferHiRes != -1)
            glDeleteFramebuffers(1, &framebufferHiRes);
        if (renderbufferHiRes != -1)
            glDeleteTextures(1, &renderbufferHiRes);
        framebufferHiRes  = -1;
        renderbufferHiRes = -1;

        glGenFramebuffers(1, &framebufferHiRes);
        glBindFramebuffer(GL_FRAMEBUFFER, framebufferHiRes);
        glGenTextures(1, &renderbufferHiRes);
        glBindTexture(GL_TEXTURE_2D, renderbufferHiRes);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, Engine.scalingMode ? GL_LINEAR : GL_NEAREST);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, Engine.scalingMode ? GL_LINEAR : GL_NEAREST);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, texWidth << 1, texHeight << 1, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, renderbufferHiRes, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
#endif

        float w                    = (((((GFX_LINESIZE >> 16) * 65536.0) + GFX_LINESIZE) + 0.5) * textureList[0].widthN) - 0.001;
        float h                    = (SCREEN_YSIZE * textureList[0].heightN) - 0.001;
        screenBufferVertexList[0]  = -1.0;
        screenBufferVertexList[1]  = 1.0;
        screenBufferVertexList[2]  = 1.0;
        screenBufferVertexList[6]  = 0.0;
        screenBufferVertexList[9]  = 1.0;
        screenBufferVertexList[10] = 1.0;
        screenBufferVertexList[11] = 1.0;
        screenBufferVertexList[7]  = h;
        screenBufferVertexList[16] = h;
        screenBufferVertexList[18] = -1.0;
        screenBufferVertexList[19] = -1.0;
        screenBufferVertexList[20] = 1.0;
        screenBufferVertexList[24] = 0.0;
        screenBufferVertexList[25] = 0.0;
        screenBufferVertexList[27] = 1.0;
        screenBufferVertexList[28] = -1.0;
        screenBufferVertexList[29] = 1.0;
        screenBufferVertexList[15] = w;
        screenBufferVertexList[34] = 0.0;
        screenBufferVertexList[33] = w;
    }
    else {
#if RETRO_USING_OPENGL
        if (framebufferHiRes != -1)
            glDeleteFramebuffers(1, &framebufferHiRes);
        if (renderbufferHiRes != -1)
            glDeleteTextures(1, &renderbufferHiRes);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        framebufferHiRes  = -1;
        renderbufferHiRes = -1;
#endif
    }

    bool transfer = false;
#if RETRO_USING_OPENGL
    if (textureList[0].id != -1) {
        glDeleteTextures(1, &textureList[0].id);
        transfer = true;
    }
    glGenTextures(1, &textureList[0].id);
    glBindTexture(GL_TEXTURE_2D, textureList[0].id);
#endif

    convertTo32Bit = true;
#if RETRO_USING_OPENGL
    if (displaySettings.height > 720) {
        convertTo32Bit = true;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texWidth, texHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    }
    else if (convertTo32Bit)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texWidth, texHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texWidth, texHeight, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 0);

    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, Engine.scalingMode ? GL_LINEAR : GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, Engine.scalingMode ? GL_LINEAR : GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
#endif

    mixFiltersOnJekyll = Engine.useHighResAssets;

    if (transfer && Engine.frameBuffer)
        TransferRetroBuffer();
}

void SetFullScreen(bool fs)
{
    if (fs) {
#if RETRO_USING_SDL1
        Engine.windowSurface =
            SDL_SetVideoMode(SCREEN_XSIZE * Engine.windowScale, SCREEN_YSIZE * Engine.windowScale, 16, SDL_SWSURFACE | SDL_FULLSCREEN);
        SDL_ShowCursor(SDL_FALSE);
#elif RETRO_USING_SDL2
        SDL_RestoreWindow(Engine.window);
        SDL_SetWindowFullscreen(Engine.window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        SDL_ShowCursor(SDL_FALSE);

#if RETRO_USING_OPENGL
        SDL_DisplayMode mode;
        SDL_GetDesktopDisplayMode(0, &mode);

        int w = mode.w;
        int h = mode.h;
        if (mode.h > mode.w) {
            w = mode.h;
            h = mode.w;
        }

#if RETRO_PLATFORM != RETRO_iOS && RETRO_PLATFORM != RETRO_ANDROID
        float aspect            = SCREEN_XSIZE_CONFIG / (float)SCREEN_YSIZE;
        displaySettings.height  = h;
        displaySettings.width   = aspect * displaySettings.height;
        displaySettings.offsetX = abs(w - displaySettings.width) / 2;
        if (displaySettings.width > w) {
            displaySettings.offsetX = 0;
            displaySettings.width   = w;
        }

        SetupViewport();
#else
        displaySettings.height = h;
        displaySettings.width  = w;
        glViewport(0, 0, displaySettings.width, displaySettings.height);
#endif
#endif
#endif
    }
    else {
#if RETRO_USING_SDL1
        Engine.windowSurface = SDL_SetVideoMode(SCREEN_XSIZE * Engine.windowScale, SCREEN_YSIZE * Engine.windowScale, 16, SDL_SWSURFACE);
        SDL_ShowCursor(SDL_TRUE);
#elif RETRO_USING_SDL2
        SDL_SetWindowFullscreen(Engine.window, false);
        SDL_ShowCursor(SDL_TRUE);
        SDL_SetWindowSize(Engine.window, SCREEN_XSIZE_CONFIG * Engine.windowScale, SCREEN_YSIZE * Engine.windowScale);
        SDL_SetWindowPosition(Engine.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_RestoreWindow(Engine.window);

        displaySettings.width   = SCREEN_XSIZE_CONFIG * Engine.windowScale;
        displaySettings.height  = SCREEN_YSIZE * Engine.windowScale;
        displaySettings.offsetX = 0;
        SetupViewport();
#endif
    }
    Engine.isFullScreen = fs;
}

void DrawObjectList(int Layer)
{
#if RETRO_USING_KOS
    // Splits DrawStageGFX's cost: this accumulates the ObjectDraw scripts, so
    // the tile passes are (draw - objdraw). DrawStageGFX interleaves the two
    // across several branches, so timing them separately there would mean a
    // dozen edits; one timer here plus a subtraction gets the same answer.
    DC_PROF_T0(dcT);

    // ---- Special-stage draw cull ------------------------------------------
    // Every act-placed object in the special stage positions itself through
    // SpecialSetup_GetBlockPos, which rotates its position about player[0] by
    // Player_stageRotation before handing it to DrawSprite*. A rotation about
    // the player preserves distance FROM the player, so an object farther away
    // than the screen rectangle's circumscribed radius cannot land on screen at
    // ANY rotation. That first test is conservative by construction: it does
    // not have to know the rotation at all, and it can never hide something
    // that was visible.
    //
    // A second, tighter test then does the real screen-rectangle check, because
    // the disc is about 1.6x the area of the rectangle inscribed in it. It needs
    // the rotation, which lives in a script static (compiled to a scriptCode[]
    // slot with no name lookup at runtime) that this side of the engine cannot
    // address -- but GetBlockPos also writes 512 - Player_stageRotation into
    // object.rotation on every object it positions, so the value is sitting in
    // the entity list by the time the layer finishes. A Boyer-Moore majority
    // vote over the objects that actually drew recovers it (they all agree; the
    // handful that never call GetBlockPos cannot outvote ~90 blocks), and it is
    // used on the following frame. One frame of lag is at most one 1/512 turn,
    // 3.1px of arc at the cull radius, which DC_SS_ROT_PAD covers several times
    // over. Until a vote succeeds -- the first frame of a stage, or a layer with
    // too few objects to be sure -- only the radial test runs.
    //
    // Worth doing because the engine's own PRIORITY_BOUNDS check is an axis-
    // aligned box in UNROTATED space -- +-288px on x and -360/+392 on y around
    // the player -- 433k px2 against the 204k px2 disc that can actually reach
    // the viewport. Replaying the three special stage layouts against every
    // ring position as a stand-in for the player's path, the objects drawn per
    // frame fall from ~181 to ~100: a little under half the ObjectDraw scripts
    // were rotating a block to a position nowhere near the screen and then
    // having DrawSprite* clip the result away.
    //
    // Only PRIORITY_BOUNDS entities are eligible, which is exactly the set the
    // act file places (blocks, rings, bumpers, the emerald -- all priority 0).
    // Anything that must draw wherever the player happens to be -- the player,
    // the HUD, Special Setup's background rect, Fade In, Special Finish -- is
    // PRIORITY_ACTIVE and is never considered here.
    //
    // Skipping ObjectDraw is safe for every one of them: those draw events are
    // pure apart from Blue/Yellow/Pink/Green Block setting object.type and
    // putting it straight back (a net no-op, used to borrow another block's
    // frame table), Gem Block's object.direction (recomputed from a global
    // timer on every draw) and Chaos Emerald's idle flash counter (cosmetic,
    // and reset on pickup). Collision lives in ObjectUpdate and is untouched.
    int ssCullR2 = 0, ssCx = 0, ssCy = 0; // ssCullR2 == 0 means cull off for this call
    if (activeStageList == STAGELIST_SPECIAL) {
        const Entity *ply = &objectEntityList[0];
        const int     cx  = (ply->xpos >> 16) - xScrollOffset; // player's screen x
        const int     cy  = (ply->ypos >> 16) - yScrollOffset; // ...and screen y
        // If the stage is not actually being drawn around the player then the
        // invariant above does not hold -- leave the cull off rather than guess.
        if (ply->type && cx >= 0 && cx <= SCREEN_XSIZE && cy >= 0 && cy <= SCREEN_YSIZE) {
            const int ex = (cx > SCREEN_XSIZE - cx ? cx : SCREEN_XSIZE - cx) + DC_SS_CULL_PAD;
            const int ey = (cy > SCREEN_YSIZE - cy ? cy : SCREEN_YSIZE - cy) + DC_SS_CULL_PAD;
            ssCullR2     = ex * ex + ey * ey;
            ssCx         = cx;
            ssCy         = cy;
        }
    }

    // Player_stageRotation as recovered from the last successful vote, or -1
    // while it is unknown. Player Object's startup zeroes the rotation on every
    // stage load, so a value carried across a stage change would be wrong for
    // one frame -- hence the explicit invalidation rather than trusting it.
    static int dcSSRot = -1, dcSSList = -1, dcSSStage = -1;
    if (activeStageList != dcSSList || stageListPosition != dcSSStage) {
        dcSSList  = activeStageList;
        dcSSStage = stageListPosition;
        dcSSRot   = -1;
    }
    const int ssRot   = dcSSRot;
    const int ssSin   = ssRot >= 0 ? sin512LookupTable[ssRot] : 0; // sin * 512
    const int ssCos   = ssRot >= 0 ? cos512LookupTable[ssRot] : 0; // cos * 512
    int       voteCand = 0, voteCount = 0;
#endif
    int size = drawListEntries[Layer].listSize;
    for (int i = 0; i < size; ++i) {
        objectEntityPos = drawListEntries[Layer].entityRefs[i];
        int type        = objectEntityList[objectEntityPos].type;
        if (type) {
#if RETRO_USING_KOS
            bool ssVote = false;
            if (ssCullR2) {
                const Entity *ent = &objectEntityList[objectEntityPos];
                if (ent->priority == PRIORITY_BOUNDS) {
                    // Bounded by the engine's own activity box, so the squares
                    // stay well inside int range (worst case ~250k).
                    const int dx = (ent->xpos - objectEntityList[0].xpos) >> 16;
                    const int dy = (ent->ypos - objectEntityList[0].ypos) >> 16;
                    ++dcSSCullTested;
                    if (dx * dx + dy * dy > ssCullR2) {
                        ++dcSSCullSkipped;
                        continue;
                    }
                    if (ssRot >= 0) {
                        // Same rotation GetBlockPos applies, in whole pixels:
                        // (sin*dy + cos*dx) / 512, tables being sin/cos * 512.
                        const int sx = (((ssSin * dy) + (ssCos * dx)) >> 9) + ssCx;
                        const int sy = (((ssCos * dy) - (ssSin * dx)) >> 9) + ssCy;
                        if (sx < -DC_SS_ROT_PAD || sx > SCREEN_XSIZE + DC_SS_ROT_PAD || sy < -DC_SS_ROT_PAD
                            || sy > SCREEN_YSIZE + DC_SS_ROT_PAD) {
                            ++dcSSCullSkipped;
                            continue;
                        }
                    }
                    ssVote = true;
                }
            }
#endif
            if (scriptCode[objectScriptList[type].eventDraw.scriptCodePtr] > 0)
                ProcessScript(objectScriptList[type].eventDraw.scriptCodePtr, objectScriptList[type].eventDraw.jumpTablePtr, EVENT_DRAW);
#if RETRO_USING_KOS
            if (ssVote) { // majority vote on the rotation GetBlockPos just wrote
                const int r = objectEntityList[objectEntityPos].rotation;
                if (voteCount == 0) {
                    voteCand  = r;
                    voteCount = 1;
                }
                else if (r == voteCand)
                    ++voteCount;
                else
                    --voteCount;
            }
#endif
        }
    }
#if RETRO_USING_KOS
    // A margin of 8 means the winner outnumbered everything else by 8; the block
    // layer clears that easily, a layer holding two or three objects will not,
    // and not committing simply leaves the previous frame's value in place.
    // Range check because only GetBlockPos produces 512 - rotation in [1, 512];
    // anything else in that field is some other object's leftover.
    if (voteCount >= 8 && voteCand >= 1 && voteCand <= 512)
        dcSSRot = (512 - voteCand) & 0x1FF;

    DC_PROF_ADD(dcProfObjDrawNs, dcT);
#endif
}
#if RETRO_USING_KOS
static void DC_EvalLowTileHW(); // defined above DC_DrawHLineScrollLayerHW below
#endif

void DrawStageGFX()
{
    waterDrawPos = waterLevel - yScrollOffset;

#if RETRO_SOFTWARE_RENDER
    if (waterDrawPos < 0)
        waterDrawPos = 0;

    if (waterDrawPos > SCREEN_YSIZE)
        waterDrawPos = SCREEN_YSIZE;
#endif

#if RETRO_USING_KOS
    // Decide ONCE per frame whether the low tile passes render as hardware
    // quads or fall back to software together (exotic layer types, or a
    // deformation too fine to band-split inside the quad budget).
    //
    // MUST run AFTER waterDrawPos is final: the band estimate splits at the
    // water line and reads the two deformation tables either side of it, so
    // a stale value would mis-count bands on the frame the water level moves
    // and could overrun the sprite queue (dropped tiles = holes). The clamp
    // above is compiled (RETRO_RENDERTYPE defaults to RETRO_SW_RENDER), which
    // is also what bounds the table indexing below to [0, SCREEN_YSIZE].
    DC_EvalLowTileHW();
#endif

    if (tLayerMidPoint < 3) {
        DrawObjectList(0);

        if (activeTileLayers[0] < LAYER_COUNT) {
            switch (stageLayouts[activeTileLayers[0]].type) {
                case LAYER_HSCROLL: DrawHLineScrollLayer(0); break;

                case LAYER_VSCROLL: DrawVLineScrollLayer(0); break;

                case LAYER_3DFLOOR:
#if RETRO_SOFTWARE_RENDER
                    drawStageGFXHQ = false;
#endif
                    Draw3DFloorLayer(0);
                    break;

                case LAYER_3DSKY:
#if RETRO_SOFTWARE_RENDER
#if !RETRO_USE_ORIGINAL_CODE
                    if (Engine.useHQModes)
#endif
                        drawStageGFXHQ = true;

                    Draw3DSkyLayer(0);
#endif
                    break;

                default: break;
            }
        }

        DrawObjectList(1);

        if (activeTileLayers[1] < LAYER_COUNT) {
            switch (stageLayouts[activeTileLayers[1]].type) {
                case LAYER_HSCROLL: DrawHLineScrollLayer(1); break;

                case LAYER_VSCROLL: DrawVLineScrollLayer(1); break;

                case LAYER_3DFLOOR:
#if RETRO_SOFTWARE_RENDER
                    drawStageGFXHQ = false;
#endif
                    Draw3DFloorLayer(1);
                    break;

                case LAYER_3DSKY:
#if RETRO_SOFTWARE_RENDER
#if !RETRO_USE_ORIGINAL_CODE
                    if (Engine.useHQModes)
#endif
                        drawStageGFXHQ = true;

                    Draw3DSkyLayer(1);
#endif
                    break;

                default: break;
            }
        }

        DrawObjectList(2);
        DrawObjectList(3);
        DrawObjectList(4);

        if (activeTileLayers[2] < LAYER_COUNT) {
            switch (stageLayouts[activeTileLayers[2]].type) {
                case LAYER_HSCROLL: DrawHLineScrollLayer(2); break;

                case LAYER_VSCROLL: DrawVLineScrollLayer(2); break;

                case LAYER_3DFLOOR:
#if RETRO_SOFTWARE_RENDER
                    drawStageGFXHQ = false;
#endif
                    Draw3DFloorLayer(2);
                    break;

                case LAYER_3DSKY:
#if RETRO_SOFTWARE_RENDER
#if !RETRO_USE_ORIGINAL_CODE
                    if (Engine.useHQModes)
#endif
                        drawStageGFXHQ = true;

                    Draw3DSkyLayer(2);
#endif
                    break;

                default: break;
            }
        }
    }
    else if (tLayerMidPoint < 6) {
        DrawObjectList(0);

        if (activeTileLayers[0] < LAYER_COUNT) {
            switch (stageLayouts[activeTileLayers[0]].type) {
                case LAYER_HSCROLL: DrawHLineScrollLayer(0); break;

                case LAYER_VSCROLL: DrawVLineScrollLayer(0); break;

                case LAYER_3DFLOOR:
#if RETRO_SOFTWARE_RENDER
                    drawStageGFXHQ = false;
#endif
                    Draw3DFloorLayer(0);
                    break;

                case LAYER_3DSKY:
#if RETRO_SOFTWARE_RENDER
#if !RETRO_USE_ORIGINAL_CODE
                    if (Engine.useHQModes)
#endif
                        drawStageGFXHQ = true;

                    Draw3DSkyLayer(0);
#endif
                    break;

                default: break;
            }
        }

        DrawObjectList(1);

        if (activeTileLayers[1] < LAYER_COUNT) {
            switch (stageLayouts[activeTileLayers[1]].type) {
                case LAYER_HSCROLL: DrawHLineScrollLayer(1); break;

                case LAYER_VSCROLL: DrawVLineScrollLayer(1); break;

                case LAYER_3DFLOOR:
#if RETRO_SOFTWARE_RENDER
                    drawStageGFXHQ = false;
#endif
                    Draw3DFloorLayer(1);
                    break;

                case LAYER_3DSKY:
#if RETRO_SOFTWARE_RENDER
#if !RETRO_USE_ORIGINAL_CODE
                    if (Engine.useHQModes)
#endif
                        drawStageGFXHQ = true;

                    Draw3DSkyLayer(1);
#endif
                    break;

                default: break;
            }
        }

        DrawObjectList(2);

        if (activeTileLayers[2] < LAYER_COUNT) {
            switch (stageLayouts[activeTileLayers[2]].type) {
                case LAYER_HSCROLL: DrawHLineScrollLayer(2); break;
                case LAYER_VSCROLL: DrawVLineScrollLayer(2); break;
                case LAYER_3DFLOOR:
#if RETRO_SOFTWARE_RENDER
                    drawStageGFXHQ = false;
#endif
                    Draw3DFloorLayer(2);
                    break;
                case LAYER_3DSKY:
#if RETRO_SOFTWARE_RENDER
#if !RETRO_USE_ORIGINAL_CODE
                    if (Engine.useHQModes)
#endif
                        drawStageGFXHQ = true;

                    Draw3DSkyLayer(2);
#endif
                    break;
                default: break;
            }
        }

        DrawObjectList(3);
        DrawObjectList(4);
    }

    if (tLayerMidPoint < 6) {
        if (activeTileLayers[3] < LAYER_COUNT) {
            switch (stageLayouts[activeTileLayers[3]].type) {
                case LAYER_HSCROLL: DrawHLineScrollLayer(3); break;

                case LAYER_VSCROLL: DrawVLineScrollLayer(3); break;

                case LAYER_3DFLOOR:
#if RETRO_SOFTWARE_RENDER
                    drawStageGFXHQ = false;
#endif
                    Draw3DFloorLayer(3);
                    break;

                case LAYER_3DSKY:
#if RETRO_SOFTWARE_RENDER
#if !RETRO_USE_ORIGINAL_CODE
                    if (Engine.useHQModes)
#endif
                        drawStageGFXHQ = true;

                    Draw3DSkyLayer(3);
#endif
                    break;
                default: break;
            }
        }

        DrawObjectList(5);
#if RETRO_REV03
#if !RETRO_USE_ORIGINAL_CODE
        // Hacky fix for Tails Object not working properly in special stages on non-Origins bytecode
        if (forceUseScripts || Engine.usingOrigins)
#endif
            DrawObjectList(7);
#endif
        DrawObjectList(6);
    }

#if !RETRO_USE_ORIGINAL_CODE
    if (drawStageGFXHQ)
        DrawDebugOverlays();
#endif

#if RETRO_SOFTWARE_RENDER
#if RETRO_USING_KOS
    // On DC the software fade rect would only darken the framebuffer (tiles),
    // missing the hardware sprites. Route it to a PVR overlay that covers the
    // whole composited scene instead (see DC_FlipScreen).
    if (fadeMode > 0)
        DC_SetFadeOverlay(fadeR, fadeG, fadeB, fadeA);
#else
    if (drawStageGFXHQ) {
        CopyFrameOverlay2x();

        if (fadeMode > 0) {
            DrawRectangle(0, 0, SCREEN_XSIZE, SCREEN_YSIZE, fadeR, fadeG, fadeB, fadeA);
            SetFadeHQ(fadeR, fadeG, fadeB, fadeA);
        }
    }
    else {
        if (fadeMode > 0) {
            DrawRectangle(0, 0, SCREEN_XSIZE, SCREEN_YSIZE, fadeR, fadeG, fadeB, fadeA);
        }
    }
#endif
#endif

#if !RETRO_USE_ORIGINAL_CODE
    if (!drawStageGFXHQ)
        DrawDebugOverlays();
#endif
}

#if !RETRO_USE_ORIGINAL_CODE
void DrawDebugOverlays()
{
    if (showHitboxes) {
        for (int i = 0; i < debugHitboxCount; ++i) {
            DebugHitboxInfo *info = &debugHitboxList[i];
            int x                 = info->xpos + (info->left << 16);
            int y                 = info->ypos + (info->top << 16);
            int w                 = abs((info->xpos + (info->right << 16)) - x) >> 16;
            int h                 = abs((info->ypos + (info->bottom << 16)) - y) >> 16;
            x                     = (x >> 16) - xScrollOffset;
            y                     = (y >> 16) - yScrollOffset;

            switch (info->type) {
                case H_TYPE_TOUCH:
                    if (showHitboxes & 1)
                        DrawRectangle(x, y, w, h, info->collision ? 0x80 : 0xFF, info->collision ? 0x80 : 0x00, 0x00, 0x60);
                    break;

                case H_TYPE_BOX:
                    if (showHitboxes & 1) {
                        DrawRectangle(x, y, w, h, 0x00, 0x00, 0xFF, 0x60);
                        if (info->collision & 1) // top
                            DrawRectangle(x, y, w, 1, 0xFF, 0xFF, 0x00, 0xC0);
                        if (info->collision & 8) // bottom
                            DrawRectangle(x, y + h, w, 1, 0xFF, 0xFF, 0x00, 0xC0);
                        if (info->collision & 2) { // left
                            int sy = y;
                            int sh = h;
                            if (info->collision & 1) {
                                sy++;
                                sh--;
                            }
                            if (info->collision & 8)
                                sh--;
                            DrawRectangle(x, sy, 1, sh, 0xFF, 0xFF, 0x00, 0xC0);
                        }
                        if (info->collision & 4) { // right
                            int sy = y;
                            int sh = h;
                            if (info->collision & 1) {
                                sy++;
                                sh--;
                            }
                            if (info->collision & 8)
                                sh--;
                            DrawRectangle(x + w, sy, 1, sh, 0xFF, 0xFF, 0x00, 0xC0);
                        }
                    }
                    break;

                case H_TYPE_PLAT:
                    if (showHitboxes & 1) {
                        DrawRectangle(x, y, w, h, 0x00, 0xFF, 0x00, 0x60);
                        if (info->collision & 1) // top
                            DrawRectangle(x, y, w, 1, 0xFF, 0xFF, 0x00, 0xC0);
                        if (info->collision & 8) // bottom
                            DrawRectangle(x, y + h, w, 1, 0xFF, 0xFF, 0x00, 0xC0);
                    }
                    break;

                case H_TYPE_FINGER:
                    if (showHitboxes & 2)
                        DrawRectangle(x + xScrollOffset, y + yScrollOffset, w, h, 0xF0, 0x00, 0xF0, 0x60);
                    break;
            }
        }
    }

    if (Engine.showPaletteOverlay) {
        for (int p = 0; p < PALETTE_COUNT; ++p) {
            int x = (SCREEN_XSIZE - (0x10 << 3));
            int y = (SCREEN_YSIZE - (0x10 << 2));
            for (int c = 0; c < PALETTE_COLOR_COUNT; ++c) {
                int g = fullPalette32[p][c].g;
                // HQ mode overrides any magenta px, so slightly change the g channel since it has the most bits to make it "not quite magenta"
                if (drawStageGFXHQ && fullPalette32[p][c].r == 0xFF && fullPalette32[p][c].g == 0x00 && fullPalette32[p][c].b == 0xFF)
                    g += 8;

                DrawRectangle(x + ((c & 0xF) << 1) + ((p % (PALETTE_COUNT / 2)) * (2 * 16)),
                              y + ((c >> 4) << 1) + ((p / (PALETTE_COUNT / 2)) * (2 * 16)), 2, 2, fullPalette32[p][c].r, g, fullPalette32[p][c].b,
                              0xFF);
            }
        }
    }
}
#endif

#if RETRO_USING_KOS
// P2b/P3: tile layers as hardware quads.
// P2b moved the HIGH (visualPlane 1) pass onto the PVR so foreground tiles
// occlude sprites. P3 generalises the same machinery to the LOW passes
// (background layers + the FG's plane-0 tiles) — the last software
// rasterisation, and the measured cause of Hill Top's frame drops on real
// hardware (the perf bars showed ~20-28ms frames with the GPU idle: 77k+
// pixels/layer/frame of SH4 palette blits).
//
// Per-line parallax is preserved EXACTLY by splitting the screen into bands
// wherever the layer's lineScroll index changes (not just at tile-row
// boundaries). Per-line DEFORMATION (water ripple) is handled the same way:
// a band also ends wherever the deformation offset changes, and at the water
// line (the above/below-water tables are separate). That is exact, not an
// approximation — the same integer the software path adds to chunkX is added
// here — but it COSTS BANDS, so DC_EvalLowTileHW counts the extra ones into
// its quad estimate and a layer whose wobble is too fine to afford simply
// stays software, exactly as before. A VSCROLL/3D layer is still software-
// only and still ends the hardware suffix outright.
//
// This replaces an all-or-nothing bail: previously ANY deformed low layer
// dropped EVERY low pass to software. Because the FG's plane-0 pass is the
// FRONT-most low pass, a zone that deforms the foreground took all three low
// passes down with it — visible in the redream log as
// "low tile passes: hardware from layer 3 (mid=3)", which the credits demos
// still hit on one of their zones today. Above the water line a zone that
// only sets the *_WATER deformations has no offset at all, so those lines go
// hardware regardless.
//
// Software layers must remain a BACK-most prefix (see below), so the walk is
// all-or-nothing from the first layer that fails. The high pass stays
// hardware regardless (a software high plane would land behind sprites —
// the P2b bug).
// First low-layer index rendered hardware this frame; tLayerMidPoint (or 4)
// means "none". Software layers render into the framebuffer, which the PVR
// shows at z=1 BEHIND all hardware quads — so software layers are only
// correct as a BACK-most prefix of the low draw order. We walk the low
// layers front-to-back and extend the hardware suffix while layers qualify;
// the first unaffordable / non-HSCROLL layer ends it. A back-most BG whose
// ripple is too fine to band-split therefore stays software while every
// layer in front of it still goes hardware.
static int dcLowTileHWFrom = 4;

// Below the water line a BACKGROUND ripple can run a full sine period every
// 16 lines — roughly 70 extra bands per layer, more quads than the queue can
// hold alongside the foreground, so such backgrounds stay software there.
// Set this to 1 to hold a BACKGROUND layer's deformation constant across each
// band instead of varying it per line: those backgrounds then cost no extra
// bands and go hardware too, at the price of quantising a +-1..3px ripple on
// layers that sit behind everything. The FOREGROUND is always exact
// regardless, so nothing the player collides with ever moves.
// A .cpp-only change: flip it, plain `make`, no `make clean`.
#define DC_BG_DEFORM_COARSE 0

// True when any active layer is a 3D type (the special stage half-pipe).
// The 3D renderers project into the FRAMEBUFFER and rely on classic
// paint-over compositing: the flat tile layout is drawn first, then the
// projected pipe is painted OVER it. Hardware tile quads float above the
// framebuffer (z >= 2), which lifted the flat layout over the projected
// pipe — the special stage showed flat rainbow diamonds instead of the
// half-pipe. In 3D stages every tile pass (high included) stays software;
// sprites remain hardware, which is above the pipe where they belong.
static bool dc3DTileStage = false;

static void DC_EvalLowTileHW()
{
    const int mid = tLayerMidPoint < 4 ? tLayerMidPoint : 4;
    int from      = mid;

    dc3DTileStage = false;
    for (int i = 0; i < 4; ++i) {
        const byte lid = activeTileLayers[i];
        if (lid < LAYER_COUNT) {
            const byte t = stageLayouts[lid].type;
            if (t == LAYER_3DFLOOR || t == LAYER_3DSKY)
                dc3DTileStage = true;
        }
    }
    if (dc3DTileStage) {
        dcLowTileHWFrom = mid;
        static bool logged3D = false;
        if (!logged3D) {
            DC_LOG("[DC] 3D stage: tile passes software (pipe compositing)\n");
            logged3D = true;
        }
        return;
    }

    int quadEstimate = 0;
    for (int i = mid - 1; i >= 0; --i) {
        const byte lid = activeTileLayers[i];
        if (lid >= LAYER_COUNT) {
            from = i; // empty slot: draws nothing, doesn't end the suffix
            continue;
        }
        TileLayer *layer = &stageLayouts[lid];
        if (!layer->xsize || !layer->ysize || layer->type == LAYER_NOSCROLL) {
            from = i; // never drawn: neutral
            continue;
        }
        if (layer->type != LAYER_HSCROLL)
            break; // VSCROLL/3D: software-only, ends the hardware suffix

        // Approximate the scroll state (scrollPos advances inside the draw
        // call itself; one frame of drift can't change these coarse checks).
        const int fullheight = layer->ysize << 7;
        int yscroll;
        if (lid)
            yscroll = ((yScrollOffset * layer->parallaxFactor >> 8) + (layer->scrollPos >> 16)) % fullheight;
        else
            yscroll = yScrollOffset % fullheight;
        if (yscroll < 0)
            yscroll += fullheight;

        // Deformation tables, indexed exactly as DrawHLineScrollLayer does:
        // the (byte) masks the BASE only and the per-line advance runs past
        // it into the duplicated tail SetLayerDeformation maintains
        // (DEFORM_STORE=256 entries copied up, DEFORM_COUNT=576 total, so a
        // base of <=255 plus 239 screen lines stays in bounds).
        const int dfYS = lid ? yscroll : yScrollOffset; // matches the drawer
        const int *dfAbove, *dfBelow;
        if (lid) {
            dfAbove = &bgDeformationData2[(byte)(dfYS + layer->deformationOffset)];
            dfBelow = &bgDeformationData3[(byte)(dfYS + waterDrawPos + layer->deformationOffsetW)];
        }
        else {
            dfAbove = &bgDeformationData0[(byte)(dfYS + layer->deformationOffset)];
            dfBelow = &bgDeformationData1[(byte)(dfYS + waterDrawPos + layer->deformationOffsetW)];
        }
        const bool coarseDf = DC_BG_DEFORM_COARSE && lid != 0;

        int bands  = 1;
        byte prev  = layer->lineScroll[yscroll];
        int prevDf = hParallax.deform[prev] ? (waterDrawPos > 0 ? dfAbove[0] : dfBelow[0]) : 0;
        for (int line = 1; line < SCREEN_YSIZE; ++line) {
            const int wy = (yscroll + line) % fullheight;
            const byte s = layer->lineScroll[wy];
            const int df = hParallax.deform[s] ? (line < waterDrawPos ? dfAbove[line] : dfBelow[line - waterDrawPos]) : 0;
            if (s != prev || (wy & 0xF) == 0 || line == waterDrawPos || (!coarseDf && df != prevDf))
                ++bands;
            prev   = s;
            prevDf = df;
        }

        // Queue-headroom bound (worst case, before blank-tile skipping).
        quadEstimate += bands * ((SCREEN_XSIZE / TILE_SIZE) + 2);
        if (quadEstimate > 2200)
            break;

        from = i;
    }

    dcLowTileHWFrom = from;

    // One-shot log whenever the split changes (stage transitions, water on
    // screen, ...): tells us from the serial log exactly which layers run
    // hardware without per-frame spam.
    static int lastFrom = -99;
    if (from != lastFrom) {
        DC_LOG("[DC] low tile passes: hardware from layer %d (mid=%d)\n", from, mid);
        lastFrom = from;
    }
}

static void DC_DrawHLineScrollLayerHW(int layerID, bool abovePass)
{
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];

    int layerwidth  = layer->xsize;
    int layerheight = layer->ysize;
    const byte plane = abovePass ? 1 : 0;

    byte *lineScroll;
    int yscrollOffset = 0;
    if (activeTileLayers[layerID]) { // BG Layer
        int yScroll    = yScrollOffset * layer->parallaxFactor >> 8;
        int fullheight = layerheight << 7;
        layer->scrollPos += layer->scrollSpeed;
        if (layer->scrollPos > fullheight << 16)
            layer->scrollPos -= fullheight << 16;
        yscrollOffset = (yScroll + (layer->scrollPos >> 16)) % fullheight;
        lineScroll    = layer->lineScroll;
    }
    else { // FG Layer
        lastXSize     = layer->xsize;
        yscrollOffset = yScrollOffset;
        lineScroll    = layer->lineScroll;
        for (int i = 0; i < PARALLAX_COUNT; ++i) hParallax.linePos[i] = xScrollOffset;
    }

    if (layer->type == LAYER_HSCROLL) {
        if (lastXSize != layerwidth) {
            int fullLayerwidth = layerwidth << 7;
            for (int i = 0; i < hParallax.entryCount; ++i) {
                hParallax.linePos[i] = xScrollOffset * hParallax.parallaxFactor[i] >> 8;
                if (hParallax.scrollPos[i] > fullLayerwidth << 16)
                    hParallax.scrollPos[i] -= fullLayerwidth << 16;
                if (hParallax.scrollPos[i] < 0)
                    hParallax.scrollPos[i] += fullLayerwidth << 16;
                hParallax.linePos[i] += hParallax.scrollPos[i] >> 16;
                hParallax.linePos[i] %= fullLayerwidth;
            }
        }
        int w = -1;
        if (activeTileLayers[layerID])
            w = layerwidth;
        lastXSize = w;
    }

    const int fullLayerwidth = layerwidth << 7;
    const int fullheight     = layerheight << 7;

    int tileYPos = yscrollOffset % fullheight;
    if (tileYPos < 0)
        tileYPos += fullheight;

    // Per-line deformation, indexed exactly as the software path does it:
    // above the water line from deformationOffset, below it from
    // deformationOffsetW biased by waterDrawPos, and only applied when the
    // line's own parallax entry carries the deform flag. The (byte) masks
    // the BASE only; the per-line advance runs into the duplicated tail that
    // SetLayerDeformation maintains.
    const int *dfAbove, *dfBelow;
    if (activeTileLayers[layerID]) {
        dfAbove = &bgDeformationData2[(byte)(yscrollOffset + layer->deformationOffset)];
        dfBelow = &bgDeformationData3[(byte)(yscrollOffset + waterDrawPos + layer->deformationOffsetW)];
    }
    else {
        dfAbove = &bgDeformationData0[(byte)(yscrollOffset + layer->deformationOffset)];
        dfBelow = &bgDeformationData1[(byte)(yscrollOffset + waterDrawPos + layer->deformationOffsetW)];
    }
    const bool coarseDf = DC_BG_DEFORM_COARSE && activeTileLayers[layerID] != 0;

    // Deformation offset for screen line y. Identical arithmetic to
    // DrawHLineScrollLayer's two-region walk, just addressed by line instead
    // of by an advancing pointer.
    auto deformAt = [&](int y) -> int {
        const byte s = lineScroll[(tileYPos + y) % fullheight];
        if (!hParallax.deform[s])
            return 0;
        return (y < waterDrawPos) ? dfAbove[y] : dfBelow[y - waterDrawPos];
    };

    // Walk the screen in horizontal bands: a band ends at a tile-row
    // boundary, wherever the per-line scroll index changes, wherever the
    // deformation offset changes, or at the water line — so parallax strips
    // and the water ripple both keep their exact per-line positions.
    int screenY = 0;
    while (screenY < SCREEN_YSIZE) {
        const int wy      = (tileYPos + screenY) % fullheight; // world line of this band
        const int tileY16 = wy & 0xF;
        int maxRows       = TILE_SIZE - tileY16; // lines to the next tile boundary
        if (screenY + maxRows > SCREEN_YSIZE)
            maxRows = SCREEN_YSIZE - screenY;
        // Never straddle the water line: the two sides read different tables.
        if (screenY < waterDrawPos && screenY + maxRows > waterDrawPos)
            maxRows = waterDrawPos - screenY;

        const byte scrollIdx = lineScroll[wy];
        const int deform     = deformAt(screenY);
        int rows             = 1;
        while (rows < maxRows && lineScroll[(tileYPos + screenY + rows) % fullheight] == scrollIdx
               && (coarseDf || deformAt(screenY + rows) == deform))
            ++rows;

        const int chunkY = wy >> 7;
        const int tileY  = (wy & 0x7F) >> 4;

        // X scroll for the band (constant across it by construction).
        int chunkX = hParallax.linePos[scrollIdx] + deform;
        chunkX %= fullLayerwidth;
        if (chunkX < 0)
            chunkX += fullLayerwidth;

        int screenX = -(chunkX & 0xF);            // first tile may start mid-tile
        int worldX  = chunkX - (chunkX & 0xF);    // tile-aligned world x
        while (screenX < SCREEN_XSIZE) {
            int wx = worldX % fullLayerwidth;
            if (wx < 0)
                wx += fullLayerwidth;
            const int chunk = (layer->tiles[(wx >> 7) + (chunkY << 8)] << 6) + ((wx & 0x7F) >> 4) + 8 * tileY;
            if (tiles128x128.visualPlane[chunk] == plane)
                DC_QueueTileQuad(tiles128x128.tileIndex[chunk], tiles128x128.direction[chunk], screenX, screenY, tileY16, rows);
            screenX += TILE_SIZE;
            worldX += TILE_SIZE;
        }
        screenY += rows;
    }
}
#endif

void DrawHLineScrollLayer(int layerID)
{
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];
    if (!layer->xsize || !layer->ysize)
        return;

#if RETRO_USING_KOS
    // High pass: always hardware (a software high plane would render behind
    // sprites). Low passes: hardware from dcLowTileHWFrom onward (see
    // DC_EvalLowTileHW) — layers before that render software into the
    // framebuffer, which correctly sits behind all hardware content.
    // 3D stages (special stage): everything software — the projected pipe
    // must paint OVER the flat layout in the framebuffer.
    if (!dc3DTileStage) {
        if (layerID >= tLayerMidPoint) {
            DC_DrawHLineScrollLayerHW(layerID, true);
            return;
        }
        if (layerID >= dcLowTileHWFrom) {
            DC_DrawHLineScrollLayerHW(layerID, false);
            return;
        }
    }
#endif

#if RETRO_SOFTWARE_RENDER
    int screenwidth16  = (GFX_LINESIZE >> 4) - 1;
    int layerwidth     = layer->xsize;
    int layerheight    = layer->ysize;
    bool aboveMidPoint = layerID >= tLayerMidPoint;

    byte *lineScroll;
    int *deformationData;
    int *deformationDataW;

    int yscrollOffset = 0;
    if (activeTileLayers[layerID]) { // BG Layer
        int yScroll    = yScrollOffset * layer->parallaxFactor >> 8;
        int fullheight = layerheight << 7;
        layer->scrollPos += layer->scrollSpeed;
        if (layer->scrollPos > fullheight << 16)
            layer->scrollPos -= fullheight << 16;
        yscrollOffset    = (yScroll + (layer->scrollPos >> 16)) % fullheight;
        layerheight      = fullheight >> 7;
        lineScroll       = layer->lineScroll;
        deformationData  = &bgDeformationData2[(byte)(yscrollOffset + layer->deformationOffset)];
        deformationDataW = &bgDeformationData3[(byte)(yscrollOffset + waterDrawPos + layer->deformationOffsetW)];
    }
    else { // FG Layer
        lastXSize     = layer->xsize;
        yscrollOffset = yScrollOffset;
        lineScroll    = layer->lineScroll;
        for (int i = 0; i < PARALLAX_COUNT; ++i) hParallax.linePos[i] = xScrollOffset;
        deformationData  = &bgDeformationData0[(byte)(yscrollOffset + layer->deformationOffset)];
        deformationDataW = &bgDeformationData1[(byte)(yscrollOffset + waterDrawPos + layer->deformationOffsetW)];
    }

    if (layer->type == LAYER_HSCROLL) {
        if (lastXSize != layerwidth) {
            int fullLayerwidth = layerwidth << 7;
            for (int i = 0; i < hParallax.entryCount; ++i) {
                hParallax.linePos[i] = xScrollOffset * hParallax.parallaxFactor[i] >> 8;
                if (hParallax.scrollPos[i] > fullLayerwidth << 16)
                    hParallax.scrollPos[i] -= fullLayerwidth << 16;
                if (hParallax.scrollPos[i] < 0)
                    hParallax.scrollPos[i] += fullLayerwidth << 16;
                hParallax.linePos[i] += hParallax.scrollPos[i] >> 16;
                hParallax.linePos[i] %= fullLayerwidth;
            }
        }
        int w = -1;
        if (activeTileLayers[layerID])
            w = layerwidth;
        lastXSize = w;
    }

    ushort *frameBufferPtr = Engine.frameBuffer;
    byte *lineBuffer       = gfxLineBuffer;
    int tileYPos           = yscrollOffset % (layerheight << 7);
    if (tileYPos < 0)
        tileYPos += layerheight << 7;
    byte *scrollIndex = &lineScroll[tileYPos];
    int tileY16       = tileYPos & 0xF;
    int chunkY        = tileYPos >> 7;
    int tileY         = (tileYPos & 0x7F) >> 4;

    // Draw Above Water (if applicable)
    int drawableLines[2] = { waterDrawPos, SCREEN_YSIZE - waterDrawPos };
    for (int i = 0; i < 2; ++i) {
        while (drawableLines[i]--) {
            activePalette   = fullPalette[*lineBuffer];
            activePalette32 = fullPalette32[*lineBuffer];
            lineBuffer++;
            int chunkX = hParallax.linePos[*scrollIndex];
            if (i == 0) {
                if (hParallax.deform[*scrollIndex])
                    chunkX += *deformationData;
                ++deformationData;
            }
            else {
                if (hParallax.deform[*scrollIndex])
                    chunkX += *deformationDataW;
                ++deformationDataW;
            }
            ++scrollIndex;

            int fullLayerwidth = layerwidth << 7;
            if (chunkX < 0)
                chunkX += fullLayerwidth;
            if (chunkX >= fullLayerwidth)
                chunkX -= fullLayerwidth;

            int chunkXPos         = chunkX >> 7;
            int tilePxXPos        = chunkX & 0xF;
            int tileXPxRemain     = TILE_SIZE - tilePxXPos;
            int chunk             = (layer->tiles[(chunkX >> 7) + (chunkY << 8)] << 6) + ((chunkX & 0x7F) >> 4) + 8 * tileY;
            int tileOffsetY       = TILE_SIZE * tileY16;
            int tileOffsetYFlipX  = TILE_SIZE * tileY16 + 0xF;
            int tileOffsetYFlipY  = TILE_SIZE * (0xF - tileY16);
            int tileOffsetYFlipXY = TILE_SIZE * (0xF - tileY16) + 0xF;
            int lineRemain        = GFX_LINESIZE;

            byte *gfxDataPtr  = NULL;
            int tilePxLineCnt = tileXPxRemain;

            // Draw the first tile to the left
            if (tiles128x128.visualPlane[chunk] == (byte)aboveMidPoint) {
                lineRemain -= tilePxLineCnt;
                switch (tiles128x128.direction[chunk]) {
                    case FLIP_NONE:
                        gfxDataPtr = &tilesetGFXData[tileOffsetY + tiles128x128.gfxDataPos[chunk] + tilePxXPos];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;
                        }
                        break;
                    case FLIP_X:

                        gfxDataPtr = &tilesetGFXData[tileOffsetYFlipX + tiles128x128.gfxDataPos[chunk] - tilePxXPos];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;
                        }
                        break;

                    case FLIP_Y:
                        gfxDataPtr = &tilesetGFXData[tileOffsetYFlipY + tiles128x128.gfxDataPos[chunk] + tilePxXPos];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;
                        }
                        break;

                    case FLIP_XY:
                        gfxDataPtr = &tilesetGFXData[tileOffsetYFlipXY + tiles128x128.gfxDataPos[chunk] - tilePxXPos];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;
                        }
                        break;
                    default: break;
                }
            }
            else {
                frameBufferPtr += tilePxLineCnt;
                lineRemain -= tilePxLineCnt;
            }

            // Draw the bulk of the tiles
            int chunkTileX   = ((chunkX & 0x7F) >> 4) + 1;
            int tilesPerLine = screenwidth16;
            while (tilesPerLine--) {
                if (chunkTileX < 8) {
                    ++chunk;
                }
                else {
                    if (++chunkXPos == layerwidth)
                        chunkXPos = 0;

                    chunkTileX = 0;
                    chunk      = (layer->tiles[chunkXPos + (chunkY << 8)] << 6) + 8 * tileY;
                }
                lineRemain -= TILE_SIZE;

                // Loop Unrolling (faster but messier code)
                if (tiles128x128.visualPlane[chunk] == (byte)aboveMidPoint) {
                    switch (tiles128x128.direction[chunk]) {
                        case FLIP_NONE:
                            gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileOffsetY];
                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            break;

                        case FLIP_X:
                            gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileOffsetYFlipX];
                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            break;

                        case FLIP_Y:
                            gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileOffsetYFlipY];
                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            ++gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            break;

                        case FLIP_XY:
                            gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileOffsetYFlipXY];
                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            --gfxDataPtr;

                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            ++frameBufferPtr;
                            break;
                    }
                }
                else {
                    frameBufferPtr += TILE_SIZE;
                }
                ++chunkTileX;
            }

            // Draw any remaining tiles
            while (lineRemain > 0) {
                if (chunkTileX++ < 8) {
                    ++chunk;
                }
                else {
                    chunkTileX = 0;
                    if (++chunkXPos == layerwidth)
                        chunkXPos = 0;

                    chunk = (layer->tiles[chunkXPos + (chunkY << 8)] << 6) + 8 * tileY;
                }

                tilePxLineCnt = lineRemain >= TILE_SIZE ? TILE_SIZE : lineRemain;
                lineRemain -= tilePxLineCnt;
                if (tiles128x128.visualPlane[chunk] == (byte)aboveMidPoint) {
                    switch (tiles128x128.direction[chunk]) {
                        case FLIP_NONE:
                            gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileOffsetY];
                            while (tilePxLineCnt--) {
                                if (*gfxDataPtr > 0)
                                    *frameBufferPtr = activePalette[*gfxDataPtr];
                                ++frameBufferPtr;
                                ++gfxDataPtr;
                            }
                            break;

                        case FLIP_X:
                            gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileOffsetYFlipX];
                            while (tilePxLineCnt--) {
                                if (*gfxDataPtr > 0)
                                    *frameBufferPtr = activePalette[*gfxDataPtr];
                                ++frameBufferPtr;
                                --gfxDataPtr;
                            }
                            break;

                        case FLIP_Y:
                            gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileOffsetYFlipY];
                            while (tilePxLineCnt--) {
                                if (*gfxDataPtr > 0)
                                    *frameBufferPtr = activePalette[*gfxDataPtr];
                                ++frameBufferPtr;
                                ++gfxDataPtr;
                            }
                            break;

                        case FLIP_XY:
                            gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileOffsetYFlipXY];
                            while (tilePxLineCnt--) {
                                if (*gfxDataPtr > 0)
                                    *frameBufferPtr = activePalette[*gfxDataPtr];
                                ++frameBufferPtr;
                                --gfxDataPtr;
                            }
                            break;

                        default: break;
                    }
                }
                else {
                    frameBufferPtr += tilePxLineCnt;
                }
            }

            if (++tileY16 >= TILE_SIZE) {
                tileY16 = 0;
                ++tileY;
            }

            if (tileY >= 8) {
                if (++chunkY == layerheight) {
                    chunkY = 0;
                    scrollIndex -= 0x80 * layerheight;
                }
                tileY = 0;
            }
        }
    }
#endif
}
void DrawVLineScrollLayer(int layerID)
{
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];
    if (!layer->xsize || !layer->ysize)
        return;
#if RETRO_SOFTWARE_RENDER
    int layerwidth     = layer->xsize;
    int layerheight    = layer->ysize;
    bool aboveMidPoint = layerID >= tLayerMidPoint;

    byte *lineScroll;
    int *deformationData;

    int xscrollOffset = 0;
    if (activeTileLayers[layerID]) { // BG Layer
        int xScroll        = xScrollOffset * layer->parallaxFactor >> 8;
        int fullLayerwidth = layerwidth << 7;
        layer->scrollPos += layer->scrollSpeed;
        if (layer->scrollPos > fullLayerwidth << 16)
            layer->scrollPos -= fullLayerwidth << 16;
        xscrollOffset   = (xScroll + (layer->scrollPos >> 16)) % fullLayerwidth;
        layerwidth      = fullLayerwidth >> 7;
        lineScroll      = layer->lineScroll;
        deformationData = &bgDeformationData2[(byte)(xscrollOffset + layer->deformationOffset)];
    }
    else { // FG Layer
        lastYSize            = layer->ysize;
        xscrollOffset        = xScrollOffset;
        lineScroll           = layer->lineScroll;
        vParallax.linePos[0] = yScrollOffset;
        vParallax.deform[0]  = true;
        deformationData      = &bgDeformationData0[(byte)(xScrollOffset + layer->deformationOffset)];
    }

    if (layer->type == LAYER_VSCROLL) {
        if (lastYSize != layerheight) {
            int fullLayerheight = layerheight << 7;
            for (int i = 0; i < vParallax.entryCount; ++i) {
                vParallax.linePos[i] = yScrollOffset * vParallax.parallaxFactor[i] >> 8;

                vParallax.scrollPos[i] += vParallax.scrollPos[i] << 16;
                if (vParallax.scrollPos[i] > fullLayerheight << 16)
                    vParallax.scrollPos[i] -= fullLayerheight << 16;

                vParallax.linePos[i] += vParallax.scrollPos[i] >> 16;
                vParallax.linePos[i] %= fullLayerheight;
            }
            layerheight = fullLayerheight >> 7;
        }
        lastYSize = layerheight;
    }

    ushort *frameBufferPtr = Engine.frameBuffer;
    activePalette          = fullPalette[gfxLineBuffer[0]];
    activePalette32        = fullPalette32[gfxLineBuffer[0]];
    int tileXPos           = xscrollOffset % (layerheight << 7);
    if (tileXPos < 0)
        tileXPos += layerheight << 7;
    byte *scrollIndex = &lineScroll[tileXPos];
    int chunkX        = tileXPos >> 7;
    int tileX16       = tileXPos & 0xF;
    int tileX         = (tileXPos & 0x7F) >> 4;

    // Draw Above Water (if applicable)
    int drawableLines = SCREEN_XSIZE;
    while (drawableLines--) {
        int chunkY = vParallax.linePos[*scrollIndex];
        if (vParallax.deform[*scrollIndex])
            chunkY += *deformationData;
        ++deformationData;
        ++scrollIndex;

        int fullLayerHeight = layerheight << 7;
        if (chunkY < 0)
            chunkY += fullLayerHeight;
        if (chunkY >= fullLayerHeight)
            chunkY -= fullLayerHeight;

        int chunkYPos         = chunkY >> 7;
        int tileY             = chunkY & 0xF;
        int tileYPxRemain     = TILE_SIZE - tileY;
        int chunk             = (layer->tiles[chunkX + (chunkY >> 7 << 8)] << 6) + tileX + 8 * ((chunkY & 0x7F) >> 4);
        int tileOffsetXFlipX  = 0xF - tileX16;
        int tileOffsetXFlipY  = tileX16 + SCREEN_YSIZE;
        int tileOffsetXFlipXY = 0xFF - tileX16;
        int lineRemain        = SCREEN_YSIZE;

        byte *gfxDataPtr  = NULL;
        int tilePxLineCnt = tileYPxRemain;

        // Draw the first tile to the left
        if (tiles128x128.visualPlane[chunk] == (byte)aboveMidPoint) {
            lineRemain -= tilePxLineCnt;
            switch (tiles128x128.direction[chunk]) {
                case FLIP_NONE:
                    gfxDataPtr = &tilesetGFXData[TILE_SIZE * tileY + tileX16 + tiles128x128.gfxDataPos[chunk]];
                    while (tilePxLineCnt--) {
                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;
                    }
                    break;

                case FLIP_X:
                    gfxDataPtr = &tilesetGFXData[TILE_SIZE * tileY + tileOffsetXFlipX + tiles128x128.gfxDataPos[chunk]];
                    while (tilePxLineCnt--) {
                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;
                    }
                    break;

                case FLIP_Y:
                    gfxDataPtr = &tilesetGFXData[tileOffsetXFlipY + tiles128x128.gfxDataPos[chunk] - TILE_SIZE * tileY];
                    while (tilePxLineCnt--) {
                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;
                    }
                    break;

                case FLIP_XY:
                    gfxDataPtr = &tilesetGFXData[tileOffsetXFlipXY + tiles128x128.gfxDataPos[chunk] - TILE_SIZE * tileY];
                    while (tilePxLineCnt--) {
                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;
                    }
                    break;

                default: break;
            }
        }
        else {
            frameBufferPtr += GFX_LINESIZE * tileYPxRemain;
            lineRemain -= tilePxLineCnt;
        }

        // Draw the bulk of the tiles
        int chunkTileY   = ((chunkY & 0x7F) >> 4) + 1;
        int tilesPerLine = (SCREEN_YSIZE >> 4) - 1;

        while (tilesPerLine--) {
            if (chunkTileY < 8) {
                chunk += 8;
            }
            else {
                if (++chunkYPos == layerheight)
                    chunkYPos = 0;

                chunkTileY = 0;
                chunk      = (layer->tiles[chunkX + (chunkYPos << 8)] << 6) + tileX;
            }
            lineRemain -= TILE_SIZE;

            // Loop Unrolling (faster but messier code)
            if (tiles128x128.visualPlane[chunk] == (byte)aboveMidPoint) {
                switch (tiles128x128.direction[chunk]) {
                    case FLIP_NONE:
                        gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileX16];
                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        break;

                    case FLIP_X:
                        gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileOffsetXFlipX];
                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        break;

                    case FLIP_Y:
                        gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileOffsetXFlipY];
                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        break;

                    case FLIP_XY:
                        gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileOffsetXFlipXY];
                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *frameBufferPtr = activePalette[*gfxDataPtr];
                        frameBufferPtr += GFX_LINESIZE;
                        break;
                }
            }
            else {
                frameBufferPtr += GFX_LINESIZE * TILE_SIZE;
            }
            ++chunkTileY;
        }

        // Draw any remaining tiles
        while (lineRemain > 0) {
            if (chunkTileY < 8) {
                chunk += 8;
            }
            else {
                if (++chunkYPos == layerheight)
                    chunkYPos = 0;

                chunkTileY = 0;
                chunk      = (layer->tiles[chunkX + (chunkYPos << 8)] << 6) + tileX;
            }

            tilePxLineCnt = lineRemain >= TILE_SIZE ? TILE_SIZE : lineRemain;
            lineRemain -= tilePxLineCnt;

            if (tiles128x128.visualPlane[chunk] == (byte)aboveMidPoint) {
                switch (tiles128x128.direction[chunk]) {
                    case FLIP_NONE:
                        gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileX16];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            frameBufferPtr += GFX_LINESIZE;
                            gfxDataPtr += TILE_SIZE;
                        }
                        break;

                    case FLIP_X:
                        gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileOffsetXFlipX];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            frameBufferPtr += GFX_LINESIZE;
                            gfxDataPtr += TILE_SIZE;
                        }
                        break;

                    case FLIP_Y:
                        gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileOffsetXFlipY];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            frameBufferPtr += GFX_LINESIZE;
                            gfxDataPtr -= TILE_SIZE;
                        }
                        break;

                    case FLIP_XY:
                        gfxDataPtr = &tilesetGFXData[tiles128x128.gfxDataPos[chunk] + tileOffsetXFlipXY];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *frameBufferPtr = activePalette[*gfxDataPtr];
                            frameBufferPtr += GFX_LINESIZE;
                            gfxDataPtr -= TILE_SIZE;
                        }
                        break;

                    default: break;
                }
            }
            else {
                frameBufferPtr += GFX_LINESIZE * tilePxLineCnt;
            }
            chunkTileY++;
        }

        if (++tileX16 >= TILE_SIZE) {
            tileX16 = 0;
            ++tileX;
        }

        if (tileX >= 8) {
            if (++chunkX == layerwidth) {
                chunkX = 0;
                scrollIndex -= 0x80 * layerwidth;
            }
            tileX = 0;
        }

        frameBufferPtr -= GFX_FBUFFERMINUSONE;
    }
#endif
}
void Draw3DFloorLayer(int layerID)
{
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];
    if (!layer->xsize || !layer->ysize)
        return;

#if RETRO_SOFTWARE_RENDER
    int layerWidth         = layer->xsize << 7;
    int layerHeight        = layer->ysize << 7;
    int layerYPos          = layer->ypos;
    int layerZPos          = layer->zpos;
    int sinValue           = sinM7LookupTable[layer->angle];
    int cosValue           = cosM7LookupTable[layer->angle];
    byte *gfxLineBufferPtr = &gfxLineBuffer[(SCREEN_YSIZE / 2) + 12];
    ushort *frameBufferPtr = &Engine.frameBuffer[((SCREEN_YSIZE / 2) + 12) * GFX_LINESIZE];
    int layerXPos          = layer->xpos >> 4;
    int ZBuffer            = layerZPos >> 4;
    for (int i = 4; i < 112; ++i) {
        if (!(i & 1)) {
            activePalette   = fullPalette[*gfxLineBufferPtr];
            activePalette32 = fullPalette32[*gfxLineBufferPtr];
            gfxLineBufferPtr++;
        }
        int XBuffer    = layerYPos / (i << 9) * -cosValue >> 8;
        int YBuffer    = sinValue * (layerYPos / (i << 9)) >> 8;
        int XPos       = layerXPos + (3 * sinValue * (layerYPos / (i << 9)) >> 2) - XBuffer * SCREEN_CENTERX;
        int YPos       = ZBuffer + (3 * cosValue * (layerYPos / (i << 9)) >> 2) - YBuffer * SCREEN_CENTERX;
        int lineBuffer = 0;
        while (lineBuffer < GFX_LINESIZE) {
            int tileX = XPos >> 12;
            int tileY = YPos >> 12;
            if (tileX > -1 && tileX < layerWidth && tileY > -1 && tileY < layerHeight) {
                int chunk       = tile3DFloorBuffer[(YPos >> 16 << 8) + (XPos >> 16)];
                byte *tilePixel = &tilesetGFXData[tiles128x128.gfxDataPos[chunk]];
                switch (tiles128x128.direction[chunk]) {
                    case FLIP_NONE: tilePixel += 16 * (tileY & 0xF) + (tileX & 0xF); break;
                    case FLIP_X: tilePixel += 16 * (tileY & 0xF) + 15 - (tileX & 0xF); break;
                    case FLIP_Y: tilePixel += (tileX & 0xF) + SCREEN_YSIZE - 16 * (tileY & 0xF); break;
                    case FLIP_XY: tilePixel += 15 - (tileX & 0xF) + SCREEN_YSIZE - 16 * (tileY & 0xF); break;
                    default: break;
                }

                if (*tilePixel > 0)
                    *frameBufferPtr = activePalette[*tilePixel];
            }
            ++frameBufferPtr;
            ++lineBuffer;
            XPos += XBuffer;
            YPos += YBuffer;
        }
    }
#endif
}
void Draw3DSkyLayer(int layerID)
{
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];
    if (!layer->xsize || !layer->ysize)
        return;

#if RETRO_SOFTWARE_RENDER
    int layerWidth         = layer->xsize << 7;
    int layerHeight        = layer->ysize << 7;
    int layerYPos          = layer->ypos;
    int sinValue           = sinM7LookupTable[layer->angle & 0x1FF];
    int cosValue           = cosM7LookupTable[layer->angle & 0x1FF];
    ushort *frameBufferPtr = &Engine.frameBuffer[((SCREEN_YSIZE / 2) + 12) * GFX_LINESIZE];
    ushort *bufferPtr      = Engine.frameBuffer2x;
    if (!drawStageGFXHQ)
        bufferPtr = &Engine.frameBuffer[((SCREEN_YSIZE / 2) + 12) * GFX_LINESIZE];
    byte *gfxLineBufferPtr = &gfxLineBuffer[((SCREEN_YSIZE / 2) + 12)];
    int layerXPos          = layer->xpos >> 4;
    int layerZPos          = layer->zpos >> 4;
    for (int i = TILE_SIZE / 2; i < SCREEN_YSIZE - TILE_SIZE; ++i) {
        if (!(i & 1)) {
            activePalette   = fullPalette[*gfxLineBufferPtr];
            activePalette32 = fullPalette32[*gfxLineBufferPtr];
            gfxLineBufferPtr++;
        }
        int xBuffer    = layerYPos / (i << 8) * -cosValue >> 9;
        int yBuffer    = sinValue * (layerYPos / (i << 8)) >> 9;
        int XPos       = layerXPos + (3 * sinValue * (layerYPos / (i << 8)) >> 2) - xBuffer * GFX_LINESIZE;
        int YPos       = layerZPos + (3 * cosValue * (layerYPos / (i << 8)) >> 2) - yBuffer * GFX_LINESIZE;
        int lineBuffer = 0;
        while (lineBuffer < GFX_LINESIZE * 2) {
            int tileX = XPos >> 12;
            int tileY = YPos >> 12;
            if (tileX > -1 && tileX < layerWidth && tileY > -1 && tileY < layerHeight) {
                int chunk       = tile3DFloorBuffer[(YPos >> 16 << 8) + (XPos >> 16)];
                byte *tilePixel = &tilesetGFXData[tiles128x128.gfxDataPos[chunk]];
                switch (tiles128x128.direction[chunk]) {
                    case FLIP_NONE: tilePixel += TILE_SIZE * (tileY & 0xF) + (tileX & 0xF); break;
                    case FLIP_X: tilePixel += TILE_SIZE * (tileY & 0xF) + 0xF - (tileX & 0xF); break;
                    case FLIP_Y: tilePixel += (tileX & 0xF) + SCREEN_YSIZE - TILE_SIZE * (tileY & 0xF); break;
                    case FLIP_XY: tilePixel += 0xF - (tileX & 0xF) + SCREEN_YSIZE - TILE_SIZE * (tileY & 0xF); break;
                    default: break;
                }

                if (*tilePixel > 0)
                    *bufferPtr = activePalette[*tilePixel];
                else if (drawStageGFXHQ)
                    *bufferPtr = *frameBufferPtr;
            }
            else if (drawStageGFXHQ) {
                *bufferPtr = *frameBufferPtr;
            }

            if (lineBuffer & 1)
                ++frameBufferPtr;

            if (drawStageGFXHQ)
                bufferPtr++;
            else if (lineBuffer & 1)
                ++bufferPtr;

            lineBuffer++;
            XPos += xBuffer;
            YPos += yBuffer;
        }

        if (!(i & 1))
            frameBufferPtr -= GFX_LINESIZE;

        if (!(i & 1) && !drawStageGFXHQ)
            bufferPtr -= GFX_LINESIZE;
    }

    if (drawStageGFXHQ) {
        frameBufferPtr = &Engine.frameBuffer[((SCREEN_YSIZE / 2) + 12) * GFX_LINESIZE];
        int cnt        = ((SCREEN_YSIZE / 2) - 12) * GFX_LINESIZE;
        while (cnt--) *frameBufferPtr++ = 0xF81F; // Magenta
    }
#endif
}

void DrawRectangle(int XPos, int YPos, int width, int height, int R, int G, int B, int A)
{
    if (A > 0xFF)
        A = 0xFF;
#if RETRO_USING_KOS
    // Dreamcast: the software path below blends into Engine.frameBuffer, which
    // is only the background layer here — a rect meant to veil the hardware
    // sprites (e.g. the title emblem's fade-in-from-black) would miss them.
    // Queue a hardware colour quad at the current draw-order z instead so it
    // layers correctly against the sprites drawn around it.
    if (DC_UseHWSprites()) {
        DC_QueueRect(XPos, YPos, width, height, R, G, B, A);
        return;
    }
#endif
#if RETRO_SOFTWARE_RENDER
    if (width + XPos > GFX_LINESIZE)
        width = GFX_LINESIZE - XPos;
    if (XPos < 0) {
        width += XPos;
        XPos = 0;
    }

    if (height + YPos > SCREEN_YSIZE)
        height = SCREEN_YSIZE - YPos;
    if (YPos < 0) {
        height += YPos;
        YPos = 0;
    }
    if (width <= 0 || height <= 0 || A <= 0)
        return;
    int pitch              = GFX_LINESIZE - width;
    ushort *frameBufferPtr = &Engine.frameBuffer[XPos + GFX_LINESIZE * YPos];
    ushort clr             = PACK_RGB888(R, G, B);
    if (A == 0xFF) {
        int h = height;
        while (h--) {
            int w = width;
            while (w--) {
                *frameBufferPtr = clr;
                ++frameBufferPtr;
            }
            frameBufferPtr += pitch;
        }
    }
    else {
        ushort *fbufferBlend = &blendLookupTable[0x20 * (0xFF - A)];
        ushort *pixelBlend   = &blendLookupTable[0x20 * A];

        int h = height;
        while (h--) {
            int w = width;
            while (w--) {
                int R = (fbufferBlend[(*frameBufferPtr & 0xF800) >> 11] + pixelBlend[(clr & 0xF800) >> 11]) << 11;
                int G = (fbufferBlend[(*frameBufferPtr & 0x7E0) >> 6] + pixelBlend[(clr & 0x7E0) >> 6]) << 6;
                int B = fbufferBlend[*frameBufferPtr & 0x1F] + pixelBlend[clr & 0x1F];

                *frameBufferPtr = R | G | B;
                ++frameBufferPtr;
            }
            frameBufferPtr += pitch;
        }
    }
#endif
}

void SetFadeHQ(int R, int G, int B, int A)
{
#if RETRO_SOFTWARE_RENDER
    if (A <= 0)
        return;
    if (A > 0xFF)
        A = 0xFF;
    int pitch              = GFX_LINESIZE * 2;
    ushort *frameBufferPtr = Engine.frameBuffer2x;
    ushort clr             = PACK_RGB888(R, G, B);
    if (A == 0xFF) {
        int h = SCREEN_YSIZE;
        while (h--) {
            int w = pitch;
            while (w--) {
                *frameBufferPtr = clr;
                ++frameBufferPtr;
            }
        }
    }
    else {
        ushort *fbufferBlend = &blendLookupTable[0x20 * (0xFF - A)];
        ushort *pixelBlend   = &blendLookupTable[0x20 * A];

        int h = SCREEN_YSIZE;
        while (h--) {
            int w = pitch;
            while (w--) {
                int R = (fbufferBlend[(*frameBufferPtr & 0xF800) >> 11] + pixelBlend[(clr & 0xF800) >> 11]) << 11;
                int G = (fbufferBlend[(*frameBufferPtr & 0x7E0) >> 6] + pixelBlend[(clr & 0x7E0) >> 6]) << 6;
                int B = fbufferBlend[*frameBufferPtr & 0x1F] + pixelBlend[clr & 0x1F];

                *frameBufferPtr = R | G | B;
                ++frameBufferPtr;
            }
        }
    }
#endif
}

void DrawTintRectangle(int XPos, int YPos, int width, int height)
{
#if RETRO_SOFTWARE_RENDER
    if (width + XPos > GFX_LINESIZE)
        width = GFX_LINESIZE - XPos;
    if (XPos < 0) {
        width += XPos;
        XPos = 0;
    }

    if (height + YPos > SCREEN_YSIZE)
        height = SCREEN_YSIZE - YPos;
    if (YPos < 0) {
        height += YPos;
        YPos = 0;
    }
	
    if (width < 0 || height < 0)
        return;
	
    int yOffset = GFX_LINESIZE - width;
    for (ushort *frameBufferPtr = &Engine.frameBuffer[XPos + GFX_LINESIZE * YPos];; frameBufferPtr += yOffset) {
        height--;
        if (height < 0)
            break;
        int w = width;
        while (w--) {
            *frameBufferPtr = tintLookupTable[*frameBufferPtr];
            ++frameBufferPtr;
        }
    }
#endif
}
void DrawScaledTintMask(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                        int sheetID)
{
#if RETRO_SOFTWARE_RENDER
    int roundedYPos = 0;
    int roundedXPos = 0;
    int truescaleX  = 4 * scaleX;
    int truescaleY  = 4 * scaleY;
    int widthM1     = width - 1;
    int trueXPos    = XPos - (truescaleX * pivotX >> 11);
    width           = truescaleX * width >> 11;
    int trueYPos    = YPos - (truescaleY * pivotY >> 11);
    height          = truescaleY * height >> 11;
    int finalscaleX = (signed int)(float)((float)(2048.0 / (float)truescaleX) * 2048.0);
    int finalscaleY = (signed int)(float)((float)(2048.0 / (float)truescaleY) * 2048.0);
    if (width + trueXPos > GFX_LINESIZE) {
        width = GFX_LINESIZE - trueXPos;
    }

    if (direction) {
        if (trueXPos < 0) {
            widthM1 -= trueXPos * -finalscaleX >> 11;
            roundedXPos = (ushort)trueXPos * -(short)finalscaleX & 0x7FF;
            width += trueXPos;
            trueXPos = 0;
        }
    }
    else if (trueXPos < 0) {
        sprX += trueXPos * -finalscaleX >> 11;
        roundedXPos = (ushort)trueXPos * -(short)finalscaleX & 0x7FF;
        width += trueXPos;
        trueXPos = 0;
    }

    if (height + trueYPos > SCREEN_YSIZE) {
        height = SCREEN_YSIZE - trueYPos;
    }
    if (trueYPos < 0) {
        sprY += trueYPos * -finalscaleY >> 11;
        roundedYPos = (ushort)trueYPos * -(short)finalscaleY & 0x7FF;
        height += trueYPos;
        trueYPos = 0;
    }

    if (width <= 0 || height <= 0)
        return;

    GFXSurface *surface = &gfxSurface[sheetID];
    int pitch           = GFX_LINESIZE - width;
    int gfxwidth        = surface->width;
    // byte *lineBuffer       = &gfxLineBuffer[trueYPos];
    byte *gfxData          = &graphicData[sprX + surface->width * sprY + surface->dataPosition];
    ushort *frameBufferPtr = &Engine.frameBuffer[trueXPos + GFX_LINESIZE * trueYPos];
    if (direction == FLIP_X) {
        byte *gfxDataPtr = &gfxData[widthM1];
        int gfxPitch     = 0;
        while (height--) {
            int roundXPos = roundedXPos;
            int w         = width;
            while (w--) {
                if (*gfxDataPtr > 0)
                    *frameBufferPtr = tintLookupTable[*frameBufferPtr];
                int offsetX = finalscaleX + roundXPos;
                gfxDataPtr -= offsetX >> 11;
                gfxPitch += offsetX >> 11;
                roundXPos = offsetX & 0x7FF;
                ++frameBufferPtr;
            }
            frameBufferPtr += pitch;
            int offsetY = finalscaleY + roundedYPos;
            gfxDataPtr += gfxPitch + (offsetY >> 11) * gfxwidth;
            roundedYPos = offsetY & 0x7FF;
            gfxPitch    = 0;
        }
    }
    else {
        int gfxPitch = 0;
        int h        = height;
        while (h--) {
            int roundXPos = roundedXPos;
            int w         = width;
            while (w--) {
                if (*gfxData > 0)
                    *frameBufferPtr = tintLookupTable[*frameBufferPtr];
                int offsetX = finalscaleX + roundXPos;
                gfxData += offsetX >> 11;
                gfxPitch += offsetX >> 11;
                roundXPos = offsetX & 0x7FF;
                ++frameBufferPtr;
            }
            frameBufferPtr += pitch;
            int offsetY = finalscaleY + roundedYPos;
            gfxData += (offsetY >> 11) * gfxwidth - gfxPitch;
            roundedYPos = offsetY & 0x7FF;
            gfxPitch    = 0;
        }
    }
#endif
}

void DrawSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID)
{
#if RETRO_USING_KOS
    // Dreamcast: render as a PVR hardware quad instead of a software blit.
    if (DC_UseHWSprites()) {
        DC_QueueSprite(sheetID, XPos, YPos, width, height, sprX, sprY, FLIP_NONE);
        return;
    }
#endif
#if RETRO_SOFTWARE_RENDER
    if (width + XPos > GFX_LINESIZE)
        width = GFX_LINESIZE - XPos;
    if (XPos < 0) {
        sprX -= XPos;
        width += XPos;
        XPos = 0;
    }
    if (height + YPos > SCREEN_YSIZE)
        height = SCREEN_YSIZE - YPos;
    if (YPos < 0) {
        sprY -= YPos;
        height += YPos;
        YPos = 0;
    }
    if (width <= 0 || height <= 0)
        return;

    GFXSurface *surface    = &gfxSurface[sheetID];
    int pitch              = GFX_LINESIZE - width;
    int gfxPitch           = surface->width - width;
    byte *lineBuffer       = &gfxLineBuffer[YPos];
    byte *gfxDataPtr       = &graphicData[sprX + surface->width * sprY + surface->dataPosition];
    ushort *frameBufferPtr = &Engine.frameBuffer[XPos + GFX_LINESIZE * YPos];
    while (height--) {
        activePalette   = fullPalette[*lineBuffer];
        activePalette32 = fullPalette32[*lineBuffer];
        lineBuffer++;
        int w = width;
        while (w--) {
            if (*gfxDataPtr > 0)
                *frameBufferPtr = activePalette[*gfxDataPtr];
            ++gfxDataPtr;
            ++frameBufferPtr;
        }
        frameBufferPtr += pitch;
        gfxDataPtr += gfxPitch;
    }
#endif
}

#if RETRO_REV00
void DrawSpriteClipped(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID, int clipY)
{
    if (width + XPos > GFX_LINESIZE)
        width = GFX_LINESIZE - XPos;
    if (XPos < 0) {
        sprX -= XPos;
        width += XPos;
        XPos = 0;
    }
    if (height + YPos > clipY)
        height = clipY - YPos;
    if (YPos < 0) {
        sprY -= YPos;
        height += YPos;
        YPos = 0;
    }
    if (width <= 0 || height <= 0)
        return;

    GFXSurface *surface    = &gfxSurface[sheetID];
    int pitch              = GFX_LINESIZE - width;
    int gfxPitch           = surface->width - width;
    byte *lineBuffer       = &gfxLineBuffer[YPos];
    byte *gfxDataPtr       = &graphicData[sprX + surface->width * sprY + surface->dataPosition];
    ushort *frameBufferPtr = &Engine.frameBuffer[XPos + GFX_LINESIZE * YPos];
    while (height--) {
        activePalette   = fullPalette[*lineBuffer];
        activePalette32 = fullPalette32[*lineBuffer];
        lineBuffer++;
        int w = width;
        while (w--) {
            if (*gfxDataPtr > 0)
                *frameBufferPtr = activePalette[*gfxDataPtr];
            ++gfxDataPtr;
            ++frameBufferPtr;
        }
        frameBufferPtr += pitch;
        gfxDataPtr += gfxPitch;
    }
}
#endif

void DrawSpriteFlipped(int XPos, int YPos, int width, int height, int sprX, int sprY, int direction, int sheetID)
{
#if RETRO_USING_KOS
    // Dreamcast: render as a PVR hardware quad instead of a software blit.
    if (DC_UseHWSprites()) {
        DC_QueueSprite(sheetID, XPos, YPos, width, height, sprX, sprY, direction);
        return;
    }
#endif
#if RETRO_SOFTWARE_RENDER
    int widthFlip  = width;
    int heightFlip = height;

    if (width + XPos > GFX_LINESIZE) {
        width = GFX_LINESIZE - XPos;
    }
    if (XPos < 0) {
        sprX -= XPos;
        width += XPos;
        widthFlip += XPos + XPos;
        XPos = 0;
    }
    if (height + YPos > SCREEN_YSIZE) {
        height = SCREEN_YSIZE - YPos;
    }
    if (YPos < 0) {
        sprY -= YPos;
        height += YPos;
        heightFlip += YPos + YPos;
        YPos = 0;
    }
    if (width <= 0 || height <= 0)
        return;

    GFXSurface *surface = &gfxSurface[sheetID];
    int pitch;
    int gfxPitch;
    byte *lineBuffer;
    byte *gfxData;
    ushort *frameBufferPtr;
    switch (direction) {
        case FLIP_NONE:
            pitch          = GFX_LINESIZE - width;
            gfxPitch       = surface->width - width;
            lineBuffer     = &gfxLineBuffer[YPos];
            gfxData        = &graphicData[sprX + surface->width * sprY + surface->dataPosition];
            frameBufferPtr = &Engine.frameBuffer[XPos + GFX_LINESIZE * YPos];

            while (height--) {
                activePalette   = fullPalette[*lineBuffer];
                activePalette32 = fullPalette32[*lineBuffer];
                lineBuffer++;
                int w = width;
                while (w--) {
                    if (*gfxData > 0)
                        *frameBufferPtr = activePalette[*gfxData];
                    ++gfxData;
                    ++frameBufferPtr;
                }
                frameBufferPtr += pitch;
                gfxData += gfxPitch;
            }
            break;
        case FLIP_X:
            pitch          = GFX_LINESIZE - width;
            gfxPitch       = width + surface->width;
            lineBuffer     = &gfxLineBuffer[YPos];
            gfxData        = &graphicData[widthFlip - 1 + sprX + surface->width * sprY + surface->dataPosition];
            frameBufferPtr = &Engine.frameBuffer[XPos + GFX_LINESIZE * YPos];
            while (height--) {
                activePalette   = fullPalette[*lineBuffer];
                activePalette32 = fullPalette32[*lineBuffer];
                lineBuffer++;
                int w = width;
                while (w--) {
                    if (*gfxData > 0)
                        *frameBufferPtr = activePalette[*gfxData];
                    --gfxData;
                    ++frameBufferPtr;
                }
                frameBufferPtr += pitch;
                gfxData += gfxPitch;
            }
            break;
        case FLIP_Y:
            pitch          = GFX_LINESIZE - width;
            gfxPitch       = width + surface->width;
            lineBuffer     = &gfxLineBuffer[YPos];
            gfxData        = &graphicData[sprX + surface->width * (sprY + heightFlip - 1) + surface->dataPosition];
            frameBufferPtr = &Engine.frameBuffer[XPos + GFX_LINESIZE * YPos];
            while (height--) {
                activePalette   = fullPalette[*lineBuffer];
                activePalette32 = fullPalette32[*lineBuffer];
                lineBuffer++;
                int w = width;
                while (w--) {
                    if (*gfxData > 0)
                        *frameBufferPtr = activePalette[*gfxData];
                    ++gfxData;
                    ++frameBufferPtr;
                }
                frameBufferPtr += pitch;
                gfxData -= gfxPitch;
            }
            break;
        case FLIP_XY:
            pitch          = GFX_LINESIZE - width;
            gfxPitch       = surface->width - width;
            lineBuffer     = &gfxLineBuffer[YPos];
            gfxData        = &graphicData[widthFlip - 1 + sprX + surface->width * (sprY + heightFlip - 1) + surface->dataPosition];
            frameBufferPtr = &Engine.frameBuffer[XPos + GFX_LINESIZE * YPos];
            while (height--) {
                activePalette   = fullPalette[*lineBuffer];
                activePalette32 = fullPalette32[*lineBuffer];
                lineBuffer++;
                int w = width;
                while (w--) {
                    if (*gfxData > 0)
                        *frameBufferPtr = activePalette[*gfxData];
                    --gfxData;
                    ++frameBufferPtr;
                }
                frameBufferPtr += pitch;
                gfxData -= gfxPitch;
            }
            break;
        default: break;
    }
#endif
}
#if RETRO_USING_KOS
// Shared corner math for the rotated / rotozoomed hardware quads. Mirrors the
// software rasterizer's transform exactly: 512-step angle tables sampled at
// the INVERTED angle (0x200 - rotation), forward mapping
//   screen = pos + (sin*ly + cos*lx, cos*ly - sin*lx) / 512
// and FLIP_X mirroring in local space about the pivot BEFORE rotation (the
// local x range flips sign; texel sprX stays on the +pivotX side).
static void DC_QueueRotatedQuad(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation,
                                float fscale, int sheetID)
{
    int angle = rotation & 0x1FF;
    if (angle)
        angle = 0x200 - angle;
    const float s = fscale * (float)sin512LookupTable[angle] * (1.0f / 512.0f);
    const float c = fscale * (float)cos512LookupTable[angle] * (1.0f / 512.0f);

    // Local corner offsets, texture order TL,TR,BL,BR (lx0 = texture-left).
    float lx0, lx1;
    if (direction == FLIP_X) {
        lx0 = (float)pivotX;
        lx1 = (float)(pivotX - width);
    }
    else {
        lx0 = (float)-pivotX;
        lx1 = (float)(width - pivotX);
    }
    const float ly0 = (float)-pivotY;
    const float ly1 = (float)(height - pivotY);

    const float lxs[4] = { lx0, lx1, lx0, lx1 };
    const float lys[4] = { ly0, ly0, ly1, ly1 };
    float xs[4], ys[4];
    for (int i = 0; i < 4; ++i) {
        xs[i] = XPos + s * lys[i] + c * lxs[i];
        ys[i] = YPos + c * lys[i] - s * lxs[i];
    }
    DC_QueueSpriteQuad(sheetID, xs, ys, sprX, sprY, width, height, DC_INK_OPAQUE, 0xFF);
}
#endif

void DrawSpriteScaled(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                      int sheetID)
{
#if RETRO_USING_KOS
    // Dreamcast: hardware quad. scale is 9-bit fixed point (512 = 1.0); the
    // software rasterizer places the rect at pos - scale*pivot/512 and sizes
    // it scale*dim/512. FLIP_X keeps the same rect and mirrors the sampling,
    // so pass mirrored geometry (texture-left corner on the right).
    if (DC_UseHWSprites()) {
        const float sx = (float)scaleX * (1.0f / 512.0f);
        const float sy = (float)scaleY * (1.0f / 512.0f);
        const float x0 = XPos - sx * pivotX, x1 = x0 + sx * width;
        const float y0 = YPos - sy * pivotY, y1 = y0 + sy * height;
        float xs[4];
        if (direction) {
            xs[0] = x1; xs[1] = x0; xs[2] = x1; xs[3] = x0;
        }
        else {
            xs[0] = x0; xs[1] = x1; xs[2] = x0; xs[3] = x1;
        }
        const float ys[4] = { y0, y0, y1, y1 };
        DC_QueueSpriteQuad(sheetID, xs, ys, sprX, sprY, width, height, DC_INK_OPAQUE, 0xFF);
        return;
    }
#endif
#if RETRO_SOFTWARE_RENDER
    int roundedYPos = 0;
    int roundedXPos = 0;
    int truescaleX  = 4 * scaleX;
    int truescaleY  = 4 * scaleY;
    int widthM1     = width - 1;
    int trueXPos    = XPos - (truescaleX * pivotX >> 11);
    width           = truescaleX * width >> 11;
    int trueYPos    = YPos - (truescaleY * pivotY >> 11);
    height          = truescaleY * height >> 11;
    int finalscaleX = (signed int)(float)((float)(2048.0 / (float)truescaleX) * 2048.0);
    int finalscaleY = (signed int)(float)((float)(2048.0 / (float)truescaleY) * 2048.0);
    if (width + trueXPos > GFX_LINESIZE) {
        width = GFX_LINESIZE - trueXPos;
    }

    if (direction) {
        if (trueXPos < 0) {
            widthM1 -= trueXPos * -finalscaleX >> 11;
            roundedXPos = (ushort)trueXPos * -(short)finalscaleX & 0x7FF;
            width += trueXPos;
            trueXPos = 0;
        }
    }
    else if (trueXPos < 0) {
        sprX += trueXPos * -finalscaleX >> 11;
        roundedXPos = (ushort)trueXPos * -(short)finalscaleX & 0x7FF;
        width += trueXPos;
        trueXPos = 0;
    }

    if (height + trueYPos > SCREEN_YSIZE) {
        height = SCREEN_YSIZE - trueYPos;
    }
    if (trueYPos < 0) {
        sprY += trueYPos * -finalscaleY >> 11;
        roundedYPos = (ushort)trueYPos * -(short)finalscaleY & 0x7FF;
        height += trueYPos;
        trueYPos = 0;
    }

    if (width <= 0 || height <= 0)
        return;

    GFXSurface *surface    = &gfxSurface[sheetID];
    int pitch              = GFX_LINESIZE - width;
    int gfxwidth           = surface->width;
    byte *lineBuffer       = &gfxLineBuffer[trueYPos];
    byte *gfxData          = &graphicData[sprX + surface->width * sprY + surface->dataPosition];
    ushort *frameBufferPtr = &Engine.frameBuffer[trueXPos + GFX_LINESIZE * trueYPos];
    if (direction == FLIP_X) {
        byte *gfxDataPtr = &gfxData[widthM1];
        int gfxPitch     = 0;
        while (height--) {
            activePalette   = fullPalette[*lineBuffer];
            activePalette32 = fullPalette32[*lineBuffer];
            lineBuffer++;
            int roundXPos = roundedXPos;
            int w         = width;
            while (w--) {
                if (*gfxDataPtr > 0)
                    *frameBufferPtr = activePalette[*gfxDataPtr];
                int offsetX = finalscaleX + roundXPos;
                gfxDataPtr -= offsetX >> 11;
                gfxPitch += offsetX >> 11;
                roundXPos = offsetX & 0x7FF;
                ++frameBufferPtr;
            }
            frameBufferPtr += pitch;
            int offsetY = finalscaleY + roundedYPos;
            gfxDataPtr += gfxPitch + (offsetY >> 11) * gfxwidth;
            roundedYPos = offsetY & 0x7FF;
            gfxPitch    = 0;
        }
    }
    else {
        int gfxPitch = 0;
        int h        = height;
        while (h--) {
            activePalette   = fullPalette[*lineBuffer];
            activePalette32 = fullPalette32[*lineBuffer];
            lineBuffer++;
            int roundXPos = roundedXPos;
            int w         = width;
            while (w--) {
                if (*gfxData > 0)
                    *frameBufferPtr = activePalette[*gfxData];
                int offsetX = finalscaleX + roundXPos;
                gfxData += offsetX >> 11;
                gfxPitch += offsetX >> 11;
                roundXPos = offsetX & 0x7FF;
                ++frameBufferPtr;
            }
            frameBufferPtr += pitch;
            int offsetY = finalscaleY + roundedYPos;
            gfxData += (offsetY >> 11) * gfxwidth - gfxPitch;
            roundedYPos = offsetY & 0x7FF;
            gfxPitch    = 0;
        }
    }
#endif
}
#if !RETRO_REV02
void DrawScaledChar(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                    int sheetID)
{
#if RETRO_SOFTWARE_RENDER
    // Not avaliable in SW Render mode
#endif
}
#endif
void DrawSpriteRotated(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation,
                       int sheetID)
{
#if RETRO_USING_KOS
    // Dreamcast: hardware quad (rolling/tilted Sonic, etc).
    if (DC_UseHWSprites()) {
        DC_QueueRotatedQuad(direction, XPos, YPos, pivotX, pivotY, sprX, sprY, width, height, rotation, 1.0f, sheetID);
        return;
    }
#endif
#if RETRO_SOFTWARE_RENDER
    int sprXPos    = (pivotX + sprX) << 9;
    int sprYPos    = (pivotY + sprY) << 9;
    int fullwidth  = width + sprX;
    int fullheight = height + sprY;
    int angle      = rotation & 0x1FF;
    if (angle < 0)
        angle += 0x200;
    if (angle)
        angle = 0x200 - angle;
    int sine   = sin512LookupTable[angle];
    int cosine = cos512LookupTable[angle];
    int xPositions[4];
    int yPositions[4];

    if (direction == FLIP_X) {
        xPositions[0] = XPos + ((sine * (-pivotY - 2) + cosine * (pivotX + 2)) >> 9);
        yPositions[0] = YPos + ((cosine * (-pivotY - 2) - sine * (pivotX + 2)) >> 9);
        xPositions[1] = XPos + ((sine * (-pivotY - 2) + cosine * (pivotX - width - 2)) >> 9);
        yPositions[1] = YPos + ((cosine * (-pivotY - 2) - sine * (pivotX - width - 2)) >> 9);
        xPositions[2] = XPos + ((sine * (height - pivotY + 2) + cosine * (pivotX + 2)) >> 9);
        yPositions[2] = YPos + ((cosine * (height - pivotY + 2) - sine * (pivotX + 2)) >> 9);
        int a         = pivotX - width - 2;
        int b         = height - pivotY + 2;
        xPositions[3] = XPos + ((sine * b + cosine * a) >> 9);
        yPositions[3] = YPos + ((cosine * b - sine * a) >> 9);
    }
    else {
        xPositions[0] = XPos + ((sine * (-pivotY - 2) + cosine * (-pivotX - 2)) >> 9);
        yPositions[0] = YPos + ((cosine * (-pivotY - 2) - sine * (-pivotX - 2)) >> 9);
        xPositions[1] = XPos + ((sine * (-pivotY - 2) + cosine * (width - pivotX + 2)) >> 9);
        yPositions[1] = YPos + ((cosine * (-pivotY - 2) - sine * (width - pivotX + 2)) >> 9);
        xPositions[2] = XPos + ((sine * (height - pivotY + 2) + cosine * (-pivotX - 2)) >> 9);
        yPositions[2] = YPos + ((cosine * (height - pivotY + 2) - sine * (-pivotX - 2)) >> 9);
        int a         = width - pivotX + 2;
        int b         = height - pivotY + 2;
        xPositions[3] = XPos + ((sine * b + cosine * a) >> 9);
        yPositions[3] = YPos + ((cosine * b - sine * a) >> 9);
    }

    int left = GFX_LINESIZE;
    for (int i = 0; i < 4; ++i) {
        if (xPositions[i] < left)
            left = xPositions[i];
    }
    if (left < 0)
        left = 0;

    int right = 0;
    for (int i = 0; i < 4; ++i) {
        if (xPositions[i] > right)
            right = xPositions[i];
    }
    if (right > GFX_LINESIZE)
        right = GFX_LINESIZE;
    int maxX = right - left;

    int top = SCREEN_YSIZE;
    for (int i = 0; i < 4; ++i) {
        if (yPositions[i] < top)
            top = yPositions[i];
    }
    if (top < 0)
        top = 0;

    int bottom = 0;
    for (int i = 0; i < 4; ++i) {
        if (yPositions[i] > bottom)
            bottom = yPositions[i];
    }
    if (bottom > SCREEN_YSIZE)
        bottom = SCREEN_YSIZE;
    int maxY = bottom - top;

    if (maxX <= 0 || maxY <= 0)
        return;

    GFXSurface *surface    = &gfxSurface[sheetID];
    int pitch              = GFX_LINESIZE - maxX;
    int lineSize           = surface->widthShift;
    ushort *frameBufferPtr = &Engine.frameBuffer[left + GFX_LINESIZE * top];
    byte *lineBuffer       = &gfxLineBuffer[top];
    int startX             = left - XPos;
    int startY             = top - YPos;
    int shiftPivot         = (sprX << 9) - 1;
    fullwidth <<= 9;
    int shiftheight = (sprY << 9) - 1;
    fullheight <<= 9;
    byte *gfxData = &graphicData[surface->dataPosition];
    if (cosine < 0 || sine < 0)
        sprYPos += sine + cosine;

    if (direction == FLIP_X) {
        int drawX = sprXPos - (cosine * startX - sine * startY) - 0x100;
        int drawY = cosine * startY + sprYPos + sine * startX;
        while (maxY--) {
            activePalette   = fullPalette[*lineBuffer];
            activePalette32 = fullPalette32[*lineBuffer];
            lineBuffer++;
            int finalX = drawX;
            int finalY = drawY;
            int w      = maxX;
            while (w--) {
                if (finalX > shiftPivot && finalX < fullwidth && finalY > shiftheight && finalY < fullheight) {
                    byte index = gfxData[(finalY >> 9 << lineSize) + (finalX >> 9)];
                    if (index > 0)
                        *frameBufferPtr = activePalette[index];
                }
                ++frameBufferPtr;
                finalX -= cosine;
                finalY += sine;
            }
            drawX += sine;
            drawY += cosine;
            frameBufferPtr += pitch;
        }
    }
    else {
        int drawX = sprXPos + cosine * startX - sine * startY;
        int drawY = cosine * startY + sprYPos + sine * startX;
        while (maxY--) {
            activePalette   = fullPalette[*lineBuffer];
            activePalette32 = fullPalette32[*lineBuffer];
            lineBuffer++;
            int finalX = drawX;
            int finalY = drawY;
            int w      = maxX;
            while (w--) {
                if (finalX > shiftPivot && finalX < fullwidth && finalY > shiftheight && finalY < fullheight) {
                    byte index = gfxData[(finalY >> 9 << lineSize) + (finalX >> 9)];
                    if (index > 0)
                        *frameBufferPtr = activePalette[index];
                }
                ++frameBufferPtr;
                finalX += cosine;
                finalY += sine;
            }
            drawX -= sine;
            drawY += cosine;
            frameBufferPtr += pitch;
        }
    }
#endif
}

void DrawSpriteRotozoom(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation, int scale,
                        int sheetID)
{
#if RETRO_USING_KOS
    // Dreamcast: hardware quad. scale is 9-bit fixed point (512 = 1.0).
    if (DC_UseHWSprites()) {
        if (scale == 0)
            return;
        DC_QueueRotatedQuad(direction, XPos, YPos, pivotX, pivotY, sprX, sprY, width, height, rotation, (float)scale * (1.0f / 512.0f), sheetID);
        return;
    }
#endif
#if RETRO_SOFTWARE_RENDER
    if (scale == 0)
        return;

    int sprXPos    = (pivotX + sprX) << 9;
    int sprYPos    = (pivotY + sprY) << 9;
    int fullwidth  = width + sprX;
    int fullheight = height + sprY;
    int angle      = rotation & 0x1FF;
    if (angle < 0)
        angle += 0x200;
    if (angle)
        angle = 0x200 - angle;
    int sine   = scale * sin512LookupTable[angle] >> 9;
    int cosine = scale * cos512LookupTable[angle] >> 9;
    int xPositions[4];
    int yPositions[4];

    if (direction == FLIP_X) {
        xPositions[0] = XPos + ((sine * (-pivotY - 2) + cosine * (pivotX + 2)) >> 9);
        yPositions[0] = YPos + ((cosine * (-pivotY - 2) - sine * (pivotX + 2)) >> 9);
        xPositions[1] = XPos + ((sine * (-pivotY - 2) + cosine * (pivotX - width - 2)) >> 9);
        yPositions[1] = YPos + ((cosine * (-pivotY - 2) - sine * (pivotX - width - 2)) >> 9);
        xPositions[2] = XPos + ((sine * (height - pivotY + 2) + cosine * (pivotX + 2)) >> 9);
        yPositions[2] = YPos + ((cosine * (height - pivotY + 2) - sine * (pivotX + 2)) >> 9);
        int a         = pivotX - width - 2;
        int b         = height - pivotY + 2;
        xPositions[3] = XPos + ((sine * b + cosine * a) >> 9);
        yPositions[3] = YPos + ((cosine * b - sine * a) >> 9);
    }
    else {
        xPositions[0] = XPos + ((sine * (-pivotY - 2) + cosine * (-pivotX - 2)) >> 9);
        yPositions[0] = YPos + ((cosine * (-pivotY - 2) - sine * (-pivotX - 2)) >> 9);
        xPositions[1] = XPos + ((sine * (-pivotY - 2) + cosine * (width - pivotX + 2)) >> 9);
        yPositions[1] = YPos + ((cosine * (-pivotY - 2) - sine * (width - pivotX + 2)) >> 9);
        xPositions[2] = XPos + ((sine * (height - pivotY + 2) + cosine * (-pivotX - 2)) >> 9);
        yPositions[2] = YPos + ((cosine * (height - pivotY + 2) - sine * (-pivotX - 2)) >> 9);
        int a         = width - pivotX + 2;
        int b         = height - pivotY + 2;
        xPositions[3] = XPos + ((sine * b + cosine * a) >> 9);
        yPositions[3] = YPos + ((cosine * b - sine * a) >> 9);
    }
    int truescale = (signed int)(float)((float)(512.0 / (float)scale) * 512.0);
    sine          = truescale * sin512LookupTable[angle] >> 9;
    cosine        = truescale * cos512LookupTable[angle] >> 9;

    int left = GFX_LINESIZE;
    for (int i = 0; i < 4; ++i) {
        if (xPositions[i] < left)
            left = xPositions[i];
    }
    if (left < 0)
        left = 0;

    int right = 0;
    for (int i = 0; i < 4; ++i) {
        if (xPositions[i] > right)
            right = xPositions[i];
    }
    if (right > GFX_LINESIZE)
        right = GFX_LINESIZE;
    int maxX = right - left;

    int top = SCREEN_YSIZE;
    for (int i = 0; i < 4; ++i) {
        if (yPositions[i] < top)
            top = yPositions[i];
    }
    if (top < 0)
        top = 0;

    int bottom = 0;
    for (int i = 0; i < 4; ++i) {
        if (yPositions[i] > bottom)
            bottom = yPositions[i];
    }
    if (bottom > SCREEN_YSIZE)
        bottom = SCREEN_YSIZE;
    int maxY = bottom - top;

    if (maxX <= 0 || maxY <= 0)
        return;

    GFXSurface *surface    = &gfxSurface[sheetID];
    int pitch              = GFX_LINESIZE - maxX;
    int lineSize           = surface->widthShift;
    ushort *frameBufferPtr = &Engine.frameBuffer[left + GFX_LINESIZE * top];
    byte *lineBuffer       = &gfxLineBuffer[top];
    int startX             = left - XPos;
    int startY             = top - YPos;
    int shiftPivot         = (sprX << 9) - 1;
    fullwidth <<= 9;
    int shiftheight = (sprY << 9) - 1;
    fullheight <<= 9;
    byte *gfxData = &graphicData[surface->dataPosition];
    if (cosine < 0 || sine < 0)
        sprYPos += sine + cosine;

    if (direction == FLIP_X) {
        int drawX = sprXPos - (cosine * startX - sine * startY) - (truescale >> 1);
        int drawY = cosine * startY + sprYPos + sine * startX;
        while (maxY--) {
            activePalette   = fullPalette[*lineBuffer];
            activePalette32 = fullPalette32[*lineBuffer];
            lineBuffer++;
            int finalX = drawX;
            int finalY = drawY;
            int w      = maxX;
            while (w--) {
                if (finalX > shiftPivot && finalX < fullwidth && finalY > shiftheight && finalY < fullheight) {
                    byte index = gfxData[(finalY >> 9 << lineSize) + (finalX >> 9)];
                    if (index > 0)
                        *frameBufferPtr = activePalette[index];
                }
                ++frameBufferPtr;
                finalX -= cosine;
                finalY += sine;
            }
            drawX += sine;
            drawY += cosine;
            frameBufferPtr += pitch;
        }
    }
    else {
        int drawX = sprXPos + cosine * startX - sine * startY;
        int drawY = cosine * startY + sprYPos + sine * startX;
        while (maxY--) {
            activePalette   = fullPalette[*lineBuffer];
            activePalette32 = fullPalette32[*lineBuffer];
            lineBuffer++;
            int finalX = drawX;
            int finalY = drawY;
            int w      = maxX;
            while (w--) {
                if (finalX > shiftPivot && finalX < fullwidth && finalY > shiftheight && finalY < fullheight) {
                    byte index = gfxData[(finalY >> 9 << lineSize) + (finalX >> 9)];
                    if (index > 0)
                        *frameBufferPtr = activePalette[index];
                }
                ++frameBufferPtr;
                finalX += cosine;
                finalY += sine;
            }
            drawX -= sine;
            drawY += cosine;
            frameBufferPtr += pitch;
        }
    }
#endif
}

void DrawBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID)
{
#if RETRO_USING_KOS
    // 50% blend -> hardware alpha quad at half opacity.
    if (DC_UseHWSprites()) {
        DC_QueueSpriteInk(sheetID, XPos, YPos, width, height, sprX, sprY, FLIP_NONE, DC_INK_ALPHA, 0x80);
        return;
    }
#endif
#if RETRO_SOFTWARE_RENDER
    if (width + XPos > GFX_LINESIZE)
        width = GFX_LINESIZE - XPos;
    if (XPos < 0) {
        sprX -= XPos;
        width += XPos;
        XPos = 0;
    }
    if (height + YPos > SCREEN_YSIZE)
        height = SCREEN_YSIZE - YPos;
    if (YPos < 0) {
        sprY -= YPos;
        height += YPos;
        YPos = 0;
    }
    if (width <= 0 || height <= 0)
        return;

    GFXSurface *surface    = &gfxSurface[sheetID];
    int pitch              = GFX_LINESIZE - width;
    int gfxPitch           = surface->width - width;
    byte *lineBuffer       = &gfxLineBuffer[YPos];
    byte *gfxData          = &graphicData[sprX + surface->width * sprY + surface->dataPosition];
    ushort *frameBufferPtr = &Engine.frameBuffer[XPos + GFX_LINESIZE * YPos];
    while (height--) {
        activePalette   = fullPalette[*lineBuffer];
        activePalette32 = fullPalette32[*lineBuffer];
        lineBuffer++;
        int w = width;
        while (w--) {
            if (*gfxData > 0)
                *frameBufferPtr = ((activePalette[*gfxData] & 0xF7DE) >> 1) + ((*frameBufferPtr & 0xF7DE) >> 1);
            ++gfxData;
            ++frameBufferPtr;
        }
        frameBufferPtr += pitch;
        gfxData += gfxPitch;
    }
#endif
}
void DrawAlphaBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID)
{
    if (alpha > 0xFF)
        alpha = 0xFF;
#if RETRO_USING_KOS
    if (DC_UseHWSprites()) {
        DC_QueueSpriteInk(sheetID, XPos, YPos, width, height, sprX, sprY, FLIP_NONE, DC_INK_ALPHA, alpha);
        return;
    }
#endif
#if RETRO_SOFTWARE_RENDER
    if (width + XPos > GFX_LINESIZE)
        width = GFX_LINESIZE - XPos;
    if (XPos < 0) {
        sprX -= XPos;
        width += XPos;
        XPos = 0;
    }
    if (height + YPos > SCREEN_YSIZE)
        height = SCREEN_YSIZE - YPos;
    if (YPos < 0) {
        sprY -= YPos;
        height += YPos;
        YPos = 0;
    }
    if (width <= 0 || height <= 0 || alpha <= 0)
        return;

    GFXSurface *surface    = &gfxSurface[sheetID];
    int pitch              = GFX_LINESIZE - width;
    int gfxPitch           = surface->width - width;
    byte *lineBuffer       = &gfxLineBuffer[YPos];
    byte *gfxData          = &graphicData[sprX + surface->width * sprY + surface->dataPosition];
    ushort *frameBufferPtr = &Engine.frameBuffer[XPos + GFX_LINESIZE * YPos];
    if (alpha == 0xFF) {
        while (height--) {
            activePalette   = fullPalette[*lineBuffer];
            activePalette32 = fullPalette32[*lineBuffer];
            lineBuffer++;
            int w = width;
            while (w--) {
                if (*gfxData > 0)
                    *frameBufferPtr = activePalette[*gfxData];
                ++gfxData;
                ++frameBufferPtr;
            }
            frameBufferPtr += pitch;
            gfxData += gfxPitch;
        }
    }
    else {
        ushort *fbufferBlend = &blendLookupTable[0x20 * (0xFF - alpha)];
        ushort *pixelBlend   = &blendLookupTable[0x20 * alpha];

        while (height--) {
            activePalette   = fullPalette[*lineBuffer];
            activePalette32 = fullPalette32[*lineBuffer];
            lineBuffer++;
            int w = width;
            while (w--) {
                if (*gfxData > 0) {
                    ushort color = activePalette[*gfxData];

                    int R = (fbufferBlend[(*frameBufferPtr & 0xF800) >> 11] + pixelBlend[(color & 0xF800) >> 11]) << 11;
                    int G = (fbufferBlend[(*frameBufferPtr & 0x7E0) >> 6] + pixelBlend[(color & 0x7E0) >> 6]) << 6;
                    int B = fbufferBlend[*frameBufferPtr & 0x1F] + pixelBlend[color & 0x1F];

                    *frameBufferPtr = R | G | B;
                }
                ++gfxData;
                ++frameBufferPtr;
            }
            frameBufferPtr += pitch;
            gfxData += gfxPitch;
        }
    }
#endif
}
void DrawAdditiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID)
{
    if (alpha > 0xFF)
        alpha = 0xFF;
#if RETRO_USING_KOS
    if (DC_UseHWSprites()) {
        DC_QueueSpriteInk(sheetID, XPos, YPos, width, height, sprX, sprY, FLIP_NONE, DC_INK_ADD, alpha);
        return;
    }
#endif
#if RETRO_SOFTWARE_RENDER
    if (width + XPos > GFX_LINESIZE)
        width = GFX_LINESIZE - XPos;
    if (XPos < 0) {
        sprX -= XPos;
        width += XPos;
        XPos = 0;
    }
    if (height + YPos > SCREEN_YSIZE)
        height = SCREEN_YSIZE - YPos;
    if (YPos < 0) {
        sprY -= YPos;
        height += YPos;
        YPos = 0;
    }
    if (width <= 0 || height <= 0 || alpha <= 0)
        return;

    ushort *blendTablePtr  = &blendLookupTable[0x20 * alpha];
    GFXSurface *surface    = &gfxSurface[sheetID];
    int pitch              = GFX_LINESIZE - width;
    int gfxPitch           = surface->width - width;
    byte *lineBuffer       = &gfxLineBuffer[YPos];
    byte *gfxData          = &graphicData[sprX + surface->width * sprY + surface->dataPosition];
    ushort *frameBufferPtr = &Engine.frameBuffer[XPos + GFX_LINESIZE * YPos];

    while (height--) {
        activePalette   = fullPalette[*lineBuffer];
        activePalette32 = fullPalette32[*lineBuffer];
        lineBuffer++;
        int w = width;
        while (w--) {
            if (*gfxData > 0) {
                ushort color = activePalette[*gfxData];

                int R = minVal((blendTablePtr[(color & 0xF800) >> 11] << 11) + (*frameBufferPtr & 0xF800), 0xF800);
                int G = minVal((blendTablePtr[(color & 0x7E0) >> 6] << 6) + (*frameBufferPtr & 0x7E0), 0x7E0);
                int B = minVal(blendTablePtr[color & 0x1F] + (*frameBufferPtr & 0x1F), 0x1F);

                *frameBufferPtr = R | G | B;
            }
            ++gfxData;
            ++frameBufferPtr;
        }
        frameBufferPtr += pitch;
        gfxData += gfxPitch;
    }
#endif
}
void DrawSubtractiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID)
{
    if (alpha > 0xFF)
        alpha = 0xFF;
#if RETRO_USING_KOS
    // No native PVR subtractive mode; approximate with alpha blend so it at
    // least layers correctly on hardware (subtractive sprites are rare).
    if (DC_UseHWSprites()) {
        DC_QueueSpriteInk(sheetID, XPos, YPos, width, height, sprX, sprY, FLIP_NONE, DC_INK_ALPHA, alpha);
        return;
    }
#endif

#if RETRO_SOFTWARE_RENDER
    if (width + XPos > GFX_LINESIZE)
        width = GFX_LINESIZE - XPos;
    if (XPos < 0) {
        sprX -= XPos;
        width += XPos;
        XPos = 0;
    }
    if (height + YPos > SCREEN_YSIZE)
        height = SCREEN_YSIZE - YPos;
    if (YPos < 0) {
        sprY -= YPos;
        height += YPos;
        YPos = 0;
    }
    if (width <= 0 || height <= 0 || alpha <= 0)
        return;

    ushort *subBlendTable  = &subtractLookupTable[0x20 * alpha];
    GFXSurface *surface    = &gfxSurface[sheetID];
    int pitch              = GFX_LINESIZE - width;
    int gfxPitch           = surface->width - width;
    byte *lineBuffer       = &gfxLineBuffer[YPos];
    byte *gfxData          = &graphicData[sprX + surface->width * sprY + surface->dataPosition];
    ushort *frameBufferPtr = &Engine.frameBuffer[XPos + GFX_LINESIZE * YPos];

    while (height--) {
        activePalette   = fullPalette[*lineBuffer];
        activePalette32 = fullPalette32[*lineBuffer];
        lineBuffer++;
        int w = width;
        while (w--) {
            if (*gfxData > 0) {
                ushort color = activePalette[*gfxData];

                int R = maxVal((*frameBufferPtr & 0xF800) - (subBlendTable[(color & 0xF800) >> 11] << 11), 0);
                int G = maxVal((*frameBufferPtr & 0x7E0) - (subBlendTable[(color & 0x7E0) >> 6] << 6), 0);
                int B = maxVal((*frameBufferPtr & 0x1F) - subBlendTable[color & 0x1F], 0);

                *frameBufferPtr = R | G | B;
            }
            ++gfxData;
            ++frameBufferPtr;
        }
        frameBufferPtr += pitch;
        gfxData += gfxPitch;
    }
#endif
}

void DrawObjectAnimation(void *objScr, void *ent, int XPos, int YPos)
{
    ObjectScript *objectScript = (ObjectScript *)objScr;
    Entity *entity             = (Entity *)ent;
    SpriteAnimation *sprAnim   = &animationList[objectScript->animFile->aniListOffset + entity->animation];
    SpriteFrame *frame         = &animFrames[sprAnim->frameListOffset + entity->frame];
    int rotation               = 0;

    switch (sprAnim->rotationStyle) {
        case ROTSTYLE_NONE:
            switch (entity->direction) {
                case FLIP_NONE:
                    DrawSpriteFlipped(frame->pivotX + XPos, frame->pivotY + YPos, frame->width, frame->height, frame->sprX, frame->sprY, FLIP_NONE,
                                      frame->sheetID);
                    break;

                case FLIP_X:
                    DrawSpriteFlipped(XPos - frame->width - frame->pivotX, frame->pivotY + YPos, frame->width, frame->height, frame->sprX,
                                      frame->sprY, FLIP_X, frame->sheetID);
                    break;
                case FLIP_Y:

                    DrawSpriteFlipped(frame->pivotX + XPos, YPos - frame->height - frame->pivotY, frame->width, frame->height, frame->sprX,
                                      frame->sprY, FLIP_Y, frame->sheetID);
                    break;

                case FLIP_XY:
                    DrawSpriteFlipped(XPos - frame->width - frame->pivotX, YPos - frame->height - frame->pivotY, frame->width, frame->height,
                                      frame->sprX, frame->sprY, FLIP_XY, frame->sheetID);
                    break;

                default: break;
            }
            break;

        case ROTSTYLE_FULL:
            DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width, frame->height,
                              entity->rotation, frame->sheetID);
            break;

        case ROTSTYLE_45DEG:
            if (entity->rotation >= 0x100)
                DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width,
                                  frame->height, 0x200 - ((0x214 - entity->rotation) >> 6 << 6), frame->sheetID);
            else
                DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width,
                                  frame->height, (entity->rotation + 20) >> 6 << 6, frame->sheetID);
            break;

        case ROTSTYLE_STATICFRAMES: {
            if (entity->rotation >= 0x100)
                rotation = 8 - ((532 - entity->rotation) >> 6);
            else
                rotation = (entity->rotation + 20) >> 6;
            int frameID = entity->frame;
            switch (rotation) {
                case 0: // 0 deg
                case 8: // 360 deg
                    rotation = 0x00;
                    break;

                case 1: // 45 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 0;
                    else
                        rotation = 0x80;
                    break;

                case 2: // 90 deg
                    rotation = 0x80;
                    break;

                case 3: // 135 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 0x80;
                    else
                        rotation = 0x100;
                    break;

                case 4: // 180 deg
                    rotation = 0x100;
                    break;

                case 5: // 225 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 0x100;
                    else
                        rotation = 384;
                    break;

                case 6: // 270 deg
                    rotation = 384;
                    break;

                case 7: // 315 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 384;
                    else
                        rotation = 0;
                    break;

                default: break;
            }

            frame = &animFrames[sprAnim->frameListOffset + frameID];
            DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width, frame->height,
                              rotation, frame->sheetID);
            // DrawSpriteRotozoom(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width,
            // frame->height,
            //                  rotation, entity->scale, frame->sheetID);
            break;
        }

        default: break;
    }
}

void DrawFace(void *v, uint color)
{
    Vertex *verts = (Vertex *)v;
    int alpha     = (color & 0x7F000000) >> 23;
    if (alpha < 1)
        return;

    if (alpha > 0xFF)
        alpha = 0xFF;

#if RETRO_USING_KOS
    // Dreamcast: scene3D colored face -> flat PVR quad (special stage pipe).
    // verts are in face winding order a,b,c,d; the PVR triangle strip wants
    // a,b,d,c. 7-bit face alpha -> 8-bit (near-opaque treated as fully opaque).
    {
        const float xs[4] = { (float)verts[0].x, (float)verts[1].x, (float)verts[3].x, (float)verts[2].x };
        const float ys[4] = { (float)verts[0].y, (float)verts[1].y, (float)verts[3].y, (float)verts[2].y };
        uint a = (alpha >= 0xF0) ? 0xFFu : (uint)alpha;
        DC_Queue3DColorQuad(xs, ys, (a << 24) | (color & 0x00FFFFFFu));
    }
    return;
#endif

    if (verts[0].x < 0 && verts[1].x < 0 && verts[2].x < 0 && verts[3].x < 0)
        return;

    if (verts[0].x > GFX_LINESIZE && verts[1].x > GFX_LINESIZE && verts[2].x > GFX_LINESIZE && verts[3].x > GFX_LINESIZE)
        return;

    if (verts[0].y < 0 && verts[1].y < 0 && verts[2].y < 0 && verts[3].y < 0)
        return;

    if (verts[0].y > SCREEN_YSIZE && verts[1].y > SCREEN_YSIZE && verts[2].y > SCREEN_YSIZE && verts[3].y > SCREEN_YSIZE)
        return;

    if (verts[0].x == verts[1].x && verts[1].x == verts[2].x && verts[2].x == verts[3].x)
        return;

    if (verts[0].y == verts[1].y && verts[1].y == verts[2].y && verts[2].y == verts[3].y)
        return;

#if RETRO_SOFTWARE_RENDER
    int vertexA = 0;
    int vertexB = 1;
    int vertexC = 2;
    int vertexD = 3;
    if (verts[1].y < verts[0].y) {
        vertexA = 1;
        vertexB = 0;
    }
    if (verts[2].y < verts[vertexA].y) {
        int temp = vertexA;
        vertexA  = 2;
        vertexC  = temp;
    }
    if (verts[3].y < verts[vertexA].y) {
        int temp = vertexA;
        vertexA  = 3;
        vertexD  = temp;
    }
    if (verts[vertexC].y < verts[vertexB].y) {
        int temp = vertexB;
        vertexB  = vertexC;
        vertexC  = temp;
    }
    if (verts[vertexD].y < verts[vertexB].y) {
        int temp = vertexB;
        vertexB  = vertexD;
        vertexD  = temp;
    }
    if (verts[vertexD].y < verts[vertexC].y) {
        int temp = vertexC;
        vertexC  = vertexD;
        vertexD  = temp;
    }

    int faceTop    = verts[vertexA].y;
    int faceBottom = verts[vertexD].y;
    if (faceTop < 0)
        faceTop = 0;
    if (faceBottom > SCREEN_YSIZE)
        faceBottom = SCREEN_YSIZE;
    for (int i = faceTop; i < faceBottom; ++i) {
        faceLineStart[i] = 100000;
        faceLineEnd[i]   = -100000;
    }

    ProcessScanEdge(&verts[vertexA], &verts[vertexB]);
    ProcessScanEdge(&verts[vertexA], &verts[vertexC]);
    ProcessScanEdge(&verts[vertexA], &verts[vertexD]);
    ProcessScanEdge(&verts[vertexB], &verts[vertexC]);
    ProcessScanEdge(&verts[vertexC], &verts[vertexD]);
    ProcessScanEdge(&verts[vertexB], &verts[vertexD]);

    ushort color16 = PACK_RGB888(((color >> 16) & 0xFF), ((color >> 8) & 0xFF), ((color >> 0) & 0xFF));

    ushort *frameBufferPtr = &Engine.frameBuffer[GFX_LINESIZE * faceTop];
    if (alpha == 255) {
        while (faceTop < faceBottom) {
            int startX = faceLineStart[faceTop];
            int endX   = faceLineEnd[faceTop];
            if (startX >= GFX_LINESIZE || endX <= 0) {
                frameBufferPtr += GFX_LINESIZE;
            }
            else {
                if (startX < 0)
                    startX = 0;
                if (endX > GFX_LINESIZE_MINUSONE)
                    endX = GFX_LINESIZE_MINUSONE;
                ushort *fbPtr = &frameBufferPtr[startX];
                frameBufferPtr += GFX_LINESIZE;
                int vertexwidth = endX - startX + 1;
                while (vertexwidth--) {
                    *fbPtr = color16;
                    ++fbPtr;
                }
            }
            ++faceTop;
        }
    }
    else {
        ushort *fbufferBlend = &blendLookupTable[0x20 * (0xFF - alpha)];
        ushort *pixelBlend   = &blendLookupTable[0x20 * alpha];

        while (faceTop < faceBottom) {
            int startX = faceLineStart[faceTop];
            int endX   = faceLineEnd[faceTop];
            if (startX >= GFX_LINESIZE || endX <= 0) {
                frameBufferPtr += GFX_LINESIZE;
            }
            else {
                if (startX < 0)
                    startX = 0;
                if (endX > GFX_LINESIZE_MINUSONE)
                    endX = GFX_LINESIZE_MINUSONE;
                ushort *fbPtr = &frameBufferPtr[startX];
                frameBufferPtr += GFX_LINESIZE;
                int vertexwidth = endX - startX + 1;
                while (vertexwidth--) {
                    int R = (fbufferBlend[(*fbPtr & 0xF800) >> 11] + pixelBlend[(color16 & 0xF800) >> 11]) << 11;
                    int G = (fbufferBlend[(*fbPtr & 0x7E0) >> 6] + pixelBlend[(color16 & 0x7E0) >> 6]) << 6;
                    int B = fbufferBlend[*fbPtr & 0x1F] + pixelBlend[color16 & 0x1F];

                    *fbPtr = R | G | B;
                    ++fbPtr;
                }
            }
            ++faceTop;
        }
    }
#endif
}
void DrawFadedFace(void *v, uint color, uint fogColor, int alpha)
{
    Vertex *verts = (Vertex *)v;
    if (alpha > 0xFF)
        alpha = 0xFF;

    if (alpha < 1)
        return;

#if RETRO_USING_KOS
    // Dreamcast: scene3D faded face -> flat PVR quad with the fog blend baked
    // in on the CPU (per-face, not per-pixel): result = lerp(fog, color, alpha),
    // opaque. verts winding a,b,c,d -> PVR strip a,b,d,c.
    {
        const uint ia = (uint)(0xFF - alpha);
        const uint fr = (((color >> 16) & 0xFF) * (uint)alpha + ((fogColor >> 16) & 0xFF) * ia) / 0xFF;
        const uint fg = (((color >> 8) & 0xFF) * (uint)alpha + ((fogColor >> 8) & 0xFF) * ia) / 0xFF;
        const uint fb = ((color & 0xFF) * (uint)alpha + (fogColor & 0xFF) * ia) / 0xFF;
        const float xs[4] = { (float)verts[0].x, (float)verts[1].x, (float)verts[3].x, (float)verts[2].x };
        const float ys[4] = { (float)verts[0].y, (float)verts[1].y, (float)verts[3].y, (float)verts[2].y };
        DC_Queue3DColorQuad(xs, ys, 0xFF000000u | (fr << 16) | (fg << 8) | fb);
    }
    return;
#endif

    if (verts[0].x < 0 && verts[1].x < 0 && verts[2].x < 0 && verts[3].x < 0)
        return;

    if (verts[0].x > GFX_LINESIZE && verts[1].x > GFX_LINESIZE && verts[2].x > GFX_LINESIZE && verts[3].x > GFX_LINESIZE)
        return;

    if (verts[0].y < 0 && verts[1].y < 0 && verts[2].y < 0 && verts[3].y < 0)
        return;

    if (verts[0].y > SCREEN_YSIZE && verts[1].y > SCREEN_YSIZE && verts[2].y > SCREEN_YSIZE && verts[3].y > SCREEN_YSIZE)
        return;

    if (verts[0].x == verts[1].x && verts[1].x == verts[2].x && verts[2].x == verts[3].x)
        return;

    if (verts[0].y == verts[1].y && verts[1].y == verts[2].y && verts[2].y == verts[3].y)
        return;

#if RETRO_SOFTWARE_RENDER
    int vertexA = 0;
    int vertexB = 1;
    int vertexC = 2;
    int vertexD = 3;
    if (verts[1].y < verts[0].y) {
        vertexA = 1;
        vertexB = 0;
    }
    if (verts[2].y < verts[vertexA].y) {
        int temp = vertexA;
        vertexA  = 2;
        vertexC  = temp;
    }
    if (verts[3].y < verts[vertexA].y) {
        int temp = vertexA;
        vertexA  = 3;
        vertexD  = temp;
    }
    if (verts[vertexC].y < verts[vertexB].y) {
        int temp = vertexB;
        vertexB  = vertexC;
        vertexC  = temp;
    }
    if (verts[vertexD].y < verts[vertexB].y) {
        int temp = vertexB;
        vertexB  = vertexD;
        vertexD  = temp;
    }
    if (verts[vertexD].y < verts[vertexC].y) {
        int temp = vertexC;
        vertexC  = vertexD;
        vertexD  = temp;
    }

    int faceTop    = verts[vertexA].y;
    int faceBottom = verts[vertexD].y;
    if (faceTop < 0)
        faceTop = 0;
    if (faceBottom > SCREEN_YSIZE)
        faceBottom = SCREEN_YSIZE;
    for (int i = faceTop; i < faceBottom; ++i) {
        faceLineStart[i] = 100000;
        faceLineEnd[i]   = -100000;
    }

    ProcessScanEdge(&verts[vertexA], &verts[vertexB]);
    ProcessScanEdge(&verts[vertexA], &verts[vertexC]);
    ProcessScanEdge(&verts[vertexA], &verts[vertexD]);
    ProcessScanEdge(&verts[vertexB], &verts[vertexC]);
    ProcessScanEdge(&verts[vertexC], &verts[vertexD]);
    ProcessScanEdge(&verts[vertexB], &verts[vertexD]);

    ushort color16    = PACK_RGB888(((color >> 16) & 0xFF), ((color >> 8) & 0xFF), ((color >> 0) & 0xFF));
    ushort fogColor16 = PACK_RGB888(((fogColor >> 16) & 0xFF), ((fogColor >> 8) & 0xFF), ((fogColor >> 0) & 0xFF));

    ushort *frameBufferPtr = &Engine.frameBuffer[GFX_LINESIZE * faceTop];
    ushort *fbufferBlend   = &blendLookupTable[0x20 * (0xFF - alpha)];
    ushort *pixelBlend     = &blendLookupTable[0x20 * alpha];

    while (faceTop < faceBottom) {
        int startX = faceLineStart[faceTop];
        int endX   = faceLineEnd[faceTop];
        if (startX >= GFX_LINESIZE || endX <= 0) {
            frameBufferPtr += GFX_LINESIZE;
        }
        else {
            if (startX < 0)
                startX = 0;
            if (endX > GFX_LINESIZE_MINUSONE)
                endX = GFX_LINESIZE_MINUSONE;
            ushort *fbPtr = &frameBufferPtr[startX];
            frameBufferPtr += GFX_LINESIZE;
            int vertexwidth = endX - startX + 1;
            while (vertexwidth--) {
                int R = (fbufferBlend[(fogColor16 & 0xF800) >> 11] + pixelBlend[(color16 & 0xF800) >> 11]) << 11;
                int G = (fbufferBlend[(fogColor16 & 0x7E0) >> 6] + pixelBlend[(color16 & 0x7E0) >> 6]) << 6;
                int B = fbufferBlend[fogColor16 & 0x1F] + pixelBlend[color16 & 0x1F];

                *fbPtr = R | G | B;
                ++fbPtr;
            }
        }
        ++faceTop;
    }
#endif
}
void DrawTexturedFace(void *v, byte sheetID)
{
    Vertex *verts = (Vertex *)v;

#if RETRO_USING_KOS
    // Dreamcast: scene3D textured face -> hardware textured quad. The special
    // stage's textured faces (FACE_TEXTURED_C checkpoint bands) are billboards
    // with rectangular UVs and corners already in TL,TR,BL,BR order — exactly
    // what DC_QueueSpriteQuad expects. UVs are raw sheet texels.
    {
        const float xs[4] = { (float)verts[0].x, (float)verts[1].x, (float)verts[2].x, (float)verts[3].x };
        const float ys[4] = { (float)verts[0].y, (float)verts[1].y, (float)verts[2].y, (float)verts[3].y };
        const int sprX = verts[0].u, sprY = verts[0].v;
        const int w = verts[1].u - verts[0].u, h = verts[2].v - verts[0].v;
        DC_QueueSpriteQuad(sheetID, xs, ys, sprX, sprY, w, h, DC_INK_OPAQUE, 0xFF);
    }
    return;
#endif

    if (verts[0].x < 0 && verts[1].x < 0 && verts[2].x < 0 && verts[3].x < 0)
        return;
    if (verts[0].x > GFX_LINESIZE && verts[1].x > GFX_LINESIZE && verts[2].x > GFX_LINESIZE && verts[3].x > GFX_LINESIZE)
        return;
    if (verts[0].y < 0 && verts[1].y < 0 && verts[2].y < 0 && verts[3].y < 0)
        return;
    if (verts[0].y > SCREEN_YSIZE && verts[1].y > SCREEN_YSIZE && verts[2].y > SCREEN_YSIZE && verts[3].y > SCREEN_YSIZE)
        return;
    if (verts[0].x == verts[1].x && verts[1].x == verts[2].x && verts[2].x == verts[3].x)
        return;
    if (verts[0].y == verts[1].y && verts[1].y == verts[2].y && verts[2].y == verts[3].y)
        return;

#if RETRO_SOFTWARE_RENDER
    int vertexA = 0;
    int vertexB = 1;
    int vertexC = 2;
    int vertexD = 3;
    if (verts[1].y < verts[0].y) {
        vertexA = 1;
        vertexB = 0;
    }
    if (verts[2].y < verts[vertexA].y) {
        int temp = vertexA;
        vertexA  = 2;
        vertexC  = temp;
    }
    if (verts[3].y < verts[vertexA].y) {
        int temp = vertexA;
        vertexA  = 3;
        vertexD  = temp;
    }
    if (verts[vertexC].y < verts[vertexB].y) {
        int temp = vertexB;
        vertexB  = vertexC;
        vertexC  = temp;
    }
    if (verts[vertexD].y < verts[vertexB].y) {
        int temp = vertexB;
        vertexB  = vertexD;
        vertexD  = temp;
    }
    if (verts[vertexD].y < verts[vertexC].y) {
        int temp = vertexC;
        vertexC  = vertexD;
        vertexD  = temp;
    }

    int faceTop    = verts[vertexA].y;
    int faceBottom = verts[vertexD].y;
    if (faceTop < 0)
        faceTop = 0;
    if (faceBottom > SCREEN_YSIZE)
        faceBottom = SCREEN_YSIZE;
    for (int i = faceTop; i < faceBottom; ++i) {
        faceLineStart[i] = 100000;
        faceLineEnd[i]   = -100000;
    }

    ProcessScanEdgeUV(&verts[vertexA], &verts[vertexB]);
    ProcessScanEdgeUV(&verts[vertexA], &verts[vertexC]);
    ProcessScanEdgeUV(&verts[vertexA], &verts[vertexD]);
    ProcessScanEdgeUV(&verts[vertexB], &verts[vertexC]);
    ProcessScanEdgeUV(&verts[vertexC], &verts[vertexD]);
    ProcessScanEdgeUV(&verts[vertexB], &verts[vertexD]);

    ushort *frameBufferPtr = &Engine.frameBuffer[GFX_LINESIZE * faceTop];
    byte *sheetPtr         = &graphicData[gfxSurface[sheetID].dataPosition];
    int shiftwidth         = gfxSurface[sheetID].widthShift;
    byte *lineBuffer       = &gfxLineBuffer[faceTop];
    while (faceTop < faceBottom) {
        activePalette   = fullPalette[*lineBuffer];
        activePalette32 = fullPalette32[*lineBuffer];
        lineBuffer++;
        int startX = faceLineStart[faceTop];
        int endX   = faceLineEnd[faceTop];
        int UPos   = faceLineStartU[faceTop];
        int VPos   = faceLineStartV[faceTop];
        if (startX >= GFX_LINESIZE || endX <= 0) {
            frameBufferPtr += GFX_LINESIZE;
        }
        else {
            int posDifference = endX - startX;
            int bufferedUPos  = 0;
            int bufferedVPos  = 0;
            if (endX == startX) {
                bufferedUPos = 0;
                bufferedVPos = 0;
            }
            else {
                bufferedUPos = (faceLineEndU[faceTop] - UPos) / posDifference;
                bufferedVPos = (faceLineEndV[faceTop] - VPos) / posDifference;
            }
            if (endX > GFX_LINESIZE_MINUSONE)
                posDifference = GFX_LINESIZE_MINUSONE - startX;
            if (startX < 0) {
                posDifference += startX;
                UPos -= startX * bufferedUPos;
                VPos -= startX * bufferedVPos;
                startX = 0;
            }
            ushort *fbPtr = &frameBufferPtr[startX];
            frameBufferPtr += GFX_LINESIZE;

            int counter = posDifference + 1;

            while (counter--) {
                if (UPos < 0)
                    UPos = 0;
                if (VPos < 0)
                    VPos = 0;
                ushort index = sheetPtr[(VPos >> 16 << shiftwidth) + (UPos >> 16)];
                if (index > 0)
                    *fbPtr = activePalette[index];
                fbPtr++;
                UPos += bufferedUPos;
                VPos += bufferedVPos;
            }
        }
        ++faceTop;
    }
#endif
}
void DrawTexturedFaceBlended(void *v, byte sheetID)
{
    Vertex *verts = (Vertex *)v;

#if RETRO_USING_KOS
    // Dreamcast: same billboard mapping as DrawTexturedFace, alpha-blended.
    {
        const float xs[4] = { (float)verts[0].x, (float)verts[1].x, (float)verts[2].x, (float)verts[3].x };
        const float ys[4] = { (float)verts[0].y, (float)verts[1].y, (float)verts[2].y, (float)verts[3].y };
        const int sprX = verts[0].u, sprY = verts[0].v;
        const int w = verts[1].u - verts[0].u, h = verts[2].v - verts[0].v;
        DC_QueueSpriteQuad(sheetID, xs, ys, sprX, sprY, w, h, DC_INK_ALPHA, 0x80);
    }
    return;
#endif

    if (verts[0].x < 0 && verts[1].x < 0 && verts[2].x < 0 && verts[3].x < 0)
        return;

    if (verts[0].x > GFX_LINESIZE && verts[1].x > GFX_LINESIZE && verts[2].x > GFX_LINESIZE && verts[3].x > GFX_LINESIZE)
        return;

    if (verts[0].y < 0 && verts[1].y < 0 && verts[2].y < 0 && verts[3].y < 0)
        return;

    if (verts[0].y > SCREEN_YSIZE && verts[1].y > SCREEN_YSIZE && verts[2].y > SCREEN_YSIZE && verts[3].y > SCREEN_YSIZE)
        return;

    if (verts[0].x == verts[1].x && verts[1].x == verts[2].x && verts[2].x == verts[3].x)
        return;

    if (verts[0].y == verts[1].y && verts[1].y == verts[2].y && verts[2].y == verts[3].y)
        return;

#if RETRO_SOFTWARE_RENDER
    int vertexA = 0;
    int vertexB = 1;
    int vertexC = 2;
    int vertexD = 3;
    if (verts[1].y < verts[0].y) {
        vertexA = 1;
        vertexB = 0;
    }
    if (verts[2].y < verts[vertexA].y) {
        int temp = vertexA;
        vertexA  = 2;
        vertexC  = temp;
    }
    if (verts[3].y < verts[vertexA].y) {
        int temp = vertexA;
        vertexA  = 3;
        vertexD  = temp;
    }
    if (verts[vertexC].y < verts[vertexB].y) {
        int temp = vertexB;
        vertexB  = vertexC;
        vertexC  = temp;
    }
    if (verts[vertexD].y < verts[vertexB].y) {
        int temp = vertexB;
        vertexB  = vertexD;
        vertexD  = temp;
    }
    if (verts[vertexD].y < verts[vertexC].y) {
        int temp = vertexC;
        vertexC  = vertexD;
        vertexD  = temp;
    }

    int faceTop    = verts[vertexA].y;
    int faceBottom = verts[vertexD].y;
    if (faceTop < 0)
        faceTop = 0;
    if (faceBottom > SCREEN_YSIZE)
        faceBottom = SCREEN_YSIZE;
    for (int i = faceTop; i < faceBottom; ++i) {
        faceLineStart[i] = 100000;
        faceLineEnd[i]   = -100000;
    }

    ProcessScanEdgeUV(&verts[vertexA], &verts[vertexB]);
    ProcessScanEdgeUV(&verts[vertexA], &verts[vertexC]);
    ProcessScanEdgeUV(&verts[vertexA], &verts[vertexD]);
    ProcessScanEdgeUV(&verts[vertexB], &verts[vertexC]);
    ProcessScanEdgeUV(&verts[vertexC], &verts[vertexD]);
    ProcessScanEdgeUV(&verts[vertexB], &verts[vertexD]);

    ushort *frameBufferPtr = &Engine.frameBuffer[GFX_LINESIZE * faceTop];
    byte *sheetPtr         = &graphicData[gfxSurface[sheetID].dataPosition];
    int shiftwidth         = gfxSurface[sheetID].widthShift;
    byte *lineBuffer       = &gfxLineBuffer[faceTop];
    while (faceTop < faceBottom) {
        activePalette   = fullPalette[*lineBuffer];
        activePalette32 = fullPalette32[*lineBuffer];
        lineBuffer++;
        int startX = faceLineStart[faceTop];
        int endX   = faceLineEnd[faceTop];
        int UPos   = faceLineStartU[faceTop];
        int VPos   = faceLineStartV[faceTop];
        if (startX >= GFX_LINESIZE || endX <= 0) {
            frameBufferPtr += GFX_LINESIZE;
        }
        else {
            int posDifference = endX - startX;
            int bufferedUPos  = 0;
            int bufferedVPos  = 0;
            if (endX == startX) {
                bufferedUPos = 0;
                bufferedVPos = 0;
            }
            else {
                bufferedUPos = (faceLineEndU[faceTop] - UPos) / posDifference;
                bufferedVPos = (faceLineEndV[faceTop] - VPos) / posDifference;
            }
            if (endX > GFX_LINESIZE_MINUSONE)
                posDifference = GFX_LINESIZE_MINUSONE - startX;
            if (startX < 0) {
                posDifference += startX;
                UPos -= startX * bufferedUPos;
                VPos -= startX * bufferedVPos;
                startX = 0;
            }
            ushort *fbPtr = &frameBufferPtr[startX];
            frameBufferPtr += GFX_LINESIZE;
#if RETRO_REV02
            int counter = posDifference;
#else
            int counter = posDifference + 1;
#endif
            while (counter--) {
                if (UPos < 0)
                    UPos = 0;
                if (VPos < 0)
                    VPos = 0;
                ushort index = sheetPtr[(VPos >> 16 << shiftwidth) + (UPos >> 16)];
                if (index > 0)
                    *fbPtr = ((activePalette[index] & 0xF7BC) >> 1) + ((*fbPtr & 0xF7BC) >> 1);
                fbPtr++;
                UPos += bufferedUPos;
                VPos += bufferedVPos;
            }
        }
        ++faceTop;
    }
#endif
}

#if !RETRO_REV02
void DrawBitmapText(void *menu, int XPos, int YPos, int scale, int spacing, int rowStart, int rowCount)
{
    TextMenu *tMenu = (TextMenu *)menu;
    int Y           = YPos << 9;
    if (rowCount < 0)
        rowCount = tMenu->rowCount;
    if (rowStart + rowCount > tMenu->rowCount)
        rowCount = tMenu->rowCount - rowStart;

    while (rowCount > 0) {
        int X = XPos << 9;
        for (int i = 0; i < tMenu->entrySize[rowStart]; ++i) {
            ushort c             = tMenu->textData[tMenu->entryStart[rowStart] + i];
            FontCharacter *fChar = &fontCharacterList[c];
#if RETRO_SOFTWARE_RENDER
            DrawSpriteScaled(FLIP_NONE, X >> 9, Y >> 9, -fChar->pivotX, -fChar->pivotY, scale, scale, fChar->width, fChar->height, fChar->srcX,
                             fChar->srcY, textMenuSurfaceNo);
#endif
            X += fChar->xAdvance * scale;
        }
        Y += spacing * scale;
        rowStart++;
        rowCount--;
    }
}
#endif

void DrawTextMenuEntry(void *menu, int rowID, int XPos, int YPos, int textHighlight)
{
    TextMenu *tMenu = (TextMenu *)menu;
    int id          = tMenu->entryStart[rowID];
    for (int i = 0; i < tMenu->entrySize[rowID]; ++i) {
        DrawSprite(XPos + (i << 3) - (((tMenu->entrySize[rowID] % 2) & (tMenu->alignment == 2)) * 4), YPos, 8, 8, ((tMenu->textData[id] & 0xF) << 3),
                   ((tMenu->textData[id] >> 4) << 3) + textHighlight, textMenuSurfaceNo);
        id++;
    }
}
void DrawStageTextEntry(void *menu, int rowID, int XPos, int YPos, int textHighlight)
{
    TextMenu *tMenu = (TextMenu *)menu;
    int id          = tMenu->entryStart[rowID];
    for (int i = 0; i < tMenu->entrySize[rowID]; ++i) {
        if (i == tMenu->entrySize[rowID] - 1) {
            DrawSprite(XPos + (i << 3), YPos, 8, 8, ((tMenu->textData[id] & 0xF) << 3), ((tMenu->textData[id] >> 4) << 3), textMenuSurfaceNo);
        }
        else {
            DrawSprite(XPos + (i << 3), YPos, 8, 8, ((tMenu->textData[id] & 0xF) << 3), ((tMenu->textData[id] >> 4) << 3) + textHighlight,
                       textMenuSurfaceNo);
        }
        id++;
    }
}
void DrawBlendedTextMenuEntry(void *menu, int rowID, int XPos, int YPos, int textHighlight)
{
    TextMenu *tMenu = (TextMenu *)menu;
    int id          = tMenu->entryStart[rowID];
    for (int i = 0; i < tMenu->entrySize[rowID]; ++i) {
        DrawBlendedSprite(XPos + (i << 3), YPos, 8, 8, ((tMenu->textData[id] & 0xF) << 3), ((tMenu->textData[id] >> 4) << 3) + textHighlight,
                          textMenuSurfaceNo);
        id++;
    }
}
void DrawTextMenu(void *menu, int XPos, int YPos)
{
    TextMenu *tMenu = (TextMenu *)menu;
    int cnt         = 0;

    if (tMenu->visibleRowCount > 0) {
        cnt = (int)(tMenu->visibleRowCount + tMenu->visibleRowOffset);
    }
    else {
        tMenu->visibleRowOffset = 0;
        cnt                     = (int)tMenu->rowCount;
    }

    if (tMenu->selectionCount == 3) {
        tMenu->selection2 = -1;
        for (int i = 0; i <= tMenu->selection1; ++i) {
            if (tMenu->entryHighlight[i]) {
                tMenu->selection2 = i;
            }
        }
    }

    switch (tMenu->alignment) {
        case 0:
            for (int i = (int)tMenu->visibleRowOffset; i < cnt; ++i) {
                switch (tMenu->selectionCount) {
                    case 1:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 0);
                        break;

                    case 2:
                        if (i == tMenu->selection1 || i == tMenu->selection2)
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 0);
                        break;

                    case 3:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 0);

                        if (i == tMenu->selection2 && i != tMenu->selection1)
                            DrawStageTextEntry(tMenu, i, XPos, YPos, 128);
                        break;
                }
                YPos += 8;
            }
            break;

        case 1:
            for (int i = (int)tMenu->visibleRowOffset; i < cnt; ++i) {
                int entryX = XPos - (tMenu->entrySize[i] << 3);
                switch (tMenu->selectionCount) {
                    case 1:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 0);
                        break;

                    case 2:
                        if (i == tMenu->selection1 || i == tMenu->selection2)
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 0);
                        break;

                    case 3:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 0);

                        if (i == tMenu->selection2 && i != tMenu->selection1)
                            DrawStageTextEntry(tMenu, i, entryX, YPos, 128);
                        break;
                }
                YPos += 8;
            }
            break;

        case 2:
            for (int i = (int)tMenu->visibleRowOffset; i < cnt; ++i) {
                int entryX = XPos - (tMenu->entrySize[i] >> 1 << 3);
                switch (tMenu->selectionCount) {
                    case 1:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 0);
                        break;
                    case 2:
                        if (i == tMenu->selection1 || i == tMenu->selection2)
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 0);
                        break;
                    case 3:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 0);

                        if (i == tMenu->selection2 && i != tMenu->selection1)
                            DrawStageTextEntry(tMenu, i, entryX, YPos, 128);
                        break;
                }
                YPos += 8;
            }
            break;

        default: break;
    }
}
