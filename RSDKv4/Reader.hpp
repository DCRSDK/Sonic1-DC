#ifndef READER_H
#define READER_H

#ifdef FORCE_CASE_INSENSITIVE

#include "fcaseopen.h"
#define FileIO                                          FILE
#define fOpen(path, mode)                               fcaseopen(path, mode)
#define fRead(buffer, elementSize, elementCount, file)  fread(buffer, elementSize, elementCount, file)
#define fSeek(file, offset, whence)                     fseek(file, offset, whence)
#define fTell(file)                                     ftell(file)
#define fClose(file)                                    fclose(file)
#define fWrite(buffer, elementSize, elementCount, file) fwrite(buffer, elementSize, elementCount, file)
#define fClearErr(file)                                 clearerr(file)

#else

#if RETRO_USING_SDL1 || RETRO_USING_SDL2
#define FileIO                                          SDL_RWops
#define fOpen(path, mode)                               SDL_RWFromFile(path, mode)
#define fRead(buffer, elementSize, elementCount, file)  SDL_RWread(file, buffer, elementSize, elementCount)
#define fSeek(file, offset, whence)                     SDL_RWseek(file, offset, whence)
#define fTell(file)                                     SDL_RWtell(file)
#define fClose(file)                                    SDL_RWclose(file)
#define fWrite(buffer, elementSize, elementCount, file) SDL_RWwrite(file, buffer, elementSize, elementCount)
#define fClearErr(file)                                 ((void)0)
#else
#define FileIO                                          FILE
#define fOpen(path, mode)                               fopen(path, mode)
#define fRead(buffer, elementSize, elementCount, file)  fread(buffer, elementSize, elementCount, file)
#define fSeek(file, offset, whence)                     fseek(file, offset, whence)
#define fTell(file)                                     ftell(file)
#define fClose(file)                                    fclose(file)
#define fWrite(buffer, elementSize, elementCount, file) fwrite(buffer, elementSize, elementCount, file)
#define fClearErr(file)                                 clearerr(file)
#endif

#endif

#define RETRO_PACKFILE_COUNT (0x1000)
#define RETRO_PACK_COUNT     (0x4)

struct FileInfo {
    char fileName[0x100];
    int fileSize;
    int vfileSize;
    int readPos;
    int bufferPosition;
    int virtualFileOffset;
    byte eStringPosA;
    byte eStringPosB;
    byte eStringNo;
    byte eNybbleSwap;
    bool useEncryption;
    byte packID;
    byte encryptionStringA[0x10];
    byte encryptionStringB[0x10];
#if !RETRO_USE_ORIGINAL_CODE
    FileIO *cFileHandle;
    bool usingDataPack;
#endif
};

struct RSDKFileInfo {
    uint hash[4];
    int offset;
    int filesize;
    bool encrypted;
    byte packID;
};

struct RSDKContainer {
    RSDKFileInfo files[RETRO_PACKFILE_COUNT];
    char packNames[RETRO_PACK_COUNT][0x400];
    int fileCount;
    int packCount;
};

extern RSDKContainer rsdkContainer;

extern char fileName[0x100];

// Bytes pulled from the disc per fill. Was 0x2000, which meant ~380 separate
// read requests for the ~3MB a stage load moves, each carrying GD-ROM command
// latency - and on hardware over half of a stage load is transfer, not CPU.
// Raising it trades a bigger static array for far fewer, larger requests.
// Only safe alongside the entry-end clamp in FillFileBuffer below: without it
// a larger fill reads past the end of a packed entry into whatever follows.
//
// Must be defined ABOVE the fileBuffer extern that uses it, and the extern and
// the definition in Reader.cpp must both spell the bound this way - declaring
// the array at one size and defining it at another is a hard error, not a
// warning.
#define FILEBUFFER_SIZE (0x8000)
extern byte fileBuffer[FILEBUFFER_SIZE];
extern int fileSize;
extern int vFileSize;
extern int readPos;
extern int readSize;
extern int bufferPosition;
extern int virtualFileOffset;
extern bool useEncryption;
extern byte packID;


#if RETRO_USING_KOS
// Disc traffic, for the [DCLoad] line. Two adds per fill, so it costs nothing
// even when nobody is looking, and it turns "the buffer change helped" from an
// assumption into a number.
extern uint dcReadBytes;
extern uint dcReadCalls;
#endif
extern byte eStringPosA;
extern byte eStringPosB;
extern byte eStringNo;
extern byte eNybbleSwap;
extern byte encryptionStringA[0x10];
extern byte encryptionStringB[0x10];

extern FileIO *cFileHandle;

inline void CopyFilePath(char *dest, const char *src)
{
    strcpy(dest, src);
    for (int i = 0;; ++i) {
        if (i >= strlen(dest)) {
            break;
        }

        if (dest[i] == '/')
            dest[i] = '\\';
    }
}
bool CheckRSDKFile(const char *filePath);
inline void CloseRSDKContainers()
{
    for (int i = 0; i < 4; ++i) {
        strcpy(rsdkContainer.packNames[i], "");
    }
    rsdkContainer.packCount = 0;
    rsdkContainer.fileCount = 0;
}

#if !RETRO_USE_ORIGINAL_CODE
int CheckFileInfo(const char *filepath);
#endif

bool LoadFile(const char *filePath, FileInfo *fileInfo);
inline bool CloseFile()
{
    int result = 0;
    if (cFileHandle)
        result = fClose(cFileHandle);

    cFileHandle = NULL;
    return result;
}

void GenerateELoadKeys(uint key1, uint key2);

void FileRead(void *dest, int size);
void FileSkip(int count);

#if !RETRO_USE_ORIGINAL_CODE
// Reports a fill that came back short (Reader.cpp). Out of line on purpose:
// Debug.hpp, and therefore PrintLog, is included after this header.
void ReportShortFileRead(int got, int want, int at);
#endif

#if RETRO_USING_KOS
// ---------------------------------------------------------------------------
// GD-ROM sector-aligned seeking.
//
// KOS's iso_read() only reaches its streaming / multi-sector fast paths when a
// read STARTS on a 2048-byte sector boundary AND the destination is 32-byte
// aligned. Miss either and it degrades to one bdread() per sector through a
// 16-entry LRU cache, aborting the drive's read-ahead stream on every miss.
//
// Datapack entries sit at arbitrary offsets, so seeking straight to one put
// every refill mid-sector. The leading partial sector is copied out on the slow
// path, and because that memcpy advances the destination by a non-multiple of
// 32, EVERY remaining sector of the same 16KB refill fails the alignment test
// too. Modelling iso_read against this file's real access pattern gave ~820
// single-sector drive transactions per stage load with the streaming engine
// never engaging once; aligned, the same load models at ~30. The dcload capture
// agrees on the shape: hardware load time fits 316ms + 1.059ms/KB at R^2=0.987,
// and 1.059ms/KB is 2.12ms per sector - transaction-bound, not bandwidth-bound.
//
// So seek to the sector CONTAINING pos and consume the remainder out of the
// stdio buffer. Because the stream's file position only advances on whole 16KB
// refills, aligning it once per seek keeps every later refill aligned for the
// rest of that file. No extra bytes come off the disc - the discarded remainder
// lives inside the refill that was going to happen anyway - and the caller ends
// up at exactly the position it asked for, so the bytes delivered are unchanged
// (verified byte-for-byte over 600 seek/read trials across every residue class
// mod 2048).
#define DC_CD_SECTOR (2048)
inline void DC_SeekSectorAligned(FileIO *f, int pos)
{
    if (!f)
        return;
    if (pos < 0) { // nothing sensible to align to; behave exactly as before
        fSeek(f, pos, SEEK_SET);
        return;
    }

    const int base = pos & ~(DC_CD_SECTOR - 1);
    fSeek(f, base, SEEK_SET);

    int skip = pos - base;
    while (skip > 0) {
        byte      sink[256];
        const int n   = skip > (int)sizeof(sink) ? (int)sizeof(sink) : skip;
        const size_t got = fRead(sink, 1u, (size_t)n, f);
        if (got != (size_t)n) {
            // Short read while skipping: fall back to the plain seek so a
            // failure here can never leave the caller at the wrong offset.
            fClearErr(f);
            fSeek(f, pos, SEEK_SET);
            return;
        }
        skip -= n;
    }
}
#define DC_SEEK_SET(f, p) DC_SeekSectorAligned((f), (p))
#else
#define DC_SEEK_SET(f, p) fSeek((f), (p), SEEK_SET)
#endif

inline size_t FillFileBuffer()
{
    // Clamp to the end of the CURRENT file, not of the container. For a packed
    // entry `fileSize` is the whole datapack, so clamping to that alone would
    // let one fill drag in the bytes of whatever follows. At 0x2000 that waste
    // was small; at FILEBUFFER_SIZE a 3KB Act.bin would pull in 29KB of the
    // next file, which is exactly the trade a bigger buffer is meant to win.
    // virtualFileOffset + vFileSize is the entry's end, and both are now
    // maintained on the loose-file paths too (see LoadFile / SetFileInfo).
    // (named `fill`, not `want`: the short-read retry below already owns a
    //  `want` and shadowing it here is a compile error, not a subtle bug)
    int fill = (virtualFileOffset + vFileSize) - readPos;
    if (fill > FILEBUFFER_SIZE)
        fill = FILEBUFFER_SIZE;
    if (fill > fileSize - readPos)
        fill = fileSize - readPos;
    if (fill < 0)
        fill = 0;
    readSize = fill;

#if RETRO_USING_KOS
    dcReadBytes += (uint)readSize;
    ++dcReadCalls;
#endif

#if !RETRO_USE_ORIGINAL_CODE
    // fread is allowed to come back short, and this used to ignore that: readPos
    // advanced by the full readSize no matter what arrived, so the tail of
    // fileBuffer kept the PREVIOUS fill's bytes while the caller - and, for a
    // packed file, the decryption keystream - carried on as though they were
    // real. Everything read after that point in the file came back as
    // plausible-looking garbage, with no error reported anywhere.
    //
    // On the Dreamcast this is not hypothetical. The music streamer reads the
    // GD-ROM from its own thread (DCAudio.cpp's reader thread) while the main
    // thread reads Data.rsdk, and a contended read hands back what it managed to
    // get. It surfaced in LoadMesh: a 24,058-byte Achievements.bin reporting 255
    // animation frames and a 5,850-byte Options.bin reporting 30,248 indices,
    // both of which then asked malloc for several megabytes, failed, and left
    // the 2013 menu drawing null meshes as geometry.
    //
    // So loop until the buffer really holds what was asked for. readSize is
    // clamped to what the file physically has left (just above), so a genuine
    // short read here means something went wrong rather than "end of file" - in
    // which case rewind to where this fill was meant to start and ask again,
    // since a drive that was busy a moment ago usually isn't a moment later.
    // Only if that keeps failing is it worth saying so and zeroing the
    // remainder, rather than letting the caller parse whatever happened to be
    // sitting in the buffer from last time.
    const size_t want = (size_t)(readSize > 0 ? readSize : 0);
    const int    at   = readPos;
    size_t       got  = 0;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt) {
            // A failed read leaves the stream's error flag set and its position
            // anyone's guess, so clear both before trying the same bytes again.
            fClearErr(cFileHandle);
            DC_SEEK_SET(cFileHandle, at);
            got = 0;
        }

        while (got < want) {
            size_t n = fRead(fileBuffer + got, 1u, want - got, cFileHandle);
            if (!n)
                break;
            got += n;
        }

        if (got >= want)
            break;
    }

    if (got < want) {
        ReportShortFileRead((int)got, (int)want, at);
        memset(fileBuffer + got, 0, want - got);

        // Put the stream where the next fill expects to find it, so one bad
        // read costs one bad buffer rather than every buffer after it.
        fClearErr(cFileHandle);
        DC_SEEK_SET(cFileHandle, at + readSize);
    }

    readPos += readSize;
    bufferPosition = 0;
    return got;
#else
    size_t result = fRead(fileBuffer, 1u, readSize, cFileHandle);
    readPos += readSize;
    bufferPosition = 0;
    return result;
#endif
}

void GetFileInfo(FileInfo *fileInfo);
void SetFileInfo(FileInfo *fileInfo);
size_t GetFilePosition();
void SetFilePosition(int newPos);
bool ReachedEndOfFile();

#endif // !READER_H
