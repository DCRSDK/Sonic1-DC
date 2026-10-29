// RSDKv4 Dreamcast audio backend
//
// Strategy (Phase 1): keep RSDKv4's existing software mixer
// (ProcessAudioPlayback) and drive it from a KOS snd_stream running at
// 44100 Hz S16 stereo. Music is decoded with libtremor (integer vorbis,
// same ov_* API). A dedicated thread polls the stream, exactly like the
// RSDKv5 Dreamcast port's sndstream_thread.
//
// Phase 2 fallback (if vorbis decode is too heavy on SH4): pre-convert
// music to raw PCM and SFX to AICA ADPCM channels, RSDKv5-DC style.

#include "../RetroEngine.hpp"

#if RETRO_USING_KOS

#include <kos.h>
#include <dc/sound/stream.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <kos/thread.h>

#include "DCCommon.hpp"

// RECURSIVE: the engine nests LockAudioDevice() calls (PlayMusic ->
// LoadMusic, StopMusic -> FreeMusInfo). SDL's audio lock permits that;
// a normal KOS mutex does not. Initialized in DC_InitAudioDevice.
static mutex_t audioMutex;
static snd_stream_hnd_t streamHandle = SND_STREAM_INVALID;
static kthread_t *audioThread        = NULL;
static volatile bool audioThreadRun  = false;

// snd_stream requests up to this many bytes per poll callback
#define DC_STREAM_BUF_SIZE (0x4000)
static uint8_t __attribute__((aligned(32))) streamBuffer[DC_STREAM_BUF_SIZE];

void DC_LockAudio() { mutex_lock(&audioMutex); }
void DC_UnlockAudio() { mutex_unlock(&audioMutex); }

// The engine mixer processes audio in MIX_BUFFER_SAMPLES chunks and
// writes whole chunks to the output — feeding it a length that isn't a
// multiple of one chunk (512 bytes) overruns the buffer. Round requests
// down to a chunk multiple (and never below one chunk).
#define DC_MIX_CHUNK_BYTES (MIX_BUFFER_SAMPLES * (int)sizeof(Sint16))

static void *DC_StreamCallback(snd_stream_hnd_t hnd, int smp_req, int *smp_recv)
{
    if (smp_req > DC_STREAM_BUF_SIZE)
        smp_req = DC_STREAM_BUF_SIZE;

    smp_req -= smp_req % DC_MIX_CHUNK_BYTES;
    if (smp_req < DC_MIX_CHUNK_BYTES)
        smp_req = DC_MIX_CHUNK_BYTES;

    // Hold the mutex so engine-thread channel updates can't race the mixer.
    mutex_lock(&audioMutex);
    ProcessAudioPlayback(NULL, streamBuffer, smp_req);
    mutex_unlock(&audioMutex);

    *smp_recv = smp_req;
    return streamBuffer;
}

static void *DC_AudioThread(void *param)
{
    (void)param;

    snd_stream_start(streamHandle, AUDIO_FREQUENCY, 1); // 1 = stereo

    while (audioThreadRun) {
        if (snd_stream_poll(streamHandle) < 0)
            break;
        thd_sleep(10);
    }

    snd_stream_stop(streamHandle);
    return NULL;
}

// ---------------------------------------------------------------------
// VRAM-backed music storage
// ---------------------------------------------------------------------
// The 320x240 RGB565 framebuffer occupies ~150KB at the bottom of the
// 8MB of PVR VRAM and we don't use the PVR for anything else, so the
// music stream buffers are parked at the top. VRAM tolerates only
// 16/32-bit accesses, hence the word-based helpers below.

#define DC_VRAM_TOTAL      (0x800000)
#define DC_MUSIC_VRAM_BASE (DC_VRAM_TOTAL - (MUSBUFFER_SIZE * STREAMFILE_COUNT))

// NOTE: do NOT use KOS's vram_l here — it tracks the current framebuffer
// and uses the interleaved 32-bit access area. We want the linear
// (64-bit data path) area at 0xA5000000, addressed from VRAM base.
#define DC_VRAM_AREA (0xA5000000UL)

static inline volatile uint32_t *DC_VramWord(uint offset) { return (volatile uint32_t *)(DC_VRAM_AREA + (offset & ~3u)); }

void DC_VramRead(void *dst, uint vramOffset, size_t n)
{
    uint8_t *out = (uint8_t *)dst;

    // leading bytes up to word alignment
    while (n && (vramOffset & 3)) {
        uint32_t w = *DC_VramWord(vramOffset);
        *out++     = (uint8_t)(w >> ((vramOffset & 3) * 8));
        ++vramOffset;
        --n;
    }

    // whole words
    while (n >= 4) {
        uint32_t w = *DC_VramWord(vramOffset);
        out[0]     = (uint8_t)(w);
        out[1]     = (uint8_t)(w >> 8);
        out[2]     = (uint8_t)(w >> 16);
        out[3]     = (uint8_t)(w >> 24);
        out += 4;
        vramOffset += 4;
        n -= 4;
    }

    // trailing bytes
    while (n) {
        uint32_t w = *DC_VramWord(vramOffset);
        *out++     = (uint8_t)(w >> ((vramOffset & 3) * 8));
        ++vramOffset;
        --n;
    }
}

void DC_VramWrite(uint vramOffset, const void *src, size_t n)
{
    const uint8_t *in = (const uint8_t *)src;

    // leading bytes: read-modify-write the containing word
    while (n && (vramOffset & 3)) {
        volatile uint32_t *w = DC_VramWord(vramOffset);
        const int shift      = (vramOffset & 3) * 8;
        *w                   = (*w & ~(0xFFu << shift)) | ((uint32_t)*in++ << shift);
        ++vramOffset;
        --n;
    }

    // whole words (assemble byte-wise: src may be unaligned)
    while (n >= 4) {
        uint32_t w = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
        *DC_VramWord(vramOffset) = w;
        in += 4;
        vramOffset += 4;
        n -= 4;
    }

    // trailing bytes
    while (n) {
        volatile uint32_t *w = DC_VramWord(vramOffset);
        const int shift      = (vramOffset & 3) * 8;
        *w                   = (*w & ~(0xFFu << shift)) | ((uint32_t)*in++ << shift);
        ++vramOffset;
        --n;
    }
}

// ---- Music CD-reader thread control (see the music section below) ----
// Guards the music FILE* (open/close/seek/fread) against concurrent access.
// The audio (consumer) path NEVER takes this — that is the whole point.
static mutex_t   musCtlMutex;
static kthread_t *musThread       = NULL;
static volatile bool musThreadRun = false;
static void *DC_MusReaderThread(void *param); // defined in the music section

bool DC_InitAudioDevice()
{
#if defined(DC_NO_AUDIO) && DC_NO_AUDIO
    // Built with -DDC_NO_AUDIO=ON: skip all sound init. audioEnabled stays
    // false, so the engine mutes itself (sfx/music loads become no-ops).
    DC_LOG("[DC] audio: DISABLED at build time\n");
    return false;
#endif
    DC_LOG("[DC] audio: init\n");
    DC_Probe(6);

    mutex_init(&audioMutex, MUTEX_TYPE_RECURSIVE);
    mutex_init(&musCtlMutex, MUTEX_TYPE_NORMAL);

    // Reserve a small VRAM staging region for OGG SFX decode (music streams
    // from CD now and no longer uses this). Kept through the PVR allocator so
    // it isn't handed back out as a sprite texture. Was 4MB (2x2MB music
    // buffers); now 1MB (2x512KB), freeing ~3MB for hardware-sprite textures.
    uint sfxStageBase = DC_MusicVramAlloc((uint)DC_SFX_STAGE_SIZE * STREAMFILE_COUNT);
    for (int i = 0; i < STREAMFILE_COUNT; ++i) streamFile[i].vramBase = sfxStageBase + i * DC_SFX_STAGE_SIZE;

    if (snd_stream_init() < 0) {
        DC_LOG("[DC] audio: snd_stream_init FAILED\n");
        return false;
    }
    DC_LOG("[DC] audio: snd_stream_init ok\n");

    streamHandle = snd_stream_alloc(DC_StreamCallback, DC_STREAM_BUF_SIZE);
    if (streamHandle == SND_STREAM_INVALID) {
        DC_LOG("[DC] audio: snd_stream_alloc FAILED\n");
        return false;
    }
    DC_LOG("[DC] audio: stream allocated\n");

    audioThreadRun = true;
    audioThread    = thd_create(0, DC_AudioThread, NULL);
    if (!audioThread) {
        DC_LOG("[DC] audio: thd_create FAILED\n");
        snd_stream_destroy(streamHandle);
        streamHandle = SND_STREAM_INVALID;
        return false;
    }
    thd_set_prio(audioThread, PRIO_DEFAULT / 2); // above the main thread

    // CD reader thread (fills the music ring buffer). Runs at main-thread
    // priority so it never preempts the game or the audio mixer; the ring
    // absorbs its scheduling jitter.
    musThreadRun = true;
    musThread    = thd_create(0, DC_MusReaderThread, NULL);
    if (!musThread) {
        DC_LOG("[DC] audio: music reader thd_create FAILED\n");
        // non-fatal: sfx still work; music simply won't stream
        musThreadRun = false;
    }
    else {
        thd_set_prio(musThread, PRIO_DEFAULT);
    }

    // NOTE: the stage-SFX arena is deliberately NOT reserved here. Taking 1.5MB
    // at audio init starves the native menu, whose models are malloc'd once at
    // boot and kept for the rest of the run (~900KB across BGLines/Box/Intro/
    // TimeAttack/...). LoadMesh did not check those allocations, and on DC a
    // write through NULL is silently swallowed by the BIOS ROM window, so the
    // menu rendered ROM bytes as vertex data instead of crashing. The arena is
    // claimed on the first stage load instead — see DC_SfxArenaAcquire.
    DC_LOG("[DC] audio: init ok\n");

    // The other 2MB. Stage sfx are uploaded here rather than to the main heap
    // (see the AICA block at the bottom of this file), so this number is the
    // budget a zone's sfx set has to fit inside — Hidden Palace, the biggest,
    // wants roughly 1.4MB of it.
    DC_LOG("[DCSfx] aica: %u bytes of sound RAM free after the music stream\n", (unsigned)snd_mem_available());

    // Baseline. Everything the heap ever gets is here — compare the stage-load
    // reports against this line to see what the menu's models are costing.
    DC_HeapReport("audio init");

    DC_Probe(7);
    return true;
}

void DC_ReleaseAudioDevice()
{
    if (musThread) {
        musThreadRun = false;
        thd_join(musThread, NULL);
        musThread = NULL;
    }
    if (audioThread) {
        audioThreadRun = false;
        thd_join(audioThread, NULL);
        audioThread = NULL;
    }
    if (streamHandle != SND_STREAM_INVALID) {
        snd_stream_destroy(streamHandle);
        streamHandle = SND_STREAM_INVALID;
    }
}

// ---------------------------------------------------------------------
// Music: raw-PCM streaming from CD (decoupled reader thread)
// ---------------------------------------------------------------------
//
// Music is pre-converted to raw, headerless, interleaved signed 8-bit
// stereo PCM @ 22050 Hz (ffmpeg -ar 22050 -ac 2 -f s8) and renamed .ogg
// so the engine's track list still finds it (same scheme as the RSDKv5
// Dreamcast port). We up-convert to the mixer's 44100 Hz stereo S16 format
// on the fly (exact 2x upsample by frame duplication).
//
// CRITICAL: the CD read (fread) must NOT happen on the audio callback
// thread. That thread runs above main-thread priority and holds audioMutex
// the whole time it mixes; a blocking GD-ROM read there starves the game's
// main loop of both the lock and the drive, and the game appears to hang
// the instant a track starts (this was the title-screen freeze). Instead a
// dedicated reader thread fills a RAM ring buffer from the CD, and the
// audio callback only ever consumes bytes from that ring (lock-free,
// single-producer / single-consumer) — it never touches the disc and never
// blocks. The previous tremor path avoided this only because it decoded out
// of VRAM rather than the CD.

#include <stdio.h>
#include <kos/fs.h>
#include <fcntl.h>

#define DC_MUS_SRC_RATE   22050
#define DC_MUS_RING_SIZE  (64 * 1024) // ~1.5s of 22050Hz stereo s8 readahead

// Per-read size on the reader thread. The right answer depends entirely on
// which transport the data is coming from, so the build picks it:
//
//   /cd/  - GD-ROM. Cost is dominated by SEEKS, not bytes: iso_read keeps one
//           read-ahead stream for the whole filesystem and every music read
//           tears down whatever the asset loader had running. Few large reads
//           win, which is what 32KB is for. UNCHANGED from the disc build.
//
//   /pc/  - dcload. There are no seeks, and cost is simply bytes / link rate -
//           EXCEPT that KOS routes every dcload call through dcload_syscall(),
//           which does irq_disable_scoped() for the whole transfer (see
//           kernel/arch/dreamcast/hardware/dcload.c). A read does not merely
//           occupy the link, it halts the machine: no timer IRQ, no thread
//           switch, no stream callback. At 44100 B/s of music a 32KB read is a
//           ~20-30ms blackout about 1.35 times a second - twice a frame's
//           entire budget, so it drops two frames every time, on a rhythm.
//           Total link cost is the same either way, so the fix is not fewer
//           bytes but ABSORBABLE ones: 2KB is ~2.8 frames of audio and stalls
//           for ~1-2ms, which fits inside a frame's slack and drops nothing.
#ifdef DC_DATA_ON_DCLOAD
#define DC_MUS_READ_CHUNK (2 * 1024)
#else
#define DC_MUS_READ_CHUNK (32 * 1024)
#endif

// The one-off synchronous prime in DC_MusicOpen stays large regardless: it
// happens while a stage is loading or a scene is switching, where a stall is
// invisible, and starting with only 2KB buffered would risk an underrun before
// the reader thread gets going. On /cd/ this is the same 32KB the prime always
// used, so that build is bit-for-bit unchanged.
#define DC_MUS_PRIME_BYTES (32 * 1024)

// Sample rate of the ORIGINAL Vorbis files the loop points were authored
// against. The engine's loopPoint (and the resume position derived from it)
// is a PCM *frame* index into that file — on desktop it is fed straight to
// ov_pcm_seek. Our raw files are the same music resampled to DC_MUS_SRC_RATE,
// so a source frame index has to be rescaled before it means anything as a
// byte offset here. Verified against Sonic2Data/Data/Music/*.ogg: the Vorbis
// identification header reads 44100 Hz, 2 channels. At 44100 the rescale
// happens to collapse to byteOffset == loopPoint (halve the frames, then two
// bytes per s8 stereo frame), but keep the arithmetic explicit so a future
// re-encode at another rate is a one-line change rather than a silent bug.
#define DC_MUS_OGG_RATE 44100

// Byte offset in the raw 22050 Hz s8 stereo file for a position expressed in
// source-ogg PCM frames. 64-bit intermediate: EmeraldHill's loop point alone
// is 152750, and Credits runs long enough that frames * 22050 overflows 32
// bits well before the end of the track.
static uint DC_MusFramesToBytes(uint srcFrames)
{
    unsigned long long f = (unsigned long long)srcFrames * DC_MUS_SRC_RATE / DC_MUS_OGG_RATE;
    return (uint)(f * 2ull);
}

// IMPORTANT: music I/O uses KOS's native fs_* API, NOT newlib stdio
// (fopen/fread). A newlib FILE* opened on one thread and read from another
// faults on KOS (per-thread reentrancy/locking); the fs_* handles are plain
// integers, are thread-safe, and can be opened on the main thread and read
// from the reader thread. This is the crash the title-screen freeze was:
// the reader thread died on its first cross-thread fread.
static file_t dcMusFile           = FILEHND_INVALID;
static bool  dcMusLoop            = false;
static volatile bool dcMusActive  = false; // a track is loaded & streaming
static volatile bool dcMusEnded   = false; // reader hit EOF (non-looping)

// Follow-on track, played gaplessly the instant the current one hits EOF.
// Empty means "nothing follows"; it is consumed (cleared) when it fires, so a
// chain is always one-shot and can never turn into an accidental loop.
// Only touched under musCtlMutex.
static char dcMusChain[0x100] = { 0 };

// Where the reader thread rewinds to on EOF, in bytes, and the file's size.
// Both are only touched under musCtlMutex. dcMusLoopByte is 0 for tracks that
// loop from the top (SetMusicTrack with no loop point) and for any loop point
// that lands past the end of the file — a wrong value there would seek the
// stream into oblivion, so it degrades to loop-to-start instead.
static uint dcMusLoopByte = 0;
static uint dcMusSize     = 0;

// SPSC ring: producer = reader thread (advances head), consumer = audio
// callback (advances tail). Each index is only written by its owner and
// only read by the other side. head/tail are kept even (whole stereo
// frames) so avail<2 always means avail==0.
// 32-byte aligned, and every read length is masked to a multiple of 32, so the
// write head stays 32-aligned across laps. That matters because KOS's iso_read
// gates BOTH of its fast paths on ((thissect & 31) == 0 && aligned(outbuf, 32))
// - miss it and the read degrades to a per-sector bread_cache miss, which calls
// iso_abort_stream and destroys whatever read-ahead the main thread had running.
static uint8_t dcMusRing[DC_MUS_RING_SIZE] __attribute__((aligned(32)));
static volatile int dcMusHead = 0;
static volatile int dcMusTail = 0;

// musCtlMutex / musThread / musThreadRun are defined above DC_InitAudioDevice
// (they are referenced by init/teardown, which precede this section).

static inline int DC_MusRingAvail() // bytes readable
{
    int h = dcMusHead, t = dcMusTail;
    return (h - t + DC_MUS_RING_SIZE) % DC_MUS_RING_SIZE;
}
static inline int DC_MusRingFree() // bytes writable (keep one slot empty)
{
    return DC_MUS_RING_SIZE - 1 - DC_MusRingAvail();
}

// Reader thread: keep the ring topped up from the CD file. Runs at ~main
// priority so it yields to the game; the 1.5s ring absorbs the jitter.
static void *DC_MusReaderThread(void *param)
{
    (void)param;

    while (musThreadRun) {
        mutex_lock(&musCtlMutex);

        if (dcMusFile == FILEHND_INVALID || !dcMusActive || dcMusEnded) {
            mutex_unlock(&musCtlMutex);
            thd_sleep(20);
            continue;
        }

        // Low-water mark. The old guard was `space < 2`, i.e. read whenever the
        // ring is not completely full — so once primed this woke every 10ms,
        // found the ~441 bytes the mixer had drained, and issued a 441-byte
        // read. That is ~100 disc transactions a second, none of them able to
        // clear iso_read's fast-path gate, so each one was a bread_cache miss
        // calling iso_abort_stream on the single global stream_fd. The cost was
        // never the reads themselves; it was killing the asset loader's
        // read-ahead 100 times a second. Waiting for a full chunk takes this to
        // ~1.35 reads/sec. Floor is 32767 bytes buffered = 0.74s of audio.
        int space = DC_MusRingFree();
        if (space < DC_MUS_READ_CHUNK) {
            mutex_unlock(&musCtlMutex);
            thd_sleep(10);
            continue;
        }

        // Read straight into the ring — the old 8KB `tmp` bounce buffer is gone
        // (it was aligned(4), which disqualified every read on its own).
        int h        = dcMusHead;
        int runToEnd = DC_MUS_RING_SIZE - h;

        int want = DC_MUS_READ_CHUNK;
        if (want > space)
            want = space; // never overwrite bytes the mixer has not consumed
        if (want > runToEnd)
            want = runToEnd; // stop at the wrap; next pass resumes at index 0

        int aligned = want & ~31;
        if (aligned >= 32)
            want = aligned; // keep head 32-aligned for the next lap
        else
            want &= ~1; // whole stereo frames only

        if (want < 2) {
            mutex_unlock(&musCtlMutex);
            thd_sleep(10);
            continue;
        }

        ssize_t rd = fs_read(dcMusFile, &dcMusRing[h], (size_t)want);
        size_t got = (rd > 0) ? (size_t)rd : 0;

        if (got < 2) { // end of file (or read error)
            if (dcMusLoop) {
                // Loop back to the track's loop point, not to byte 0 — every
                // stage track is an intro followed by the looping body, and
                // rewinding to the top replayed the intro on every lap.
                fs_seek(dcMusFile, (long)dcMusLoopByte, SEEK_SET);
                mutex_unlock(&musCtlMutex);
                continue; // seamless loop; refill from the loop point
            }
            // Chain straight into the follow-on track, if one was armed.
            // The ring is deliberately NOT reset: it still holds up to ~1.5s of
            // the outgoing track that the mixer has not played yet, and the new
            // file's bytes are simply appended behind them. That is what makes
            // the join gapless — the mixer never sees a discontinuity, it just
            // keeps draining a ring that happens to be fed from a new file.
            if (dcMusChain[0]) {
                char next[sizeof(dcMusChain)];
                memcpy(next, dcMusChain, sizeof(next));
                dcMusChain[0] = 0; // one-shot

                fs_close(dcMusFile);
                dcMusFile = fs_open(next, O_RDONLY);

                if (dcMusFile != FILEHND_INVALID) {
                    const long total = (long)fs_total(dcMusFile);
                    dcMusSize        = (total > 0) ? ((uint)total & ~1u) : 0;
                    dcMusLoopByte    = 0;
                    dcMusLoop        = false; // the chained track ends for real
                    DC_LOG("[DC] music: chained to '%s' (size=%u)\n", next, dcMusSize);
                    mutex_unlock(&musCtlMutex);
                    continue; // next pass refills the ring from the new file
                }

                // Open failed: fall through and end normally rather than
                // leaving a dead handle behind.
                DC_LOG("[DC] music: chain open FAILED '%s'\n", next);
                dcMusFile = FILEHND_INVALID;
            }

            dcMusEnded = true;
            mutex_unlock(&musCtlMutex);
            continue;
        }
        got &= ~(size_t)1; // drop an odd trailing byte at EOF

        // No copy needed — fs_read already landed the bytes at head. Publishing
        // the new head is what makes them visible to the mixer.
        dcMusHead = (h + (int)got) % DC_MUS_RING_SIZE;

        mutex_unlock(&musCtlMutex);
    }
    return NULL;
}

// Called on the main thread under LockAudioDevice (LoadMusic / StopMusic).
bool DC_MusicOpen(const char *path, bool loop, uint loopPoint, uint startFrame)
{
    mutex_lock(&musCtlMutex);

    if (dcMusFile != FILEHND_INVALID) {
        fs_close(dcMusFile);
        dcMusFile = FILEHND_INVALID;
    }
    dcMusActive   = false;
    dcMusEnded    = false;
    dcMusHead     = 0;
    dcMusTail     = 0;
    dcMusLoopByte = 0;
    dcMusSize     = 0;

    dcMusFile = fs_open(path, O_RDONLY);
    if (dcMusFile == FILEHND_INVALID) {
        mutex_unlock(&musCtlMutex);
        DC_LOG("[DC] music: open FAILED '%s'\n", path);
        return false;
    }
    dcMusLoop = loop;

    // ---- Credits -> Ending chain --------------------------------------------
    // Credits.ogg runs 121.3s. The credit sequence is FRAME-timed while the
    // music is wall-clock, so on hardware the 17 synchronous stage loads plus a
    // ~58fps demo frame rate stretch it to ~156s and the track runs out with
    // the final Green Hill demo still to play. Measured against a capture of
    // the PC decomp (128.8s sequence, music ending at 121.8s), the shortfall is
    // ~24s. Ending.ogg is 18.2s, so chaining it covers the whole final demo and
    // a few seconds past it; the last screens still fade out quiet, which is
    // what the original does anyway (it ends 7s before the sequence does).
    //
    // Matched on the FILENAME, case-insensitively, and the follow-on is built
    // as a sibling of whatever directory the track came from, so this works
    // from /cd or a loose Data folder without knowing which.
    dcMusChain[0] = 0;
    {
        const char *slash = NULL;
        for (const char *p = path; *p; ++p)
            if (*p == '/' || *p == '\\')
                slash = p;
        const char *name = slash ? slash + 1 : path;

        const char *want = "credits.ogg";
        int i = 0;
        for (; want[i] && name[i]; ++i) {
            char c = name[i];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (c != want[i])
                break;
        }
        if (!want[i] && !name[i]) { // exact, case-insensitive match
            const size_t dirLen = slash ? (size_t)(slash - path) + 1 : 0;
            // sizeof, not strlen: no string.h dependency beyond the memcpy
            // this file already uses, and it includes the terminator.
            static const char tail[] = "Ending.ogg";
            if (dirLen + sizeof(tail) <= sizeof(dcMusChain)) {
                memcpy(dcMusChain, path, dirLen);
                memcpy(dcMusChain + dirLen, tail, sizeof(tail));
                DC_LOG("[DC] music: will chain '%s' after this track\n", dcMusChain);
            }
        }
    }

    // Round the size down to a whole stereo frame so every offset derived from
    // it stays 2-byte aligned — an odd seek would swap the channels for the
    // rest of the track.
    // Cast explicitly: fs_total's return type has been size_t and off_t in
    // different KOS versions, and music files are far below the 2 GB where the
    // narrowing would matter.
    long total = (long)fs_total(dcMusFile);
    dcMusSize  = (total > 0) ? ((uint)total & ~1u) : 0;

    // Resolve the loop point. Out-of-range means the loop table and the audio
    // file disagree (a track re-encoded without updating the scripts, say);
    // falling back to 0 gives a wrong-but-musical loop rather than silence.
    // Rounded DOWN to a 32-byte boundary, not just to a stereo frame: the reader
    // thread seeks here on every lap, and an unaligned file position would put
    // the whole looping body - i.e. most of playback - back on iso_read's slow
    // path. 31 bytes is 15 stereo frames, 0.7ms at 22050Hz, and always early
    // rather than late, so it is inaudible.
    uint loopByte = DC_MusFramesToBytes(loopPoint) & ~31u;
    if (dcMusSize == 0 || loopByte >= dcMusSize)
        loopByte = 0;
    dcMusLoopByte = loopByte;

    // A resume position (speed shoes expiring, invincibility ending) is a
    // position in the OUTGOING track scaled by musicRatio, so it can legally
    // land past the end of this one. Wrap it into the looping body rather than
    // the whole file: past the end, the music has conceptually already looped.
    // Also floored to 32 bytes, for the same reason and with the same 0.7ms
    // worst-case error. Rounding down only ever decreases it, so the range
    // checks below still hold.
    uint startByte = DC_MusFramesToBytes(startFrame) & ~31u;
    if (dcMusSize > 0 && startByte >= dcMusSize) {
        uint bodyLen = dcMusSize - dcMusLoopByte;
        startByte    = bodyLen ? (dcMusLoopByte + (startByte - dcMusLoopByte) % bodyLen) & ~31u : 0;
    }
    if (startByte >= dcMusSize)
        startByte = 0;
    if (startByte)
        fs_seek(dcMusFile, (long)startByte, SEEK_SET);

    // Prime the ring synchronously so playback doesn't open with an
    // underrun gap while the reader thread spins up.
    ssize_t rd = fs_read(dcMusFile, dcMusRing, DC_MUS_PRIME_BYTES);
    size_t got = (rd > 0) ? ((size_t)rd & ~(size_t)1) : 0;
    dcMusHead   = (int)got;
    dcMusActive = true;

    mutex_unlock(&musCtlMutex);

    // Logged in full because a wrong DC_MUS_OGG_RATE or a stale loop table is
    // otherwise indistinguishable by ear from a bad conversion: loopByte close
    // to size, or a loopPoint that clamped to 0, both show up here immediately.
    DC_LOG("[DC] music: streaming '%s' (loop=%d pt=%u byte=%u start=%u size=%u)\n", path, (int)loop, loopPoint, dcMusLoopByte, startByte, dcMusSize);
    return true;
}

// Position actually HEARD so far, in source-ogg PCM frames — the DC answer to
// ov_pcm_tell. Note the ring: fs_tell is ahead of the mixer by everything the
// reader thread has already buffered (up to ~1.5s), so the file position on
// its own would resume the next track a second and a half late.
uint DC_MusicTell()
{
    mutex_lock(&musCtlMutex);
    long pos = (dcMusFile != FILEHND_INVALID) ? (long)fs_tell(dcMusFile) : 0;
    mutex_unlock(&musCtlMutex);

    long heard = pos - (long)DC_MusRingAvail();
    if (heard < 0)
        heard = 0;

    unsigned long long frames = (unsigned long long)(heard / 2);
    return (uint)(frames * DC_MUS_OGG_RATE / DC_MUS_SRC_RATE);
}

void DC_MusicClose()
{
    mutex_lock(&musCtlMutex);
    dcMusActive = false;
    dcMusEnded  = false;
    if (dcMusFile != FILEHND_INVALID) {
        fs_close(dcMusFile);
        dcMusFile = FILEHND_INVALID;
    }
    dcMusHead     = 0;
    dcMusTail     = 0;
    dcMusLoopByte = 0;
    dcMusSize     = 0;
    mutex_unlock(&musCtlMutex);
}

// Produces interleaved 44100 Hz stereo S16 into `out`. Returns S16 count.
// Runs on the audio thread — consumes the ring only, never the disc.
int DC_MusicReadSamples(Sint16 *out, size_t samplesWanted)
{
    if (!dcMusActive)
        return 0;

    size_t written = 0;
    while (written + 4 <= samplesWanted) { // one src frame -> 2 dst frames (4 S16)
        if (DC_MusRingAvail() < 2) {
            // Ring empty: either a transient underrun (reader behind) or the
            // real end of a non-looping track. Only stop on a true end;
            // otherwise just emit what we have and let it catch up.
            if (dcMusEnded)
                musicStatus = MUSIC_STOPPED;
            break;
        }

        int t = dcMusTail;
        // signed 8-bit -> S16 (<<8)
        Sint16 l = (Sint16)((Sint8)dcMusRing[t]) << 8;
        Sint16 r = (Sint16)((Sint8)dcMusRing[(t + 1) % DC_MUS_RING_SIZE]) << 8;
        dcMusTail = (t + 2) % DC_MUS_RING_SIZE;

        // 2x upsample: emit the frame twice
        out[written++] = l;
        out[written++] = r;
        out[written++] = l;
        out[written++] = r;
    }

    return (int)written;
}

Sint16 *DC_HalfRateCopy(const Sint16 *src, size_t samples, int channels, size_t *outSamples)
{
    if (channels < 1)
        channels = 1;

    const size_t frames    = samples / channels;
    const size_t outFrames = frames / 2;

    Sint16 *dst = (Sint16 *)malloc(outFrames * channels * sizeof(Sint16));
    if (!dst)
        return NULL;

    for (size_t f = 0; f < outFrames; ++f) {
        for (int c = 0; c < channels; ++c) {
            const Sint32 a = src[(f * 2) * channels + c];
            const Sint32 b = src[(f * 2 + 1) * channels + c];
            dst[f * channels + c] = (Sint16)((a + b) >> 1);
        }
    }

    *outSamples = outFrames * channels;
    return dst;
}

// ---------------------------------------------------------------------
// SFX loading (native format — the mixer converts on the fly)
// ---------------------------------------------------------------------

// Every rejection below funnels into the same "Unable to read sfx: %s" line in
// LoadSfx, which cannot tell an unsupported codec apart from an exhausted heap.
// These say which, and are the fastest way to settle it from a log.
//
// DC_WAV_FAIL is deliberately NOT under DC_WAV_DIAG (split 2026-07-29). It only
// prints when a sound has genuinely failed to load, so on a healthy run it costs
// nothing and stays silent — and switching it off is exactly the wrong trade:
// the next silent sfx would report "Unable to read sfx" with no reason attached,
// and finding out why would mean rebuilding with it back on. DC_WAV_DIAG now
// gates only the per-sfx chatter, which is load-time only (never per frame), so
// there is no reason to turn that off either. Leave both as they are unless a
// log is being cut down for size.
#define DC_WAV_FAIL(...)                                                                                                                             \
    do {                                                                                                                                             \
        printf("[DCWav] ");                                                                                                                          \
        printf(__VA_ARGS__);                                                                                                                         \
        printf("\n");                                                                                                                                \
    } while (0)

// Verbose per-sfx accounting: the "[DCSfx] aica <id>: F frames /D -> R Hz ..."
// lines and the decimation-retry notice. Load-time only.
#define DC_WAV_DIAG 1

// Minimal RIFF/WAVE parser: PCM (format 1), 8 or 16 bit, mono/stereo.
Sint16 *DC_LoadWAVNative(const byte *data, size_t dataLen, size_t *outSamples, int *outRate, int *outChannels)
{
    *outSamples  = 0;
    *outRate     = 0;
    *outChannels = 0;

    if (dataLen < 44 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        DC_WAV_FAIL("not RIFF/WAVE, len=%u first4=%02x%02x%02x%02x", (unsigned)dataLen, dataLen > 0 ? data[0] : 0, dataLen > 1 ? data[1] : 0,
                    dataLen > 2 ? data[2] : 0, dataLen > 3 ? data[3] : 0);
        return NULL;
    }

    const byte *ptr = data + 12;
    const byte *end = data + dataLen;

    int audioFormat = 0, channels = 0, sampleRate = 0, bitsPerSample = 0;
    const byte *pcmData = NULL;
    size_t pcmLen       = 0;

    while (ptr + 8 <= end) {
        uint32_t chunkLen = ptr[4] | (ptr[5] << 8) | (ptr[6] << 16) | ((uint32_t)ptr[7] << 24);

        if (memcmp(ptr, "fmt ", 4) == 0 && ptr + 8 + 16 <= end) {
            const byte *fmt = ptr + 8;
            audioFormat     = fmt[0] | (fmt[1] << 8);
            channels        = fmt[2] | (fmt[3] << 8);
            sampleRate      = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            bitsPerSample   = fmt[14] | (fmt[15] << 8);
        }
        else if (memcmp(ptr, "data", 4) == 0) {
            pcmData = ptr + 8;
            pcmLen  = chunkLen;
            if (pcmData + pcmLen > end)
                pcmLen = end - pcmData;
        }

        ptr += 8 + chunkLen + (chunkLen & 1);
    }

    if (audioFormat != 1 || !pcmData || !channels || !sampleRate) {
        // 0xFFFE is WAVE_FORMAT_EXTENSIBLE, 2 is MS-ADPCM, 3 is IEEE float,
        // 0x11 is IMA-ADPCM — all common exporter defaults, none supported here.
        DC_WAV_FAIL("unsupported: fmt=%d ch=%d rate=%d bits=%d data=%s", audioFormat, channels, sampleRate, bitsPerSample, pcmData ? "yes" : "no");
        return NULL;
    }
    if (bitsPerSample != 8 && bitsPerSample != 16) {
        DC_WAV_FAIL("unsupported bit depth %d", bitsPerSample);
        return NULL;
    }

    // Promote to S16, keep native rate/channels
    size_t srcSamples;
    Sint16 *s16buf;

    if (bitsPerSample == 16) {
        srcSamples = pcmLen / sizeof(Sint16);
        s16buf     = (Sint16 *)malloc(srcSamples * sizeof(Sint16));
        if (!s16buf) {
            DC_WAV_FAIL("heap: %u bytes for 16-bit sfx refused", (unsigned)(srcSamples * sizeof(Sint16)));
            return NULL;
        }
        memcpy(s16buf, pcmData, srcSamples * sizeof(Sint16));
    }
    else {
        srcSamples = pcmLen;
        s16buf     = (Sint16 *)malloc(srcSamples * sizeof(Sint16));
        if (!s16buf) {
            DC_WAV_FAIL("heap: %u bytes for 8-bit sfx refused", (unsigned)(srcSamples * sizeof(Sint16)));
            return NULL;
        }
        for (size_t i = 0; i < srcSamples; ++i) s16buf[i] = ((Sint16)pcmData[i] - 128) << 8;
    }

    // store at half rate when the source is 32kHz+ (halves heap cost)
    if (sampleRate >= 32000) {
        size_t halfSamples = 0;
        Sint16 *half       = DC_HalfRateCopy(s16buf, srcSamples, channels, &halfSamples);
        if (half) {
            free(s16buf);
            s16buf     = half;
            srcSamples = halfSamples;
            sampleRate /= 2;
        }
    }

    *outSamples  = srcSamples;
    *outRate     = sampleRate;
    *outChannels = channels;
    return s16buf;
}

// ---------------------------------------------------------------------
// Streaming WAV loader (the one LoadSfx uses)
// ---------------------------------------------------------------------
//
// DC_LoadWAVNative above needs the whole file in RAM, then builds a full-rate
// S16 copy of it, then — for anything 32kHz or over — a half-rate copy of THAT
// before freeing the first two. Peak is fileSize + pcm + pcm/2 while the kept
// result is only pcm/2, so roughly five bytes have to be free for every one it
// keeps. Global SFX load at boot on an empty heap and never notice; stage SFX
// load with a tileset, sheets and the previous stage's fragmentation already in
// place, and that 5x is exactly the kind of thing that starts failing for the
// bigger files only. Hence the reports of "certain" sound effects going missing.
//
// This version walks the RIFF chunk headers through the engine's reader, seeks
// back to the data chunk, and decimates straight into the final buffer in 4KB
// bites. Peak is the result plus a static staging block — no multiplier at all.
// Output is bit-identical to DC_LoadWAVNative's (same pair-averaging decimation).

static inline Sint16 dcWavSample(const byte *p, int bits)
{
    if (bits == 16)
        return (Sint16)((ushort)p[0] | ((ushort)p[1] << 8)); // little-endian, as SH4 is
    return (Sint16)(((int)p[0] - 128) << 8);                 // 8-bit is unsigned
}

size_t DC_LargestFreeBlock()
{
    size_t lo = 0, hi = 8u << 20; // no single sfx should ever want 8MB
    while (hi - lo > 4096) {
        size_t mid = lo + (hi - lo) / 2;
        void  *p   = malloc(mid);
        if (p) {
            free(p);
            lo = mid;
        }
        else
            hi = mid;
    }
    return lo;
}

// ---- stage-SFX bump arena ----------------------------------------------
// Stage sfx are freed and reloaded wholesale on every stage change, and each
// zone wants a different set of sizes. That churn fragments the heap: by the
// time Hidden Palace asks for its 23 buffers totalling ~1.4MB, the free list
// is shredded and the tail of the set silently fails to load (LoadSfx just
// logs and leaves sfxList[id].loaded false).
// A bump arena, taken once and never handed back, cannot fragment — after a
// reset the next stage's layout is byte-identical to the last one's, so a zone
// either fits or it doesn't, deterministically, with no dependence on what
// was loaded before it.
//
// WHEN it is taken matters as much as the fact that it is. It used to be
// claimed at audio init, on the theory that the heap is least fragmented there.
// It is — but that is also before the native menu has loaded its models, and
// those are malloc'd once at boot and held for the whole run (~900KB across
// BGLines/Box/Intro/TimeAttack/...). Renderer.cpp's LoadMesh did not check its
// mallocs, and a write through NULL on DC lands in the BIOS ROM window and is
// silently dropped, so the 2013 menu quietly rendered ROM bytes as geometry.
// So the arena is claimed at the top of the first stage load instead
// (DC_SfxArenaAcquire, called from Scene.cpp::LoadStageFiles), at the point
// where the previous stage's graphics and sfx have just been released and
// nothing new has been read yet — the cleanest the heap ever gets with the
// menu already resident.
// The size the arena WANTS. Hidden Palace is the high-water zone at 1,469,818
// bytes across 23 buffers; everything above that is slack.
#define DC_SFX_ARENA_TARGET ((size_t)(1536 * 1024))
#define DC_SFX_ARENA_ALIGN  ((size_t)32)

// The arena is taken as up to this many blocks rather than one slab. A single
// 1.5MB malloc is all-or-nothing: the log from the first attempt at this shows
// "1572864 bytes REFUSED (largest free block 245760)" followed by eighteen of
// Hidden Palace's twenty-three sfx failing, even though the heap had well over
// a megabyte free in total — just never in one piece. Grabbing the free list
// hole by hole, largest first, converts that all-or-nothing into "we get what
// there is". Blocks are only ever taken whole and released whole, so nothing
// inside the arena can fragment; only the block set varies.
#define DC_SFX_ARENA_BLOCKS 6

// Never take a hole all the way down: the rest of the stage load still has to
// read scripts, sheets and the tileset after this runs.
#define DC_SFX_ARENA_FLOOR ((size_t)(96 * 1024))

// A hole smaller than this is not worth a block slot — no stage sfx that small
// has ever been the one that failed.
#define DC_SFX_ARENA_MIN ((size_t)(32 * 1024))

struct DCSfxBlock {
    byte  *base;
    size_t size;
    size_t used;
};

static DCSfxBlock dcSfxBlk[DC_SFX_ARENA_BLOCKS];
static int        dcSfxBlkCount  = 0;
static size_t     dcSfxArenaSize = 0; // sum of block sizes actually obtained
static size_t     dcSfxArenaUsed = 0;
static size_t     dcSfxArenaPeak = 0; // high-water mark, reported on release
static bool       dcSfxArenaTried = false;
// Stage sfx bytes that ended up on the plain heap rather than in the arena or in
// sound RAM. Now that the arena is claimed lazily it is usually empty, so it can
// no longer stand in for "is there anything worth releasing?" — see
// DC_SfxArenaEmergencyRelease.
static size_t     dcSfxSpillBytes = 0;

// Walk the free list downwards, taking one block per hole until the target is
// met or the holes get too small to bother with. DC_LargestFreeBlock probes
// with real mallocs, so after each grab it reports the NEXT biggest hole —
// which is what makes this come out largest-first without a free-list walk.
// Largest-first matters: LoudHorn is a single 194,334-byte buffer and needs one
// contiguous block, so it has to land in the first (biggest) one.
static void dcSfxArenaGrab()
{
    while (dcSfxBlkCount < DC_SFX_ARENA_BLOCKS && dcSfxArenaSize < DC_SFX_ARENA_TARGET) {
        const size_t largest = DC_LargestFreeBlock();
        if (largest <= DC_SFX_ARENA_FLOOR)
            break;

        size_t want = largest - DC_SFX_ARENA_FLOOR;
        if (want > DC_SFX_ARENA_TARGET - dcSfxArenaSize)
            want = DC_SFX_ARENA_TARGET - dcSfxArenaSize;
        if (want < DC_SFX_ARENA_MIN)
            break;

        byte *p = (byte *)malloc(want);
        if (!p)
            break; // DC_LargestFreeBlock just proved this fits, so this is paranoia

        dcSfxBlk[dcSfxBlkCount].base = p;
        dcSfxBlk[dcSfxBlkCount].size = want;
        dcSfxBlk[dcSfxBlkCount].used = 0;
        ++dcSfxBlkCount;
        dcSfxArenaSize += want;
    }
}

static bool dcSfxArenaEnsure()
{
    if (dcSfxBlkCount)
        return true;
    if (dcSfxArenaTried)
        return false; // asked once, got nothing; don't thrash the heap retrying

    dcSfxArenaTried = true;
    dcSfxArenaGrab();

    if (dcSfxBlkCount) {
        DC_LOG("[DCSfx] stage arena: %u bytes in %d block(s) -", (unsigned)dcSfxArenaSize, dcSfxBlkCount);
        for (int i = 0; i < dcSfxBlkCount; ++i) DC_LOG(" %u", (unsigned)dcSfxBlk[i].size);
        DC_LOG("\n");
    }
    else {
        DC_LOG("[DCSfx] stage arena: NOTHING available (largest free block %u) - falling back to malloc\n", (unsigned)DC_LargestFreeBlock());
    }

    return dcSfxBlkCount != 0;
}

void DC_SfxArenaAcquire()
{
    // Called once per stage load, before the stage reads anything back in.
    // Clearing `tried` first means a refusal is only sticky for the rest of
    // that one stage load (so DC_SfxAlloc doesn't retry the whole grab 23 times
    // in a row), and the next stage gets a fresh attempt on whatever the heap
    // looks like by then.
    if (!dcSfxBlkCount)
        dcSfxArenaTried = false;

    // Deliberately does NOT grab anything here any more (2026-07-29). Stage sfx
    // go to AICA sound RAM first now, and that loader needs a transient staging
    // malloc of up to ~130KB for one sfx at a time. Claiming the arena up front
    // pulled the heap down to the 96KB floor before the sfx loop had run, so six
    // of Hidden Palace's sounds were refused by the AICA on size alone, got
    // pushed into the arena instead, filled it, and left LoudHorn's 194KB heap
    // buffer with nowhere to go at all - one silent sound, caused entirely by
    // the allocator that exists to prevent exactly that.
    //
    // So take nothing now. DC_SfxAlloc still calls dcSfxArenaEnsure() itself, so
    // the first sfx that genuinely falls through to the heap grabs the arena
    // then - by which point the AICA has already taken everything it can and the
    // heap it grabs from is still whole.
}

void *DC_SfxAlloc(size_t bytes, bool stageSfx)
{
    if (!bytes)
        return NULL;

    if (stageSfx && dcSfxArenaEnsure()) {
        const size_t aligned = (bytes + (DC_SFX_ARENA_ALIGN - 1)) & ~(DC_SFX_ARENA_ALIGN - 1);

        for (int pass = 0; pass < 2; ++pass) {
            // Best fit across the blocks: put each buffer in the tightest block
            // that still holds it, so the big blocks stay whole for the big sfx.
            int    best     = -1;
            size_t bestFree = 0;
            for (int i = 0; i < dcSfxBlkCount; ++i) {
                const size_t avail = dcSfxBlk[i].size - dcSfxBlk[i].used;
                if (avail < aligned)
                    continue;
                if (best < 0 || avail < bestFree) {
                    best     = i;
                    bestFree = avail;
                }
            }

            if (best >= 0) {
                void *p = dcSfxBlk[best].base + dcSfxBlk[best].used;
                dcSfxBlk[best].used += aligned;
                dcSfxArenaUsed += aligned;
                if (dcSfxArenaUsed > dcSfxArenaPeak)
                    dcSfxArenaPeak = dcSfxArenaUsed;
                return p;
            }

            // Nothing fits. If the first grab came up short of the target the
            // heap may have freed up since (the sheets for this stage are in by
            // now and the loader's scratch is back), so try to take one more
            // block before giving up on the arena for this buffer.
            if (pass == 0 && dcSfxArenaSize < DC_SFX_ARENA_TARGET && dcSfxBlkCount < DC_SFX_ARENA_BLOCKS) {
                const int before = dcSfxBlkCount;
                dcSfxArenaGrab();
                if (dcSfxBlkCount > before) {
                    DC_LOG("[DCSfx] stage arena grew to %u bytes in %d block(s)\n", (unsigned)dcSfxArenaSize, dcSfxBlkCount);
                    continue;
                }
            }
            break;
        }

        DC_LOG("[DCSfx] stage arena FULL: %u used of %u in %d block(s), %u more wanted - spilling to malloc\n", (unsigned)dcSfxArenaUsed,
               (unsigned)dcSfxArenaSize, dcSfxBlkCount, (unsigned)aligned);
    }

    void *p = malloc(bytes);
    if (p && stageSfx)
        dcSfxSpillBytes += bytes;
    return p;
}

static inline bool dcSfxInArena(const void *p)
{
    for (int i = 0; i < dcSfxBlkCount; ++i) {
        if ((const byte *)p >= dcSfxBlk[i].base && (const byte *)p < dcSfxBlk[i].base + dcSfxBlk[i].size)
            return true;
    }
    return false;
}

void DC_SfxFree(void *ptr)
{
    if (!ptr)
        return;
    if (dcSfxInArena(ptr))
        return; // reclaimed wholesale by DC_SfxArenaReset
    free(ptr);
}

void DC_SfxArenaReset()
{
    // The spill counter tracks buffers ReleaseStageSfx has just freed, so it is
    // cleared whether or not any arena blocks were ever taken.
    dcSfxSpillBytes = 0;

    if (!dcSfxBlkCount)
        return;

    DC_LOG("[DCSfx] stage arena released: %u used, peak %u, of %u in %d block(s)\n", (unsigned)dcSfxArenaUsed, (unsigned)dcSfxArenaPeak,
           (unsigned)dcSfxArenaSize, dcSfxBlkCount);

    // Hand the blocks back rather than just rewinding the bump pointers. This
    // is the other half of the fix: while a stage is running the arena is
    // holding ~1.5MB that the native menu's models need the moment the player
    // quits back to the title, and ReleaseStageSfx (this function's only real
    // caller) fires on exactly that transition. Re-acquiring costs one grab at
    // the top of the next stage load, at the point where the heap is emptiest.
    for (int i = 0; i < dcSfxBlkCount; ++i) {
        free(dcSfxBlk[i].base);
        dcSfxBlk[i].base = NULL;
        dcSfxBlk[i].size = 0;
        dcSfxBlk[i].used = 0;
    }

    dcSfxBlkCount   = 0;
    dcSfxArenaSize  = 0;
    dcSfxArenaUsed  = 0;
    dcSfxArenaPeak  = 0;
    dcSfxArenaTried = false;
}

bool DC_SfxArenaEmergencyRelease()
{
    // Two ways stage sfx can be occupying the heap: arena blocks, or individual
    // buffers that spilled past it. Since the arena went lazy the second case is
    // the common one, and gating on dcSfxBlkCount alone would make this a no-op
    // in exactly the situation it was written for. Sfx that live in AICA sound
    // RAM are counted by neither, which is right — releasing those buys the heap
    // nothing, so if they are all that is loaded this should still decline and
    // let the caller fail honestly instead of going quiet for no gain.
    if (!dcSfxBlkCount && !dcSfxSpillBytes)
        return false;

    DC_LOG("[DCSfx] EMERGENCY: giving stage sfx back (arena %u of %u used, %u spilled to heap) - stage sfx are silent until the next stage load\n",
           (unsigned)dcSfxArenaUsed, (unsigned)dcSfxArenaSize, (unsigned)dcSfxSpillBytes);

    // StopAllSfx first: the mixer thread is reading these buffers, and it takes
    // the audio lock to clear the channels. ReleaseStageSfx then frees each
    // buffer (DC_SfxFree no-ops the ones inside the arena), zeroes
    // stageSFXCount and calls DC_SfxArenaReset, which hands the blocks back.
    StopAllSfx();
    ReleaseStageSfx();
    return true;
}

// Total free heap, measured the only way that needs nothing but malloc/free:
// take the largest block repeatedly until nothing sizeable is left, add up what
// we got, then give it all back. Deliberately not mallinfo() — that is a glibc
// extension KOS's newlib does not dependably expose, and a build failure here
// would cost more than the convenience is worth.
//
// Reads low by design. Resolution is DC_LargestFreeBlock's 4KB, and it stops at
// 16KB rather than draining the heap to the last byte, so a concurrent
// allocation on another thread can't be starved by the measurement itself. The
// numbers that matter are relative anyway: it is the hole COUNT next to the
// total that says whether a refused sfx means "no memory" or "no memory in one
// piece". Diagnostic only.
#define DC_HEAP_PROBE_MIN   ((size_t)(16 * 1024))
#define DC_HEAP_PROBE_HOLES 32

void DC_HeapReport(const char *tag)
{
    void  *held[DC_HEAP_PROBE_HOLES];
    int    holes   = 0;
    size_t total   = 0;
    size_t largest = 0;

    while (holes < DC_HEAP_PROBE_HOLES) {
        const size_t s = DC_LargestFreeBlock();
        if (s < DC_HEAP_PROBE_MIN)
            break;
        void *p = malloc(s);
        if (!p)
            break;
        if (!holes)
            largest = s;
        held[holes++] = p;
        total += s;
    }

    for (int i = 0; i < holes; ++i) free(held[i]);

    DC_LOG("[DCHeap] %s: free>=%u in %d hole(s), largest=%u\n", tag ? tag : "?", (unsigned)total, holes, (unsigned)largest);
}

// The RIFF header walk, shared by both sfx loaders below (the heap one and the
// AICA one after it) so there is exactly one chunk parser to be right about.
// Reads from the start of the already-open engine file and leaves the read
// position wherever it stopped; callers seek to dataPos before taking samples.
struct DCWavHeader {
    int    channels;
    int    sampleRate;
    int    bitsPerSample;
    size_t dataPos;
    size_t dataLen;
};

static bool dcWavReadHeader(size_t fileSize, DCWavHeader *out)
{
    memset(out, 0, sizeof(*out));

    if (fileSize < 44) {
        DC_WAV_FAIL("too short (%u bytes)", (unsigned)fileSize);
        return false;
    }

    byte hdr[12];
    SetFilePosition(0);
    FileRead(hdr, 12);
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        DC_WAV_FAIL("not RIFF/WAVE, first4=%02x%02x%02x%02x", hdr[0], hdr[1], hdr[2], hdr[3]);
        return false;
    }

    int    audioFormat = 0, channels = 0, sampleRate = 0, bitsPerSample = 0;
    size_t dataPos = 0, dataLen = 0;
    size_t pos = 12;

    // Chunk walk. Bail the moment both chunks are known — FileSkip through an
    // encrypted container advances the keystream a byte at a time, so stepping
    // over a data chunk we already have the offset of is pure waste.
    while (pos + 8 <= fileSize) {
        byte ch[8];
        FileRead(ch, 8);
        pos += 8;

        size_t clen = (size_t)(ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24));
        if (clen > fileSize - pos)
            clen = fileSize - pos;

        if (memcmp(ch, "fmt ", 4) == 0 && clen >= 16) {
            byte f[16];
            FileRead(f, 16);
            audioFormat   = f[0] | (f[1] << 8);
            channels      = f[2] | (f[3] << 8);
            sampleRate    = (int)((uint32_t)f[4] | ((uint32_t)f[5] << 8) | ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24));
            bitsPerSample = f[14] | (f[15] << 8);
            if (clen > 16)
                FileSkip((int)(clen - 16));
        }
        else if (memcmp(ch, "data", 4) == 0) {
            dataPos = pos;
            dataLen = clen;
            if (audioFormat)
                break; // have both — stop before walking the samples
            FileSkip((int)clen);
        }
        else
            FileSkip((int)clen); // LIST/INFO/fact/cue, all ignorable

        pos += clen;
        if ((clen & 1) && pos < fileSize) { // chunks are word-padded
            FileSkip(1);
            pos++;
        }
    }

    if (audioFormat != 1 || !dataLen || !channels || !sampleRate) {
        // 0xFFFE is WAVE_FORMAT_EXTENSIBLE, 2 MS-ADPCM, 3 IEEE float, 0x11 IMA
        // ADPCM — common exporter defaults, none of them supported here.
        DC_WAV_FAIL("unsupported: fmt=%d ch=%d rate=%d bits=%d dataLen=%u", audioFormat, channels, sampleRate, bitsPerSample, (unsigned)dataLen);
        return false;
    }
    if (bitsPerSample != 8 && bitsPerSample != 16) {
        DC_WAV_FAIL("unsupported bit depth %d", bitsPerSample);
        return false;
    }
    if (channels < 1 || channels > 2) {
        DC_WAV_FAIL("unsupported channel count %d", channels);
        return false;
    }

    out->channels      = channels;
    out->sampleRate    = sampleRate;
    out->bitsPerSample = bitsPerSample;
    out->dataPos       = dataPos;
    out->dataLen       = dataLen;
    return true;
}

Sint16 *DC_LoadWAVStreamed(size_t fileSize, size_t *outSamples, int *outRate, int *outChannels, bool stageSfx)
{
    *outSamples  = 0;
    *outRate     = 0;
    *outChannels = 0;

    DCWavHeader wav;
    if (!dcWavReadHeader(fileSize, &wav))
        return NULL;

    const int    channels       = wav.channels;
    const int    sampleRate     = wav.sampleRate;
    const int    bitsPerSample  = wav.bitsPerSample;
    const size_t dataPos        = wav.dataPos;
    const size_t dataLen        = wav.dataLen;
    const int    bytesPerSample = bitsPerSample / 8;
    const size_t frameBytes     = (size_t)channels * (size_t)bytesPerSample;
    const size_t srcFrames      = dataLen / frameBytes;
    const bool   halve          = (sampleRate >= 32000); // stored at half rate; the mixer resamples anyway
    const size_t outFrames      = halve ? srcFrames / 2 : srcFrames;

    if (!outFrames) {
        DC_WAV_FAIL("empty after decode (dataLen=%u)", (unsigned)dataLen);
        return NULL;
    }

    const size_t outBytes = outFrames * (size_t)channels * sizeof(Sint16);
    Sint16      *dst      = (Sint16 *)DC_SfxAlloc(outBytes, stageSfx);
    if (!dst) {
        DC_WAV_FAIL("heap: %u bytes refused (%s), largest free block is %u", (unsigned)outBytes, stageSfx ? "stage" : "global",
                    (unsigned)DC_LargestFreeBlock());
        return NULL;
    }

    SetFilePosition((int)dataPos);

    static byte staging[4096] __attribute__((aligned(4)));

    // Keep the chunk an even number of frames so a decimation pair can never
    // straddle a chunk boundary. frameBytes is at most 4, so this is >= 1024.
    size_t chunkFrames = sizeof(staging) / frameBytes;
    if (halve)
        chunkFrames &= ~(size_t)1;

    size_t framesLeft = halve ? outFrames * 2 : outFrames;
    size_t w          = 0;

    while (framesLeft) {
        const size_t n = framesLeft < chunkFrames ? framesLeft : chunkFrames;
        FileRead(staging, (int)(n * frameBytes));
        framesLeft -= n;

        if (halve) {
            for (size_t f = 0; f + 1 < n; f += 2) {
                const byte *p0 = staging + f * frameBytes;
                const byte *p1 = p0 + frameBytes;
                for (int c = 0; c < channels; ++c) {
                    const Sint32 a = dcWavSample(p0 + c * bytesPerSample, bitsPerSample);
                    const Sint32 b = dcWavSample(p1 + c * bytesPerSample, bitsPerSample);
                    dst[w++]       = (Sint16)((a + b) >> 1);
                }
            }
        }
        else {
            for (size_t f = 0; f < n; ++f) {
                const byte *p = staging + f * frameBytes;
                for (int c = 0; c < channels; ++c) dst[w++] = dcWavSample(p + c * bytesPerSample, bitsPerSample);
            }
        }
    }

    *outSamples  = outFrames * (size_t)channels;
    *outRate     = halve ? sampleRate / 2 : sampleRate;
    *outChannels = channels;
    return dst;
}

// ---------------------------------------------------------------------
// Stage sfx in AICA sound RAM
// ---------------------------------------------------------------------
// Hidden Palace is the zone that forced this. Its 23 stage sfx want 1,469,818
// bytes at half rate, and by the time the stage loads there are only ~831KB of
// main heap left — in five holes, the largest 380KB — because the 2013 menu's
// models are, correctly, all resident now that FillFileBuffer stopped handing
// back short reads. That is a ~640KB CAPACITY shortfall, not a fragmentation
// one, and no arrangement of the bump arena can conjure memory that isn't
// there.
//
// The Dreamcast has a second 2MB the main heap never touches: the AICA's sound
// RAM. Sfx uploaded there cost the heap nothing, and play on the AICA's own
// channels, so they cost the SH4 no mixing time either. Only STAGE sfx move —
// the global set is small, loads once onto an empty heap and never reloads, and
// leaving it on the software mixer keeps the change contained to the thing that
// was actually failing.
//
// Two AICA constraints shape the loader:
//   * a channel plays at most 65534 samples, so each sfx is decimated by
//     whatever factor D >= 2 brings it under that. Across the whole game only
//     LoudHorn, LargeWall and Buzzsaw need D=3; everything else is D=2, the
//     same half-rate the software path already stores.
//   * sound-RAM buffers are 32-byte aligned and sized, so the stored sample
//     count is rounded up to a multiple of 16 and the pad zero-filled.
//
// Every failure path here returns false and the caller falls through to the
// existing heap arena, so the worst case is exactly today's behaviour.

#define DC_AICA_MAX_FRAMES   ((size_t)65504) // 65534, rounded down to a multiple of 32
#define DC_AICA_ALIGN_BYTES  ((size_t)32)    // sound-RAM buffers are 32-byte aligned and sized

// Samples are stored in sound RAM as SIGNED 8-bit, not 16.
//
// The AICA plays 8-bit natively (AICA_SM_8BIT), so this costs nothing at
// playback and halves what every sfx occupies. That is what makes it possible
// to put the ~40 GLOBAL sfx in sound RAM alongside the stage set: at 16-bit the
// globals need 1,550,144 B against the 1,372,064 B left after the music stream
// reservation and Green Hill's stage set — 178 KB over. At 8-bit they need
// 781,024 B and fit with ~591 KB spare.
//
// Quality: these are 44.1 kHz sources already being decimated to 22050 Hz, and
// the material is Mega Drive-era blips. 8-bit quantisation is most audible on
// the long sustained stings (1Up ~3.9s, Transform ~2.8s, Event) — if those ever
// sound rough, sound RAM has room to special-case them back to 16.
#define DC_AICA_SAMPLE_BITS  (8)
#define DC_AICA_SAMPLE_BYTES ((size_t)1)
// Floor for the retry-on-refusal path below. 44100 reaches it at /4, which is
// audibly duller but still recognisably the sound; going further is not worth it.
#define DC_AICA_MIN_RATE     ((size_t)11025)
#define DC_AICA_CHANNELS     64              // the AICA's channel count

static sfxhnd_t dcAicaSfx[SFX_COUNT];  // SFXHND_INVALID is 0, so this zero-inits correctly
static int      dcAicaRate[SFX_COUNT]; // stored (decimated) rate, passed back at play time
// Channel each sfx is currently playing on, stored +1 so 0 means "not playing".
static int   dcAicaChn[SFX_COUNT];
static sbyte dcAicaPan[SFX_COUNT];
static bool  dcAicaLoop[SFX_COUNT];
static bool  dcAicaRetuned[SFX_COUNT];
// Which sfx owns each AICA channel (+1; 0 == unowned). snd_sfx_play_ex hands
// channels out round-robin and will reuse one whose sound has finished, so
// "stop the channel I started on" is only safe while we still own it.
static int dcAicaChnOwner[DC_AICA_CHANNELS];

static size_t dcAicaBytesUsed = 0;
static int    dcAicaCount     = 0;

bool DC_AicaSfxHas(int sfxID) { return sfxID >= 0 && sfxID < SFX_COUNT && dcAicaSfx[sfxID] != SFXHND_INVALID; }

void DC_AicaSfxStop(int sfxID)
{
    if (sfxID < 0 || sfxID >= SFX_COUNT)
        return;

    const int chn = dcAicaChn[sfxID] - 1;
    if (chn >= 0 && chn < DC_AICA_CHANNELS && dcAicaChnOwner[chn] == sfxID + 1) {
        snd_sfx_stop(chn);
        dcAicaChnOwner[chn] = 0;
    }
    dcAicaChn[sfxID] = 0;
}

static void dcAicaSfxUnload(int sfxID)
{
    if (sfxID < 0 || sfxID >= SFX_COUNT)
        return;

    DC_AicaSfxStop(sfxID);

    if (dcAicaSfx[sfxID] != SFXHND_INVALID) {
        snd_sfx_unload(dcAicaSfx[sfxID]);
        dcAicaSfx[sfxID] = SFXHND_INVALID;
        if (dcAicaCount)
            --dcAicaCount;
    }

    dcAicaRate[sfxID]    = 0;
    dcAicaPan[sfxID]     = 0;
    dcAicaLoop[sfxID]    = false;
    dcAicaRetuned[sfxID] = false;
}

bool DC_AicaSfxLoad(size_t fileSize, int sfxID)
{
    if (sfxID < 0 || sfxID >= SFX_COUNT)
        return false;

    dcAicaSfxUnload(sfxID); // never leak a handle across a reload

    DCWavHeader wav;
    if (!dcWavReadHeader(fileSize, &wav))
        return false;

    const int    bytesPerSample = wav.bitsPerSample / 8;
    const size_t frameBytes     = (size_t)wav.channels * (size_t)bytesPerSample;
    const size_t srcFrames      = wav.dataLen / frameBytes;
    if (!srcFrames)
        return false;

    // Half rate to begin with, matching what the software path stores, then as
    // much more decimation as the channel-length limit demands.
    size_t decim = (wav.sampleRate >= 32000) ? 2 : 1;
    while (srcFrames / decim > DC_AICA_MAX_FRAMES) ++decim;

    // Transient: snd_sfx_load_raw_buf copies this into sound RAM and we hand it
    // straight back, so the heap only ever holds one of these at a time and at
    // most 64KB of it now that samples are 8-bit (the channel limit caps the
    // frame count). Handing it back
    // before the next sfx is read is what keeps this from fragmenting anything.
    size_t outFrames = 0, outBytes = 0;
    sbyte *buf = NULL;

    for (;;) {
        outFrames = srcFrames / decim;
        if (!outFrames)
            return false;

        // Round the BYTE count up to the 32-byte granularity rather than the
        // frame count: at 8 bits a frame is 1 byte (mono) or 2 (stereo), so a
        // frame-based pad would not land on 32 for both. The tail is left as
        // the memset zeroes below.
        outBytes = ((outFrames * (size_t)wav.channels * DC_AICA_SAMPLE_BYTES) + (DC_AICA_ALIGN_BYTES - 1)) & ~(DC_AICA_ALIGN_BYTES - 1);

        buf = (sbyte *)malloc(outBytes);
        if (buf)
            break;

        // Decimating once more halves this buffer too, so a refusal is worth one
        // more try rather than a fall-through. The fall-through is not the safe
        // option it looks like: the heap loader wants MORE memory than this did
        // (LoudHorn, 194,334 bytes there against 129,568 here), so on a heap too
        // tight for the staging buffer it is the one path that ends in silence.
        // A duller sound in sound RAM beats no sound at all.
        if ((size_t)wav.sampleRate / (decim * 2) < DC_AICA_MIN_RATE) {
            DC_WAV_FAIL("aica: %u-byte staging refused (largest free block %u)", (unsigned)outBytes, (unsigned)DC_LargestFreeBlock());
            return false;
        }

        decim *= 2;
#if DC_WAV_DIAG
        DC_LOG("[DCSfx] aica %d: %u-byte staging refused, retrying at /%u (%u Hz)\n", sfxID, (unsigned)outBytes, (unsigned)decim,
               (unsigned)(wav.sampleRate / (int)decim));
#endif
    }

    memset(buf, 0, outBytes);

    SetFilePosition((int)wav.dataPos);

    static byte staging[4096] __attribute__((aligned(4)));

    // Whole decimation groups per chunk, so a group can never straddle a read.
    size_t chunkFrames = sizeof(staging) / frameBytes;
    chunkFrames -= chunkFrames % decim;

    size_t framesLeft = outFrames * decim;
    size_t w          = 0;

    // Specialised for 16-bit source at /2, which is what every stage sfx in the
    // game actually is (44100 Hz down to 22050). Worth splitting out because the
    // generic loop below divides by `decim` — a RUNTIME value, so SH-4, having
    // no integer divide, calls __sdivsi3 for it. That is ~35 cycles per output
    // sample, and a regular stage decimates around half a million of them: Green
    // Hill alone reads 991,234 source frames across its 12 sounds. With the 2 a
    // constant the compiler emits the add-and-shift idiom instead, and the inner
    // per-sample loop and its dcWavSample call fold away with it.
    //
    // `(a + b) / 2` and not `(a + b) >> 1`: they differ on negative odd sums
    // (truncation toward zero versus toward negative infinity) and that survives
    // the >> 8 often enough to matter. Written as a divide by a literal, the
    // compiler produces the exact truncating form for free. Verified byte-for-
    // byte against the generic loop over 1M frames in every bits/channels/decim
    // combination the loader can produce.
    // Nothing to decimate: the source is already at or below the rate we want,
    // which is what a pre-converted 22050 Hz sfx set looks like. Split out for
    // the same reason as the /2 case below — the generic loop's `acc / decim`
    // is a runtime divide, and dividing by 1 through __sdivsi3 is the most
    // wasteful possible way to copy a sample.
    //   8-bit  in: (x - 128) << 8, then >> 8, i.e. just (x - 128).
    //   16-bit in: little-endian, then >> 8, i.e. just the signed high byte.
    if (decim == 1) {
        while (framesLeft) {
            const size_t n = framesLeft < chunkFrames ? framesLeft : chunkFrames;
            FileRead(staging, (int)(n * frameBytes));
            framesLeft -= n;

            const size_t samples = n * (size_t)wav.channels;
            if (wav.bitsPerSample == 8) {
                for (size_t s = 0; s < samples; ++s) buf[w++] = (sbyte)((int)staging[s] - 128);
            }
            else {
                for (size_t s = 0; s < samples; ++s) buf[w++] = (sbyte)staging[s * 2 + 1];
            }
        }
    }
    else if (decim == 2 && wav.bitsPerSample == 16) {
        while (framesLeft) {
            const size_t n = framesLeft < chunkFrames ? framesLeft : chunkFrames;
            FileRead(staging, (int)(n * frameBytes));
            framesLeft -= n;

            const byte *q = staging;
            if (wav.channels == 1) {
                for (size_t f = 0; f + 2 <= n; f += 2, q += 4) {
                    const Sint32 a = (Sint16)((ushort)q[0] | ((ushort)q[1] << 8));
                    const Sint32 b = (Sint16)((ushort)q[2] | ((ushort)q[3] << 8));
                    buf[w++]       = (sbyte)(((a + b) / 2) >> 8);
                }
            }
            else {
                for (size_t f = 0; f + 2 <= n; f += 2, q += 2 * frameBytes) {
                    for (int c = 0; c < wav.channels; ++c) {
                        const byte  *r = q + (size_t)c * 2;
                        const Sint32 a = (Sint16)((ushort)r[0] | ((ushort)r[1] << 8));
                        const Sint32 b = (Sint16)((ushort)r[frameBytes] | ((ushort)r[frameBytes + 1] << 8));
                        buf[w++]       = (sbyte)(((a + b) / 2) >> 8);
                    }
                }
            }
        }
    }
    else {
        while (framesLeft) {
            const size_t n = framesLeft < chunkFrames ? framesLeft : chunkFrames;
            FileRead(staging, (int)(n * frameBytes));
            framesLeft -= n;

            for (size_t f = 0; f + decim <= n; f += decim) {
                for (int c = 0; c < wav.channels; ++c) {
                    Sint32 acc = 0;
                    for (size_t d = 0; d < decim; ++d)
                        acc += dcWavSample(staging + (f + d) * frameBytes + (size_t)c * bytesPerSample, wav.bitsPerSample);
                    // Decimate, then take the top 8 bits. AICA 8-bit is signed,
                    // so an arithmetic shift is the whole conversion — no bias.
                    buf[w++] = (sbyte)((acc / (Sint32)decim) >> 8);
                }
            }
        }
    }

    const uint32_t outRate = (uint32_t)(wav.sampleRate / (int)decim);
    const sfxhnd_t h       = snd_sfx_load_raw_buf((char *)buf, outBytes, outRate, DC_AICA_SAMPLE_BITS, (uint16_t)wav.channels);

    free(buf);

    if (h == SFXHND_INVALID) {
        DC_WAV_FAIL("aica: sound RAM refused %u bytes (%u free after %d sfx)", (unsigned)outBytes, (unsigned)snd_mem_available(), dcAicaCount);
        return false;
    }

    dcAicaSfx[sfxID]  = h;
    dcAicaRate[sfxID] = (int)outRate;
    dcAicaBytesUsed += outBytes;
    ++dcAicaCount;

#if DC_WAV_DIAG
    DC_LOG("[DCSfx] aica %d: %u frames /%u -> %u Hz, %u bytes (%d loaded, %u used, %u sound RAM free)\n", sfxID, (unsigned)srcFrames,
           (unsigned)decim, (unsigned)outRate, (unsigned)outBytes, dcAicaCount, (unsigned)dcAicaBytesUsed, (unsigned)snd_mem_available());
#endif

    return true;
}

static void dcAicaStart(int sfxID, bool loop, sbyte pan)
{
    DC_AicaSfxStop(sfxID);

    sfx_play_data_t d;
    memset(&d, 0, sizeof(d));

    d.chn = -1; // next free channel
    d.idx = dcAicaSfx[sfxID];

    // The software mixer applies sfxVolume alone per sfx and leaves
    // masterVolume to the music, so match that and scale into the AICA's 0-255.
    int vol = (sfxVolume * 255) / MAX_VOLUME;
    if (vol < 0)
        vol = 0;
    if (vol > 255)
        vol = 255;
    d.vol = vol;

    // Engine pan is -100 (left) .. 0 (centre) .. 100 (right); the AICA wants
    // 0 .. 128 .. 255.
    int p = 128 + (((int)pan * 127) / 100);
    if (p < 0)
        p = 0;
    if (p > 255)
        p = 255;
    d.pan = p;

    d.loop      = loop ? 1 : 0;
    d.freq      = dcAicaRate[sfxID];
    d.loopstart = 0;
    d.loopend   = 0; // 0 == the whole sound

    const int chn = snd_sfx_play_ex(&d);
    if (chn >= 0 && chn < DC_AICA_CHANNELS) {
        // Whatever held this channel before has just been cut off by the AICA,
        // so drop its claim rather than letting it stop us later.
        const int prev = dcAicaChnOwner[chn];
        if (prev && prev - 1 != sfxID)
            dcAicaChn[prev - 1] = 0;

        dcAicaChnOwner[chn] = sfxID + 1;
        dcAicaChn[sfxID]    = chn + 1;
    }
    else {
        dcAicaChn[sfxID] = 0;
    }

    dcAicaPan[sfxID]     = pan;
    dcAicaLoop[sfxID]    = loop;
    dcAicaRetuned[sfxID] = false;
}

bool DC_AicaSfxPlay(int sfxID, bool loop)
{
    if (!DC_AicaSfxHas(sfxID))
        return false;

    dcAicaStart(sfxID, loop, 0);
    return true;
}

bool DC_AicaSfxSetAttr(int sfxID, int loopCount, sbyte pan)
{
    if (!DC_AicaSfxHas(sfxID))
        return false;

    const bool loop = (loopCount == -1) ? dcAicaLoop[sfxID] : (loopCount != 0);

    // A running AICA channel's pan can't be changed from here, and RSDK's
    // scripts almost always call SetSFXAttributes in the same frame as PlaySfx
    // — so re-triggering costs a few microseconds of a sound nobody has heard
    // yet. Guarded twice so it can never become a stutter: only when something
    // actually changed, and only once per playback, in case some object sets
    // attributes every frame on a looping sound.
    if (dcAicaChn[sfxID] && !dcAicaRetuned[sfxID] && (pan != dcAicaPan[sfxID] || loop != dcAicaLoop[sfxID])) {
        dcAicaStart(sfxID, loop, pan);
        dcAicaRetuned[sfxID] = true;
        return true;
    }

    dcAicaPan[sfxID]  = pan;
    dcAicaLoop[sfxID] = loop;
    return true;
}

void DC_AicaSfxStopAll()
{
    for (int i = 0; i < SFX_COUNT; ++i)
        if (dcAicaChn[i])
            DC_AicaSfxStop(i);
}

void DC_AicaSfxUnloadStage()
{
    if (!dcAicaCount) {
        dcAicaBytesUsed = 0;
        return;
    }

    DC_LOG("[DCSfx] aica: releasing %d sfx (%u bytes of sound RAM)\n", dcAicaCount, (unsigned)dcAicaBytesUsed);

    for (int i = 0; i < SFX_COUNT; ++i) dcAicaSfxUnload(i);

    dcAicaBytesUsed = 0;
    dcAicaCount     = 0;

    DC_LOG("[DCSfx] aica: %u bytes of sound RAM free\n", (unsigned)snd_mem_available());
}

// ---------------------------------------------------------------------
// Loose raw-PCM sound effects
// ---------------------------------------------------------------------
// The global sfx set ships as headerless raw PCM alongside Data.rsdk instead
// of as Vorbis inside it. That is the whole reason libtremor can eventually
// leave the build: the packed .ogg sfx were the only genuinely live user of
// it (LoadMusic's vorbis tail and StreamInfo's OggVorbis_File member are
// artifacts that guards can remove once nothing decodes any more).
//
// Two properties are inherited from the music path deliberately:
//
//   * The files keep their .ogg names. GameConfig.bin stores the extension,
//     so renaming them would mean repacking Data.rsdk. Content, not
//     extension, decides how they are read — exactly as DC_NativeTryLoadDtex
//     identifies textures by their 'DTEX' magic while they are named .png.
//   * They are opened directly, not through LoadFile. LoadFile MD5-resolves
//     the datapack first and its loose-file fallback never runs, so a loose
//     file could not otherwise override a packed one.
//
// FORMAT IS A CONTRACT. A headerless file states nothing about itself, so
// these constants and the assets have to move together — re-encode at
// another rate without changing DC_RAWSFX_RATE and every sound plays at the
// wrong pitch, silently. The conversion is:
//   ffmpeg -i in.ogg -f s8 -ac 1 -ar 22050 out.ogg
#define DC_RAWSFX_RATE     22050
#define DC_RAWSFX_CHANNELS 1

Sint16 *DC_LoadRawSfx(const char *fullPath, size_t *outSamples, int *outRate, int *outChannels)
{
    char path[0x110];
    int  i = 0;
    for (const char *p = BASE_PATH; *p && i < (int)sizeof(path) - 1; ++p) path[i++] = *p;
    for (const char *p = fullPath; *p && i < (int)sizeof(path) - 1; ++p) path[i++] = *p;
    path[i] = '\0';

    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL; // not shipped loose — the packed asset is used instead

    // A real Vorbis file starts "OggS". Finding one means this sound was
    // missed by the conversion; say so rather than playing the container as
    // if it were samples, and let the caller fall back to the packed path.
    char magic[4];
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic)) {
        fclose(f);
        return NULL;
    }
    if (magic[0] == 'O' && magic[1] == 'g' && magic[2] == 'g' && magic[3] == 'S') {
        fclose(f);
        DC_LOG("[DCSfx] raw: '%s' is still Vorbis - using the packed asset\n", fullPath);
        return NULL;
    }
    // Sonic 1 addition. Sonic 2's global sfx ship as .ogg, so a stray RIFF file
    // was never a possibility there; Sonic 1's are .wav, and a folder of real
    // RIFF wavs sitting at these paths is the likely first mistake. Reading one
    // as headerless PCM plays the 78-byte header as samples and misreads
    // unsigned bytes as signed - loud noise rather than an obvious failure. So
    // check for it and fall back to the packed asset instead.
    if (magic[0] == 'R' && magic[1] == 'I' && magic[2] == 'F' && magic[3] == 'F') {
        fclose(f);
        DC_LOG("[DCSfx] raw: '%s' is a RIFF wav, not headerless PCM - using the packed asset\n", fullPath);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long fileBytes = ftell(f);
    if (fileBytes <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    // One s8 sample per byte, widened to the Sint16 the mixer consumes
    // (sfxList[].buffer is Sint16 *), so the resident cost is 2x the file.
    Sint16 *out = (Sint16 *)malloc((size_t)fileBytes * sizeof(Sint16));
    if (!out) {
        fclose(f);
        DC_LOG("[DCSfx] raw: %ld bytes refused for '%s' (largest free block %u)\n", fileBytes * 2, fullPath,
               (unsigned)DC_LargestFreeBlock());
        return NULL;
    }

    // Read through a small static block rather than slurping the file and
    // converting in place: peak == kept, the same reason DC_LoadWAVStreamed
    // exists. 4KB is one GD-ROM-friendly chunk and costs nothing resident.
    static signed char staging[4096];
    long done = 0;
    while (done < fileBytes) {
        long want = fileBytes - done;
        if (want > (long)sizeof(staging))
            want = (long)sizeof(staging);
        const size_t got = fread(staging, 1, (size_t)want, f);
        if (!got)
            break;
        for (size_t k = 0; k < got; ++k) out[done + k] = (Sint16)(staging[k] << 8);
        done += (long)got;
    }
    fclose(f);

    if (done != fileBytes) {
        free(out);
        DC_LOG("[DCSfx] raw: short read on '%s' (%ld of %ld)\n", fullPath, done, fileBytes);
        return NULL;
    }

    *outSamples  = (size_t)fileBytes; // mono: one Sint16 per sample
    *outRate     = DC_RAWSFX_RATE;
    *outChannels = DC_RAWSFX_CHANNELS;

#if DC_WAV_DIAG
    DC_LOG("[DCSfx] raw: '%s' %ld samples @ %d Hz mono (%ld bytes resident)\n", fullPath, fileBytes, DC_RAWSFX_RATE,
           fileBytes * 2);
#endif
    return out;
}

#endif // RETRO_USING_KOS
