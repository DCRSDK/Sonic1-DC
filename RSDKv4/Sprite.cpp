#include "RetroEngine.hpp"

#if RETRO_USING_KOS
#include "Dreamcast/DCCommon.hpp"
#endif

struct GifDecoder {
    int depth;
    int clearCode;
    int eofCode;
    int runningCode;
    int runningBits;
    int prevCode;
    int currentCode;
    int maxCodePlusOne;
    int stackPtr;
    int shiftState;
    int fileState;
    int position;
    int bufferSize;
    uint shiftData;
    uint pixelCount;
    byte buffer[256];
    byte stack[4096];
    byte suffix[4096];
    uint prefix[4096];
};

const int LOADING_IMAGE = 0;
const int LOAD_COMPLETE = 1;
const int LZ_MAX_CODE   = 4095;
const int LZ_BITS       = 12;
const int FIRST_CODE    = 4097;
const int NO_SUCH_CODE  = 4098;

GifDecoder gifDecoder;
int codeMasks[] = { 0, 1, 3, 7, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095 };

// File-local: nothing outside this file has ever referenced them, and static
// lets the compiler inline ReadGifByte into ReadGifCode, which is the hottest
// call pair in the decoder (once or twice per LZW code).
static int ReadGifCode(void);
static byte ReadGifByte(void);
static byte TraceGifPrefix(uint *prefix, int code, int clearCode);

void InitGifDecoder()
{
    byte code = 0;
    FileRead(&code, 1);
    gifDecoder.fileState      = LOADING_IMAGE;
    gifDecoder.position       = 0;
    gifDecoder.bufferSize     = 0;
    gifDecoder.buffer[0]      = 0;
    gifDecoder.depth          = code;
    gifDecoder.clearCode      = 1 << code;
    gifDecoder.eofCode        = gifDecoder.clearCode + 1;
    gifDecoder.runningCode    = gifDecoder.eofCode + 1;
    gifDecoder.runningBits    = code + 1;
    gifDecoder.maxCodePlusOne = 1 << gifDecoder.runningBits;
    gifDecoder.stackPtr       = 0;
    gifDecoder.prevCode       = NO_SUCH_CODE;
    gifDecoder.shiftState     = 0;
    gifDecoder.shiftData      = 0;
    for (int i = 0; i <= LZ_MAX_CODE; ++i) gifDecoder.prefix[i] = (byte)NO_SUCH_CODE;
}
void ReadGifLine(byte *line, int length, int offset)
{
    int i         = 0;
    int stackPtr  = gifDecoder.stackPtr;
    int eofCode   = gifDecoder.eofCode;
    int clearCode = gifDecoder.clearCode;
    int prevCode  = gifDecoder.prevCode;
    // Hoisted out of the loop: these are fixed-address globals otherwise, and
    // the inner loops index all three per output byte.
    byte *const stack  = gifDecoder.stack;
    byte *const suffix = gifDecoder.suffix;
    uint *const prefix = gifDecoder.prefix;
    if (stackPtr != 0) {
        while (stackPtr != 0) {
            if (i >= length)
                break;
            line[offset++] = stack[--stackPtr];
            i++;
        }
    }
    while (i < length) {
        int gifCode = ReadGifCode();
        if (gifCode == eofCode) {
            if (i != length - 1 || gifDecoder.pixelCount != 0)
                return;
            i++;
        }
        else {
            if (gifCode == clearCode) {
                for (int p = 0; p <= LZ_MAX_CODE; p++) prefix[p] = NO_SUCH_CODE;
                gifDecoder.runningCode    = gifDecoder.eofCode + 1;
                gifDecoder.runningBits    = gifDecoder.depth + 1;
                gifDecoder.maxCodePlusOne = 1 << gifDecoder.runningBits;
                prevCode                  = (gifDecoder.prevCode = NO_SUCH_CODE);
            }
            else {
                // The first byte of the string this code expands to. The new
                // table entry's suffix is exactly that byte, and the decode
                // below already computes it, so it is carried out rather than
                // recovered afterwards with a second walk of the prefix chain
                // (see the note where it is used).
                int firstByte = gifCode;

                if (gifCode < clearCode) {
                    line[offset] = (byte)gifCode;
                    offset++;
                    i++;
                }
                else {
                    if (gifCode < 0 || gifCode > LZ_MAX_CODE)
                        return;

                    int code = 0;
                    if (prefix[gifCode] == NO_SUCH_CODE) {
                        if (gifCode != gifDecoder.runningCode - 2)
                            return;

                        code                                  = prevCode;
                        suffix[gifDecoder.runningCode - 2] = stack[stackPtr++] = TraceGifPrefix(prefix, prevCode, clearCode);
                    }
                    else {
                        code = gifCode;
                    }
                    int c = 0;
                    while (c++ <= LZ_MAX_CODE && code > clearCode && code <= LZ_MAX_CODE) {
                        stack[stackPtr++] = suffix[code];
                        code              = prefix[code];
                    }
                    if (c >= LZ_MAX_CODE || code > LZ_MAX_CODE)
                        return;

                    // The walk above stops on a code <= clearCode, i.e. a raw
                    // byte: the root of the chain, which IS the first byte of
                    // this code's string.
                    firstByte = code;

                    stack[stackPtr++] = (byte)code;
                    while (stackPtr != 0 && i++ < length) line[offset++] = stack[--stackPtr];
                }
                if (prevCode != NO_SUCH_CODE) {
                    if (gifDecoder.runningCode < 2 || gifDecoder.runningCode > FIRST_CODE)
                        return;

                    prefix[gifDecoder.runningCode - 2] = prevCode;

                    // This used to be a second TraceGifPrefix walk, one of two
                    // branches: TraceGifPrefix(prevCode) when gifCode was the
                    // code being defined, TraceGifPrefix(gifCode) otherwise.
                    // Both return the root of the chain just walked above, so
                    // both are firstByte:
                    //   - gifCode == runningCode-2 is the KwKwK case, which is
                    //     also the only way prefix[gifCode] was NO_SUCH_CODE, so
                    //     the walk started at prevCode and rooted at
                    //     root(prevCode) == TraceGifPrefix(prevCode).
                    //   - otherwise the walk started at gifCode and rooted at
                    //     root(gifCode) == TraceGifPrefix(gifCode).
                    // The prefix[] write above cannot change either answer:
                    // chains only ever point at codes defined earlier, and
                    // nothing points at runningCode-2 yet.
                    // Removing it takes one full chain walk per code out of the
                    // decoder — on a 256x256 sheet that is the bulk of the
                    // pointer chasing, and the chain is a dependent load chain,
                    // which an in-order SH-4 cannot hide.
                    suffix[gifDecoder.runningCode - 2] = (byte)firstByte;
                }
                prevCode = gifCode;
            }
        }
    }
    gifDecoder.prevCode = prevCode;
    gifDecoder.stackPtr = stackPtr;
}

static int ReadGifCode()
{
    while (gifDecoder.shiftState < gifDecoder.runningBits) {
        byte b = ReadGifByte();
        gifDecoder.shiftData |= (uint)((uint)b << gifDecoder.shiftState);
        gifDecoder.shiftState += 8;
    }
    int code = (gifDecoder.shiftData & codeMasks[gifDecoder.runningBits]);
    gifDecoder.shiftData >>= gifDecoder.runningBits;
    gifDecoder.shiftState -= gifDecoder.runningBits;
    if (++gifDecoder.runningCode > gifDecoder.maxCodePlusOne && gifDecoder.runningBits < LZ_BITS) {
        gifDecoder.maxCodePlusOne <<= 1;
        gifDecoder.runningBits++;
    }
    return code;
}

static byte ReadGifByte()
{
    byte c = 0;
    if (gifDecoder.fileState == LOAD_COMPLETE)
        return c;

    byte b;
    if (gifDecoder.position == gifDecoder.bufferSize) {
        FileRead(&b, 1);
        gifDecoder.bufferSize = (int)b;
        if (gifDecoder.bufferSize == 0) {
            gifDecoder.fileState = LOAD_COMPLETE;
            return c;
        }
        FileRead(gifDecoder.buffer, gifDecoder.bufferSize);
        b                   = gifDecoder.buffer[0];
        gifDecoder.position = 1;
    }
    else {
        b = gifDecoder.buffer[gifDecoder.position++];
    }
    return b;
}

static byte TraceGifPrefix(uint *prefix, int code, int clearCode)
{
    int i = 0;
    while (code > clearCode && i++ <= LZ_MAX_CODE) code = prefix[code];

    return code;
}
void ReadGifPictureData(int width, int height, bool interlaced, byte *gfxData, int offset)
{
    int initialRow[] = { 0, 4, 2, 1 };
    int rowInc[]     = { 8, 8, 4, 2 };
    InitGifDecoder();
    if (interlaced) {
        for (int p = 0; p < 4; ++p) {
            for (int y = initialRow[p]; y < height; y += rowInc[p]) ReadGifLine(gfxData, width, y * width + offset);
        }
    }
    else {
        for (int y = 0; y < height; ++y) ReadGifLine(gfxData, width, y * width + offset);
    }
}

int AddGraphicsFile(const char *filePath)
{
    char sheetPath[0x100];

    StrCopy(sheetPath, "Data/Sprites/");
    StrAdd(sheetPath, filePath);
    int sheetID = 0;
    while (StrLength(gfxSurface[sheetID].fileName) > 0) {
        if (StrComp(gfxSurface[sheetID].fileName, sheetPath))
            return sheetID;
        if (++sheetID == SURFACE_COUNT) // Max Sheet cnt
            return 0;
    }
    byte fileExtension = (byte)sheetPath[(StrLength(sheetPath) - 1) & 0xFF];
    switch (fileExtension) {
        case 'f': LoadGIFFile(sheetPath, sheetID); break;
        case 'p': LoadBMPFile(sheetPath, sheetID); break;
        case 'r': LoadPVRFile(sheetPath, sheetID); break;
    }

#if RETRO_USING_KOS
    // Mirror the freshly-loaded sheet into PVR VRAM for hardware sprites.
    DC_UploadSheet(sheetID);
#endif

    return sheetID;
}
void RemoveGraphicsFile(const char *filePath, int sheetID)
{
    if (sheetID < 0) {
        for (int i = 0; i < SURFACE_COUNT; ++i) {
            if (StrLength(gfxSurface[i].fileName) > 0 && StrComp(gfxSurface[i].fileName, filePath))
                sheetID = i;
        }
    }

    if (sheetID >= 0 && StrLength(gfxSurface[sheetID].fileName)) {
#if RETRO_USING_KOS
        // Free this slot's VRAM texture. The pool compaction below only moves
        // other sheets' bytes (their textures are independent VRAM copies and
        // stay valid), so only the removed slot needs invalidating.
        DC_InvalidateSheet(sheetID);
#endif
        StrCopy(gfxSurface[sheetID].fileName, "");
        int dataPosStart = gfxSurface[sheetID].dataPosition;
        int dataPosEnd   = gfxSurface[sheetID].dataPosition + gfxSurface[sheetID].height * gfxSurface[sheetID].width;
#if RETRO_USING_KOS
        // Compact the pool with one memmove over the LIVE region only.
        //
        // Stock does this a BYTE AT A TIME, all the way to GFXDATA_SIZE:
        //     for (int i = GFXDATA_SIZE - dataPosEnd; i > 0; --i)
        //         graphicData[dataPosStart++] = graphicData[dataPosEnd++];
        //
        // On a PC that is free. On the SH-4 it is a 3MB byte-by-byte walk
        // through an 8KB data cache, and the dcload capture of the end credits
        // measured it at ~200ms PER SHEET: releasing a demo's nine sheets cost
        // ~1.8s, 47% of ALL stage-load time in the credits and the single
        // largest cost anywhere in the load path. Redream hides it completely
        // (~180ms for the whole release phase) because its JIT runs the loop on
        // the host CPU, which is why every redream measurement missed it.
        //
        // Two changes, both safe:
        //   * memmove instead of the byte loop. The regions overlap and dst is
        //     BELOW src, which is precisely what memmove is specified for.
        //   * copy only as far as gfxDataPosition, the pool's high-water mark,
        //     rather than to the end of the array. Everything past it is bytes
        //     no sheet has ever occupied - LoadGIFFile/LoadBMPFile always place
        //     a new sheet AT gfxDataPosition and grow it - so nothing can read
        //     them. With ~64KB sheets the live region is a few hundred KB, not
        //     3MB.
        // Neither changes a single byte of the live pool; verified against the
        // original loop over randomised add/remove sequences.
        if (gfxDataPosition > dataPosEnd)
            memmove(&graphicData[dataPosStart], &graphicData[dataPosEnd], (size_t)(gfxDataPosition - dataPosEnd));
#else
        for (int i = GFXDATA_SIZE - dataPosEnd; i > 0; --i) graphicData[dataPosStart++] = graphicData[dataPosEnd++];
#endif
        gfxDataPosition -= gfxSurface[sheetID].height * gfxSurface[sheetID].width;
        for (int i = 0; i < SURFACE_COUNT; ++i) {
            if (gfxSurface[i].dataPosition > gfxSurface[sheetID].dataPosition)
                gfxSurface[i].dataPosition -= gfxSurface[sheetID].height * gfxSurface[sheetID].width;
        }
    }
}

int LoadBMPFile(const char *filePath, byte sheetID)
{
    FileInfo info;
    if (LoadFile(filePath, &info)) {
        GFXSurface *surface = &gfxSurface[sheetID];
        StrCopy(surface->fileName, filePath);

        byte fileBuffer = 0;

        SetFilePosition(18);
        FileRead(&fileBuffer, 1);
        surface->width = fileBuffer;
        FileRead(&fileBuffer, 1);
        surface->width |= fileBuffer << 8;
        FileRead(&fileBuffer, 1);
        surface->width |= fileBuffer << 16;
        FileRead(&fileBuffer, 1);
        surface->width |= fileBuffer << 24;

        FileRead(&fileBuffer, 1);
        surface->height = fileBuffer;
        FileRead(&fileBuffer, 1);
        surface->height |= fileBuffer << 8;
        FileRead(&fileBuffer, 1);
        surface->height |= fileBuffer << 16;
        FileRead(&fileBuffer, 1);
        surface->height |= fileBuffer << 24;

        SetFilePosition(info.vfileSize - surface->height * surface->width);
        surface->dataPosition = gfxDataPosition;
        byte *gfxData         = &graphicData[surface->dataPosition + surface->width * (surface->height - 1)];
        for (int y = 0; y < surface->height; ++y) {
            for (int x = 0; x < surface->width; ++x) {
                FileRead(&fileBuffer, 1);
                *gfxData++ = fileBuffer;
            }
            gfxData -= 2 * surface->width;
        }
        gfxDataPosition += surface->height * surface->width;

#if RETRO_SOFTWARE_RENDER
        surface->widthShift = 0;
        int w               = surface->width;
        while (w > 1) {
            w >>= 1;
            ++surface->widthShift;
        }
#endif

        if (gfxDataPosition >= GFXDATA_SIZE) {
            gfxDataPosition = 0;
            PrintLog("WARNING: Exceeded max gfx size!");
        }

        CloseFile();
        return true;
    }
    return false;
}
int LoadGIFFile(const char *filePath, byte sheetID)
{
    FileInfo info;
    if (LoadFile(filePath, &info)) {
        GFXSurface *surface = &gfxSurface[sheetID];
        StrCopy(surface->fileName, filePath);

        byte fileBuffer = 0;

        SetFilePosition(6); // GIF89a
        FileRead(&fileBuffer, 1);
        surface->width = fileBuffer;
        FileRead(&fileBuffer, 1);
        surface->width |= fileBuffer << 8;
        FileRead(&fileBuffer, 1);
        surface->height = fileBuffer;
        FileRead(&fileBuffer, 1);
        surface->height |= fileBuffer << 8;

        FileRead(&fileBuffer, 1); // Palette Size
        // int has_pallete  = (fileBuffer & 0x80) >> 7;
        // int colors       = ((fileBuffer & 0x70) >> 4) + 1;
        int palette_size = (fileBuffer & 0x7) + 1;
        if (palette_size > 0)
            palette_size = 1 << palette_size;
        FileRead(&fileBuffer, 1); // BG Color index (thrown away)
        FileRead(&fileBuffer, 1); // idk actually (still thrown away)

        int c = 0;
        byte clr[3];
        do {
            ++c;
            FileRead(clr, 3);
        } while (c != palette_size);

        FileRead(&fileBuffer, 1);
        while (fileBuffer != ',') FileRead(&fileBuffer, 1); // gif image start identifier

        ushort fileBuffer2 = 0;
        FileRead(&fileBuffer2, 2);
        FileRead(&fileBuffer2, 2);
        FileRead(&fileBuffer2, 2);
        FileRead(&fileBuffer2, 2);
        FileRead(&fileBuffer, 1);
        bool interlaced = (fileBuffer & 0x40) >> 6;
        if (fileBuffer >> 7 == 1) {
            int c = 0x80;
            do {
                ++c;
                FileRead(clr, 3);
            } while (c != 0x100);
        }

        surface->dataPosition = gfxDataPosition;

#if RETRO_SOFTWARE_RENDER
        surface->widthShift = 0;
        int w               = surface->width;
        while (w > 1) {
            w >>= 1;
            ++surface->widthShift;
        }
#endif

        gfxDataPosition += surface->width * surface->height;
        if (gfxDataPosition < GFXDATA_SIZE) {
            ReadGifPictureData(surface->width, surface->height, interlaced, graphicData, surface->dataPosition);
        }
        else {
            gfxDataPosition = 0;
            PrintLog("WARNING: Exceeded max gfx size!");
        }

        CloseFile();
        return true;
    }
    return false;
}
int LoadPVRFile(const char *filePath, byte sheetID)
{
    // ONLY READS "PVRTC 2bpp RGB" PVR FILES
    FileInfo info;
    if (LoadFile(filePath, &info)) {
        GFXSurface *surface = &gfxSurface[sheetID];
        StrCopy(surface->fileName, filePath);

        byte fileBuffer[2];

        SetFilePosition(28);
        FileRead(fileBuffer, 1);
        int width = fileBuffer[0];
        FileRead(fileBuffer, 1);
        width |= fileBuffer[0] << 8;
        FileRead(fileBuffer, 1);
        int height = fileBuffer[0];
        FileRead(fileBuffer, 1);
        height = fileBuffer[0] << 8;

        surface->width        = width;
        surface->height       = height;
        surface->dataPosition = gfxDataPosition;
        gfxDataPosition += surface->width * surface->height;

        if (gfxDataPosition >= GFXDATA_SIZE) {
            gfxDataPosition = 0;
            PrintLog("WARNING: Exceeded max gfx size!");
        }

#if RETRO_SOFTWARE_RENDER
        surface->widthShift = 0;
        int w               = surface->width;
        while (w > 1) {
            w >>= 1;
            ++surface->widthShift;
        }
#endif

        return false; // yeah I have no clue how to handle this, cd lite has this be loaded every frame on framebuffer update and does it that way

        ushort *buffer = NULL;
        for (int h = 0; h < height; ++h) {
            for (int w = 0; w < width; ++w) {
                FileRead(fileBuffer, 2);
                buffer[w] = 2 * (fileBuffer[0] + (fileBuffer[1] << 8)) | 1;
            }
            buffer += width;
        }
        buffer += 0x400 - width;

        CloseFile();
        return true;
    }
    return false;
}
