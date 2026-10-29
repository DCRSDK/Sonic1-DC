#include "RetroEngine.hpp"
#include <cmath>

#if RETRO_USING_KOS
#include "Dreamcast/DCCommon.hpp"
#endif

int globalSFXCount = 0;
int stageSFXCount  = 0;

int masterVolume  = MAX_VOLUME;
int trackID       = -1;
int sfxVolume     = MAX_VOLUME;
int bgmVolume     = MAX_VOLUME;
bool audioEnabled = false;

bool musicEnabled = 0;
int musicStatus   = MUSIC_STOPPED;
int musicStartPos = 0;
int musicPosition = 0;
int musicRatio    = 0;
TrackInfo musicTracks[TRACK_COUNT];
SFXInfo sfxList[SFX_COUNT];
char sfxNames[SFX_COUNT][0x40];

int currentStreamIndex = 0;
StreamFile streamFile[STREAMFILE_COUNT];
StreamInfo streamInfo[STREAMFILE_COUNT];
StreamFile *streamFilePtr = NULL;
StreamInfo *streamInfoPtr = NULL;

ChannelInfo sfxChannels[CHANNEL_COUNT];

int currentMusicTrack = -1;

#if RETRO_USING_SDL1 || RETRO_USING_SDL2

#if RETRO_USING_SDL2
SDL_AudioDeviceID audioDevice;
#endif
SDL_AudioSpec audioDeviceFormat;

#define AUDIO_FREQUENCY (44100)
#define AUDIO_FORMAT    (AUDIO_S16SYS) /**< Signed 16-bit samples */
#define AUDIO_SAMPLES   (0x800)
#define AUDIO_CHANNELS  (2)

#define ADJUST_VOLUME(s, v) (s = (s * v) / MAX_VOLUME)
#endif

#if RETRO_USING_KOS
// AUDIO_FREQUENCY / AUDIO_CHANNELS come from Audio.hpp
#define ADJUST_VOLUME(s, v) (s = (s * v) / MAX_VOLUME)
#endif

int InitAudioPlayback()
{
    StopAllSfx(); //"init"

#if !RETRO_USE_ORIGINAL_CODE
#if RETRO_USING_SDL1 || RETRO_USING_SDL2
    SDL_AudioSpec want;
    want.freq     = AUDIO_FREQUENCY;
    want.format   = AUDIO_FORMAT;
    want.samples  = AUDIO_SAMPLES;
    want.channels = AUDIO_CHANNELS;
    want.callback = ProcessAudioPlayback;

#if RETRO_USING_SDL2
    if ((audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &audioDeviceFormat, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE)) > 0) {
        audioEnabled = true;
        SDL_PauseAudioDevice(audioDevice, 0);
    }
    else {
        PrintLog("Unable to open audio device: %s", SDL_GetError());
        audioEnabled = false;
        return true; // no audio but game wont crash now
    }
#elif RETRO_USING_SDL1
    if (SDL_OpenAudio(&want, &audioDeviceFormat) == 0) {
        audioEnabled = true;
        SDL_PauseAudio(0);
    }
    else {
        PrintLog("Unable to open audio device: %s", SDL_GetError());
        audioEnabled = false;
        return true; // no audio but game wont crash now
    }
#endif // !RETRO_USING_SDL1
#elif RETRO_USING_KOS
    if (DC_InitAudioDevice()) {
        audioEnabled = true;
    }
    else {
        PrintLog("Unable to init KOS audio");
        audioEnabled = false;
        return true; // no audio but game wont crash now
    }
#endif
#endif

    LoadGlobalSfx();

    return true;
}

void LoadGlobalSfx()
{
    FileInfo info;
    FileInfo infoStore;
    char strBuffer[0x100];
    byte fileBuffer = 0;
    int fileBuffer2 = 0;

    globalSFXCount = 0;

    if (LoadFile("Data/Game/GameConfig.bin", &info)) {
        infoStore = info;

        FileRead(&fileBuffer, 1);
        FileRead(strBuffer, fileBuffer);

        FileRead(&fileBuffer, 1);
        FileRead(strBuffer, fileBuffer);

        byte buf[3];
        for (int c = 0; c < 0x60; ++c) FileRead(buf, 3);

        // Read Obect Names
        byte objectCount = 0;
        FileRead(&objectCount, 1);
        for (byte o = 0; o < objectCount; ++o) {
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);
        }

        // Read Script Paths
        for (byte s = 0; s < objectCount; ++s) {
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);
        }

        byte varCount = 0;
        FileRead(&varCount, 1);
        for (byte v = 0; v < varCount; ++v) {
            // Read Variable Name
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);

            // Read Variable Value
            FileRead(&fileBuffer2, 4);
        }

        // Read SFX
        FileRead(&fileBuffer, 1);
        globalSFXCount = fileBuffer;
        for (byte s = 0; s < globalSFXCount; ++s) { // SFX Names
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);
            strBuffer[fileBuffer] = 0;

            SetSfxName(strBuffer, s);
        }
        for (byte s = 0; s < globalSFXCount; ++s) { // SFX Paths
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);
            strBuffer[fileBuffer] = 0;

            GetFileInfo(&infoStore);
            CloseFile();
            LoadSfx(strBuffer, s);
            SetFileInfo(&infoStore);
        }

        CloseFile();

#if RETRO_USE_MOD_LOADER
        Engine.LoadXMLSoundFX();
#endif
    }

    for (int i = 0; i < CHANNEL_COUNT; ++i) sfxChannels[i].sfxID = -1;
}

#if !RETRO_USING_KOS
// Vorbis stream callbacks. Compiled out on Dreamcast: nothing there decodes
// Vorbis any more, and seekVorbis' ogg_int64_t parameter would drag the ogg
// headers back in on its own even though no caller remains.
size_t readVorbis(void *mem, size_t size, size_t nmemb, void *ptr)
{
    StreamFile *file = (StreamFile *)ptr;

    int n = size * nmemb;
    if (size * nmemb > file->fileSize - file->filePos)
        n = file->fileSize - file->filePos;

    if (n) {
#if RETRO_USING_KOS
        DC_VramRead(mem, file->vramBase + file->filePos, n);
#else
        memcpy(mem, &file->buffer[file->filePos], n);
#endif
        file->filePos += n;
    }
    return n;
}
int seekVorbis(void *ptr, ogg_int64_t offset, int whence)
{
    StreamFile *file = (StreamFile *)ptr;

    switch (whence) {
        case SEEK_SET: whence = 0; break;
        case SEEK_CUR: whence = file->filePos; break;
        case SEEK_END: whence = file->fileSize; break;
        default: break;
    }
    file->filePos = whence + offset;
    return 0;
}
long tellVorbis(void *ptr)
{
    StreamFile *file = (StreamFile *)ptr;
    return file->filePos;
}
int closeVorbis(void *ptr) { return 1; }
#endif // !RETRO_USING_KOS

#if !RETRO_USE_ORIGINAL_CODE
void ProcessMusicStream(Sint32 *stream, size_t bytes_wanted)
{
#if !RETRO_USING_KOS
    if (!streamFilePtr || !streamInfoPtr)
        return;
    if (!streamFilePtr->fileSize)
        return;
#endif
    switch (musicStatus) {
        case MUSIC_READY:
        case MUSIC_PLAYING: {
#if RETRO_USING_KOS
            // Stream raw PCM from CD + upsample straight into the mix buffer
            static Sint16 dcMusicBuf[MIX_BUFFER_SAMPLES];
            size_t samplesWanted = bytes_wanted / sizeof(Sint16);
            if (samplesWanted > MIX_BUFFER_SAMPLES)
                samplesWanted = MIX_BUFFER_SAMPLES;

            int samplesDone = DC_MusicReadSamples(dcMusicBuf, samplesWanted);
            if (samplesDone > 0)
                ProcessAudioMixing(stream, dcMusicBuf, samplesDone, (bgmVolume * masterVolume) / MAX_VOLUME, 0);
#endif
#if RETRO_USING_SDL2
            while (musicStatus == MUSIC_PLAYING && streamInfoPtr->stream && SDL_AudioStreamAvailable(streamInfoPtr->stream) < bytes_wanted) {
                // We need more samples: get some
                long bytes_read = ov_read(&streamInfoPtr->vorbisFile, (char *)streamInfoPtr->buffer, sizeof(streamInfoPtr->buffer), 0, 2, 1,
                                          &streamInfoPtr->vorbBitstream);

                if (bytes_read == 0) {
                    // We've reached the end of the file
                    if (streamInfoPtr->trackLoop) {
                        ov_pcm_seek(&streamInfoPtr->vorbisFile, streamInfoPtr->loopPoint);
                        continue;
                    }
                    else {
                        musicStatus = MUSIC_STOPPED;
                        break;
                    }
                }

                if (musicStatus != MUSIC_PLAYING
                    || (streamInfoPtr->stream && SDL_AudioStreamPut(streamInfoPtr->stream, streamInfoPtr->buffer, (int)bytes_read) == -1))
                    return;
            }

            // Now that we know there are enough samples, read them and mix them
            int bytes_done = SDL_AudioStreamGet(streamInfoPtr->stream, streamInfoPtr->buffer, (int)bytes_wanted);
            if (bytes_done == -1) {
                return;
            }
            if (bytes_done != 0)
                ProcessAudioMixing(stream, streamInfoPtr->buffer, bytes_done / sizeof(Sint16), (bgmVolume * masterVolume) / MAX_VOLUME, 0);
#endif

#if RETRO_USING_SDL1
            size_t bytes_gotten = 0;
            byte *buffer        = (byte *)malloc(bytes_wanted);
            memset(buffer, 0, bytes_wanted);
            while (bytes_gotten < bytes_wanted) {
                // We need more samples: get some
                long bytes_read =
                    ov_read(&oggFilePtr->vorbisFile, (char *)oggFilePtr->buffer,
                            sizeof(oggFilePtr->buffer) > (bytes_wanted - bytes_gotten) ? (bytes_wanted - bytes_gotten) : sizeof(oggFilePtr->buffer),
                            0, 2, 1, &oggFilePtr->vorbBitstream);

                if (bytes_read == 0) {
                    // We've reached the end of the file
                    if (oggFilePtr->trackLoop) {
                        ov_pcm_seek(&oggFilePtr->vorbisFile, oggFilePtr->loopPoint);
                        continue;
                    }
                    else {
                        musicStatus = MUSIC_STOPPED;
                        break;
                    }
                }

                if (bytes_read > 0) {
                    memcpy(buffer + bytes_gotten, oggFilePtr->buffer, bytes_read);
                    bytes_gotten += bytes_read;
                }
                else {
                    PrintLog("Music read error: vorbis error: %d", bytes_read);
                }
            }

            if (bytes_gotten > 0) {
                SDL_AudioCVT convert;
                MEM_ZERO(convert);
                int cvtResult = SDL_BuildAudioCVT(&convert, oggFilePtr->spec.format, oggFilePtr->spec.channels, oggFilePtr->spec.freq,
                                                  audioDeviceFormat.format, audioDeviceFormat.channels, audioDeviceFormat.freq);
                if (cvtResult == 0) {
                    if (convert.len_mult > 0) {
                        convert.buf = (byte *)malloc(bytes_gotten * convert.len_mult);
                        convert.len = bytes_gotten;
                        memcpy(convert.buf, buffer, bytes_gotten);
                        SDL_ConvertAudio(&convert);
                    }
                }

                // Now that we know there are enough samples, read them and mix them
                // int bytes_done = SDL_AudioStreamGet(oggFilePtr->stream, oggFilePtr->buffer, bytes_wanted);
                // if (bytes_done == -1) {
                //    return;
                //}

                if (cvtResult == 0)
                    ProcessAudioMixing(stream, (const Sint16 *)convert.buf, bytes_gotten / sizeof(Sint16), (bgmVolume * masterVolume) / MAX_VOLUME,
                                       0);

                if (convert.len > 0 && convert.buf)
                    free(convert.buf);
            }
            if (bytes_wanted > 0)
                free(buffer);
#endif

#if !RETRO_USING_KOS
            // KOS streams raw PCM with no OggVorbis_File / streamInfoPtr —
            // this line dereferences a NULL streamInfoPtr on Dreamcast and
            // faults the audio thread the instant music starts. (This was
            // the real title-screen crash.) musicPosition is only used by
            // the music-swap ratio feature, which the raw-PCM path skips.
            musicPosition = ov_pcm_tell(&streamInfoPtr->vorbisFile);
#endif
            break;
        }
        case MUSIC_STOPPED:
        case MUSIC_PAUSED:
        case MUSIC_LOADING:
            // dont play
            break;
    }
}

void ProcessAudioPlayback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata; // Unused

    if (!audioEnabled)
        return;

    Sint16 *output_buffer = (Sint16 *)stream;

    size_t samples_remaining = (size_t)len / sizeof(Sint16);
    while (samples_remaining != 0) {
        Sint32 mix_buffer[MIX_BUFFER_SAMPLES];
        memset(mix_buffer, 0, sizeof(mix_buffer));

        const size_t samples_to_do = (samples_remaining < MIX_BUFFER_SAMPLES) ? samples_remaining : MIX_BUFFER_SAMPLES;

        // Mix music
        ProcessMusicStream(mix_buffer, samples_to_do * sizeof(Sint16));

        // Mix SFX
        for (byte i = 0; i < CHANNEL_COUNT; ++i) {
            ChannelInfo *sfx = &sfxChannels[i];
            if (sfx == NULL)
                continue;

            if (sfx->sfxID < 0)
                continue;

#if RETRO_USING_KOS
            if (sfx->samplePtr) {
                // samples are in native rate/channels; convert while mixing
                Sint16 buffer[MIX_BUFFER_SAMPLES];
                SFXInfo *sfxData = &sfxList[sfx->sfxID];
                const int schan  = sfxData->channels >= 2 ? 2 : 1;
                const uint step  = (uint)(((Uint64)sfxData->rate << 16) / AUDIO_FREQUENCY);

                size_t frames_done        = 0;
                const size_t frames_to_do = samples_to_do / 2;
                while (frames_done < frames_to_do && sfx->samplePtr) {
                    Sint16 l = sfx->samplePtr[0];
                    Sint16 r = (schan == 2 && sfx->sampleLength > 1) ? sfx->samplePtr[1] : l;

                    buffer[frames_done * 2]     = l;
                    buffer[frames_done * 2 + 1] = r;
                    ++frames_done;

                    sfx->stepPos += step;
                    size_t adv = (size_t)(sfx->stepPos >> 16) * schan;
                    sfx->stepPos &= 0xFFFF;

                    if (adv >= sfx->sampleLength) {
                        if (sfx->loopSFX) {
                            sfx->samplePtr    = sfxData->buffer;
                            sfx->sampleLength = sfxData->length;
                            sfx->stepPos      = 0;
                        }
                        else {
                            MEM_ZEROP(sfx);
                            sfx->sfxID = -1;
                            break;
                        }
                    }
                    else {
                        sfx->samplePtr += adv;
                        sfx->sampleLength -= adv;
                    }
                }

                if (frames_done > 0)
                    ProcessAudioMixing(mix_buffer, buffer, (int)(frames_done * 2), sfxVolume, sfx->pan);
            }
#else
            if (sfx->samplePtr) {
                Sint16 buffer[MIX_BUFFER_SAMPLES];

                size_t samples_done = 0;
                while (samples_done != samples_to_do) {
                    size_t sampleLen = (sfx->sampleLength < samples_to_do - samples_done) ? sfx->sampleLength : samples_to_do - samples_done;
                    memcpy(&buffer[samples_done], sfx->samplePtr, sampleLen * sizeof(Sint16));

                    samples_done += sampleLen;
                    sfx->samplePtr += sampleLen;
                    sfx->sampleLength -= sampleLen;

                    if (sfx->sampleLength == 0) {
                        if (sfx->loopSFX) {
                            sfx->samplePtr    = sfxList[sfx->sfxID].buffer;
                            sfx->sampleLength = sfxList[sfx->sfxID].length;
                        }
                        else {
                            MEM_ZEROP(sfx);
                            sfx->sfxID = -1;
                            break;
                        }
                    }
                }

#if RETRO_USING_SDL1 || RETRO_USING_SDL2
                ProcessAudioMixing(mix_buffer, buffer, (int)samples_done, sfxVolume, sfx->pan);
#endif
            }
#endif // RETRO_USING_KOS
        }

        // Clamp mixed samples back to 16-bit and write them to the output buffer
#if RETRO_USING_KOS
        // only write what was actually requested — writing the full chunk
        // regardless (as below) overruns the output buffer when the request
        // isn't a chunk multiple
        for (size_t i = 0; i < samples_to_do; ++i) {
#else
        for (size_t i = 0; i < sizeof(mix_buffer) / sizeof(*mix_buffer); ++i) {
#endif
            const Sint16 max_audioval = ((1 << (16 - 1)) - 1);
            const Sint16 min_audioval = -(1 << (16 - 1));

            const Sint32 sample = mix_buffer[i];

            if (sample > max_audioval)
                *output_buffer++ = max_audioval;
            else if (sample < min_audioval)
                *output_buffer++ = min_audioval;
            else
                *output_buffer++ = sample;
        }

        samples_remaining -= samples_to_do;
    }
}

#if RETRO_USING_SDL1 || RETRO_USING_SDL2 || RETRO_USING_KOS
void ProcessAudioMixing(Sint32 *dst, const Sint16 *src, int len, int volume, sbyte pan)
{
    if (volume == 0)
        return;

    if (volume > MAX_VOLUME)
        volume = MAX_VOLUME;

    float panL = 0.0;
    float panR = 0.0;
    int i      = 0;

    if (pan < 0) {
        panR = 1.0f - abs(pan / 100.0f);
        panL = 1.0f;
    }
    else if (pan > 0) {
        panL = 1.0f - abs(pan / 100.0f);
        panR = 1.0f;
    }

    while (len--) {
        Sint32 sample = *src++;
        ADJUST_VOLUME(sample, volume);

        if (pan != 0) {
            if ((i % 2) != 0) {
                sample *= panR;
            }
            else {
                sample *= panL;
            }
        }

        *dst++ += sample;

        i++;
    }
}
#endif
#endif

void LoadMusic(void *userdata)
{
#if RETRO_USING_KOS
    // Dreamcast: stream raw 8-bit PCM straight from the loose file on the CD
    // (bypassing the datafile), no vorbis decode. Music must be present at
    // /cd/Data/Music/*.ogg (raw s8, per the conversion scripts).
    LockAudioDevice();

    // Mirror of the vorbis path's musicStartPos handling below. SwapMusicTrack
    // (speed shoes on/off, invincibility starting/ending) sets musicStartPos so
    // the incoming track picks up where the outgoing one left off, scaled by
    // musicRatio because the "_F" twins run at a different tempo — 8000 is 0.8,
    // 12500 is 1.25, and the divisor is 10,000. Without this the DC branch
    // restarted every swap from zero, so grabbing an invincibility box and
    // waiting it out dropped you back at the top of the stage theme.
    //
    // The read has to happen BEFORE DC_MusicClose(), which drops the handle the
    // position is measured from.
    uint dcStartPos = 0;
    if (musicStartPos) {
        float newPos = (float)DC_MusicTell() * ((float)musicRatio * 0.0001f);
        if (newPos > 0.0f)
            dcStartPos = (uint)newPos; // DC_MusicOpen wraps this into the new track
    }

    DC_MusicClose();
    char dcPath[0x120];
    sprintf(dcPath, "%s%s", BASE_PATH, musicTracks[currentMusicTrack].fileName);
    if (DC_MusicOpen(dcPath, musicTracks[currentMusicTrack].trackLoop, musicTracks[currentMusicTrack].loopPoint, dcStartPos)) {
        musicStatus  = MUSIC_PLAYING;
        masterVolume = MAX_VOLUME;
        trackID      = currentMusicTrack;
    }
    else {
        musicStatus = MUSIC_STOPPED;
    }
    musicStartPos     = 0;
    currentMusicTrack = -1;
    musicPosition     = 0;
    UnlockAudioDevice();
    return;
#endif

    int oldStreamID = currentStreamIndex;
    currentStreamIndex++;
    currentStreamIndex %= STREAMFILE_COUNT;

    LockAudioDevice();

    if (streamFile[currentStreamIndex].fileSize > 0)
        StopMusic(false);

    FileInfo info;
    if (LoadFile(musicTracks[currentMusicTrack].fileName, &info)) {
        StreamInfo *strmInfo = &streamInfo[currentStreamIndex];

        StreamFile *musFile = &streamFile[currentStreamIndex];
        musFile->filePos    = 0;
        musFile->fileSize   = info.vfileSize;
        if (info.vfileSize > MUSBUFFER_SIZE)
            musFile->fileSize = MUSBUFFER_SIZE;

#if RETRO_USING_KOS
        {
            // stream the file into VRAM through a small staging buffer
            // (VRAM only tolerates 32-bit accesses)
            static byte staging[0x2000] __attribute__((aligned(4)));
            int remaining = musFile->fileSize;
            uint writePos = 0;
            while (remaining > 0) {
                int chunk = remaining > (int)sizeof(staging) ? (int)sizeof(staging) : remaining;
                FileRead(staging, chunk);
                DC_VramWrite(musFile->vramBase + writePos, staging, chunk);
                writePos += chunk;
                remaining -= chunk;
            }
        }
#else
        FileRead(streamFile[currentStreamIndex].buffer, musFile->fileSize);
#endif
        CloseFile();

#if !RETRO_USING_KOS
        unsigned long long samples = 0;
        ov_callbacks callbacks;

        callbacks.read_func  = readVorbis;
        callbacks.seek_func  = seekVorbis;
        callbacks.tell_func  = tellVorbis;
        callbacks.close_func = closeVorbis;

        int error = ov_open_callbacks(musFile, &strmInfo->vorbisFile, NULL, 0, callbacks);
        if (error == 0) {
            strmInfo->vorbBitstream = -1;
#if RETRO_USING_KOS
            // tremor: vorbisFile.vi is an embedded struct maintained by the
            // library itself — nothing to assign (use ov_info() to read it)
#else
            strmInfo->vorbisFile.vi = ov_info(&strmInfo->vorbisFile, -1);
#endif

            samples = (unsigned long long)ov_pcm_total(&strmInfo->vorbisFile, -1);

#if RETRO_USING_SDL2
            strmInfo->stream = SDL_NewAudioStream(AUDIO_S16, strmInfo->vorbisFile.vi->channels, (int)strmInfo->vorbisFile.vi->rate,
                                                  audioDeviceFormat.format, audioDeviceFormat.channels, audioDeviceFormat.freq);
            if (!strmInfo->stream)
                PrintLog("Failed to create stream: %s", SDL_GetError());
#endif

#if RETRO_USING_SDL1
            playbackInfo->spec.format   = AUDIO_S16;
            playbackInfo->spec.channels = playbackInfo->vorbisFile.vi->channels;
            playbackInfo->spec.freq     = (int)playbackInfo->vorbisFile.vi->rate;
#endif

            if (musicStartPos) {
                uint oldPos = (uint)ov_pcm_tell(&streamInfo[oldStreamID].vorbisFile);

                float newPos  = oldPos * ((float)musicRatio * 0.0001); // 8,000 == 0.8, 10,000 == 1.0 (ratio / 10,000)
                musicStartPos = fmod(newPos, samples);

                ov_pcm_seek(&strmInfo->vorbisFile, musicStartPos);
            }
            musicStartPos = 0;

            musicStatus         = MUSIC_PLAYING;
            masterVolume        = MAX_VOLUME;
            trackID             = currentMusicTrack;
            strmInfo->trackLoop = musicTracks[currentMusicTrack].trackLoop;
            strmInfo->loopPoint = musicTracks[currentMusicTrack].loopPoint;
            strmInfo->loaded    = true;
            streamFilePtr       = &streamFile[currentStreamIndex];
            streamInfoPtr       = &streamInfo[currentStreamIndex];
            currentMusicTrack   = -1;
            musicPosition       = 0;
        }
        else {
            musicStatus = MUSIC_STOPPED;
            PrintLog("Failed to load vorbis! error: %d", error);
            switch (error) {
                default: PrintLog("Vorbis open error: Unknown (%d)", error); break;
                case OV_EREAD: PrintLog("Vorbis open error: A read from media returned an error"); break;
                case OV_ENOTVORBIS: PrintLog("Vorbis open error: Bitstream does not contain any Vorbis data"); break;
                case OV_EVERSION: PrintLog("Vorbis open error: Vorbis version mismatch"); break;
                case OV_EBADHEADER: PrintLog("Vorbis open error: Invalid Vorbis bitstream header"); break;
                case OV_EFAULT: PrintLog("Vorbis open error: Internal logic fault; indicates a bug or heap / stack corruption"); break;
            }
        }
#endif // !RETRO_USING_KOS
    }
    else {
        musicStatus = MUSIC_STOPPED;
    }
    UnlockAudioDevice();
}

void SetMusicTrack(const char *filePath, byte trackID, bool loop, uint loopPoint)
{
    LockAudioDevice();
    TrackInfo *track = &musicTracks[trackID];
    StrCopy(track->fileName, "Data/Music/");
    StrAdd(track->fileName, filePath);
    track->trackLoop = loop;
    track->loopPoint = loopPoint;
    UnlockAudioDevice();
}

void SwapMusicTrack(const char *filePath, byte trackID, uint loopPoint, uint ratio)
{
    if (StrLength(filePath) <= 0) {
        StopMusic(true);
    }
    else {
        LockAudioDevice();
        TrackInfo *track = &musicTracks[trackID];
        StrCopy(track->fileName, "Data/Music/");
        StrAdd(track->fileName, filePath);
        track->trackLoop = true;
        track->loopPoint = loopPoint;
        musicRatio       = ratio;
        UnlockAudioDevice();
        PlayMusic(trackID, 1);
    }
}

bool PlayMusic(int track, int musStartPos)
{
    if (!audioEnabled)
        return false;

    if (musicTracks[track].fileName[0]) {
        if (musicStatus != MUSIC_LOADING) {
            LockAudioDevice();
            if (track < 0 || track >= TRACK_COUNT) {
                StopMusic(true);
                currentMusicTrack = -1;
                return false;
            }
            musicStartPos     = musStartPos;
            currentMusicTrack = track;
            musicStatus       = MUSIC_LOADING;
            LoadMusic(NULL);
            UnlockAudioDevice();
            return true;
        }
        else {
            PrintLog("WARNING music tried to play while music was loading!");
        }
    }
    else {
        StopMusic(true);
    }

    return false;
}

void SetSfxName(const char *sfxName, int sfxID)
{
    int sfxNameID   = 0;
    int soundNameID = 0;
    while (sfxName[sfxNameID]) {
        if (sfxName[sfxNameID] != ' ')
            sfxNames[sfxID][soundNameID++] = sfxName[sfxNameID];
        ++sfxNameID;
    }
    sfxNames[sfxID][soundNameID] = 0;
    PrintLog("Set SFX (%d) name to: %s", sfxID, sfxName);
}

void LoadSfx(char *filePath, byte sfxID)
{
    if (!audioEnabled)
        return;

    FileInfo info;
    char fullPath[0x80];

    StrCopy(fullPath, "Data/SoundFX/");
    StrAdd(fullPath, filePath);

#if RETRO_USING_KOS
    // A loose raw-PCM copy next to Data.rsdk wins over the packed .ogg. This
    // has to happen BEFORE LoadFile: LoadFile MD5-resolves the datapack first
    // and its loose-file fallback never runs, so the loose file would never
    // be seen otherwise (the same reason DC_NativeTryLoadDtex bypasses it).
    //
    // GLOBAL sfx only, deliberately. Stage sfx are packed .wav that route into
    // AICA sound RAM, that path is working, and nothing dropped on the disc
    // should be able to divert it. The ID range test is the same one used
    // below and is only this simple because RETRO_MOD_LOADER is forced OFF on
    // DC — see the longer note in the 'w' branch.
    if (sfxID < globalSFXCount || sfxID >= globalSFXCount + stageSFXCount) {
        size_t  rawSamples  = 0;
        int     rawRate     = 0;
        int     rawChannels = 0;
        Sint16 *raw         = DC_LoadRawSfx(fullPath, &rawSamples, &rawRate, &rawChannels);
        if (raw) {
            LockAudioDevice();
            StrCopy(sfxList[sfxID].name, filePath);
            sfxList[sfxID].buffer   = raw;
            sfxList[sfxID].length   = rawSamples;
            sfxList[sfxID].rate     = rawRate;
            sfxList[sfxID].channels = (byte)rawChannels;
            sfxList[sfxID].loaded   = true;
            UnlockAudioDevice();
            return;
        }
    }
#endif

    if (LoadFile(fullPath, &info)) {
#if !RETRO_USE_ORIGINAL_CODE
        byte type = fullPath[StrLength(fullPath) - 3];
#if RETRO_USING_KOS
        if (type == 'w') {
            // Streamed rather than slurped: the old path held the whole file, a
            // full-rate S16 copy and the half-rate copy live at once, needing
            // ~5 bytes free for every 1 it kept. That is fine at boot when the
            // global sfx load onto an empty heap, and not fine at stage load,
            // where the bigger Data/SoundFX/Stage/ wavs were the ones failing.
            // DC_LoadWAVStreamed reads through the open file and decimates
            // straight into the final buffer, so peak == kept.
            //
            // Stage sfx additionally come out of a bump arena rather than the
            // heap (see the arena block in DCAudio.cpp): reloading a whole
            // differently-sized set on every stage change fragments the free
            // list, and Hidden Palace's 23 buffers were failing at the tail.
            // The ID range test below is exactly the range ReleaseStageSfx
            // frees. It is only this simple because RETRO_MOD_LOADER is forced
            // OFF for DC in platforms/KallistiOS.cmake — the mod loader is the
            // one caller that bumps globalSFXCount after each LoadSfx, which
            // would otherwise make the two ranges overlap. stageSFXCount is set
            // in Scene.cpp before its LoadSfx loop, so it is valid here.
            size_t sampleCount = 0;
            int wavRate        = 0;
            int wavChannels    = 0;
            const bool isStageSfx = sfxID >= globalSFXCount && sfxID < globalSFXCount + stageSFXCount;

            // Stage sfx go to the AICA's own 2MB of sound RAM before they go to
            // the heap at all. Hidden Palace's set wants 1.4MB and the heap has
            // ~830KB left by then, so on that zone the arena below cannot win
            // however it is arranged — see the AICA block at the bottom of
            // DCAudio.cpp. Anything the AICA won't take (bad format, sound RAM
            // full, staging refused) falls through to the arena exactly as
            // before, so no zone can regress.
            if (isStageSfx && DC_AicaSfxLoad(info.vfileSize, sfxID)) {
                CloseFile();

                LockAudioDevice();
                StrCopy(sfxList[sfxID].name, filePath);
                // NULL buffer is the marker: the software mixer already skips a
                // channel whose samplePtr is NULL, so an AICA-resident sfx costs
                // it nothing and needs no test of its own in the mix loop.
                sfxList[sfxID].buffer   = NULL;
                sfxList[sfxID].length   = 0;
                sfxList[sfxID].rate     = 0;
                sfxList[sfxID].channels = 0;
                sfxList[sfxID].loaded   = true;
                UnlockAudioDevice();
                return;
            }

            // The AICA attempt consumed the header, so rewind before the heap
            // loader walks the same chunks again.
            if (isStageSfx)
                SetFilePosition(0);

            Sint16 *samples = DC_LoadWAVStreamed(info.vfileSize, &sampleCount, &wavRate, &wavChannels, isStageSfx);
            CloseFile();

            if (!samples) {
                PrintLog("Unable to read sfx: %s", info.fileName);
                sfxList[sfxID].loaded = false;
            }
            else {
                LockAudioDevice();
                StrCopy(sfxList[sfxID].name, filePath);
                sfxList[sfxID].buffer   = samples;
                sfxList[sfxID].length   = sampleCount;
                sfxList[sfxID].rate     = wavRate;
                sfxList[sfxID].channels = (byte)wavChannels;
                sfxList[sfxID].loaded   = true;
                UnlockAudioDevice();
            }
        }
        else if (type == 'o') {
            // Vorbis sfx can no longer be decoded on Dreamcast — libtremor is
            // gone (see the note in RetroEngine.hpp). Getting here means this
            // .ogg had no loose raw-PCM copy beside Data.rsdk for
            // DC_LoadRawSfx to pick up, so it simply cannot be played.
            //
            // Deliberately loud rather than silent: a missed conversion is a
            // one-line fix on the asset side, and a sound that vanishes with
            // no explanation costs a rebuild to diagnose.
            CloseFile();
            PrintLog("Sfx '%s' is packed Vorbis with no loose raw copy - not loaded", info.fileName);
            sfxList[sfxID].loaded = false;
        }
        else {
            // wtf lol
            CloseFile();
            PrintLog("Sfx format not supported!");
        }
#else
        if (type == 'w') {
            byte *sfx = new byte[info.vfileSize];
            FileRead(sfx, info.vfileSize);
            CloseFile();

            SDL_RWops *src = SDL_RWFromMem(sfx, info.vfileSize);
            if (src == NULL) {
                PrintLog("Unable to open sfx: %s", info.fileName);
            }
            else {
                SDL_AudioSpec wav_spec;
                uint wav_length;
                byte *wav_buffer;
                SDL_AudioSpec *wav = SDL_LoadWAV_RW(src, 0, &wav_spec, &wav_buffer, &wav_length);

                SDL_RWclose(src);
                delete[] sfx;
                if (wav == NULL) {
                    PrintLog("Unable to read sfx: %s", info.fileName);
                }
                else {
                    SDL_AudioCVT convert;
                    if (SDL_BuildAudioCVT(&convert, wav->format, wav->channels, wav->freq, audioDeviceFormat.format, audioDeviceFormat.channels,
                                          audioDeviceFormat.freq)
                        > 0) {
                        convert.buf = (byte *)malloc(wav_length * convert.len_mult);
                        convert.len = wav_length;
                        memcpy(convert.buf, wav_buffer, wav_length);
                        SDL_ConvertAudio(&convert);

                        LockAudioDevice();
                        StrCopy(sfxList[sfxID].name, filePath);
                        sfxList[sfxID].buffer = (Sint16 *)convert.buf;
                        sfxList[sfxID].length = convert.len_cvt / sizeof(Sint16);
                        sfxList[sfxID].loaded = true;
                        UnlockAudioDevice();
                        SDL_FreeWAV(wav_buffer);
                    }
                    else { // this causes errors, actually
                        PrintLog("Unable to read sfx: %s (error: %s)", info.fileName, SDL_GetError());
                        sfxList[sfxID].loaded = false;
                        SDL_FreeWAV(wav_buffer);
                        // LockAudioDevice()
                        // StrCopy(sfxList[sfxID].name, filePath);
                        // sfxList[sfxID].buffer = (Sint16 *)wav_buffer;
                        // sfxList[sfxID].length = wav_length / sizeof(Sint16);
                        // sfxList[sfxID].loaded = false;
                        // UnlockAudioDevice()
                    }
                }
            }
        }
        else if (type == 'o') {
            // ogg sfx :(
            OggVorbis_File vf;
            ov_callbacks callbacks = OV_CALLBACKS_NOCLOSE;
            vorbis_info *vinfo;
            byte *buf;
            SDL_AudioSpec spec;
            int bitstream = -1;
            long samplesize;
            long samples;
            int read, toRead;

            currentStreamIndex++;
            currentStreamIndex %= STREAMFILE_COUNT;

            StreamFile *sfxFile = &streamFile[currentStreamIndex];
            sfxFile->filePos    = 0;
            sfxFile->fileSize   = info.vfileSize;
            if (info.vfileSize > MUSBUFFER_SIZE)
                sfxFile->fileSize = MUSBUFFER_SIZE;

            FileRead(streamFile[currentStreamIndex].buffer, sfxFile->fileSize);
            CloseFile();

            callbacks.read_func  = readVorbis;
            callbacks.seek_func  = seekVorbis;
            callbacks.tell_func  = tellVorbis;
            callbacks.close_func = closeVorbis;

            // GetFileInfo(&info);
            int error = ov_open_callbacks(sfxFile, &vf, NULL, 0, callbacks);
            if (error != 0) {
                ov_clear(&vf);
                PrintLog("failed to load ogg sfx!");
                return;
            }

            vinfo = ov_info(&vf, -1);

            byte *audioBuf = NULL;
            uint audioLen  = 0;
            memset(&spec, 0, sizeof(SDL_AudioSpec));

            spec.format   = AUDIO_S16;
            spec.channels = vinfo->channels;
            spec.freq     = (int)vinfo->rate;
            spec.samples  = 4096; /* buffer size */

            samples = (long)ov_pcm_total(&vf, -1);

            audioLen = spec.size = (Uint32)(samples * spec.channels * 2);
            audioBuf             = (byte *)malloc(audioLen);
            buf                  = audioBuf;
            toRead               = audioLen;

            for (read = (int)ov_read(&vf, (char *)buf, toRead, 0, 2, 1, &bitstream); read > 0;
                 read = (int)ov_read(&vf, (char *)buf, toRead, 0, 2, 1, &bitstream)) {
                if (read < 0) {
                    free(audioBuf);
                    ov_clear(&vf);
                    PrintLog("failed to read ogg sfx!");
                    return;
                }
                toRead -= read;
                buf += read;
            }

            ov_clear(&vf); // clears & closes vorbis file

            /* Don't return a buffer that isn't a multiple of samplesize */
            samplesize = ((spec.format & 0xFF) / 8) * spec.channels;
            audioLen &= ~(samplesize - 1);

            SDL_AudioCVT convert;
            if (SDL_BuildAudioCVT(&convert, spec.format, spec.channels, spec.freq, audioDeviceFormat.format, audioDeviceFormat.channels,
                                  audioDeviceFormat.freq)
                > 0) {
                convert.buf = (byte *)malloc(audioLen * convert.len_mult);
                convert.len = audioLen;
                memcpy(convert.buf, audioBuf, audioLen);
                SDL_ConvertAudio(&convert);

                LockAudioDevice();
                StrCopy(sfxList[sfxID].name, filePath);
                sfxList[sfxID].buffer = (Sint16 *)convert.buf;
                sfxList[sfxID].length = convert.len_cvt / sizeof(Sint16);
                sfxList[sfxID].loaded = true;
                UnlockAudioDevice();
                free(audioBuf);
            }
            else {
                LockAudioDevice();
                StrCopy(sfxList[sfxID].name, filePath);
                sfxList[sfxID].buffer = (Sint16 *)audioBuf;
                sfxList[sfxID].length = audioLen / sizeof(Sint16);
                sfxList[sfxID].loaded = true;
                UnlockAudioDevice();
            }
        }
        else {
            // wtf lol
            CloseFile();
            PrintLog("Sfx format not supported!");
        }
#endif // RETRO_USING_KOS
#endif
    }
}
void PlaySfx(int sfx, bool loop)
{
#if RETRO_USING_KOS
    // An sfx living in AICA sound RAM takes no sfxChannels[] slot at all. That
    // is deliberate: its buffer is NULL, so the software mixer never runs the
    // branch that clears a finished channel, and a slot handed to it would
    // never come back — until the search below found nothing free, left
    // sfxChannelID at -1, and wrote to sfxChannels[-1].
    if (DC_AicaSfxPlay(sfx, loop))
        return;
#endif
    LockAudioDevice();
    int sfxChannelID = -1;
    for (int c = 0; c < CHANNEL_COUNT; ++c) {
        if (sfxChannels[c].sfxID == sfx || sfxChannels[c].sfxID == -1) {
            sfxChannelID = c;
            break;
        }
    }

    ChannelInfo *sfxInfo  = &sfxChannels[sfxChannelID];
    sfxInfo->sfxID        = sfx;
    sfxInfo->samplePtr    = sfxList[sfx].buffer;
    sfxInfo->sampleLength = sfxList[sfx].length;
    sfxInfo->loopSFX      = loop;
    sfxInfo->pan          = 0;
#if RETRO_USING_KOS
    sfxInfo->stepPos      = 0;
#endif
    UnlockAudioDevice();
}
void SetSfxAttributes(int sfx, int loopCount, sbyte pan)
{
#if RETRO_USING_KOS
    if (DC_AicaSfxSetAttr(sfx, loopCount, pan))
        return;
#endif
    LockAudioDevice();
    int sfxChannel = -1;
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        if (sfxChannels[i].sfxID == sfx) {
            sfxChannel = i;
            break;
        }
    }
    if (sfxChannel == -1) {
        UnlockAudioDevice();
        return; // wasn't found
    }

    ChannelInfo *sfxInfo = &sfxChannels[sfxChannel];
    sfxInfo->loopSFX     = loopCount == -1 ? sfxInfo->loopSFX : loopCount;
    sfxInfo->pan         = pan;
    sfxInfo->sfxID       = sfx;
    UnlockAudioDevice();
}
