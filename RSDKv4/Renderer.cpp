#include "RetroEngine.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#if RETRO_USING_KOS
#include "Dreamcast/DCNative.hpp"

// Meshes and stage sfx are the port's two big heap tenants and between them they
// want more than there is. Scene.cpp frees the menu's models at the top of each
// stage load so the sfx arena can have that space, and ReleaseStageSfx gives the
// arena back on the way out of a stage so the models can have it again. That
// hand-off covers the normal path, but it depends on the engine passing through
// those points in the expected order — so LoadMesh keeps a backstop of its own.
// If a mesh allocation fails outright, take the arena back by force and try once
// more: silent stage sfx for the rest of a stage is a far better failure than a
// menu drawing BIOS ROM as geometry, which is what the unchecked mallocs here
// used to produce (address 0 on DC is ROM — writes vanish, reads succeed).
static void *dcMeshAlloc(size_t bytes, const char *what, const char *filePath)
{
    void *p = malloc(bytes);
    if (p)
        return p;

    if (DC_SfxArenaEmergencyRelease()) {
        p = malloc(bytes);
        PrintLog("LoadMesh: reclaimed the stage-sfx arena for %s in '%s' - %s", what, filePath, p ? "it fits now" : "still short");
    }
    return p;
}
#define RETRO_MESH_ALLOC(bytes, what, path) dcMeshAlloc((size_t)(bytes), (what), (path))
#else
#define RETRO_MESH_ALLOC(bytes, what, path) malloc((size_t)(bytes))
#endif

float retroVertexList[40];
float screenBufferVertexList[40];

int vertexListSize = 0;
DrawVertex drawVertexList[DRAWVERTEX_COUNT];
ushort drawIndexList[DRAWINDEX_COUNT];

byte vertexR = 0xFF;
byte vertexG = 0xFF;
byte vertexB = 0xFF;

TextureInfo textureList[TEXTURE_COUNT];
MeshInfo meshList[MESH_COUNT];

#if RETRO_USING_KOS
// Dreamcast: a stage load evicts the native menu's model payloads but leaves the
// slots themselves alone (see EvictMeshData below for why that distinction is
// the whole point). These three run alongside meshList and hold what an evicted
// slot needs in order to become itself again.
static bool meshEvicted[MESH_COUNT];   // payload freed, slot still spoken for
static bool meshNoReload[MESH_COUNT];  // a refill failed - stop asking the disc
static uint meshEvictColor[MESH_COUNT]; // the vertex colour it was wearing (RGBA)

// Refills granted per frame on the lazy path. The texture side allows four; two
// is right here because a model is a much larger read than a tile texture and
// there are only a handful of them - the menu's fade-in covers the couple of
// frames it takes for all of them to come back.
#define DC_MESH_RELOAD_PER_FRAME (2)
static int meshReloadBudget = DC_MESH_RELOAD_PER_FRAME;

static inline int MeshSlotOf(const MeshInfo *mesh)
{
    if (mesh < &meshList[0] || mesh >= &meshList[MESH_COUNT])
        return -1;
    return (int)(mesh - &meshList[0]);
}
#endif

int renderStateCount = 0;
RenderState renderStateList[RENDERSTATE_COUNT];
RenderState currentRenderState;

void SetIdentityMatrixF(MatrixF *matrix)
{
    matrix->values[0][0] = 1.0f;
    matrix->values[0][1] = 0.0f;
    matrix->values[0][2] = 0.0f;
    matrix->values[0][3] = 0.0f;
    matrix->values[1][0] = 0.0f;
    matrix->values[1][1] = 1.0f;
    matrix->values[1][2] = 0.0f;
    matrix->values[1][3] = 0.0f;
    matrix->values[2][0] = 0.0f;
    matrix->values[2][1] = 0.0f;
    matrix->values[2][2] = 1.0f;
    matrix->values[2][3] = 0.0f;
    matrix->values[3][0] = 0.0f;
    matrix->values[3][1] = 0.0f;
    matrix->values[3][2] = 0.0f;
    matrix->values[3][3] = 1.0f;
}
void MatrixMultiplyF(MatrixF *matrixA, MatrixF *matrixB)
{
    float output[16];

    for (int i = 0; i < 0x10; ++i) {
        uint rowA = i / 4;
        uint rowB = i % 4;
        output[i] = (matrixA->values[rowA][3] * matrixB->values[3][rowB]) + (matrixA->values[rowA][2] * matrixB->values[2][rowB])
                    + (matrixA->values[rowA][1] * matrixB->values[1][rowB]) + (matrixA->values[rowA][0] * matrixB->values[0][rowB]);
    }

    for (int i = 0; i < 0x10; ++i) matrixA->values[i / 4][i % 4] = output[i];
}
void MatrixTranslateXYZF(MatrixF *matrix, float x, float y, float z)
{
    matrix->values[0][0] = 1.0f;
    matrix->values[0][1] = 0.0f;
    matrix->values[0][2] = 0.0f;
    matrix->values[0][3] = 0.0f;
    matrix->values[1][0] = 0.0f;
    matrix->values[1][1] = 1.0f;
    matrix->values[1][2] = 0.0f;
    matrix->values[1][3] = 0.0f;
    matrix->values[2][0] = 0.0f;
    matrix->values[2][1] = 0.0f;
    matrix->values[2][2] = 1.0f;
    matrix->values[2][3] = 0.0f;
    matrix->values[3][0] = x;
    matrix->values[3][1] = y;
    matrix->values[3][2] = z;
    matrix->values[3][3] = 1.0f;
}
void MatrixScaleXYZF(MatrixF *matrix, float scaleX, float scaleY, float scaleZ)
{
    matrix->values[0][0] = scaleX;
    matrix->values[0][1] = 0.0f;
    matrix->values[0][2] = 0.0f;
    matrix->values[0][3] = 0.0f;
    matrix->values[1][0] = 0.0f;
    matrix->values[1][1] = scaleY;
    matrix->values[1][2] = 0.0f;
    matrix->values[1][3] = 0.0f;
    matrix->values[2][0] = 0.0f;
    matrix->values[2][1] = 0.0f;
    matrix->values[2][2] = scaleZ;
    matrix->values[2][3] = 0.0f;
    matrix->values[3][0] = 0.0f;
    matrix->values[3][1] = 0.0f;
    matrix->values[3][2] = 0.0f;
    matrix->values[3][3] = 1.0f;
}
void MatrixRotateXF(MatrixF *matrix, float angle)
{
    float sine           = sinf(angle);
    float cosine         = cosf(angle);
    matrix->values[0][0] = 1.0f;
    matrix->values[0][1] = 0.0f;
    matrix->values[0][2] = 0.0f;
    matrix->values[0][3] = 0.0f;
    matrix->values[1][0] = 0.0f;
    matrix->values[1][1] = cosine;
    matrix->values[1][2] = sine;
    matrix->values[1][3] = 0.0f;
    matrix->values[2][0] = 0.0f;
    matrix->values[2][1] = -sine;
    matrix->values[2][2] = cosine;
    matrix->values[2][3] = 0.0f;
    matrix->values[3][0] = 0.0f;
    matrix->values[3][1] = 0.0f;
    matrix->values[3][2] = 0.0f;
    matrix->values[3][3] = 1.0f;
}
void MatrixRotateYF(MatrixF *matrix, float angle)
{
    float sine           = sinf(angle);
    float cosine         = cosf(angle);
    matrix->values[0][0] = cosine;
    matrix->values[0][1] = 0.0f;
    matrix->values[0][2] = sine;
    matrix->values[0][3] = 0.0f;
    matrix->values[1][0] = 0.0f;
    matrix->values[1][1] = 1.0f;
    matrix->values[1][2] = 0.0f;
    matrix->values[1][3] = 0.0f;
    matrix->values[2][0] = -sine;
    matrix->values[2][1] = 0.0f;
    matrix->values[2][2] = cosine;
    matrix->values[2][3] = 0.0f;
    matrix->values[3][0] = 0.0f;
    matrix->values[3][1] = 0.0f;
    matrix->values[3][2] = 0.0f;
    matrix->values[3][3] = 1.0f;
}
void MatrixRotateZF(MatrixF *matrix, float angle)
{
    float sine   = sinf(angle);
    float cosine = cosf(angle);

    matrix->values[0][0] = cosine;
    matrix->values[0][1] = -sine;
    matrix->values[0][2] = 0.0;
    matrix->values[0][3] = 0.0;
    matrix->values[1][0] = sine;
    matrix->values[1][1] = cosine;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][0] = 0.0;
    matrix->values[2][1] = 0.0;
    matrix->values[2][2] = 1.0;
    matrix->values[2][3] = 0.0;
    matrix->values[3][0] = 0.0;
    matrix->values[3][1] = 0.0;
    matrix->values[3][3] = 1.0;
    matrix->values[3][2] = 0.0;
}
void MatrixRotateXYZF(MatrixF *matrix, float angleX, float angleY, float angleZ)
{
    float sinX = sinf(angleX);
    float cosX = cosf(angleX);
    float sinY = sinf(angleY);
    float cosY = cosf(angleY);
    float sinZ = sinf(angleZ);
    float cosZ = cosf(angleZ);

    matrix->values[0][0] = (cosZ * cosY) + (sinZ * (sinY * sinX));
    matrix->values[0][1] = (sinZ * cosY) - (cosZ * (sinY * sinX));
    matrix->values[0][2] = sinY * cosX;
    matrix->values[0][3] = 0.0f;
    matrix->values[1][0] = sinZ * -cosX;
    matrix->values[1][1] = cosZ * cosX;
    matrix->values[1][2] = sinX;
    matrix->values[1][3] = 0.0f;
    matrix->values[2][0] = (sinZ * (cosY * sinX)) - (cosZ * sinY);
    matrix->values[2][1] = (sinZ * -sinY) - (cosZ * (cosY * sinX));
    matrix->values[2][2] = cosY * cosX;
    matrix->values[2][3] = 0.0f;
    matrix->values[3][0] = 0.0f;
    matrix->values[3][1] = 0.0f;
    matrix->values[3][2] = 0.0f;
    matrix->values[3][3] = 1.0f;
}

void MatrixInvertF(MatrixF *dstMatrix, MatrixF *matrix)
{
    double inv[16], det;
    double m[16];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            m[(y << 2) + x] = matrix->values[y][x];
        }
    }

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];

    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];

    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];

    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];

    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];

    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];

    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];

    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];

    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];

    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];

    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];

    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];

    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (det == 0)
        return;

    det = 1.0 / det;

    for (int i = 0; i < 0x10; ++i) dstMatrix->values[i / 4][i % 4] = inv[i] * det;
}

// Render States
void ResetRenderStates()
{
    currentRenderState.renderMatrix = NULL;
    currentRenderState.vertPtr      = drawVertexList;
    currentRenderState.indexCount   = 0;
    currentRenderState.id           = 0;
    currentRenderState.blendMode    = 0;
    currentRenderState.useColors    = false;
    currentRenderState.useTexture   = false;
    currentRenderState.depthTest    = false;
    currentRenderState.useNormals   = false;
    currentRenderState.useFilter    = false;
    currentRenderState.indexPtr     = drawIndexList;
    renderStateCount                = -1;
    vertexListSize                  = 0;
    vertexR                         = 0xFF;
    vertexG                         = 0xFF;
    vertexB                         = 0xFF;
}
void SetRenderBlendMode(byte mode)
{
    if (currentRenderState.blendMode != mode && currentRenderState.indexCount) {
        RenderState *state = &renderStateList[renderStateCount++];
        memcpy(state, &currentRenderState, sizeof(RenderState));

        currentRenderState.indexCount = 0;
        currentRenderState.id         = 0;
        currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
    }
    currentRenderState.blendMode = mode;
}
void SetRenderVertexColor(byte r, byte g, byte b)
{
    vertexR = r;
    vertexG = g;
    vertexB = b;
}

#undef near
#undef far
void SetPerspectiveMatrix(float w, float h, float near, float far)
{
    float m[19];

    float val = tanf((float)(0.017453292f * w) * 0.5f);
    m[11]     = 1.0;
    m[0]      = 1.0 / val;
    m[1]      = 0.0;
    m[2]      = 0.0;
    m[3]      = 0.0;
    m[4]      = 0.0;
    m[6]      = 0.0;
    m[7]      = 0.0;
    m[8]      = 0.0;
    m[9]      = 0.0;
    m[12]     = 0.0;
    m[13]     = 0.0;
    m[15]     = 0.0;
    m[5]      = 1.0 / (val * h);
    m[10]     = (far + near) / (far - near);
    m[14]     = -((far + far) * near) / (far - near);
#if RETRO_USING_OPENGL
    glMultMatrixf(m);
#endif
#if RETRO_USING_KOS
    // No GL matrix stack on DC — hand the projection to the PVR native
    // renderer's software vertex pipeline (see DCNative.cpp).
    DC_NativeSetProjection(m);
#endif
}
void SetupDrawIndexList()
{
    int index = 0;
    for (int i = 0; i < DRAWINDEX_COUNT;) {
        drawIndexList[i + 2] = index + 0;
        drawIndexList[i + 1] = index + 1;
        drawIndexList[i + 0] = index + 2;
        drawIndexList[i + 5] = index + 1;
        drawIndexList[i + 4] = index + 3;
        drawIndexList[i + 3] = index + 2;
        index += 4;
        i += 6;
    }
}
void SetRenderMatrix(MatrixF *matrix) { currentRenderState.renderMatrix = matrix; }
void NewRenderState()
{
    if (renderStateCount < RENDERSTATE_COUNT) {
        if (currentRenderState.indexCount) {
            RenderState *state = &renderStateList[renderStateCount];
            memcpy(state, &currentRenderState, sizeof(RenderState));

            currentRenderState.id         = 0;
            currentRenderState.indexCount = 0;
            currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
            currentRenderState.indexPtr   = drawIndexList;
            renderStateCount++;
        }
    }
}
void RenderScene()
{
#if RETRO_USING_OPENGL
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#endif
    if (renderStateCount == -1)
        return;

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

    if (dimAmount < 1.0) {
        SetRenderBlendMode(RENDER_BLEND_ALPHA);
        RenderRect(-SCREEN_CENTERX_F, SCREEN_CENTERY_F, 160.0, SCREEN_XSIZE_F, SCREEN_YSIZE_F, 0, 0, 0, 0xFF - (dimAmount * 0xFF));
        SetRenderBlendMode(RENDER_BLEND_NONE);
    }
#endif

#if RETRO_USING_OPENGL
    glEnableClientState(GL_VERTEX_ARRAY);
    glLoadIdentity();
#endif
    if (currentRenderState.indexCount) {
        RenderState *state = &renderStateList[renderStateCount];
        memcpy(state, &currentRenderState, sizeof(RenderState));

        currentRenderState.indexCount = 0;
        currentRenderState.id         = 0;
        currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
        renderStateCount++;
    }

    MatrixF *prevMat   = NULL;
    bool prevTextures  = false;
    uint prevTexID     = 0;
    bool prevColors    = false;
    bool prevNormals   = false;
    bool prevDepth     = false;
    byte prevBlendMode = 0;

    for (int i = 0; i < renderStateCount; ++i) {
        RenderState *state = &renderStateList[i];

        if (state->renderMatrix != prevMat) {
            if (state->renderMatrix) {
#if RETRO_USING_OPENGL
                glLoadMatrixf((const GLfloat *)state->renderMatrix);
#endif
                prevMat = state->renderMatrix;
            }
            else {
#if RETRO_USING_OPENGL
                glLoadIdentity();
#endif
                prevMat = NULL;
            }
        }

#if RETRO_USING_OPENGL
        glVertexPointer(3, GL_FLOAT, sizeof(DrawVertex), &state->vertPtr->vertX);
#endif
        if (state->useTexture) {
            if (!prevTextures) {
#if RETRO_USING_OPENGL
                glEnable(GL_TEXTURE_2D);
                glEnableClientState(GL_TEXTURE_COORD_ARRAY);
#endif
            }
#if RETRO_USING_OPENGL
            glTexCoordPointer(2, GL_FLOAT, sizeof(DrawVertex), &state->vertPtr->texCoordX);
#endif
            prevTextures = true;
            if (state->id != prevTexID) {
#if RETRO_USING_OPENGL
                glBindTexture(GL_TEXTURE_2D, state->id);
#endif
                prevTexID = state->id;
            }
        }
        else {
            if (prevTextures) {
#if RETRO_USING_OPENGL
                glDisable(GL_TEXTURE_2D);
                glDisableClientState(GL_TEXTURE_COORD_ARRAY);
#endif
            }
            prevTextures = false;
        }

        if (state->useColors) {
#if RETRO_USING_OPENGL
            if (!prevColors)
                glEnableClientState(GL_COLOR_ARRAY);
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(DrawVertex), &state->vertPtr->r);
#endif
            prevColors = true;
        }
        else {
#if RETRO_USING_OPENGL
            if (prevColors)
                glDisableClientState(GL_COLOR_ARRAY);
#endif
            prevColors = false;
        }

        if (state->useNormals) {
            if (!prevNormals) {
#if RETRO_USING_OPENGL
                glEnableClientState(GL_NORMAL_ARRAY);
                glEnable(GL_LIGHTING);
#endif
            }
#if RETRO_USING_OPENGL
            glNormalPointer(GL_FLOAT, sizeof(DrawVertex), &state->vertPtr->normalX);
#endif
            prevNormals = true;
        }
        else {
            if (prevNormals) {
#if RETRO_USING_OPENGL
                glDisableClientState(GL_NORMAL_ARRAY);
                glDisable(GL_LIGHTING);
#endif
            }
            prevNormals = false;
        }

        if (state->depthTest) {
#if RETRO_USING_OPENGL
            if (!prevDepth)
                glEnable(GL_DEPTH_TEST);
#endif
            prevDepth = true;
        }
        else {
#if RETRO_USING_OPENGL
            if (prevDepth)
                glDisable(GL_DEPTH_TEST);
#endif
            prevDepth = false;
        }

        if (state->blendMode != prevBlendMode) {
            switch (state->blendMode) {
                default: prevBlendMode = state->blendMode; break;
                case 0:
#if RETRO_USING_OPENGL
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    glDisable(GL_BLEND);
#endif
                    prevBlendMode = 0;
                    break;
                case 1:
#if RETRO_USING_OPENGL
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    glEnable(GL_BLEND);
#endif
                    prevBlendMode = 1;
                    break;
                case 2:
#if RETRO_USING_OPENGL
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    glEnable(GL_BLEND);
#endif
                    prevBlendMode = 2;
                    break;
                case 3:
#if RETRO_USING_OPENGL
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    glEnable(GL_BLEND);
#endif
                    prevBlendMode = 3;
                    break;
            }
        }

        if (state->useFilter && mixFiltersOnJekyll) {
#if RETRO_USING_OPENGL
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &defaultFramebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, framebufferHiRes);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, Engine.scalingMode ? GL_LINEAR : GL_NEAREST);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, Engine.scalingMode ? GL_LINEAR : GL_NEAREST);
            glVertexPointer(3, GL_FLOAT, sizeof(DrawVertex), screenBufferVertexList);
            glTexCoordPointer(2, GL_FLOAT, sizeof(DrawVertex), &screenBufferVertexList[6]);
            glViewport(0, 0, GFX_LINESIZE_DOUBLE, SCREEN_YSIZE * 2);
            glPushMatrix();
            glLoadIdentity();
            glMatrixMode(GL_PROJECTION);
            glPushMatrix();
            glLoadIdentity();
            glDrawElements(GL_TRIANGLES, state->indexCount, GL_UNSIGNED_SHORT, state->indexPtr);

            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebuffer);
            glBindTexture(GL_TEXTURE_2D, renderbufferHiRes);
            glVertexPointer(3, GL_FLOAT, sizeof(DrawVertex), state->vertPtr);
            glTexCoordPointer(2, GL_FLOAT, sizeof(DrawVertex), &state->vertPtr->texCoordX);
            glViewport(displaySettings.offsetX, 0, displaySettings.width, displaySettings.height);
            glPopMatrix();
            glMatrixMode(GL_MODELVIEW);
            glPopMatrix();
#endif
        }

#if RETRO_USING_OPENGL
        glDrawElements(GL_TRIANGLES, state->indexCount, GL_UNSIGNED_SHORT, state->indexPtr);
#endif
    }

#if RETRO_USING_OPENGL
    glDisableClientState(GL_VERTEX_ARRAY);
    if (prevTextures)
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (prevColors)
        glDisableClientState(GL_COLOR_ARRAY);
#endif
}

int stb_read_cb(void *user, char *data, int size)
{
    FileRead(data, size);
    return size;
}
void stb_skip_cb(void *user, int n) { FileSkip(n); }
int stb_eof_cb(void *user) { return ReachedEndOfFile(); }

// Textures
int LoadTexture(const char *filePath, int format)
{
    int texID = 0;
    for (int i = 0; i < TEXTURE_COUNT; ++i) {
        if (StrComp(textureList[texID].fileName, filePath))
            return texID;
        if (!StrLength(textureList[texID].fileName))
            break;
        texID++;
    }
    if (texID == TEXTURE_COUNT)
        return 0;

#if RETRO_USING_KOS
    // Prefer a pre-converted native PVR texture (tools/dtex_convert.py) streamed
    // straight to VRAM. stbi_load OOMs on the DC heap for images bigger than
    // ~256x256, so most menu/model PNGs never decoded — the .dtex path avoids
    // decoding entirely. Falls through to stbi if no .dtex is present.
    {
        int dw = 0, dh = 0;
        if (DC_NativeTryLoadDtex(texID, filePath, &dw, &dh)) {
            TextureInfo *texture = &textureList[texID];
            texture->width       = dw;
            texture->height      = dh;
            texture->format      = format;
            StrCopy(texture->fileName, filePath);
            float normalize = 1.0;
            if (FindStringToken(filePath, "@2", 1) > 0)
                normalize = 2.0;
            else if (FindStringToken(filePath, "@1", 1) > 0)
                normalize = 0.5;
            texture->widthN  = normalize / dw;
            texture->heightN = normalize / dh;
            texture->id      = (uint)texID;
            return texID;
        }
    }
#endif

    FileInfo info;
    if (LoadFile(filePath, &info)) {

        stbi_io_callbacks callbacks;
        callbacks.read = stb_read_cb;
        callbacks.skip = stb_skip_cb;
        callbacks.eof  = stb_eof_cb;

        int width     = 0;
        int height    = 0;
        int channels  = 0;
        stbi_uc *data = stbi_load_from_callbacks(&callbacks, NULL, &width, &height, &channels, 4);

        if (width > 0 && height > 0) {
            TextureInfo *texture = &textureList[texID];
            texture->width       = width;
            texture->height      = height;
            texture->format      = format;
            StrCopy(texture->fileName, filePath);

            float normalize = 0;
            if (FindStringToken(fileName, "@2", 1) > 0)
                normalize = 2.0;
            else if (FindStringToken(fileName, "@1", 1) > 0)
                normalize = 0.5;
            else
                normalize = 1.0;
            texture->widthN  = normalize / width;
            texture->heightN = normalize / height;

#if RETRO_USING_OPENGL
            glGenTextures(1, &texture->id);
            glBindTexture(GL_TEXTURE_2D, texture->id);
#endif

            // The per-format pixel conversion below exists only to feed
            // glTexImage2D. On the Dreamcast (no GL) it is pure waste — it
            // malloc's a full-size buffer, converts, and frees it, fragmenting
            // the heap right before the next stbi_load and starving it of the
            // contiguous RAM it needs to decode. That silently dropped most menu
            // textures. Skip it entirely on DC; the KOS hook below uploads to
            // the PVR straight from stb's raw `data`.
#if RETRO_USING_OPENGL
            int id = 0;
            switch (format) {
                default: break;
                case TEXFMT_RGBA4444: {
                    ushort *pixels = (ushort *)malloc(width * height * sizeof(ushort));

                    for (int y = 0; y < height; ++y) {
                        for (int x = 0; x < width; ++x) {
                            int r                   = (data[id++] >> 4) << 12;
                            int g                   = (data[id++] >> 4) << 8;
                            int b                   = (data[id++]) & 0xF0;
                            int a                   = (data[id++] >> 4);
                            pixels[x + (y * width)] = a | r | g | b;
                        }
                    }

#if RETRO_USING_OPENGL
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture->width, texture->height, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, pixels);
                    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glBindTexture(GL_TEXTURE_2D, 0);
#endif

                    free(pixels);
                    break;
                }
                case TEXFMT_RGBA5551: {
                    ushort *pixels = (ushort *)malloc(width * height * sizeof(ushort));

                    for (int y = 0; y < height; ++y) {
                        for (int x = 0; x < width; ++x) {
                            int r                   = data[id++];
                            int g                   = data[id++];
                            int b                   = data[id++];
                            int a                   = data[id++];
                            pixels[x + (y * width)] = RGB888_TO_RGB5551(r, g, b) | (a ? 1 : 0);
                        }
                    }

#if RETRO_USING_OPENGL
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture->width, texture->height, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, pixels);
                    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glBindTexture(GL_TEXTURE_2D, 0);
#endif

                    free(pixels);
                    break;
                }
                case TEXFMT_RGBA8888: {
                    uint *pixels = (uint *)malloc(width * height * sizeof(uint));

                    for (int y = 0; y < height; ++y) {
                        for (int x = 0; x < width; ++x) {
                            int r                   = data[id++];
                            int g                   = data[id++];
                            int b                   = data[id++];
                            int a                   = data[id++];
                            pixels[x + (y * width)] = (a << 24) | (b << 16) | (g << 8) | (r << 0);
                        }
                    }

#if RETRO_USING_OPENGL
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture->width, texture->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glBindTexture(GL_TEXTURE_2D, 0);
#endif

                    free(pixels);
                    break;
                }
            }
#endif // RETRO_USING_OPENGL

#if RETRO_USING_KOS
            // Mirror this menu texture into PVR VRAM and key it by texID so the
            // native renderer can bind it. On DC the GL `id` is unused, so we
            // repurpose it to carry texID into the render state list.
            texture->id = (uint)texID;
            DC_NativeUploadTexture(texID, data, width, height, format);
#endif
        }

        CloseFile();
        stbi_image_free(data);

        return texID;
    }
    return 0;
}
void ReplaceTexture(const char *filePath, int texID)
{
    FileInfo info;
    if (LoadFile(filePath, &info)) {

        stbi_io_callbacks callbacks;
        callbacks.read = stb_read_cb;
        callbacks.skip = stb_skip_cb;
        callbacks.eof  = stb_eof_cb;

        int width     = 0;
        int height    = 0;
        int channels  = 0;
        stbi_uc *data = stbi_load_from_callbacks(&callbacks, NULL, &width, &height, &channels, 4);

        if (width > 0 && height > 0) {
            TextureInfo *texture = &textureList[texID];
            StrCopy(texture->fileName, filePath);

            float normalize = 0;
            if (FindStringToken(fileName, "@2", 1) > 0)
                normalize = 2.0;
            else if (FindStringToken(fileName, "@1", 1) > 0)
                normalize = 0.5;
            else
                normalize = 1.0;
            texture->widthN  = normalize / width;
            texture->heightN = normalize / height;

#if RETRO_USING_OPENGL
            glBindTexture(GL_TEXTURE_2D, texture->id);
#endif

            int id = 0;
            switch (texture->format) {
                default: break;
                case TEXFMT_RGBA4444: {
                    ushort *pixels = (ushort *)malloc(width * height * sizeof(ushort));

                    for (int y = 0; y < height; ++y) {
                        for (int x = 0; x < width; ++x) {
                            int r                   = (data[id++] >> 4) << 12;
                            int g                   = (data[id++] >> 4) << 8;
                            int b                   = (data[id++]) & 0xF0;
                            int a                   = (data[id++] >> 4);
                            pixels[x + (y * width)] = a | r | g | b;
                        }
                    }

#if RETRO_USING_OPENGL
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->width, texture->height, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, pixels);
                    glBindTexture(GL_TEXTURE_2D, 0);
#endif

                    free(pixels);
                    break;
                }
                case TEXFMT_RGBA5551: {
                    ushort *pixels = (ushort *)malloc(width * height * sizeof(ushort));

                    for (int y = 0; y < height; ++y) {
                        for (int x = 0; x < width; ++x) {
                            int r                   = data[id++];
                            int g                   = data[id++];
                            int b                   = data[id++];
                            int a                   = data[id++];
                            pixels[x + (y * width)] = RGB888_TO_RGB5551(r, g, b) | (a ? 1 : 0);
                        }
                    }
#if RETRO_USING_OPENGL
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->width, texture->height, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, pixels);
                    glBindTexture(GL_TEXTURE_2D, 0);
#endif

                    free(pixels);
                    break;
                }
                case TEXFMT_RGBA8888: {
                    uint *pixels = (uint *)malloc(width * height * sizeof(uint));

                    for (int y = 0; y < height; ++y) {
                        for (int x = 0; x < width; ++x) {
                            int r                   = data[id++];
                            int g                   = data[id++];
                            int b                   = data[id++];
                            int a                   = data[id++];
                            pixels[x + (y * width)] = (a << 24) | (b << 16) | (g << 8) | (r << 0);
                        }
                    }

#if RETRO_USING_OPENGL
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->width, texture->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                    glBindTexture(GL_TEXTURE_2D, 0);
#endif

                    free(pixels);
                    break;
                }
            }
        }

        CloseFile();
        stbi_image_free(data);
    }
}
void ClearTextures(bool keepBuffer)
{
    for (int i = (keepBuffer ? 1 : 0); i < TEXTURE_COUNT; ++i) {
#if RETRO_USING_OPENGL
        glDeleteTextures(1, &textureList[i].id);
#endif
        StrCopy(textureList[i].fileName, "");
    }
}

// Meshes
//
// The mesh format is fixed and entirely self-describing, which makes a file's
// own length a checksum on the three counts inside it:
//
//     4                        magic, "R3D\0"
//     2                        vertexCount, V
//     V * 8                    texture coords, two floats per vertex
//     2                        indexCount, I
//     I * 6                    three ushorts per triangle
//     2                        frameCount, F
//     (F > 1 ? F * V : V) * 24 position + normal per vertex, per frame
//
// That holds exactly for every model Sonic 2 ships - 2PVS.bin 16954,
// Achievements.bin 24058, BackArrow.bin 1930, Leaderboards.bin 17858,
// Options.bin 5850, TimeAttack.bin 72326 - so the counts can be checked against
// the file before a single byte is allocated for them.
//
// This is worth doing because a bad read used to sail straight through here. A
// contended GD-ROM read handed back a partly filled buffer, the datapack's
// decryption keystream carried on regardless, and the counts came out as
// plausible nonsense: 255 frames for a 24 KB Achievements.bin, 30248 indices for
// a 5.8 KB Options.bin. Each then asked malloc for several megabytes, got NULL,
// and - because address 0 on the Dreamcast is BIOS ROM, where writes vanish and
// reads succeed - the 2013 menu quietly drew ROM contents as geometry, with no
// crash and nothing in the log but an out-of-memory line for a size that could
// never have been real.
//
// FillFileBuffer no longer ignores short reads (see Reader.hpp), so the counts
// should be right now. The checks stay regardless: they cost a few multiplies,
// they turn any future flavour of bad read into a retry and a log line rather
// than a corrupted menu, and they guarantee no allocation is ever sized from a
// number the file itself contradicts.

enum MeshReadResult {
    MESH_READ_OK      = 0, // loaded
    MESH_READ_MISSING = 1, // not there, or not a mesh - asking again won't help
    MESH_READ_NOMEM   = 2, // the counts were believable, the memory just wasn't there
    MESH_READ_BADDATA = 3, // the counts contradict the file's length - bad read, retry
};

// Releases a slot's geometry and leaves the counts at zero, but says nothing
// about WHICH mesh the slot belongs to - fileName and textureID are untouched.
// Safe to call twice: the pointers are nulled as they go.
static void FreeMeshPayload(MeshInfo *mesh)
{
    if (mesh->vertices)
        free(mesh->vertices);
    if (mesh->indices)
        free(mesh->indices);
    if (mesh->frames)
        free(mesh->frames);

    mesh->vertices    = NULL;
    mesh->indices     = NULL;
    mesh->frames      = NULL;
    mesh->vertexCount = 0;
    mesh->indexCount  = 0;
    mesh->frameCount  = 0;
}

// Releases whatever a half-finished load managed to allocate and puts the slot
// back to empty - identity included, so the next LoadMesh may claim it for a
// different file.
static void ClearMeshSlot(MeshInfo *mesh)
{
    FreeMeshPayload(mesh);
    StrCopy(mesh->fileName, "");

#if RETRO_USING_KOS
    const int slot = MeshSlotOf(mesh);
    if (slot >= 0) {
        meshEvicted[slot]    = false;
        meshNoReload[slot]   = false;
        meshEvictColor[slot] = 0xFFFFFFFF;
    }
#endif
}

// One attempt at reading a mesh off the disc into an empty slot. On anything
// other than MESH_READ_OK the caller clears the slot; nothing is freed in here.
static int ReadMeshFile(MeshInfo *mesh, const char *filePath, byte textureID, bool lastAttempt)
{
    // The slot is empty by contract, so null rather than free - a slot that has
    // been through ClearMeshSlot (or ClearMeshData) holds no live pointers.
    mesh->vertices    = NULL;
    mesh->indices     = NULL;
    mesh->frames      = NULL;
    mesh->vertexCount = 0;
    mesh->indexCount  = 0;
    mesh->frameCount  = 0;

    FileInfo info;
    if (!LoadFile(filePath, &info))
        return MESH_READ_MISSING;

    byte buffer[4];
    FileRead(buffer, 4 * sizeof(byte));
    if (buffer[0] != 'R' || buffer[1] != '3' || buffer[2] != 'D' || buffer[3] != '\0') {
        CloseFile();
        return MESH_READ_MISSING;
    }

    // vfileSize is the mesh's own length in both of LoadFile's paths: the entry
    // size for a file inside Data.rsdk, the file's own size for a loose one.
    // (fileSize, by contrast, is the whole datapack for a packed file.)
    const long long fileBytes = (long long)info.vfileSize;

    StrCopy(mesh->fileName, filePath);
    mesh->textureID = textureID;

    FileRead(buffer, sizeof(ushort));
    mesh->vertexCount = buffer[0] + (buffer[1] << 8);

    long long need = 4 + 2 + (long long)mesh->vertexCount * 8 + 2;
    if (need > fileBytes) {
        PrintLog("LoadMesh: '%s' claims %d vertices, needing %d bytes of a %d byte file - bad read", filePath, mesh->vertexCount, (int)need,
                 (int)fileBytes);
        CloseFile();
        return MESH_READ_BADDATA;
    }

    mesh->vertices = (DrawVertex *)RETRO_MESH_ALLOC(sizeof(DrawVertex) * mesh->vertexCount, "vertices", filePath);
    if (mesh->vertexCount && !mesh->vertices) {
        PrintLog("LoadMesh: OUT OF MEMORY for %d vertices (%d bytes) in '%s'", mesh->vertexCount, (int)(sizeof(DrawVertex) * mesh->vertexCount),
                 filePath);
        CloseFile();
        return MESH_READ_NOMEM;
    }

    for (int v = 0; v < mesh->vertexCount; ++v) {
        float buf = 0;
        FileRead(&buf, sizeof(float));
        mesh->vertices[v].texCoordX = buf;

        FileRead(&buf, sizeof(float));
        mesh->vertices[v].texCoordY = buf;

        mesh->vertices[v].r = 0xFF;
        mesh->vertices[v].g = 0xFF;
        mesh->vertices[v].b = 0xFF;
        mesh->vertices[v].a = 0xFF;
    }

    FileRead(buffer, sizeof(ushort));
    mesh->indexCount = buffer[0] + (buffer[1] << 8);

    need += (long long)mesh->indexCount * 6 + 2;
    if (need > fileBytes) {
        PrintLog("LoadMesh: '%s' claims %d triangles, needing %d bytes of a %d byte file - bad read", filePath, mesh->indexCount, (int)need,
                 (int)fileBytes);
        CloseFile();
        return MESH_READ_BADDATA;
    }

    mesh->indices = (ushort *)RETRO_MESH_ALLOC(sizeof(ushort) * (3 * mesh->indexCount), "indices", filePath);
    if (mesh->indexCount && !mesh->indices) {
        PrintLog("LoadMesh: OUT OF MEMORY for %d indices (%d bytes) in '%s'", 3 * mesh->indexCount,
                 (int)(sizeof(ushort) * (3 * mesh->indexCount)), filePath);
        CloseFile();
        return MESH_READ_NOMEM;
    }

    int id = 0;
    for (int i = 0; i < mesh->indexCount; ++i) {
        FileRead(buffer, sizeof(ushort));
        mesh->indices[id + 2] = buffer[0] + (buffer[1] << 8);

        FileRead(buffer, sizeof(ushort));
        mesh->indices[id + 1] = buffer[0] + (buffer[1] << 8);

        FileRead(buffer, sizeof(ushort));
        mesh->indices[id + 0] = buffer[0] + (buffer[1] << 8);

        id += 3;
    }

    FileRead(buffer, sizeof(ushort));
    mesh->frameCount = buffer[0] + (buffer[1] << 8);

    // The vertex payload is the rest of the file, so this last check is the
    // strict one: the arithmetic should land exactly on the file's length.
    const long long total = need + (mesh->frameCount > 1 ? (long long)mesh->frameCount * mesh->vertexCount * 24 : (long long)mesh->vertexCount * 24);
    if (total > fileBytes) {
        PrintLog("LoadMesh: '%s' claims %d frames of %d verts, needing %d bytes of a %d byte file - bad read", filePath, mesh->frameCount,
                 mesh->vertexCount, (int)total, (int)fileBytes);
        CloseFile();
        return MESH_READ_BADDATA;
    }
    if (total != fileBytes && !lastAttempt) {
        // Fits, but leaves bytes unaccounted for - suspicious rather than fatal,
        // so it is worth another read. The last attempt takes it as it comes.
        PrintLog("LoadMesh: '%s' accounts for %d of %d bytes - reading it again", filePath, (int)total, (int)fileBytes);
        CloseFile();
        return MESH_READ_BADDATA;
    }

    if (mesh->frameCount <= 1) {
        for (int v = 0; v < mesh->vertexCount; ++v) {
            float buf = 0;
            FileRead(&buf, sizeof(float));
            mesh->vertices[v].vertX = buf;

            FileRead(&buf, sizeof(float));
            mesh->vertices[v].vertY = buf;

            FileRead(&buf, sizeof(float));
            mesh->vertices[v].vertZ = buf;

            FileRead(&buf, sizeof(float));
            mesh->vertices[v].normalX = buf;

            FileRead(&buf, sizeof(float));
            mesh->vertices[v].normalY = buf;

            FileRead(&buf, sizeof(float));
            mesh->vertices[v].normalZ = buf;
        }
    }
    else {
        mesh->frames = (MeshVertex *)RETRO_MESH_ALLOC(mesh->frameCount * mesh->vertexCount * sizeof(MeshVertex), "frames", filePath);

        if (!mesh->frames) {
            PrintLog("LoadMesh: OUT OF MEMORY for %d frames x %d verts (%d bytes) in '%s'", mesh->frameCount, mesh->vertexCount,
                     (int)(mesh->frameCount * mesh->vertexCount * sizeof(MeshVertex)), filePath);
            CloseFile();
            return MESH_READ_NOMEM;
        }

        for (int f = 0; f < mesh->frameCount; ++f) {
            int frameOff = (f * mesh->vertexCount);
            for (int v = 0; v < mesh->vertexCount; ++v) {
                float buf = 0;
                FileRead(&buf, sizeof(float));
                mesh->frames[frameOff + v].vertX = buf;

                FileRead(&buf, sizeof(float));
                mesh->frames[frameOff + v].vertY = buf;

                FileRead(&buf, sizeof(float));
                mesh->frames[frameOff + v].vertZ = buf;

                FileRead(&buf, sizeof(float));
                mesh->frames[frameOff + v].normalX = buf;

                FileRead(&buf, sizeof(float));
                mesh->frames[frameOff + v].normalY = buf;

                FileRead(&buf, sizeof(float));
                mesh->frames[frameOff + v].normalZ = buf;
            }
        }
    }

    CloseFile();
    return MESH_READ_OK;
}

#if RETRO_USING_KOS
// Reads an evicted slot's geometry back off the disc, IN PLACE. The slot index
// must not move: entities that came back through RestoreNativeObjects still
// hold a MeshInfo* straight into this array, so the mesh has to reappear at the
// address they are already pointing at.
//
// A refill that fails keeps the slot's NAME (so no other file can take it out
// from under those pointers) and marks it, so a menu that is missing a model
// costs one failed read rather than one per frame forever.
static bool RefillMeshSlot(MeshInfo *mesh)
{
    const int slot = MeshSlotOf(mesh);
    if (slot < 0 || meshNoReload[slot] || !StrLength(mesh->fileName))
        return false;

    char filePath[0x40];
    StrCopy(filePath, mesh->fileName);
    const byte textureID = mesh->textureID;

    const int attempts = 3;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        int result = ReadMeshFile(mesh, filePath, textureID, attempt == attempts);

        if (result == MESH_READ_OK) {
            meshEvicted[slot] = false;

            // Vertex colours are set once, in the owning object's Create event -
            // MenuBG tints its circles 0xE0D0C0, SubMenuButton tints its buttons.
            // Coming back from a stage no Create event runs, so a slot that was
            // refilled straight off the disc would come back plain white. Put
            // back whatever it was wearing when it was evicted.
            const uint c = meshEvictColor[slot];
            SetMeshVertexColors(mesh, (byte)(c >> 24), (byte)(c >> 16), (byte)(c >> 8), (byte)c);

            // An animated mesh keeps its geometry in frames[]; vertices[] only
            // receives positions when AnimateMesh interpolates a pair of them,
            // which happens after this. Seed it from frame 0 so the first frame
            // back does not draw whatever malloc happened to hand over.
            if (mesh->frameCount > 1 && mesh->frames && mesh->vertices) {
                for (int v = 0; v < mesh->vertexCount; ++v) {
                    mesh->vertices[v].vertX   = mesh->frames[v].vertX;
                    mesh->vertices[v].vertY   = mesh->frames[v].vertY;
                    mesh->vertices[v].vertZ   = mesh->frames[v].vertZ;
                    mesh->vertices[v].normalX = mesh->frames[v].normalX;
                    mesh->vertices[v].normalY = mesh->frames[v].normalY;
                    mesh->vertices[v].normalZ = mesh->frames[v].normalZ;
                }
            }
            return true;
        }

        // Note FreeMeshPayload, not ClearMeshSlot: the slot keeps its identity
        // even across a failed read.
        FreeMeshPayload(mesh);
        StrCopy(mesh->fileName, filePath);
        mesh->textureID = textureID;

        if (result != MESH_READ_BADDATA)
            break;

        if (attempt < attempts)
            PrintLog("LoadMesh: re-reading '%s', attempt %d of %d", filePath, attempt + 1, attempts);
    }

    PrintLog("LoadMesh: could not restore '%s' after a stage load - it will not be drawn", filePath);
    meshNoReload[slot] = true;
    return false;
}

// Dreamcast stage load. Frees the native menu's model geometry - some 620KB
// between Intro, JPBox, the carousel icons and the rest - while KEEPING every
// slot's fileName and textureID.
//
// That distinction is the entire fix. This used to call ClearMeshData(), on the
// argument that meshList is a static array so a MeshInfo* held by a native
// entity stays a valid pointer, an emptied slot draws nothing, and LoadMesh
// refills it when the menu comes back. The first two are true. The third is
// where it fell apart: ClearMeshSlot wipes fileName, and LoadMesh allocates by
// scanning from slot 0 to the first nameless one. So after a stage the pause
// menu's own Create events - BGCircle1, BGCircle2, BGLines, Button, ButtonH,
// Panel - refilled the list from 0 up in THEIR request order, which is not the
// order the main menu had built. Exiting to the main menu then ran
// RestoreNativeObjects(), a raw memcpy of the entity bank with no Create events,
// so those entities came back holding their ORIGINAL pointers: MenuBG's
// bgCircle1 still pointed at slot 3, which by then held Button.bin. The menu
// came back drawing buttons where its background circles belong and its line
// mesh through a cartridge's transform.
//
// This is the same trap DC_NativeResetTextures already documents on the texture
// side, and the same answer: free the payload, keep the bookkeeping, and let the
// slot be filled back in - at the same index - when something asks to draw it.
void EvictMeshData()
{
    for (int i = 0; i < MESH_COUNT; ++i) {
        MeshInfo *mesh = &meshList[i];
        if (!StrLength(mesh->fileName) || meshEvicted[i])
            continue;

        uint c = 0xFFFFFFFF;
        if (mesh->vertexCount && mesh->vertices)
            c = ((uint)mesh->vertices[0].r << 24) | ((uint)mesh->vertices[0].g << 16) | ((uint)mesh->vertices[0].b << 8) | (uint)mesh->vertices[0].a;

        meshEvictColor[i] = c;
        meshEvicted[i]    = true;
        meshNoReload[i]   = false;

        FreeMeshPayload(mesh);
    }
}

// Once per frame, from the DC flip - alongside DC_NativeReloadEvicted, which
// does the same job for textures. Everything the menu draws gets its geometry
// back within a few frames of being asked for, without a stage load's worth of
// reads landing on one of them.
void ResetEvictedMeshBudget() { meshReloadBudget = DC_MESH_RELOAD_PER_FRAME; }
#endif

MeshInfo *LoadMesh(const char *filePath, byte textureID)
{
    int meshID = 0;
    for (int i = 0; i < MESH_COUNT; ++i) {
        if (StrComp(meshList[meshID].fileName, filePath) && meshList[meshID].textureID == textureID) {
#if RETRO_USING_KOS
            // Right slot, no geometry: a stage load evicted it. Fill it back in
            // here rather than handing back an empty mesh - this is the path the
            // pause menu takes, and it must land in THIS slot, not a fresh one.
            // Not budgeted: an explicit LoadMesh is a demand load.
            if (meshEvicted[meshID])
                RefillMeshSlot(&meshList[meshID]);
#endif
            return &meshList[meshID];
        }
        if (!StrLength(meshList[meshID].fileName))
            break;
        meshID++;
    }
    if (meshID == MESH_COUNT)
        return 0;

    MeshInfo *mesh = &meshList[meshID];

#if RETRO_USING_KOS
    // A fresh claim on an empty slot: nothing about a previous tenant applies.
    meshEvicted[meshID]    = false;
    meshNoReload[meshID]   = false;
    meshEvictColor[meshID] = 0xFFFFFFFF;
#endif

    // Three goes at it. A read that comes back wrong is a timing accident - the
    // disc is being read from two threads - so asking again usually gets the
    // real bytes, and a mesh that loads on the second try is worth far more than
    // a menu built out of null pointers.
    const int attempts = 3;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        int result = ReadMeshFile(mesh, filePath, textureID, attempt == attempts);
        if (result == MESH_READ_OK)
            return mesh;

        ClearMeshSlot(mesh);

        // Missing, not a mesh, or genuinely short of memory: nothing a retry fixes.
        if (result != MESH_READ_BADDATA)
            return NULL;

        if (attempt < attempts)
            PrintLog("LoadMesh: re-reading '%s', attempt %d of %d", filePath, attempt + 1, attempts);
    }

    PrintLog("LoadMesh: gave up on '%s' after %d attempts", filePath, attempts);
    return NULL;
}
void ClearMeshData()
{
    for (int i = 0; i < MESH_COUNT; ++i) {
        MeshInfo *mesh = &meshList[i];
        if (StrLength(mesh->fileName))
            ClearMeshSlot(mesh);
    }
}
void SetMeshAnimation(MeshInfo *mesh, MeshAnimator *animator, ushort frameID, ushort frameCount, float speed)
{
    animator->frameCount = frameCount;
    if (frameCount >= mesh->frameCount)
        animator->frameCount = mesh->frameCount - 1;
    if (frameID < mesh->frameCount) {
        animator->loopIndex = frameID;
        animator->frameID   = frameID;
    }
    else {
        animator->loopIndex = 0;
        animator->frameID   = 0;
    }
    animator->animationSpeed = speed;
}
void AnimateMesh(MeshInfo *mesh, MeshAnimator *animator)
{
    if (mesh->frameCount > 1) {
        if (!animator->animationFinished) {
            animator->animationTimer += animator->animationSpeed;

            while (animator->animationTimer > 1.0f) { // new frame (forwards)
                animator->animationTimer -= 1.0f;
                animator->frameID++;

                if (animator->loopAnimation) {
                    if (animator->frameID >= animator->frameCount)
                        animator->frameID = animator->loopIndex;
                }
                else if (animator->frameID >= animator->frameCount) {
                    animator->frameID           = animator->frameCount;
                    animator->animationFinished = true;
                    animator->animationTimer    = 0.0f;
                }
            }
            while (animator->animationTimer < 0.0f) { // new frame (backwards)
                animator->animationTimer += 1.0f;
                animator->frameID--;

                if (animator->frameID < animator->loopIndex || animator->frameID >= animator->frameCount) {
                    if (animator->loopAnimation) {
                        animator->frameID = animator->frameCount;
                    }
                    else {
                        animator->frameID           = animator->loopIndex;
                        animator->animationTimer    = 0.0f;
                        animator->animationFinished = true;
                    }
                }
            }

            ushort frameID   = animator->frameID;
            ushort nextFrame = animator->frameID + 1;
            if (nextFrame >= animator->frameCount && animator->animationSpeed >= 0)
                nextFrame = animator->loopIndex;
            if (frameID >= animator->frameCount && animator->animationSpeed < 0)
                frameID = animator->loopIndex;

            float interp2 = animator->animationTimer;
            float interp  = 1.0 - animator->animationTimer;

            MeshVertex *vert     = &mesh->frames[frameID * mesh->vertexCount];
            MeshVertex *nextVert = &mesh->frames[nextFrame * mesh->vertexCount];
            for (int v = 0; v < mesh->vertexCount; ++v) {
                mesh->vertices[v].vertX   = (vert->vertX * interp) + (nextVert->vertX * interp2);
                mesh->vertices[v].vertY   = (vert->vertY * interp) + (nextVert->vertY * interp2);
                mesh->vertices[v].vertZ   = (vert->vertZ * interp) + (nextVert->vertZ * interp2);
                mesh->vertices[v].normalX = (vert->normalX * interp) + (nextVert->normalX * interp2);
                mesh->vertices[v].normalY = (vert->normalY * interp) + (nextVert->normalY * interp2);
                mesh->vertices[v].normalZ = (vert->normalZ * interp) + (nextVert->normalZ * interp2);

                vert++;
                nextVert++;
            }
        }
        else if (animator->loopAnimation)
            animator->animationFinished = false;
    }
}

void SetMeshVertexColors(MeshInfo *mesh, byte r, byte g, byte b, byte a)
{
    for (int v = 0; v < mesh->vertexCount; ++v) {
        mesh->vertices[v].r = r;
        mesh->vertices[v].g = g;
        mesh->vertices[v].b = b;
        mesh->vertices[v].a = a;
    }
}

// Rendering
void TransferRetroBuffer()
{
#if RETRO_USING_OPENGL
    glBindTexture(GL_TEXTURE_2D, textureList[0].id);
    if (convertTo32Bit) {
        ushort *frameBufferPtr = Engine.frameBuffer;
        uint *texBufferPtr     = Engine.texBuffer;
        for (int y = 0; y < SCREEN_YSIZE; ++y) {
            for (int x = 0; x < GFX_LINESIZE; ++x) {
                texBufferPtr[x] = gfxPalette16to32[frameBufferPtr[x]];
            }
            texBufferPtr += GFX_LINESIZE;
            frameBufferPtr += GFX_LINESIZE;
        }

        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GFX_LINESIZE, SCREEN_YSIZE, GL_RGBA, GL_UNSIGNED_BYTE, Engine.texBuffer);
    }
    else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GFX_LINESIZE, SCREEN_YSIZE, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, Engine.frameBuffer);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
#endif
}
void RenderRetroBuffer(int alpha, float z)
{
    if (vertexListSize < DRAWVERTEX_COUNT && textureList[0].format) {
        if (renderStateCount < 0 || currentRenderState.id != textureList[0].id) {
            if (renderStateCount >= 0) {
                RenderState *state = &renderStateList[renderStateCount];
                memcpy(state, &currentRenderState, sizeof(RenderState));
            }
            currentRenderState.indexCount = 0;
            currentRenderState.id         = textureList[0].id;
            currentRenderState.useColors  = true;
            currentRenderState.useTexture = true;
            currentRenderState.useFilter  = true;
            currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
            currentRenderState.indexPtr   = drawIndexList;
            renderStateCount++;
        }

        if (renderStateCount < RENDERSTATE_COUNT) {
            int a = 0;
            if (alpha >= 0)
                a = alpha;
            if (a > 0xFF)
                a = 0xFF;

            DrawVertex *vertex1 = &drawVertexList[vertexListSize];
            vertex1->vertX      = retroVertexList[0];
            vertex1->vertY      = retroVertexList[1];
            vertex1->vertZ      = z;
            vertex1->texCoordX  = retroVertexList[6];
            vertex1->texCoordY  = retroVertexList[7];
            vertex1->r          = vertexR;
            vertex1->g          = vertexG;
            vertex1->b          = vertexB;
            vertex1->a          = a;

            DrawVertex *vertex2 = &drawVertexList[vertexListSize + 1];
            vertex2->vertX      = retroVertexList[9];
            vertex2->vertY      = retroVertexList[10];
            vertex2->vertZ      = z;
            vertex2->texCoordX  = retroVertexList[15];
            vertex2->texCoordY  = retroVertexList[16];
            vertex2->r          = vertexR;
            vertex2->g          = vertexG;
            vertex2->b          = vertexB;
            vertex2->a          = a;

            DrawVertex *vertex3 = &drawVertexList[vertexListSize + 2];
            vertex3->vertX      = retroVertexList[18];
            vertex3->vertY      = retroVertexList[19];
            vertex3->vertZ      = z;
            vertex3->texCoordX  = retroVertexList[24];
            vertex3->texCoordY  = retroVertexList[25];
            vertex3->r          = vertexR;
            vertex3->b          = vertexB;
            vertex3->g          = vertexG;
            vertex3->a          = a;

            DrawVertex *vertex4 = &drawVertexList[vertexListSize + 3];
            vertex4->vertX      = retroVertexList[27];
            vertex4->vertY      = retroVertexList[28];
            vertex4->vertZ      = z;
            vertex4->texCoordX  = retroVertexList[33];
            vertex4->texCoordY  = retroVertexList[34];
            vertex4->r          = vertexR;
            vertex4->g          = vertexG;
            vertex4->b          = vertexB;
            vertex4->a          = a;

            currentRenderState.indexCount += 6;
            vertexListSize += 4;
        }
    }
}

void RenderImage(float x, float y, float z, float scaleX, float scaleY, float pivotX, float pivotY, float sprW, float sprH, float sprX, float sprY,
                 int alpha, byte texture)
{
    if (vertexListSize < DRAWVERTEX_COUNT && textureList[texture].format) {
        if (renderStateCount < 0) {
            currentRenderState.indexCount = 0;
            currentRenderState.id         = textureList[texture].id;
            currentRenderState.useColors  = true;
            currentRenderState.useTexture = true;
            currentRenderState.useFilter  = false;
            currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
            currentRenderState.indexPtr   = drawIndexList;
            renderStateCount++;
        }
        else {
            bool flag = false;
            if (currentRenderState.useTexture)
                flag = currentRenderState.id == textureList[texture].id;

            if (!flag) {
                RenderState *state = &renderStateList[renderStateCount];
                memcpy(state, &currentRenderState, sizeof(RenderState));

                currentRenderState.indexCount = 0;
                currentRenderState.id         = textureList[texture].id;
                currentRenderState.useColors  = true;
                currentRenderState.useTexture = true;
                currentRenderState.useFilter  = false;
                currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
                currentRenderState.indexPtr   = drawIndexList;
                renderStateCount++;
            }
        }

        if (renderStateCount < RENDERSTATE_COUNT) {
            int a = 0;
            if (alpha >= 0)
                a = alpha;
            if (a > 0xFF)
                a = 0xFF;

            DrawVertex *vertex1 = &drawVertexList[vertexListSize];
            vertex1->vertX      = x - (pivotX * scaleX);
            vertex1->vertY      = (pivotY * scaleY) + y;
            vertex1->vertZ      = z;
            vertex1->texCoordX  = sprX * textureList[texture].widthN;
            vertex1->texCoordY  = sprY * textureList[texture].heightN;
            vertex1->r          = vertexR;
            vertex1->g          = vertexG;
            vertex1->b          = vertexB;
            vertex1->a          = a;

            DrawVertex *vertex2 = &drawVertexList[vertexListSize + 1];
            vertex2->vertX      = ((sprW - pivotX) * scaleX) + x;
            vertex2->vertY      = vertex1->vertY;
            vertex2->vertZ      = z;
            vertex2->texCoordX  = (sprX + sprW) * textureList[texture].widthN;
            vertex2->texCoordY  = vertex1->texCoordY;
            vertex2->r          = vertexR;
            vertex2->g          = vertexG;
            vertex2->b          = vertexB;
            vertex2->a          = a;

            DrawVertex *vertex3 = &drawVertexList[vertexListSize + 2];
            vertex3->vertX      = vertex1->vertX;
            vertex3->vertY      = y - ((sprH - pivotY) * scaleY);
            vertex3->vertZ      = z;
            vertex3->texCoordX  = vertex1->texCoordX;
            vertex3->texCoordY  = (sprY + sprH) * textureList[texture].heightN;
            vertex3->r          = vertexR;
            vertex3->g          = vertexG;
            vertex3->b          = vertexB;
            vertex3->a          = a;

            DrawVertex *vertex4 = &drawVertexList[vertexListSize + 3];
            vertex4->vertX      = vertex2->vertX;
            vertex4->vertY      = vertex3->vertY;
            vertex4->vertZ      = z;
            vertex4->texCoordX  = vertex2->texCoordX;
            vertex4->texCoordY  = vertex3->texCoordY;
            vertex4->r          = vertexR;
            vertex4->g          = vertexG;
            vertex4->b          = vertexB;
            vertex4->a          = a;
            vertexListSize += 4;
            currentRenderState.indexCount += 6;
        }
    }
}
void RenderImageClipped(float x, float y, float z, float scaleX, float scaleY, float pivotX, float pivotY, float sprW, float sprH, float sprX,
                        float sprY, int alpha, byte texture)
{
    if (vertexListSize < DRAWVERTEX_COUNT && textureList[texture].format) {
        if (renderStateCount < 0) {
            currentRenderState.indexCount = 0;
            currentRenderState.id         = textureList[texture].id;
            currentRenderState.useColors  = true;
            currentRenderState.useTexture = true;
            currentRenderState.useFilter  = false;
            currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
            currentRenderState.indexPtr   = drawIndexList;
            renderStateCount++;
        }
        else {
            bool flag = false;
            if (currentRenderState.useTexture)
                flag = currentRenderState.id == textureList[texture].id;

            if (!flag) {
                RenderState *state = &renderStateList[renderStateCount];
                memcpy(state, &currentRenderState, sizeof(RenderState));

                currentRenderState.indexCount = 0;
                currentRenderState.id         = textureList[texture].id;
                currentRenderState.useColors  = true;
                currentRenderState.useTexture = true;
                currentRenderState.useFilter  = false;
                currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
                currentRenderState.indexPtr   = drawIndexList;
                renderStateCount++;
            }
        }

        if (renderStateCount < RENDERSTATE_COUNT) {
            int a = 0;
            if (alpha >= 0)
                a = alpha;
            if (a > 0xFF)
                a = 0xFF;

            DrawVertex *vertex1 = &drawVertexList[vertexListSize];
            vertex1->vertX      = x - (pivotX * scaleX);
            vertex1->vertY      = (pivotY * scaleY) + y;
            vertex1->vertZ      = z;
            vertex1->texCoordX  = sprX * textureList[texture].widthN;
            vertex1->texCoordY  = sprY * textureList[texture].heightN;
            vertex1->r          = vertexR;
            vertex1->g          = vertexG;
            vertex1->b          = vertexB;
            vertex1->a          = a;
            if (vertex1->vertY > 76.0) {
                vertex1->texCoordY = (((vertex1->vertY - 76.0) / scaleY) + sprY) * textureList[texture].heightN;
                vertex1->vertY     = 76.0;
            }

            DrawVertex *vertex2 = &drawVertexList[vertexListSize + 1];
            vertex2->vertX      = ((sprW - pivotX) * scaleX) + x;
            vertex2->vertY      = vertex1->vertY;
            vertex2->vertZ      = z;
            vertex2->texCoordX  = (sprX + sprW) * textureList[texture].widthN;
            vertex2->texCoordY  = vertex1->texCoordY;
            vertex2->r          = vertexR;
            vertex2->g          = vertexG;
            vertex2->b          = vertexB;
            vertex2->a          = a;

            DrawVertex *vertex3 = &drawVertexList[vertexListSize + 2];
            vertex3->vertX      = vertex1->vertX;
            vertex3->vertY      = y - ((sprH - pivotY) * scaleY);
            vertex3->vertZ      = z;
            vertex3->texCoordX  = vertex1->texCoordX;
            vertex3->texCoordY  = (sprY + sprH) * textureList[texture].heightN;
            vertex3->r          = vertexR;
            vertex3->g          = vertexG;
            vertex3->b          = vertexB;
            vertex3->a          = a;
            if (vertex3->vertY < -76.0) {
                vertex3->texCoordY = (((vertex3->vertY + 76.0) / scaleY) + (sprY + sprH)) * textureList[texture].heightN;
                vertex3->vertY     = -76.0;
            }

            DrawVertex *vertex4 = &drawVertexList[vertexListSize + 3];
            vertex4->vertX      = vertex2->vertX;
            vertex4->vertY      = vertex3->vertY;
            vertex4->vertZ      = z;
            vertex4->texCoordX  = vertex2->texCoordX;
            vertex4->texCoordY  = vertex3->texCoordY;
            vertex4->r          = vertexR;
            vertex4->g          = vertexG;
            vertex4->b          = vertexB;
            vertex4->a          = a;
            vertexListSize += 4;
            currentRenderState.indexCount += 6;
        }
    }
}

void RenderImageFlipH(float x, float y, float z, float scaleX, float scaleY, float pivotX, float pivotY, float sprW, float sprH, float sprX,
                      float sprY, int alpha, byte texture)
{
    if (vertexListSize < DRAWVERTEX_COUNT && textureList[texture].format) {
        if (renderStateCount < 0) {
            currentRenderState.indexCount = 0;
            currentRenderState.id         = textureList[texture].id;
            currentRenderState.useColors  = true;
            currentRenderState.useTexture = true;
            currentRenderState.useFilter  = false;
            currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
            currentRenderState.indexPtr   = drawIndexList;
            renderStateCount++;
        }
        else {
            bool flag = false;
            if (currentRenderState.useTexture)
                flag = currentRenderState.id == textureList[texture].id;

            if (!flag) {
                RenderState *state = &renderStateList[renderStateCount];
                memcpy(state, &currentRenderState, sizeof(RenderState));

                currentRenderState.indexCount = 0;
                currentRenderState.id         = textureList[texture].id;
                currentRenderState.useColors  = true;
                currentRenderState.useTexture = true;
                currentRenderState.useFilter  = false;
                currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
                currentRenderState.indexPtr   = drawIndexList;
                renderStateCount++;
            }
        }

        if (renderStateCount < RENDERSTATE_COUNT) {
            int a = 0;
            if (alpha >= 0)
                a = alpha;
            if (a > 0xFF)
                a = 0xFF;

            DrawVertex *vertex1 = &drawVertexList[vertexListSize];
            vertex1->vertX      = x - (pivotX * scaleX);
            vertex1->vertY      = (pivotY * scaleY) + y;
            vertex1->vertZ      = z;
            vertex1->texCoordX  = (sprX + sprW) * textureList[texture].widthN;
            vertex1->texCoordY  = sprY * textureList[texture].heightN;
            vertex1->r          = vertexR;
            vertex1->g          = vertexG;
            vertex1->b          = vertexB;
            vertex1->a          = a;

            DrawVertex *vertex2 = &drawVertexList[vertexListSize + 1];
            vertex2->vertX      = ((sprW - pivotX) * scaleX) + x;
            vertex2->vertY      = vertex1->vertY;
            vertex2->vertZ      = z;
            vertex2->texCoordX  = sprX * textureList[texture].widthN;
            vertex2->texCoordY  = vertex1->texCoordY;
            vertex2->r          = vertexR;
            vertex2->g          = vertexG;
            vertex2->b          = vertexB;
            vertex2->a          = a;

            DrawVertex *vertex3 = &drawVertexList[vertexListSize + 2];
            vertex3->vertX      = vertex1->vertX;
            vertex3->vertY      = y - ((sprH - pivotY) * scaleY);
            vertex3->vertZ      = z;
            vertex3->texCoordX  = vertex1->texCoordX;
            vertex3->texCoordY  = (sprY + sprH) * textureList[texture].heightN;
            vertex3->r          = vertexR;
            vertex3->g          = vertexG;
            vertex3->b          = vertexB;
            vertex3->a          = a;

            DrawVertex *vertex4 = &drawVertexList[vertexListSize + 3];
            vertex4->vertX      = vertex2->vertX;
            vertex4->vertY      = vertex3->vertY;
            vertex4->vertZ      = z;
            vertex4->texCoordX  = vertex2->texCoordX;
            vertex4->texCoordY  = vertex3->texCoordY;
            vertex4->r          = vertexR;
            vertex4->g          = vertexG;
            vertex4->b          = vertexB;
            vertex4->a          = a;
            vertexListSize += 4;
            currentRenderState.indexCount += 6;
        }
    }
}

void RenderText(ushort *text, int fontID, float x, float y, int z, float scale, int alpha)
{
    BitmapFont *font = &fontList[fontID];
    float posX       = x;
    float posY       = (font->base * scale) + y;

    if (vertexListSize < DRAWVERTEX_COUNT) {
        if (renderStateCount < 0 || (!currentRenderState.useTexture || currentRenderState.useColors)) {
            if (renderStateCount >= 0) {
                RenderState *state = &renderStateList[renderStateCount];
                memcpy(state, &currentRenderState, sizeof(RenderState));
            }
            currentRenderState.indexCount = 0;
            currentRenderState.id         = textureList[font->characters[*text].textureID].id;
            currentRenderState.useColors  = true;
            currentRenderState.useTexture = true;
            currentRenderState.useFilter  = false;
            currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
            currentRenderState.indexPtr   = drawIndexList;
            renderStateCount++;
        }

        if (renderStateCount < RENDERSTATE_COUNT) {
            int a = 0;
            if (alpha >= 0)
                a = alpha;
            if (a > 0xFF)
                a = 0xFF;

            ushort character = *text++;
            while (character && vertexListSize < DRAWVERTEX_COUNT) {
                BitmapFontCharacter *fontChar = &font->characters[character];
                TextureInfo *texture          = &textureList[fontChar->textureID];

                if (texture->format) {
                    if (character == 1) {
                        posX = x;
                        posY -= (font->lineHeight * scale);
                    }
                    else {
                        if (currentRenderState.id != texture->id && renderStateCount < RENDERSTATE_COUNT) {
                            currentRenderState.indexCount = 0;
                            memcpy(&renderStateList[renderStateCount++], &currentRenderState, sizeof(RenderState));
                            currentRenderState.vertPtr  = &drawVertexList[vertexListSize];
                            currentRenderState.indexPtr = drawIndexList;
                            currentRenderState.id       = texture->id;
                        }

                        DrawVertex *vertex1 = &drawVertexList[vertexListSize];
                        vertex1->vertX      = posX + (fontChar->xOffset * scale);
                        vertex1->vertY      = posY - (fontChar->yOffset * scale);
                        vertex1->vertZ      = z;
                        vertex1->texCoordX  = fontChar->x * texture->widthN;
                        vertex1->texCoordY  = fontChar->y * texture->heightN;
                        vertex1->r          = vertexR;
                        vertex1->g          = vertexG;
                        vertex1->b          = vertexB;
                        vertex1->a          = a;

                        DrawVertex *vertex2 = &drawVertexList[vertexListSize + 1];
                        vertex2->vertX      = posX + ((fontChar->width + fontChar->xOffset) * scale);
                        vertex2->vertY      = vertex1->vertY;
                        vertex2->vertZ      = z;
                        vertex2->texCoordX  = (fontChar->x + fontChar->width) * texture->widthN;
                        vertex2->texCoordY  = vertex1->texCoordY;
                        vertex2->r          = vertexR;
                        vertex2->g          = vertexG;
                        vertex2->b          = vertexB;
                        vertex2->a          = a;

                        DrawVertex *vertex3 = &drawVertexList[vertexListSize + 2];
                        vertex3->vertX      = vertex1->vertX;
                        vertex3->vertY      = posY - ((fontChar->height + fontChar->yOffset) * scale);
                        vertex3->vertZ      = z;
                        vertex3->texCoordX  = vertex1->texCoordX;
                        vertex3->texCoordY  = (fontChar->y + fontChar->height) * texture->heightN;
                        vertex3->r          = vertexR;
                        vertex3->g          = vertexG;
                        vertex3->b          = vertexB;
                        vertex3->a          = a;

                        DrawVertex *vertex4 = &drawVertexList[vertexListSize + 3];
                        vertex4->vertX      = vertex2->vertX;
                        vertex4->vertY      = vertex3->vertY;
                        vertex4->vertZ      = z;
                        vertex4->texCoordX  = vertex2->texCoordX;
                        vertex4->texCoordY  = vertex3->texCoordY;
                        vertex4->r          = vertexR;
                        vertex4->g          = vertexG;
                        vertex4->b          = vertexB;
                        vertex4->a          = a;
                        vertexListSize += 4;
                        currentRenderState.indexCount += 6;
                    }
                }
                posX += (fontChar->xAdvance * scale);
                character = *text++;
            }
        }
    }
}
void RenderTextClipped(ushort *text, int fontID, float x, float y, int z, float scale, int alpha)
{
    BitmapFont *font = &fontList[fontID];
    float posX       = x;
    float posY       = (font->base * scale) + y;

    if (vertexListSize < DRAWVERTEX_COUNT) {
        if (renderStateCount < 0 || (!currentRenderState.useTexture || currentRenderState.useColors)) {
            if (renderStateCount >= 0) {
                RenderState *state = &renderStateList[renderStateCount];
                memcpy(state, &currentRenderState, sizeof(RenderState));
            }
            currentRenderState.indexCount = 0;
            currentRenderState.id         = textureList[font->characters[*text].textureID].id;
            currentRenderState.useColors  = true;
            currentRenderState.useTexture = true;
            currentRenderState.useFilter  = false;
            currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
            currentRenderState.indexPtr   = drawIndexList;
            renderStateCount++;
        }

        if (renderStateCount < RENDERSTATE_COUNT) {
            int a = 0;
            if (alpha >= 0)
                a = alpha;
            if (a > 0xFF)
                a = 0xFF;

            ushort character = *text++;
            while (character && vertexListSize < DRAWVERTEX_COUNT) {
                BitmapFontCharacter *fontChar = &font->characters[character];
                TextureInfo *texture          = &textureList[fontChar->textureID];

                if (texture->format) {
                    if (character == 1) {
                        posX = x;
                        posY -= (font->lineHeight * scale);
                    }
                    else {
                        if (currentRenderState.id != texture->id && renderStateCount < RENDERSTATE_COUNT) {
                            currentRenderState.indexCount = 0;
                            memcpy(&renderStateList[renderStateCount++], &currentRenderState, sizeof(RenderState));
                            currentRenderState.vertPtr  = &drawVertexList[vertexListSize];
                            currentRenderState.indexPtr = drawIndexList;
                            currentRenderState.id       = texture->id;
                        }

                        DrawVertex *vertex1 = &drawVertexList[vertexListSize];
                        vertex1->vertX      = posX + (fontChar->xOffset * scale);
                        vertex1->vertY      = posY - (fontChar->yOffset * scale);
                        vertex1->vertZ      = z;
                        vertex1->texCoordX  = fontChar->x * texture->widthN;
                        vertex1->texCoordY  = fontChar->y * texture->heightN;
                        vertex1->r          = vertexR;
                        vertex1->g          = vertexG;
                        vertex1->b          = vertexB;
                        vertex1->a          = a;
                        if (vertex1->vertY > 76.0) {
                            vertex1->texCoordY = (((vertex1->vertY - 76.0) / scale) + fontChar->y) * texture->heightN;
                            vertex1->vertY     = 76.0;
                        }

                        DrawVertex *vertex2 = &drawVertexList[vertexListSize + 1];
                        vertex2->vertX      = posX + ((fontChar->width + fontChar->xOffset) * scale);
                        vertex2->vertY      = vertex1->vertY;
                        vertex2->vertZ      = z;
                        vertex2->texCoordX  = (fontChar->x + fontChar->width) * texture->widthN;
                        vertex2->texCoordY  = vertex1->texCoordY;
                        vertex2->r          = vertexR;
                        vertex2->g          = vertexG;
                        vertex2->b          = vertexB;
                        vertex2->a          = a;

                        DrawVertex *vertex3 = &drawVertexList[vertexListSize + 2];
                        vertex3->vertX      = vertex1->vertX;
                        vertex3->vertY      = posY - ((fontChar->height + fontChar->yOffset) * scale);
                        vertex3->vertZ      = z;
                        vertex3->texCoordX  = vertex1->texCoordX;
                        vertex3->texCoordY  = (fontChar->y + fontChar->height) * texture->heightN;
                        vertex3->r          = vertexR;
                        vertex3->g          = vertexG;
                        vertex3->b          = vertexB;
                        vertex3->a          = a;
                        if (vertex3->vertY < -76.0) {
                            vertex3->texCoordY = (((vertex3->vertY + 76.0) / scale) + (fontChar->y + fontChar->height)) * texture->heightN;
                            vertex3->vertY     = -76.0;
                        }

                        DrawVertex *vertex4 = &drawVertexList[vertexListSize + 3];
                        vertex4->vertX      = vertex2->vertX;
                        vertex4->vertY      = vertex3->vertY;
                        vertex4->vertZ      = z;
                        vertex4->texCoordX  = vertex2->texCoordX;
                        vertex4->texCoordY  = vertex3->texCoordY;
                        vertex4->r          = vertexR;
                        vertex4->g          = vertexG;
                        vertex4->b          = vertexB;
                        vertex4->a          = a;
                        vertexListSize += 4;
                        currentRenderState.indexCount += 6;
                    }
                }
                posX += (fontChar->xAdvance * scale);
                character = *text++;
            }
        }
    }
}

void RenderRect(float x, float y, float z, float w, float h, byte r, byte g, byte b, int alpha)
{
    if (vertexListSize < DRAWVERTEX_COUNT) {
        if (renderStateCount < 0 || (currentRenderState.useTexture || !currentRenderState.useColors)) {
            if (renderStateCount >= 0) {
                RenderState *state = &renderStateList[renderStateCount];
                memcpy(state, &currentRenderState, sizeof(RenderState));
            }

            currentRenderState.indexCount = 0;
            currentRenderState.id         = 0;
            currentRenderState.useColors  = true;
            currentRenderState.useTexture = false;
            currentRenderState.useFilter  = false;
            currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
            currentRenderState.indexPtr   = drawIndexList;
            renderStateCount++;
        }

        if (renderStateCount < RENDERSTATE_COUNT) {
            int a = 0;
            if (alpha >= 0)
                a = alpha;
            if (a > 0xFF)
                a = 0xFF;

            DrawVertex *vertex1 = &drawVertexList[vertexListSize];
            vertex1->vertX      = x;
            vertex1->vertY      = y;
            vertex1->vertZ      = z;
            vertex1->r          = r;
            vertex1->g          = g;
            vertex1->b          = b;
            vertex1->a          = a;

            DrawVertex *vertex2 = &drawVertexList[vertexListSize + 1];
            vertex2->vertX      = w + x;
            vertex2->vertY      = y;
            vertex2->vertZ      = z;
            vertex2->r          = r;
            vertex2->g          = g;
            vertex2->b          = b;
            vertex2->a          = a;

            DrawVertex *vertex3 = &drawVertexList[vertexListSize + 2];
            vertex3->vertX      = x;
            vertex3->vertY      = y - h;
            vertex3->vertZ      = z;
            vertex3->r          = r;
            vertex3->g          = g;
            vertex3->b          = b;
            vertex3->a          = a;

            DrawVertex *vertex4 = &drawVertexList[vertexListSize + 3];
            vertex4->vertX      = vertex2->vertX;
            vertex4->vertY      = y - h;
            vertex4->vertZ      = z;
            vertex4->r          = r;
            vertex4->g          = g;
            vertex4->b          = b;
            vertex4->a          = a;

            vertexListSize += 4;
            currentRenderState.indexCount += 6;
        }
    }
}

#if !RETRO_USE_ORIGINAL_CODE
void RenderRectClipped(float x, float y, float z, float w, float h, byte r, byte g, byte b, int alpha)
{
    if (vertexListSize < DRAWVERTEX_COUNT) {
        if (renderStateCount < 0 || (currentRenderState.useTexture || !currentRenderState.useColors)) {
            if (renderStateCount >= 0) {
                RenderState *state = &renderStateList[renderStateCount];
                memcpy(state, &currentRenderState, sizeof(RenderState));
            }

            currentRenderState.indexCount = 0;
            currentRenderState.id         = 0;
            currentRenderState.useColors  = true;
            currentRenderState.useTexture = false;
            currentRenderState.useFilter  = false;
            currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
            currentRenderState.indexPtr   = drawIndexList;
            renderStateCount++;
        }

        if (renderStateCount < RENDERSTATE_COUNT) {
            int a = 0;
            if (alpha >= 0)
                a = alpha;
            if (a > 0xFF)
                a = 0xFF;

            DrawVertex *vertex1 = &drawVertexList[vertexListSize];
            vertex1->vertX      = x;
            vertex1->vertY      = y;
            vertex1->vertZ      = z;
            vertex1->r          = r;
            vertex1->g          = g;
            vertex1->b          = b;
            vertex1->a          = a;
            if (vertex1->vertY > 76.0)
                vertex1->vertY = 76.0;

            DrawVertex *vertex2 = &drawVertexList[vertexListSize + 1];
            vertex2->vertX      = w + x;
            vertex2->vertY      = y;
            vertex2->vertZ      = z;
            vertex2->r          = r;
            vertex2->g          = g;
            vertex2->b          = b;
            vertex2->a          = a;
            if (vertex2->vertY > 76.0)
                vertex2->vertY = 76.0;

            DrawVertex *vertex3 = &drawVertexList[vertexListSize + 2];
            vertex3->vertX      = x;
            vertex3->vertY      = y - h;
            vertex3->vertZ      = z;
            vertex3->r          = r;
            vertex3->g          = g;
            vertex3->b          = b;
            vertex3->a          = a;
            if (vertex3->vertY < -76.0)
                vertex3->vertY = -76.0;

            DrawVertex *vertex4 = &drawVertexList[vertexListSize + 3];
            vertex4->vertX      = vertex2->vertX;
            vertex4->vertY      = y - h;
            vertex4->vertZ      = z;
            vertex4->r          = r;
            vertex4->g          = g;
            vertex4->b          = b;
            vertex4->a          = a;
            if (vertex4->vertY < -76.0)
                vertex4->vertY = -76.0;

            vertexListSize += 4;
            currentRenderState.indexCount += 6;
        }
    }
}
#endif

void RenderMesh(MeshInfo *mesh, byte type, byte depthTest)
{
    if (!mesh)
        return;

#if RETRO_USING_KOS
    // The lazy half of the eviction (EvictMeshData). Entities restored from the
    // backup bank never run a Create event, so they never call LoadMesh again -
    // asking to be drawn is the only signal a model gives that it is wanted, and
    // it is a good one: the models the menu does not draw stay evicted and the
    // heap keeps most of what the eviction won.
    //
    // Safe to read the disc from here: native objects build their render states
    // during the game loop, well before the PVR scene is opened.
    if (meshReloadBudget > 0) {
        const int slot = MeshSlotOf(mesh);
        if (slot >= 0 && meshEvicted[slot]) {
            --meshReloadBudget;
            RefillMeshSlot(mesh);
        }
    }
#endif

    if (renderStateCount < RENDERSTATE_COUNT) {
        if (currentRenderState.indexCount) {
            RenderState *state = &renderStateList[renderStateCount++];
            memcpy(state, &currentRenderState, sizeof(RenderState));
        }

        currentRenderState.vertPtr    = mesh->vertices;
        currentRenderState.indexPtr   = mesh->indices;
        currentRenderState.indexCount = mesh->indexCount * 3;
        if (mesh->textureID >= TEXTURE_COUNT) {
            currentRenderState.useTexture = false;
            currentRenderState.id         = 0;
        }
        else {
            currentRenderState.useTexture = true;
            currentRenderState.id         = textureList[mesh->textureID].id;
        }

        switch (type) {
            case MESH_COLORS:
                currentRenderState.useColors  = true;
                currentRenderState.useNormals = false;
                break;
            case MESH_NORMALS:
                currentRenderState.useColors  = false;
                currentRenderState.useNormals = true;
                break;
            case MESH_COLORS_NORMALS:
                currentRenderState.useColors  = true;
                currentRenderState.useNormals = true;
                break;
        }
        currentRenderState.depthTest = depthTest;

        RenderState *state = &renderStateList[renderStateCount];
        memcpy(state, &currentRenderState, sizeof(RenderState));

        currentRenderState.indexCount = 0;
        currentRenderState.id         = 0;
        currentRenderState.useColors  = true;
        currentRenderState.useTexture = false;
        currentRenderState.useNormals = false;
        currentRenderState.depthTest  = false;
        currentRenderState.useFilter  = false;
        currentRenderState.vertPtr    = &drawVertexList[vertexListSize];
        currentRenderState.indexPtr   = drawIndexList;

        renderStateCount++;
    }
}