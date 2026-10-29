#include "RetroEngine.hpp"

void FadeScreen_Create(void *objPtr)
{
    RSDK_THIS(FadeScreen);
    self->timer             = 0.0f;
    self->delay             = 1.5f;
    self->fadeSpeed         = 2.0f;
    self->state             = FADESCREEN_STATE_GAMEFADEOUT;
    Engine.nativeMenuFadeIn = true;
}
void FadeScreen_Main(void *objPtr)
{
    RSDK_THIS(FadeScreen);

    SetRenderBlendMode(RENDER_BLEND_ALPHA);
    // Keep the overlay coordinate calculation out of SH-4 double-precision
    // mode (GCC 14.3 can otherwise schedule FNEG before restoring FPSCR.PR).
    self->timer += self->fadeSpeed * (float)Engine.deltaTime;
    switch (self->state) {
        case FADESCREEN_STATE_MENUFADEIN:
            self->fadeA = ((self->delay - self->timer) * 256.0f);
            RenderRect(-SCREEN_CENTERX_F, SCREEN_CENTERY, 160.0f, SCREEN_XSIZE_F, SCREEN_YSIZE_F, self->fadeR, self->fadeG, self->fadeB, self->fadeA);
            if (self->timer > self->delay) {
                RemoveNativeObject(self);
                Engine.nativeMenuFadeIn = false;
                SetMusicTrack("MainMenu.ogg", 0, true, 106596);
                PlayMusic(0, 0);
            }
            break;

        case FADESCREEN_STATE_FADEOUT:
            self->fadeA = self->timer * 256.0f;
            RenderRect(-SCREEN_CENTERX_F, SCREEN_CENTERY_F, 160.0f, SCREEN_XSIZE_F, SCREEN_YSIZE_F, self->fadeR, self->fadeG, self->fadeB,
                       self->fadeA);
            if (self->timer > self->delay)
                RemoveNativeObject(self);
            break;

        case FADESCREEN_STATE_GAMEFADEOUT:
            self->fadeA = self->timer * 256.0f;
            RenderRect(-SCREEN_CENTERX_F, SCREEN_CENTERY_F, 160.0f, SCREEN_XSIZE_F, SCREEN_YSIZE_F, self->fadeR, self->fadeG, self->fadeB,
                       self->fadeA);
            SetMusicVolume(masterVolume - 2);

            if (self->timer > self->delay) {
                ClearNativeObjects();
                CREATE_ENTITY(RetroGameLoop);
                if (Engine.gameDeviceType == RETRO_MOBILE)
                    CREATE_ENTITY(VirtualDPad);
            }
            break;

#if !RETRO_USE_ORIGINAL_CODE
        case FADESCREEN_STATE_FADEIN:
            self->fadeA = ((self->delay - self->timer) * 256.0f);
            RenderRect(-SCREEN_CENTERX_F, SCREEN_CENTERY_F, 160.0f, SCREEN_XSIZE_F, SCREEN_YSIZE_F, self->fadeR, self->fadeG, self->fadeB,
                       self->fadeA);
            if (self->timer > self->delay) {
                RemoveNativeObject(self);
                Engine.nativeMenuFadeIn = false;
            }
            break;
#endif
    }

#if !RETRO_USE_ORIGINAL_CODE
    NewRenderState();
    MatrixScaleXYZF(&self->render, Engine.windowScale, Engine.windowScale, 1.0f);
    MatrixTranslateXYZF(&self->temp, 0.0f, 0.0f, 160.0f);
    MatrixMultiplyF(&self->render, &self->temp);
    SetRenderMatrix(&self->render);
#endif
}
