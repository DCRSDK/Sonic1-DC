#ifndef RENDERER_H
#define RENDERER_H

#if RETRO_USING_KOS
// Dreamcast: the GL half of the HW renderer is inert, but DCNative.cpp DOES
// walk renderStateList/drawVertexList to build its PVR batches, so this cap is
// a real limit on how much native-menu geometry can exist in one frame.
//
// It was 0x100 (256 quads), which the staff credits overran: StaffCredits keeps
// 0x10 credit lines alive at once (StaffCredits.hpp), every one of them calls
// RenderTextClipped every frame whether it is inside the visible band or not,
// and RenderTextClipped's glyph loop simply stops dead when vertexListSize hits
// DRAWVERTEX_COUNT (Renderer.cpp:1563). Sixteen lines is well over 240 glyphs,
// so the tail of the list — the bottom-most lines, since credit entities are
// processed in creation order which is top-to-bottom — lost its last few
// characters. ("Title & Menu Musi", with nothing under it.) The heading, the
// panel and the back arrow survived because StaffCredits_Main draws those
// before its child entities run.
//
// 0x400 gives ~1024 quads: worst case is 16 long credit lines (~35 glyphs each
// = 560) plus the heading, the arrow and whatever the options menu behind it
// still has queued, so this is roughly 1.7x headroom rather than 0.5x. Costs
// ~117KB more BSS than 0x100 (36 bytes/DrawVertex + 2 bytes/index) and nothing
// in PVR VRAM — the 512KB vertex buffer in DCGraphics.cpp already dwarfs it.
#define DRAWFACE_COUNT (0x400)
#else
#define DRAWFACE_COUNT    (0x1000)
#endif
#define DRAWVERTEX_COUNT  (DRAWFACE_COUNT * 4)
#define DRAWINDEX_COUNT   (DRAWFACE_COUNT * 6)
#define TEXTURE_COUNT     (0x80)
#define MESH_COUNT        (0x40)
#define RENDERSTATE_COUNT (0x100)

enum RenderBlendModes {
    RENDER_BLEND_NONE,
    RENDER_BLEND_ALPHA,
    RENDER_BLEND_ALPHA2,
    RENDER_BLEND_ALPHA3,
};

enum TextureFormats {
    TEXFMT_NONE,
    TEXFMT_RGBA4444,
    TEXFMT_RGBA5551,
    TEXFMT_RGBA8888,
    TEXFMT_RETROBUFFER,
};

enum MeshRenderTypes {
    MESH_COLORS,
    MESH_NORMALS,
    MESH_COLORS_NORMALS,
};

struct DrawVertex {
    float vertX;
    float vertY;
    float vertZ;
    float normalX;
    float normalY;
    float normalZ;
    float texCoordX;
    float texCoordY;
    byte r;
    byte g;
    byte b;
    byte a;
};

struct MatrixF {
    float values[4][4];
};

struct TextureInfo {
    char fileName[64];
    int width;
    int height;
    float widthN;
    float heightN;
    int format;
    uint id;
};

struct MeshVertex {
    float vertX;
    float vertY;
    float vertZ;
    float normalX;
    float normalY;
    float normalZ;
};

struct MeshInfo {
    char fileName[64];
    DrawVertex *vertices;
    ushort *indices;
    ushort vertexCount;
    ushort indexCount;
    MeshVertex *frames;
    ushort frameCount;
    byte textureID;
};

struct MeshAnimator {
    float animationSpeed;
    float animationTimer;
    ushort frameID;
    ushort loopIndex;
    ushort frameCount;
    byte loopAnimation;
    byte animationFinished;
};

struct RenderState {
    MatrixF *renderMatrix;
    DrawVertex *vertPtr;
    ushort *indexPtr;
    ushort indexCount;
    int id;
    byte blendMode;
    byte useTexture;
    byte useColors;
    byte depthTest;
    byte useNormals;
    byte useFilter;
};

extern float retroVertexList[40];
extern float screenBufferVertexList[40];

extern int vertexListSize;
extern DrawVertex drawVertexList[DRAWVERTEX_COUNT];
extern ushort drawIndexList[DRAWINDEX_COUNT];

extern byte vertexR;
extern byte vertexG;
extern byte vertexB;

extern TextureInfo textureList[TEXTURE_COUNT];
extern MeshInfo meshList[MESH_COUNT];

extern int renderStateCount;
extern RenderState renderStateList[RENDERSTATE_COUNT];
extern RenderState currentRenderState;

// Matricies
void SetIdentityMatrixF(MatrixF *matrix);
void MatrixMultiplyF(MatrixF *matrixA, MatrixF *matrixB);
void MatrixTranslateXYZF(MatrixF *Matrix, float x, float y, float z);
void MatrixScaleXYZF(MatrixF *matrix, float scaleX, float scaleY, float scaleZ);
void MatrixRotateXF(MatrixF *matrix, float rotationX);
void MatrixRotateYF(MatrixF *matrix, float rotationY);
void MatrixRotateZF(MatrixF *matrix, float rotationZ);
void MatrixRotateXYZF(MatrixF *matrix, float rotationX, float rotationY, float rotationZ);
void MatrixInvertF(MatrixF *dstMatrix, MatrixF *matrix);

// Render States
void ResetRenderStates();
void SetRenderBlendMode(byte mode);
void SetRenderVertexColor(byte r, byte g, byte b);
void SetPerspectiveMatrix(float w, float h, float near, float far);
void SetupDrawIndexList();
void SetRenderMatrix(MatrixF *matrix);
void NewRenderState();
void RenderScene();

// Textures
int LoadTexture(const char *filePath, int format);
void ReplaceTexture(const char *filePath, int textureID);
void ClearTextures(bool keepBuffer);

#if RETRO_USING_KOS
// Last-resort heap reclaim for LoadMesh (DCAudio.cpp). The menu's models and a
// stage's sfx want the same ~1.5MB; this drops the sfx so the models can load.
// Returns false when there was no arena to reclaim, i.e. retrying is pointless.
bool DC_SfxArenaEmergencyRelease();
#endif

// Meshes
MeshInfo *LoadMesh(const char *filePath, byte textureID);
void SetMeshVertexColors(MeshInfo *mesh, byte r, byte g, byte b, byte a);
void ClearMeshData();

#if RETRO_USING_KOS
// Frees the native menu's model geometry at a stage load WITHOUT giving up the
// slots (Scene.cpp). Unlike ClearMeshData this keeps each slot's fileName and
// textureID, so a MeshInfo* held by an entity that RestoreNativeObjects brings
// back still refers to the model it was pointing at rather than to whatever the
// pause menu happened to load into that index. The geometry comes back on
// demand - from LoadMesh for anything created afresh, from RenderMesh for
// anything restored. See the comment on EvictMeshData in Renderer.cpp.
void EvictMeshData();

// Refreshes RenderMesh's per-frame refill allowance. Called once per frame from
// the DC flip, next to DC_NativeReloadEvicted.
void ResetEvictedMeshBudget();
#endif

void SetMeshAnimation(MeshInfo *mesh, MeshAnimator *animator, ushort frameID, ushort frameCount, float speed);
void AnimateMesh(MeshInfo *mesh, MeshAnimator *animator);

// Rendering
void TransferRetroBuffer();
void RenderRetroBuffer(int alpha, float z);
void RenderImage(float x, float y, float z, float scaleX, float scaleY, float pivotX, float pivotY, float sprW, float sprH, float sprX, float sprY,
                 int alpha, byte texture);
void RenderImageClipped(float x, float y, float z, float scaleX, float scaleY, float pivotX, float pivotY, float sprW, float sprH, float sprX,
                        float sprY, int alpha, byte texture);
void RenderImageFlipH(float x, float y, float z, float scaleX, float scaleY, float pivotX, float pivotY, float sprW, float sprH, float sprX,
                      float sprY, int alpha, byte texture);
void RenderText(ushort *text, int fontID, float x, float y, int z, float scale, int alpha);
void RenderTextClipped(ushort *text, int fontID, float x, float y, int z, float scale, int alpha);
void RenderRect(float x, float y, float z, float w, float h, byte r, byte g, byte b, int alpha);
#if !RETRO_USE_ORIGINAL_CODE
void RenderRectClipped(float x, float y, float z, float w, float h, byte r, byte g, byte b, int alpha);
#endif
void RenderMesh(MeshInfo *mesh, byte type, byte depthTest);

#endif // !RENDERER_H