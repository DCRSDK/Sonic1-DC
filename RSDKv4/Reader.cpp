#include "RetroEngine.hpp"
#include <string>

RSDKContainer rsdkContainer;

char fileName[0x100];
byte fileBuffer[FILEBUFFER_SIZE];
int fileSize          = 0;
int vFileSize         = 0;
int readPos           = 0;
int readSize          = 0;
int bufferPosition    = 0;
int virtualFileOffset = 0;
bool useEncryption    = false;
#if RETRO_USING_KOS
uint dcReadBytes = 0;
uint dcReadCalls = 0;
#endif
byte packID           = 0;
byte eStringPosA;
byte eStringPosB;
byte eStringNo;
byte eNybbleSwap;
byte encryptionStringA[0x10];
byte encryptionStringB[0x10];

FileIO *cFileHandle = nullptr;

#if RETRO_USING_KOS
// ---------------------------------------------------------------------------
// GD-ROM: give every file we read a big, 32-byte-aligned stdio buffer.
//
// KOS's iso_fstat() reports st_blksize = 512, and newlib's __swhatbuf_r() sizes
// each FILE*'s buffer from exactly that field, so by default every stream on
// /cd refills 512 bytes at a time out of a malloc'd (8-byte-aligned) buffer.
// That is the worst possible shape for this drive, for two reasons.
//
// First, iso_read() has three tiers, and the good ones are gated on alignment:
//
//     if((thissect & 31) == 0 && toread >= 32 && (((uintptr_t)outbuf) & 31) == 0)
//
// Only inside that branch does it use the drive's streaming engine
// (cdrom_stream_start/cdrom_stream_request), which keeps a sequential DMA read
// running ahead of us for the whole remainder of the file. The fallback path,
// read_loop, has its own multi-sector short-circuit - and that one *also*
// requires __is_aligned(outbuf, 32). Miss the alignment and every read of any
// size degrades to one bdread() per 2048-byte sector: a single-sector
// cdrom_read_sectors_ex through a 16-block LRU cache, one drive transaction at
// a time. A buffer newlib malloc'd is 8-byte aligned, so today we are almost
// certainly never taking either fast path.
//
// Second - and this is the expensive one - bread_cache() calls
// iso_abort_stream() on every cache miss, and iso_seek() aborts on every
// position change. There is exactly one stream_fd for the whole filesystem. So
// each slow-path read does not merely cost a transaction, it tears down the
// read-ahead stream that another file (notably the music the reader thread in
// DCAudio.cpp is pulling) had running. Sixteen 512-byte reads where one 16 KB
// read would do is sixteen chances to stop the drive streaming and make the
// head go somewhere else.
//
// One 32-byte-aligned buffer therefore buys three separate things: the fast
// paths become reachable at all, FillFileBuffer's 0x2000-byte request stops
// being decomposed into sixteen syscalls, and fh_mutex is taken once per
// refill instead of once per 512 bytes - which is also sixteen fewer windows
// for the music thread to interleave.
//
// Sharing one static buffer between opens is safe here: Reader.cpp reads
// through the single global cFileHandle above, and LoadFile() closes the
// previous handle before opening the next, so only one stream is ever actually
// read. It is also safe to hand newlib a static: setvbuf() clears __SMBF, so
// fclose() will not try to free it.
//
// Size is a deliberate compromise. 0x4000 covers two FillFileBuffer refills per
// underlying read while costing 16 KB of BSS - and BSS comes out of the same
// pool as the stage-load heap, which is documented tight (largest hole ~372 KB).
// The DCAudio.cpp reader thread gave 8 KB back by dropping its bounce buffer,
// so the net cost across both changes is about 8 KB.
// ---------------------------------------------------------------------------
#define DC_CD_STDIO_BUF_SIZE (0x4000)
static byte __attribute__((aligned(32))) dcCdStdioBuf[DC_CD_STDIO_BUF_SIZE];

static inline void DC_TuneCDStream(FileIO *f)
{
    if (!f)
        return;

    // Must happen before any I/O on the stream, which is why every call site
    // sits directly under its fOpen and above the first fSeek.
    setvbuf(f, (char *)dcCdStdioBuf, _IOFBF, DC_CD_STDIO_BUF_SIZE);
}
#else
#define DC_TuneCDStream(f) ((void)0)
#endif

bool CheckRSDKFile(const char *filePath)
{
    FileInfo info;

    char filePathBuffer[0x100];
#if RETRO_PLATFORM == RETRO_OSX
    char pathBuf[0x100];
    sprintf(pathBuf, "%s/%s", gamePath, filePath);
    sprintf(filePathBuffer, "%s", pathBuf);
#else
    sprintf(filePathBuffer, "%s", filePath);
#endif

    cFileHandle = fOpen(filePathBuffer, "rb");
    DC_TuneCDStream(cFileHandle);
    if (cFileHandle) {
        byte signature[6] = { 'R', 'S', 'D', 'K', 'v', 'B' };
        byte buf          = 0;
        for (int i = 0; i < 6; ++i) {
            fRead(&buf, 1, 1, cFileHandle);
            if (buf != signature[i])
                return false;
        }

        Engine.usingDataFile = true;
#if !RETRO_USE_ORIGINAL_CODE
        Engine.usingDataFile_Config = true;
#endif

        StrCopy(rsdkContainer.packNames[rsdkContainer.packCount], filePathBuffer);

        byte b[4];
        fRead(&b, 2, 1, cFileHandle);
        ushort fileCount = (b[1] << 8) | (b[0] << 0);
        for (int f = 0; f < fileCount; ++f) {
            for (int y = 0; y < 4; ++y) {
                fRead(b, 1, 4, cFileHandle);
                rsdkContainer.files[f].hash[y] = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | (b[3] << 0);
            }

            fRead(b, 4, 1, cFileHandle);
            rsdkContainer.files[f].offset = (b[3] << 24) | (b[2] << 16) | (b[1] << 8) | (b[0] << 0);
            fRead(b, 4, 1, cFileHandle);
            rsdkContainer.files[f].filesize = (b[3] << 24) | (b[2] << 16) | (b[1] << 8) | (b[0] << 0);

            rsdkContainer.files[f].encrypted = (rsdkContainer.files[f].filesize & 0x80000000);
            rsdkContainer.files[f].filesize &= 0x7FFFFFFF;

            rsdkContainer.files[f].packID = rsdkContainer.packCount;

            rsdkContainer.fileCount++;
        }

        fClose(cFileHandle);
        cFileHandle = NULL;
        if (LoadFile("Bytecode/GlobalCode.bin", &info)) {
            Engine.usingBytecode = true;
            CloseFile();
        }
        PrintLog("loaded datapack '%s'", filePathBuffer);

        rsdkContainer.packCount++;
        return true;
    }
    else {
        Engine.usingDataFile = false;
#if !RETRO_USE_ORIGINAL_CODE
        Engine.usingDataFile_Config = false;
#endif
        cFileHandle = NULL;

        if (LoadFile("Bytecode/GlobalCode.bin", &info)) {
            Engine.usingBytecode = true;
            CloseFile();
        }
        PrintLog("Couldn't load datapack '%s'", filePathBuffer);
        return false;
    }
}

#if !RETRO_USE_ORIGINAL_CODE
int CheckFileInfo(const char *filepath)
{
    char pathBuf[0x100];
    StrCopy(pathBuf, filepath);
    uint hash[4];
    int len = StrLength(pathBuf);
    GenerateMD5FromString(pathBuf, len, &hash[0], &hash[1], &hash[2], &hash[3]);

    for (int f = 0; f < rsdkContainer.fileCount; ++f) {
        RSDKFileInfo *file = &rsdkContainer.files[f];

        bool match = true;
        for (int h = 0; h < 4; ++h) {
            if (hash[h] != file->hash[h]) {
                match = false;
                break;
            }
        }
        if (!match)
            continue;

        return f;
    }
    return -1;
}

inline bool ends_with(std::string const &value, std::string const &ending)
{
    if (ending.size() > value.size())
        return false;
    return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}
#endif

bool LoadFile(const char *filePath, FileInfo *fileInfo)
{
    MEM_ZEROP(fileInfo);

    if (cFileHandle)
        fClose(cFileHandle);

    char filePathBuf[0x100];
    StrCopy(filePathBuf, filePath);
    bool forceFolder = false;
#if RETRO_USE_MOD_LOADER
    // Fixes ".ani" ".Ani" bug and any other case differences
    char pathLower[0x100];
    memset(pathLower, 0, sizeof(char) * 0x100);
    for (int c = 0; c < strlen(filePathBuf); ++c) {
        pathLower[c] = tolower(filePathBuf[c]);
    }

    bool addPath = true;
    int m = activeMod != -1 ? activeMod : 0; 
    for (; m < modList.size(); ++m) {
        if (modList[m].active) {
            std::map<std::string, std::string>::const_iterator iter = modList[m].fileMap.find(pathLower);
            if (iter != modList[m].fileMap.cend()) {
                StrCopy(filePathBuf, iter->second.c_str());
                forceFolder = true;
                addPath     = false;
                break;
            }
        }
        if (activeMod != -1)
            break;
    }

    if (forceUseScripts && !forceFolder) {
        if (std::string(filePathBuf).rfind("Data/Scripts/", 0) == 0 && ends_with(std::string(filePathBuf), "txt")) {
            // is a script, since those dont exist normally, load them from "scripts/"
            forceFolder   = true;
            Engine.usingDataFile = false;
            addPath              = true;
            std::string fStr     = std::string(filePathBuf);
            fStr.erase(fStr.begin(), fStr.begin() + 5); // remove "Data/"
            StrCopy(filePathBuf, fStr.c_str());
        }
    }
#endif

#if RETRO_PLATFORM == RETRO_OSX || RETRO_PLATFORM == RETRO_ANDROID
#if RETRO_USE_MOD_LOADER
    if (addPath) {
#else
    if (true) {
#endif
        char pathBuf[0x100];
        sprintf(pathBuf, "%s/%s", gamePath, filePathBuf);
        sprintf(filePathBuf, "%s", pathBuf);
    }
#endif

    cFileHandle = NULL;
#if !RETRO_USE_ORIGINAL_CODE
    StringLowerCase(fileInfo->fileName, filePath);
    StrCopy(fileName, fileInfo->fileName);

    int fileIndex = CheckFileInfo(fileName);
    if (fileIndex != -1 && !forceFolder) {
        RSDKFileInfo *file = &rsdkContainer.files[fileIndex];
        packID      = file->packID;
        cFileHandle = fOpen(rsdkContainer.packNames[file->packID], "rb");
        DC_TuneCDStream(cFileHandle);
        if (cFileHandle) {
            fSeek(cFileHandle, 0, SEEK_END);
            fileSize = (int)fTell(cFileHandle);

            vFileSize         = file->filesize;
            virtualFileOffset = file->offset;
            readPos           = file->offset;
            readSize          = 0;
            bufferPosition    = 0;
            DC_SEEK_SET(cFileHandle, virtualFileOffset);

            useEncryption = file->encrypted;
            memset(fileInfo->encryptionStringA, 0, 0x10 * sizeof(byte));
            memset(fileInfo->encryptionStringB, 0, 0x10 * sizeof(byte));
            if (useEncryption) {
                GenerateELoadKeys(vFileSize, (vFileSize >> 1) + 1);
                eStringNo   = (vFileSize & 0x1FC) >> 2;
                eStringPosA = 0;
                eStringPosB = 8;
                eNybbleSwap = 0;
                memcpy(fileInfo->encryptionStringA, encryptionStringA, 0x10 * sizeof(byte));
                memcpy(fileInfo->encryptionStringB, encryptionStringB, 0x10 * sizeof(byte));
            }

            fileInfo->readPos           = readPos;
            fileInfo->fileSize          = fileSize;
            fileInfo->vfileSize         = vFileSize;
            fileInfo->virtualFileOffset = virtualFileOffset;
            fileInfo->eStringNo         = eStringNo;
            fileInfo->eStringPosB       = eStringPosB;
            fileInfo->eStringPosA       = eStringPosA;
            fileInfo->eNybbleSwap       = eNybbleSwap;
            fileInfo->bufferPosition    = bufferPosition;
            fileInfo->useEncryption     = useEncryption;
            fileInfo->packID            = packID;
            fileInfo->usingDataPack     = true;
            PrintLog("Loaded Data File '%s'", filePath);

            Engine.usingDataFile = true;

            return true;
        }
#else
    if (Engine.usingDataFile) {
        StringLowerCase(fileInfo->fileName, filePath);
        StrCopy(fileName, fileInfo->fileName);
        uint hash[4];
        int len = StrLength(fileInfo->fileName);
        GenerateMD5FromString(fileInfo->fileName, len, &hash[0], &hash[1], &hash[2], &hash[3]);

        for (int f = 0; f < rsdkContainer.fileCount; ++f) {
            RSDKFileInfo *file = &rsdkContainer.files[f];

            bool match = true;
            for (int h = 0; h < 4; ++h) {
                if (hash[h] != file->hash[h]) {
                    match = false;
                    break;
                }
            }
            if (!match)
                continue;

            packID      = file->packID;
            cFileHandle = fOpen(rsdkContainer.packNames[file->packID], "rb");
            DC_TuneCDStream(cFileHandle);
            if (cFileHandle) {
                fSeek(cFileHandle, 0, SEEK_END);
                fileSize = (int)fTell(cFileHandle);

                vFileSize         = file->filesize;
                virtualFileOffset = file->offset;
                readPos           = file->offset;
                readSize          = 0;
                bufferPosition    = 0;
                DC_SEEK_SET(cFileHandle, virtualFileOffset);

                useEncryption = file->encrypted;
                memset(fileInfo->encryptionStringA, 0, 0x10 * sizeof(byte));
                memset(fileInfo->encryptionStringB, 0, 0x10 * sizeof(byte));
                if (useEncryption) {
                    GenerateELoadKeys(vFileSize, (vFileSize >> 1) + 1);
                    eStringNo   = (vFileSize & 0x1FC) >> 2;
                    eStringPosA = 0;
                    eStringPosB = 8;
                    eNybbleSwap = 0;
                    memcpy(fileInfo->encryptionStringA, encryptionStringA, 0x10 * sizeof(byte));
                    memcpy(fileInfo->encryptionStringB, encryptionStringB, 0x10 * sizeof(byte));
                }

                fileInfo->readPos           = readPos;
                fileInfo->fileSize          = fileSize;
                fileInfo->vfileSize         = vFileSize;
                fileInfo->virtualFileOffset = virtualFileOffset;
                fileInfo->eStringNo         = eStringNo;
                fileInfo->eStringPosB       = eStringPosB;
                fileInfo->eStringPosA       = eStringPosA;
                fileInfo->eNybbleSwap       = eNybbleSwap;
                fileInfo->bufferPosition    = bufferPosition;
                fileInfo->useEncryption     = useEncryption;
                fileInfo->packID            = packID;
                fileInfo->usingDataPack     = true;
                PrintLog("Loaded Data File '%s'", filePath);

                return true;
            }
            else {
                break;
            }
        }
#endif
        PrintLog("Couldn't load file '%s'", filePath);
        return false;
    }
    else {
        StrCopy(fileInfo->fileName, filePathBuf);
        StrCopy(fileName, fileInfo->fileName);

        cFileHandle = fOpen(fileInfo->fileName, "rb");
        DC_TuneCDStream(cFileHandle);
        if (!cFileHandle) {
            PrintLog("Couldn't load file '%s'", filePath);
            return false;
        }
        virtualFileOffset = 0;
        fSeek(cFileHandle, 0, SEEK_END);
        fileInfo->fileSize = (int)fTell(cFileHandle);
        fileSize = fileInfo->vfileSize = fileInfo->fileSize;
        // The GLOBAL vFileSize was never set here, only the per-file copy, so it
        // stayed at whatever the last packed entry left behind. Nothing read it
        // on this path before (it only feeds the encryption keys, and loose
        // files are not encrypted) but FillFileBuffer's clamp does now.
        vFileSize = fileSize;
        fSeek(cFileHandle, 0, SEEK_SET);
        readPos           = 0;
        fileInfo->readPos = readPos;
        packID = fileInfo->packID = -1;
        fileInfo->usingDataPack   = false;
        bufferPosition            = 0;
        readSize                  = 0;
        useEncryption             = false;

#if !RETRO_USE_ORIGINAL_CODE
        Engine.usingDataFile = false;
#endif

        PrintLog("Loaded File '%s'", filePath);
        return true;
    }
}

#if !RETRO_USE_ORIGINAL_CODE
// See FillFileBuffer in Reader.hpp. Kept out of line so the header does not have
// to reach for PrintLog, which is declared in a header included after it.
void ReportShortFileRead(int got, int want, int at)
{
    PrintLog("FileRead: SHORT READ, got %d of %d bytes at %d in '%s' - the rest is zeroed", got, want, at, fileName);
}
#endif

void GenerateELoadKeys(uint key1, uint key2)
{
    char buffer[0x20];
    uint hash[0x4];

    // StringA
    ConvertIntegerToString(buffer, key1);
    int len = StrLength(buffer);
    GenerateMD5FromString(buffer, len, &hash[0], &hash[1], &hash[2], &hash[3]);

#if !RETRO_USE_ORIGINAL_CODE
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) encryptionStringA[i * 4 + j] = (hash[i] >> (8 * (j ^ 3))) & 0xFF;
#else
    for (int y = 0; y < 0x10; y += 4) {
        encryptionStringA[y + 3] = hash[y + 0];
        encryptionStringA[y + 2] = hash[y + 1];
        encryptionStringA[y + 1] = hash[y + 2];
        encryptionStringA[y + 0] = hash[y + 3];
    }
#endif

    // StringB
    ConvertIntegerToString(buffer, key2);
    len = StrLength(buffer);
    GenerateMD5FromString(buffer, len, &hash[0], &hash[1], &hash[2], &hash[3]);

#if !RETRO_USE_ORIGINAL_CODE
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) encryptionStringB[i * 4 + j] = (hash[i] >> (8 * (j ^ 3))) & 0xFF;
#else
    for (int y = 0; y < 0x10; y += 4) {
        encryptionStringB[y + 3] = hash[y + 0];
        encryptionStringB[y + 2] = hash[y + 1];
        encryptionStringB[y + 1] = hash[y + 2];
        encryptionStringB[y + 0] = hash[y + 3];
    }
#endif
}

const uint ENC_KEY_2 = 0x24924925;
const uint ENC_KEY_1 = 0xAAAAAAAB;
int mulUnsignedHigh(uint arg1, int arg2) { return (int)(((unsigned long long)arg1 * (unsigned long long)arg2) >> 32); }

void FileRead(void *dest, int size)
{
    byte *data = (byte *)dest;
    memset(data, 0, size);

    if (readPos <= fileSize) {
        if (useEncryption) {
            while (size > 0) {
                if (bufferPosition == readSize)
                    FillFileBuffer();

                *data = encryptionStringB[eStringPosB] ^ eStringNo ^ fileBuffer[bufferPosition++];
                if (eNybbleSwap)
                    *data = ((*data << 4) + (*data >> 4)) & 0xFF;
                *data ^= encryptionStringA[eStringPosA];

                ++eStringPosA;
                ++eStringPosB;
                if (eStringPosA <= 0x0F) {
                    if (eStringPosB > 0x0C) {
                        eStringPosB = 0;
                        eNybbleSwap ^= 0x01;
                    }
                }
                else if (eStringPosB <= 0x08) {
                    eStringPosA = 0;
                    eNybbleSwap ^= 0x01;
                }
                else {
                    eStringNo += 2;
                    eStringNo &= 0x7F;

                    if (eNybbleSwap != 0) {
                        int key1    = mulUnsignedHigh(ENC_KEY_1, eStringNo);
                        int key2    = mulUnsignedHigh(ENC_KEY_2, eStringNo);
                        eNybbleSwap = 0;

                        int temp1 = key2 + (eStringNo - key2) / 2;
                        int temp2 = key1 / 8 * 3;

                        eStringPosA = eStringNo - temp1 / 4 * 7;
                        eStringPosB = eStringNo - temp2 * 4 + 2;
                    }
                    else {
                        int key1    = mulUnsignedHigh(ENC_KEY_1, eStringNo);
                        int key2    = mulUnsignedHigh(ENC_KEY_2, eStringNo);
                        eNybbleSwap = 1;

                        int temp1 = key2 + (eStringNo - key2) / 2;
                        int temp2 = key1 / 8 * 3;

                        eStringPosB = eStringNo - temp1 / 4 * 7;
                        eStringPosA = eStringNo - temp2 * 4 + 3;
                    }
                }

                ++data;
                --size;
            }
        }
        else {
            while (size > 0) {
                if (bufferPosition == readSize)
                    FillFileBuffer();

                *data++ = fileBuffer[bufferPosition++];
                size--;
            }
        }
    }
}

void FileSkip(int count)
{
    if (readPos <= fileSize) {
        if (useEncryption) {
            while (count > 0) {
                if (bufferPosition == readSize)
                    FillFileBuffer();
                bufferPosition++;

                ++eStringPosA;
                ++eStringPosB;
                if (eStringPosA <= 0x0F) {
                    if (eStringPosB > 0x0C) {
                        eStringPosB = 0;
                        eNybbleSwap ^= 0x01;
                    }
                }
                else if (eStringPosB <= 0x08) {
                    eStringPosA = 0;
                    eNybbleSwap ^= 0x01;
                }
                else {
                    eStringNo += 2;
                    eStringNo &= 0x7F;

                    if (eNybbleSwap != 0) {
                        int key1    = mulUnsignedHigh(ENC_KEY_1, eStringNo);
                        int key2    = mulUnsignedHigh(ENC_KEY_2, eStringNo);
                        eNybbleSwap = 0;

                        int temp1 = key2 + (eStringNo - key2) / 2;
                        int temp2 = key1 / 8 * 3;

                        eStringPosA = eStringNo - temp1 / 4 * 7;
                        eStringPosB = eStringNo - temp2 * 4 + 2;
                    }
                    else {
                        int key1    = mulUnsignedHigh(ENC_KEY_1, eStringNo);
                        int key2    = mulUnsignedHigh(ENC_KEY_2, eStringNo);
                        eNybbleSwap = 1;

                        int temp1 = key2 + (eStringNo - key2) / 2;
                        int temp2 = key1 / 8 * 3;

                        eStringPosB = eStringNo - temp1 / 4 * 7;
                        eStringPosA = eStringNo - temp2 * 4 + 3;
                    }
                }

                --count;
            }
        }
        else {
            while (count > 0) {
                if (bufferPosition == readSize)
                    FillFileBuffer();
                bufferPosition++;
                count--;
            }
        }
    }
}

void GetFileInfo(FileInfo *fileInfo)
{
    StrCopy(fileInfo->fileName, fileName);
    fileInfo->bufferPosition    = bufferPosition;
    fileInfo->readPos           = readPos - readSize;
    fileInfo->fileSize          = fileSize;
    fileInfo->vfileSize         = vFileSize;
    fileInfo->virtualFileOffset = virtualFileOffset;
    fileInfo->eStringPosA       = eStringPosA;
    fileInfo->eStringPosB       = eStringPosB;
    fileInfo->eStringNo         = eStringNo;
    fileInfo->eNybbleSwap       = eNybbleSwap;
    fileInfo->useEncryption     = useEncryption;
    fileInfo->packID            = packID;
    fileInfo->usingDataPack     = Engine.usingDataFile;
    memcpy(encryptionStringA, fileInfo->encryptionStringA, 0x10 * sizeof(byte));
    memcpy(encryptionStringB, fileInfo->encryptionStringB, 0x10 * sizeof(byte));
}

void SetFileInfo(FileInfo *fileInfo)
{
#if !RETRO_USE_ORIGINAL_CODE
    if (fileInfo->usingDataPack) {
#else
    if (Engine.usingDataFile) {
#endif
        cFileHandle = fOpen(rsdkContainer.packNames[fileInfo->packID], "rb");
        DC_TuneCDStream(cFileHandle);
        if (cFileHandle) {
            virtualFileOffset = fileInfo->virtualFileOffset;
            vFileSize         = fileInfo->vfileSize;
            fSeek(cFileHandle, 0, SEEK_END);
            fileSize = (int)fTell(cFileHandle);
            readPos  = fileInfo->readPos;
            DC_SEEK_SET(cFileHandle, readPos);
            FillFileBuffer();
            bufferPosition       = fileInfo->bufferPosition;
            eStringPosA          = fileInfo->eStringPosA;
            eStringPosB          = fileInfo->eStringPosB;
            eStringNo            = fileInfo->eStringNo;
            eNybbleSwap          = fileInfo->eNybbleSwap;
            useEncryption        = fileInfo->useEncryption;
            packID               = fileInfo->packID;
            Engine.usingDataFile = fileInfo->usingDataPack;

            if (useEncryption) {
                GenerateELoadKeys(vFileSize, (vFileSize >> 1) + 1);
            }
        }
    }
    else {
        StrCopy(fileName, fileInfo->fileName);
        cFileHandle       = fOpen(fileInfo->fileName, "rb");
        DC_TuneCDStream(cFileHandle);
        virtualFileOffset = 0;
        fileSize          = fileInfo->fileSize;
        vFileSize         = fileSize; // same reason as in LoadFile above
        readPos           = fileInfo->readPos;
        DC_SEEK_SET(cFileHandle, readPos);
        FillFileBuffer();
        bufferPosition       = fileInfo->bufferPosition;
        eStringPosA          = 0;
        eStringPosB          = 0;
        eStringNo            = 0;
        eNybbleSwap          = 0;
        useEncryption        = fileInfo->useEncryption;
        packID               = fileInfo->packID;
        Engine.usingDataFile = fileInfo->usingDataPack;
    }
}

size_t GetFilePosition()
{
    if (Engine.usingDataFile)
        return bufferPosition + readPos - readSize - virtualFileOffset;
    else
        return bufferPosition + readPos - readSize;
}

void SetFilePosition(int newPos)
{
    if (useEncryption) {
        readPos     = virtualFileOffset + newPos;
        eStringNo   = (vFileSize & 0x1FC) >> 2;
        eStringPosA = 0;
        eStringPosB = 8;
        eNybbleSwap = false;
        while (newPos) {
            ++eStringPosA;
            ++eStringPosB;
            if (eStringPosA <= 0x0F) {
                if (eStringPosB > 0x0C) {
                    eStringPosB = 0;
                    eNybbleSwap ^= 0x01;
                }
            }
            else if (eStringPosB <= 0x08) {
                eStringPosA = 0;
                eNybbleSwap ^= 0x01;
            }
            else {
                eStringNo += 2;
                eStringNo &= 0x7F;

                if (eNybbleSwap != 0) {
                    int key1    = mulUnsignedHigh(ENC_KEY_1, eStringNo);
                    int key2    = mulUnsignedHigh(ENC_KEY_2, eStringNo);
                    eNybbleSwap = 0;

                    int temp1 = key2 + (eStringNo - key2) / 2;
                    int temp2 = key1 / 8 * 3;

                    eStringPosA = eStringNo - temp1 / 4 * 7;
                    eStringPosB = eStringNo - temp2 * 4 + 2;
                }
                else {
                    int key1    = mulUnsignedHigh(ENC_KEY_1, eStringNo);
                    int key2    = mulUnsignedHigh(ENC_KEY_2, eStringNo);
                    eNybbleSwap = 1;

                    int temp1 = key2 + (eStringNo - key2) / 2;
                    int temp2 = key1 / 8 * 3;

                    eStringPosB = eStringNo - temp1 / 4 * 7;
                    eStringPosA = eStringNo - temp2 * 4 + 3;
                }
            }
            --newPos;
        }
    }
    else {
        if (Engine.usingDataFile)
            readPos = virtualFileOffset + newPos;
        else
            readPos = newPos;
    }
    DC_SEEK_SET(cFileHandle, readPos);
    FillFileBuffer();
}

bool ReachedEndOfFile()
{
    if (Engine.usingDataFile)
        return bufferPosition + readPos - readSize - virtualFileOffset >= vFileSize;
    else
        return bufferPosition + readPos - readSize >= fileSize;
}
