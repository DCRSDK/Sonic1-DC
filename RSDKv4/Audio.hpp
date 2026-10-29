#ifndef AUDIO_H
#define AUDIO_H

#define TRACK_COUNT (0x10)
#define SFX_COUNT   (0x100)
#if !RETRO_USE_ORIGINAL_CODE
#define CHANNEL_COUNT (0x10) // 4 in the original, 16 for convenience
#else
#define CHANNEL_COUNT (0x4)
#endif

#define MAX_VOLUME (100)

#define MUSBUFFER_SIZE   (0x200000)
#define STREAMFILE_COUNT (2)

#define MIX_BUFFER_SAMPLES (256)

#if RETRO_USING_KOS
// Device output format for the Dreamcast backend (see Dreamcast/DCAudio.cpp)
#define AUDIO_FREQUENCY (44100)
#define AUDIO_CHANNELS  (2)
// VRAM staging size per stream slot for OGG SFX decode. Music used to need
// the full 2MB MUSBUFFER_SIZE here, but music now streams raw PCM from CD and
// never touches this VRAM — only small OGG SFX briefly stage into it while
// tremor decodes. A 512KB slot is far larger than any SFX .ogg, and shrinking
// from 2MB frees ~3MB of VRAM for hardware-sprite textures.
#define DC_SFX_STAGE_SIZE (0x80000) // 512 KB per slot
// Raw-PCM music streaming hooks (implemented in Dreamcast/DCAudio.cpp).
// Declared here so every TU that includes Audio.hpp (StopMusic/FreeMusInfo
// are inline) can see them without pulling in DCCommon.hpp.
//
// loopPoint and startFrame are PCM *frame* indices in the ORIGINAL 44100 Hz
// ogg the loop tables were authored against — the same units ov_pcm_seek takes
// on desktop — not byte offsets into the raw 22050 Hz file. DC_MusicOpen does
// the rescale. startFrame is 0 for a plain PlayMusic; SwapMusicTrack passes
// the scaled position of the outgoing track so the incoming one resumes in
// musical sync.
//
// Keep this signature byte-identical to the one in DCCommon.hpp. A stale copy
// here does not error out — it just becomes a second overload that nothing
// defines, so the mismatch only surfaces at link time or, worse, not at all.
bool DC_MusicOpen(const char *path, bool loop, uint loopPoint, uint startFrame);
// Position already heard, in original-ogg PCM frames (the DC ov_pcm_tell).
uint DC_MusicTell();
void DC_MusicClose();
#endif

#if RETRO_USING_SDL1 || RETRO_USING_SDL2

#define LockAudioDevice()   SDL_LockAudio()
#define UnlockAudioDevice() SDL_UnlockAudio()

#elif RETRO_USING_KOS
// Implemented in Dreamcast/DCAudio.cpp — a KOS mutex shared with the
// snd_stream polling thread.
void DC_LockAudio();
void DC_UnlockAudio();
#define LockAudioDevice()   DC_LockAudio()
#define UnlockAudioDevice() DC_UnlockAudio()
#else
#define LockAudioDevice()   ;
#define UnlockAudioDevice() ;
#endif

struct TrackInfo {
    char fileName[0x40];
    bool trackLoop;
    uint loopPoint;
};

struct StreamInfo {
#if !RETRO_USING_KOS
    // Dreamcast decodes no Vorbis (see the note in RetroEngine.hpp): this
    // member is the reason the type had to be complete, and therefore the
    // reason libtremor was still linked long after nothing called it.
    OggVorbis_File vorbisFile;
#endif
    int vorbBitstream;
#if RETRO_USING_SDL1
    SDL_AudioSpec spec;
#endif
#if RETRO_USING_SDL2
    SDL_AudioStream *stream;
#endif
    Sint16 buffer[MIX_BUFFER_SAMPLES];
    bool trackLoop;
    uint loopPoint;
    bool loaded;
};

struct SFXInfo {
    char name[0x40];
    Sint16 *buffer;
    size_t length;
    bool loaded;
#if RETRO_USING_KOS
    // Dreamcast: samples are stored in their native format (no upconversion
    // to device format — that would 4x the memory cost); the mixer converts
    // rate/channels on the fly.
    int rate;
    byte channels;
#endif
};

struct ChannelInfo {
    size_t sampleLength;
    Sint16 *samplePtr;
    int sfxID;
    byte loopSFX;
    sbyte pan;
#if RETRO_USING_KOS
    uint stepPos; // 16.16 fixed-point resample accumulator
#endif
};

struct StreamFile {
#if RETRO_USING_KOS
    // Dreamcast: the music file buffer lives in PVR VRAM (8MB, of which the
    // 320x240 framebuffer uses only ~150KB) instead of main RAM. Accessed
    // through DC_VramRead/DC_VramWrite (32-bit safe accesses only).
    uint vramBase; // byte offset into VRAM
#else
    byte buffer[MUSBUFFER_SIZE];
#endif
    int fileSize;
    int filePos;
};

enum MusicStatuses {
    MUSIC_STOPPED = 0,
    MUSIC_PLAYING = 1,
    MUSIC_PAUSED  = 2,
    MUSIC_LOADING = 3,
    MUSIC_READY   = 4,
};

extern int globalSFXCount;
extern int stageSFXCount;

extern int masterVolume;
extern int trackID;
extern int sfxVolume;
extern int bgmVolume;
extern bool audioEnabled;

extern bool musicEnabled;
extern int musicStatus;
extern int musicStartPos;
extern int musicPosition;
extern int musicRatio;
extern TrackInfo musicTracks[TRACK_COUNT];

extern int currentStreamIndex;
extern StreamFile streamFile[STREAMFILE_COUNT];
extern StreamInfo streamInfo[STREAMFILE_COUNT];
extern StreamFile *streamFilePtr;
extern StreamInfo *streamInfoPtr;

extern SFXInfo sfxList[SFX_COUNT];
extern char sfxNames[SFX_COUNT][0x40];

extern ChannelInfo sfxChannels[CHANNEL_COUNT];

#if RETRO_USING_SDL1 || RETRO_USING_SDL2
extern SDL_AudioSpec audioDeviceFormat;
#endif

int InitAudioPlayback();
void LoadGlobalSfx();

#if RETRO_USING_SDL1 || RETRO_USING_SDL2 || RETRO_USING_KOS
#if !RETRO_USE_ORIGINAL_CODE
// These functions did exist, but with different signatures
void ProcessMusicStream(Sint32 *stream, size_t bytes_wanted);
void ProcessAudioPlayback(void *data, Uint8 *stream, int len);
void ProcessAudioMixing(Sint32 *dst, const Sint16 *src, int len, int volume, sbyte pan);
#endif

#if !RETRO_USE_ORIGINAL_CODE
inline void FreeMusInfo()
{
    LockAudioDevice();

#if RETRO_USING_KOS
    DC_MusicClose();
#else
#if RETRO_USING_SDL2
    if (streamInfo[currentStreamIndex].stream)
        SDL_FreeAudioStream(streamInfo[currentStreamIndex].stream);
    streamInfo[currentStreamIndex].stream = NULL;
#endif

    ov_clear(&streamInfo[currentStreamIndex].vorbisFile);

#if RETRO_USING_SDL2
    streamInfo[currentStreamIndex].stream = nullptr;
#endif
#endif // RETRO_USING_KOS

    UnlockAudioDevice();
}
#endif
#else
void ProcessMusicStream() {}
void ProcessAudioPlayback() {}
void ProcessAudioMixing() {}

#if !RETRO_USE_ORIGINAL_CODE
inline void FreeMusInfo() { ov_clear(&streamInfo[currentStreamIndex].vorbisFile); }
#endif
#endif

void LoadMusic(void *userdata);
void SetMusicTrack(const char *filePath, byte trackID, bool loop, uint loopPoint);
void SwapMusicTrack(const char *filePath, byte trackID, uint loopPoint, uint ratio);
bool PlayMusic(int track, int musStartPos);
inline void StopMusic(bool setStatus)
{
    if (setStatus)
        musicStatus = MUSIC_STOPPED;
    musicPosition = 0;

#if !RETRO_USE_ORIGINAL_CODE
    LockAudioDevice();
    FreeMusInfo();
    UnlockAudioDevice();
#endif
}

#if RETRO_USING_KOS
// Dreamcast: a stage sfx may live in the AICA's own 2MB of sound RAM rather
// than on the main heap, playing on a hardware channel instead of through the
// software mixer (see the AICA block at the bottom of DCAudio.cpp). Those have
// no sfxChannels[] entry at all, so every stop/attribute path has to hand off
// here first. Declared in DCCommon.hpp too, but repeated because the inline
// functions just below are compiled before that header is reachable.
bool DC_AicaSfxHas(int sfxID);
bool DC_AicaSfxPlay(int sfxID, bool loop);
bool DC_AicaSfxSetAttr(int sfxID, int loopCount, sbyte pan);
void DC_AicaSfxStop(int sfxID);
void DC_AicaSfxStopAll();
void DC_AicaSfxUnloadStage();
#endif

void LoadSfx(char *filePath, byte sfxID);
void PlaySfx(int sfx, bool loop);
inline void StopSfx(int sfx)
{
#if RETRO_USING_KOS
    DC_AicaSfxStop(sfx);
#endif

    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        if (sfxChannels[i].sfxID == sfx) {
            MEM_ZERO(sfxChannels[i]);
            sfxChannels[i].sfxID = -1;
        }
    }
}
void SetSfxAttributes(int sfx, int loopCount, sbyte pan);

void SetSfxName(const char *sfxName, int sfxID);

#if !RETRO_USE_ORIGINAL_CODE
// Helper Funcs
inline bool PlaySfxByName(const char *sfx, sbyte loopCnt)
{
    char buffer[0x40];
    int pos = 0;
    while (*sfx) {
        if (*sfx != ' ')
            buffer[pos++] = *sfx;
        sfx++;
    }
    buffer[pos] = 0;

    for (int s = 0; s < globalSFXCount + stageSFXCount; ++s) {
        if (StrComp(sfxNames[s], buffer)) {
            PlaySfx(s, loopCnt);
            return true;
        }
    }
    return false;
}
inline bool StopSFXByName(const char *sfx)
{
    char buffer[0x40];
    int pos = 0;
    while (*sfx) {
        if (*sfx != ' ')
            buffer[pos++] = *sfx;
        sfx++;
    }
    buffer[pos] = 0;

    for (int s = 0; s < globalSFXCount + stageSFXCount; ++s) {
        if (StrComp(sfxNames[s], buffer)) {
            StopSfx(s);
            return true;
        }
    }
    return false;
}
#endif

inline void SetMusicVolume(int volume)
{
    if (volume < 0)
        volume = 0;
    if (volume > MAX_VOLUME)
        volume = MAX_VOLUME;

    masterVolume = volume;
}

inline void SetGameVolumes(int bgmVol, int sfxVol)
{
    bgmVolume = bgmVol;
    sfxVolume = sfxVol;

    if (bgmVolume < 0)
        bgmVolume = 0;
    if (bgmVolume > MAX_VOLUME)
        bgmVolume = MAX_VOLUME;

    if (sfxVolume < 0)
        sfxVolume = 0;
    if (sfxVolume > MAX_VOLUME)
        sfxVolume = MAX_VOLUME;
}

inline bool PauseSound()
{
    if (musicStatus == MUSIC_PLAYING) {
        musicStatus = MUSIC_PAUSED;
        return true;
    }
    return false;
}

inline void ResumeSound()
{
    if (musicStatus == MUSIC_PAUSED)
        musicStatus = MUSIC_PLAYING;
}

inline void StopAllSfx()
{
#if !RETRO_USE_ORIGINAL_CODE
    LockAudioDevice();
#endif

    for (int i = 0; i < CHANNEL_COUNT; ++i) sfxChannels[i].sfxID = -1;

#if RETRO_USING_KOS
    // The hardware channels aren't in sfxChannels[], so clearing that array
    // leaves them running. Silence them here too.
    DC_AicaSfxStopAll();
#endif

#if !RETRO_USE_ORIGINAL_CODE
    UnlockAudioDevice();
#endif
}
#if RETRO_USING_KOS
// Dreamcast: a stage sfx buffer may live in the bump arena rather than on the
// heap (see the arena block in DCAudio.cpp), and an arena pointer must never
// reach free(). DC_SfxFree checks which it is, so it is correct for every sfx
// buffer regardless of where that one came from. DC_SfxArenaReset then reclaims
// the whole arena in one go, once the individual buffers have been let go.
void DC_SfxFree(void *ptr);
void DC_SfxArenaReset();
// Claims the arena block. Deliberately NOT called at audio init — see the arena
// block in DCAudio.cpp; Scene.cpp calls it at the top of each stage load.
void DC_SfxArenaAcquire();
#define RETRO_SFX_FREE(ptr) DC_SfxFree(ptr)
#else
#define RETRO_SFX_FREE(ptr) free(ptr)
#endif

inline void ReleaseGlobalSfx()
{
    for (int i = globalSFXCount - 1; i >= 0; --i) {
        if (sfxList[i].loaded) {
            StrCopy(sfxList[i].name, "");
            StrCopy(sfxNames[i], "");
            if (sfxList[i].buffer)
                RETRO_SFX_FREE(sfxList[i].buffer);

            sfxList[i].buffer = NULL;
            sfxList[i].length = 0;
            sfxList[i].loaded = false;
        }
    }

    globalSFXCount = 0;
}
inline void ReleaseStageSfx()
{
#if RETRO_USING_KOS
    // Stage sfx that were uploaded to the AICA have no buffer to free, so the
    // loop below skips straight past them — the sound RAM they hold has to be
    // handed back explicitly. Stops them first, so nothing is still reading a
    // buffer that is about to be handed out again.
    DC_AicaSfxUnloadStage();
#endif

    for (int i = (stageSFXCount + globalSFXCount) - 1; i >= globalSFXCount; --i) {
        if (sfxList[i].loaded) {
            StrCopy(sfxList[i].name, "");
            StrCopy(sfxNames[i], "");
            if (sfxList[i].buffer)
                RETRO_SFX_FREE(sfxList[i].buffer);

            sfxList[i].buffer = NULL;
            sfxList[i].length = 0;
            sfxList[i].loaded = false;
        }
    }

    stageSFXCount = 0;

#if RETRO_USING_KOS
    // Every stage sfx has now been released, so the arena is empty by
    // definition — rewind it. This is what makes the next stage's layout
    // identical to this one's instead of being whatever the heap left behind.
    DC_SfxArenaReset();
#endif
}

inline void ReleaseAudioDevice()
{
    StopMusic(true);
    StopAllSfx();
    ReleaseStageSfx();
    ReleaseGlobalSfx();
}

#endif // !AUDIO_H
