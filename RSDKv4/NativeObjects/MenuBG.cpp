#include "RetroEngine.hpp"

void MenuBG_Create(void *objPtr)
{
    RSDK_THIS(MenuBG);
    self->textureID = LoadTexture("Data/Game/Menu/BG1.png", TEXFMT_RGBA5551);
    self->bgCircle1 = LoadMesh("Data/Game/Models/BGCircle1.bin", -1);
    self->bgCircle2 = LoadMesh("Data/Game/Models/BGCircle2.bin", -1);
    self->bgLines   = LoadMesh("Data/Game/Models/BGLines.bin", -1);
    SetMeshAnimation(self->bgLines, &self->animator, 0, 40, 0.0f);
    self->animator.loopAnimation = true;
    self->fadeR                  = -96;
    self->fadeG                  = -64;
    self->fadeB                  = -8;
    self->isFading               = true;
    SetMeshVertexColors(self->bgCircle1, 0xE0, 0xD0, 0xC0, 0xFF);
    SetMeshVertexColors(self->bgCircle2, 0xE0, 0xD0, 0xC0, 0xFF);
    SetMeshVertexColors(self->bgLines, 0xE0, 0, 0, 0xFF);
    self->Ztrans1      = -32.0f;
    self->Ztrans2      = -64.0f;
    self->ZtransRender = -128.0f;
}
void MenuBG_Main(void *objPtr)
{
    RSDK_THIS(MenuBG);
    // Keep SH-4 animation arithmetic in single precision. Mixing doubles here
    // lets GCC 14.3 schedule FNEG before it restores FPSCR.PR, flipping the
    // backdrop's x coordinate and leaving the menu background black.
    const float deltaTime = (float)Engine.deltaTime;
    if (self->isFading) {
        SetRenderBlendMode(RENDER_BLEND_NONE);
        self->fadeTimer += deltaTime;
        if (self->fadeTimer > 1.5f)
            self->isFading = false;

        if (self->fadeR < 0xF8)
            self->fadeR += 8;

        if (self->fadeG < 0xF8)
            self->fadeG += 8;

        if (self->fadeB < 0xF8)
            self->fadeB += 8;

        if (self->alpha < 0x100)
            self->alpha += 0x10;

        self->Ztrans1      = ((160.0f - self->Ztrans1) / (16.0f * deltaTime * 60.0f)) + self->Ztrans1;
        self->Ztrans2      = ((160.0f - self->Ztrans2) / (18.0f * deltaTime * 60.0f)) + self->Ztrans2;
        self->ZtransRender = ((160.0f - self->ZtransRender) / (deltaTime * 60.0f * 20.0f)) + self->ZtransRender;
        RenderRect(-SCREEN_CENTERX_F, SCREEN_CENTERY_F, 160.0f, SCREEN_XSIZE_F, SCREEN_YSIZE_F, self->fadeR, self->fadeG, self->fadeB, 255);
        SetRenderBlendMode(RENDER_BLEND_ALPHA);
        SetRenderVertexColor(224, 208, 192);
        RenderImage(-64.0f, 0.0f, 160.0f, 0.45f, 0.45f, 256.0f, 256.0f, 512.0f, 512.0f, 0.0f, 0.0f, self->alpha, self->textureID);
        SetRenderVertexColor(255, 255, 255);
        SetRenderBlendMode(RENDER_BLEND_NONE);
        self->circle1Rot = self->circle1Rot - deltaTime;
        if (self->circle1Rot < -(float)M_PI_2)
            self->circle1Rot += (float)M_PI_2;
        self->circle2Rot += deltaTime;
        if (self->circle2Rot > (float)M_PI_2)
            self->circle2Rot -= (float)M_PI_2;
        NewRenderState();
        MatrixRotateZF(&self->circle1, self->circle1Rot);
        MatrixTranslateXYZF(&self->matrixTemp, 120.0f, 94.0f, self->Ztrans1);
        MatrixMultiplyF(&self->circle1, &self->matrixTemp);
        SetRenderMatrix(&self->circle1);
        RenderMesh(self->bgCircle1, MESH_COLORS, false);

        MatrixRotateZF(&self->circle2, self->circle2Rot);
        MatrixTranslateXYZF(&self->matrixTemp, 4.0f, 150.0f, self->Ztrans2);
        MatrixMultiplyF(&self->circle2, &self->matrixTemp);
        SetRenderMatrix(&self->circle2);
        RenderMesh(self->bgCircle2, MESH_COLORS, false);

        MatrixTranslateXYZF(&self->renderMatrix, 0.0f, 0.0f, self->ZtransRender);
    }
    else {
        SetRenderBlendMode(RENDER_BLEND_NONE);
        RenderRect(-SCREEN_CENTERX_F, SCREEN_CENTERY_F, 160.0f, SCREEN_XSIZE_F, SCREEN_YSIZE_F, 248, 248, 248, 255);
        SetRenderBlendMode(RENDER_BLEND_ALPHA);
        SetRenderVertexColor(0xE0, 0xD0, 0xC0);
        RenderImage(-64.0f, 0.0f, 160.0f, 0.45f, 0.45f, 256.0f, 256.0f, 512.0f, 512.0f, 0.0f, 0.0f, 255, self->textureID);
        SetRenderVertexColor(0xFF, 0xFF, 0xFF);
        SetRenderBlendMode(RENDER_BLEND_NONE);
        self->circle1Rot -= deltaTime;
        if (self->circle1Rot < -(float)M_PI_2)
            self->circle1Rot += (float)M_PI_2;
        self->circle2Rot += deltaTime;
        if (self->circle2Rot > (float)M_PI_2)
            self->circle2Rot -= (float)M_PI_2;
        NewRenderState();
        MatrixRotateZF(&self->circle1, self->circle1Rot);
        MatrixTranslateXYZF(&self->matrixTemp, 120.0f, 94.0f, 160.0f);
        MatrixMultiplyF(&self->circle1, &self->matrixTemp);
        SetRenderMatrix(&self->circle1);
        RenderMesh(self->bgCircle1, MESH_COLORS, false);

        MatrixRotateZF(&self->circle2, self->circle2Rot);
        MatrixTranslateXYZF(&self->matrixTemp, 4.0f, 150.0f, 160.0f);
        MatrixMultiplyF(&self->circle2, &self->matrixTemp);
        SetRenderMatrix(&self->circle2);
        RenderMesh(self->bgCircle2, MESH_COLORS, false);

        MatrixTranslateXYZF(&self->renderMatrix, 0.0f, 0.0f, 160.0f);
    }
    SetRenderMatrix(&self->renderMatrix);
    self->animator.animationSpeed = 8.0f * deltaTime;
    AnimateMesh(self->bgLines, &self->animator);
    RenderMesh(self->bgLines, MESH_COLORS, false);
    SetRenderMatrix(NULL);
}
