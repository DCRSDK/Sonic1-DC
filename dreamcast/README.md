# Sonic 1 / RSDKv4 — Dreamcast asset conversion

Mirrors the Sonic CD (RSDKv3) pipeline in shape, **but not in settings** —
three of Sonic CD's are wrong for this port. See "Differences" below before
copying anything across.

## Usage

```sh
./generate_assets.sh /path/to/Data.rsdk [stage_dir]
```

That is the whole input — the game's own datafile. Extraction is built in, so
nothing has to be unpacked by hand first, and `Data.rsdk` is never modified.

`stage_dir` defaults to `../build-dc`, which is where the loose `Data/` tree
has to sit next to the untouched `Data.rsdk` anyway. The disc needs **both**.

Requires `bash`, `python3`, `ffmpeg`, `ffprobe`.

A directory holding `Data.rsdk` is accepted too, as is an already-extracted
loose tree, for anyone who has one.

## What comes out

| path | format |
|---|---|
| `Data/Music/*.ogg` | headerless signed 8-bit, **stereo**, 22050 Hz |
| `Data/SoundFX/Global/*.wav` | headerless signed 8-bit, **mono**, 22050 Hz |

Both keep their original extensions — `GameConfig.bin` and the StageConfigs
resolve assets by filename, so only the contents change. Neither is a container
or a RIFF file any more; the names are a lie the loader is in on.

Everything else — sprites, stages, scripts, palettes, **and stage sfx** — keeps
streaming out of `Data.rsdk`, untouched.

## Differences from the Sonic CD scripts

| | Sonic CD (v3) | Sonic 1 (v4) | why |
|---|---|---|---|
| music channels | mono | **stereo** | `DC_MusicReadSamples` reads two bytes per frame; the ring is `22050Hz stereo s8`. Mono plays at half speed with channels read as alternating samples. |
| sfx container | RIFF WAV `pcm_u8` | **headerless `s8`** | `DC_LoadRawSfx` sniffs for `RIFF` and *refuses* it, falling back to the packed asset. A RIFF conversion is silently ignored, not rejected loudly. |
| sfx scope | Global + Stage | **Global only** | the loose-file override for stage sfx was measured and reverted — 11–22 extra file opens per stage load cost more than the smaller reads saved. |
| unpacking | walks a directory table | **bootstraps from `GameConfig.bin` + a filename list** | RSDKv3 packs store plaintext paths; RSDKv4 stores only an MD5 of each path, so there is nothing to enumerate. See below. |

## Where the assets come from

`rsdkv4_extract.py` reads `Data.rsdk` directly. It is a standalone script and
can be run on its own:

```sh
python3 rsdkv4_extract.py Data.rsdk outdir [--all] [--names=file]
```

The awkward part is that **RSDKv4 has no file list**. `CheckFileInfo` looks an
asset up by the MD5 of its lowercased path, and that hash is all the pack
stores, so the names cannot be read back out — each one has to be known before
it can be found. So the extractor bootstraps:

- `Data/Game/GameConfig.bin` is at a path we know. It names every **global
  sound effect** and lists every stage folder, and each stage's
  `StageConfig.bin` then names that stage's sfx. This is authoritative and
  works on a pack nobody has ever listed.
- **Music** is the one class nothing in the pack can tell us about — RSDKv4
  sets tracks from script bytecode, not from a config — so music names come
  from `rsdkv4_filelist.txt` (compiled from RetroED). Anything in the list that
  is not in the pack is simply skipped, so the same list covers Sonic 1 and
  Sonic 2.

Entries are decrypted with the key schedule from `FileRead()` in
`RSDKv4/Reader.cpp`, keyed on the entry's own size. Verified against the
shipping assets: all 26 music tracks and all 40 global sfx come out
byte-identical to the converted tree already running on hardware.

By default only `Music/` and `SoundFX/Global/` are written, which is all the
Dreamcast build reads loose. `--all` also writes the stage sfx and
`GameConfig.bin`, which is useful for inspection but not for the disc.

## Verification

`verify_assets.sh <stage_data> [source_data]` runs automatically at the end of
`generate_assets.sh`. It exists because a headerless file states nothing about
itself — a wrong rate or channel count is not an error anywhere, just a pitch
shift. That has already cost this port a round trip: a global sfx set converted
at 11025 against a `DC_RAWSFX_RATE` of 22050 came out exactly an octave high,
and every file was exactly 0.5× the expected size.

Given the source tree it checks `bytes == rate * channels * seconds` per file,
to 1%. Tested against all four ways this goes wrong:

```
FAIL Jump.wav       4785 bytes, expected ~9569  (0.50x -- wrong rate or channel count?)
FAIL odd byte count (not whole stereo frames): Ending.ogg
FAIL still a real Ogg container: GreenHill.ogg
FAIL RIFF header -- DC_LoadRawSfx will refuse this and use the packed asset: Ring.wav
```

## Format is a contract

These settings and the constants in `RSDKv4/Dreamcast/DCAudio.cpp` have to move
together:

| script | constant |
|---|---|
| music 22050 / stereo | `DC_MUS_SRC_RATE`, the `dcMusRing` stereo assumption |
| global sfx 22050 | `DC_RAWSFX_RATE` |
| global sfx mono | `DC_RAWSFX_CHANNELS` |

22050 × 2ch × 1 byte = 44,100 B/s is also the figure the credits timing
(`Credits.ogg` = 5,351,202 B = 121.3 s) and the dcload chunk sizing were
measured against. Changing the music format invalidates both.

## Not done here: repacking stage sfx

Stage sfx are still 44.1 kHz 16-bit *inside* `Data.rsdk`, decimated to
22050/8-bit at load. That is ~17% of hardware load time and makes those entries
4× larger than they need to be — for Scrap Brain, ~1,284 KB of its 2,279 KB
read. The fix is to **repack** them at 22050/8-bit, not to stage them loose.

One gotcha first: the AICA's 65,504-frame cap forces long sounds to decimate
harder, and pre-halving the source leaves the loader less room. A 3.7 s sound
currently lands at 14700 Hz; from an already-22050 pack it would land at
11025 Hz. Repacking would need those trimmed or split.
