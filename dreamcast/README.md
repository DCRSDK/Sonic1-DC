# Sonic 1 / RSDKv4 — Dreamcast asset conversion

Mirrors the Sonic CD (RSDKv3) pipeline in shape, **but not in settings** —
three of Sonic CD's are wrong for this port. See "Differences" below before
copying anything across.

## Usage

```sh
./generate_assets.sh /path/to/Data.rsdk [stage_dir] [--menu-no-vq] [--skip-menu]
```

That is the whole input — the game's own datafile. Extraction is built in, so
nothing has to be unpacked by hand first, and `Data.rsdk` is never modified.

`stage_dir` defaults to `../build-dc`, which is where the loose `Data/` tree
has to sit next to the untouched `Data.rsdk` anyway. The disc needs **both**.

Requires `bash`, `python3`, `ffmpeg`, `ffprobe`, plus `python3-pil` and
`python3-numpy` for the menu textures. `--skip-menu` drops that dependency and
the five minutes the VQ pass takes; `--menu-no-vq` keeps the textures but uses
plain 16bpp, which does **not** fit in VRAM at full resolution — see below.

Budget **about five minutes** in total. The audio is seconds; the menu VQ pass
is nearly all of it (45 textures, most of them 1024×1024, clustered one at a
time).

A directory holding `Data.rsdk` is accepted too, as is an already-extracted
loose tree, for anyone who has one.

## What comes out

| path | format |
|---|---|
| `Data/Music/*.ogg` | headerless signed 8-bit, **stereo**, 22050 Hz |
| `Data/SoundFX/Global/*.wav` | headerless signed 8-bit, **mono**, 22050 Hz |
| `Data/Game/Menu/*.png` | VQ `.dtex`, 16-byte header + 2 KB codebook + index array |
| `Data/Game/Models/*.png` | ditto — the About-screen box art |

Every one keeps its original extension. The audio does because `GameConfig.bin`
and the StageConfigs resolve assets by filename; the textures do because
`DC_NativeTryLoadDtex` identifies the format by its `DTEX` magic, not the
extension. Only the contents change — none of these is a container, a RIFF or a
PNG any more. The names are a lie the loaders are in on.

Everything else — sprites, stages, scripts, palettes, the `.fnt` bitmap fonts,
**and stage sfx** — keeps streaming out of `Data.rsdk`, untouched.

## The 2013 menu textures

The 2013 menu is drawn by `DCNative.cpp`, not by the script engine, and
`DC_NativeTryLoadDtex` reads its textures with a plain `fopen` on
`BASE_PATH + path`, bypassing `LoadFile` entirely. That is the whole reason a
loose `Data/Game/` tree exists: those files **cannot** come from the pack.

They also cannot stay PNGs. `stbi_load` fails on the Dreamcast's fragmented
heap for anything much over 256×256, and every one of these is 512×512 or
larger, so before the `.dtex` path existed most of the menu was simply
invisible — with nothing logged. A `.dtex` is streamed into VRAM in 32 KB
chunks with no decode at all.

VQ is the default because that is what ships. Checked against the working
build: all of its menu textures are VQ at the converter's default `maxdim` of
1024 — format 3 (VQ/ARGB4444) for most, format 4 (VQ/ARGB1555) for `Intro`,
`HelpGraphics` and `TimeAttack1`–`4`. VQ is a 256-entry codebook of 2×2 texel
blocks plus one index byte per block: about 1/8 the VRAM of plain 16bpp, at
full resolution. The set comes to ~9.1 MB on disc; without VQ it does not fit
in 8 MB of VRAM at all and you have to drop to `--maxdim 512`, which visibly
softens the artwork.

Resident cost would exceed VRAM if everything were loaded at once. That is
expected: the menu never needs all of them together, and
`DC_NativeResetTextures` / `DC_NativeReloadEvicted` stream them back on demand.

### Which files, and which are dead weight

`rsdkv4_filelist.txt` lists the 48 paths that appear as `LoadTexture()` string
literals in `RSDKv4/NativeObjects` (with `TimeAttack%d.png`, the only name the
code builds at runtime, expanded to 1–4). Those are the only paths
`DC_NativeTryLoadDtex` will ever open.

The retail pack also holds `Amazon`, `Sonic`, `Heading_JA`, `Label_JA`,
`Text_JA` and `Text_KO@1x`, which **nothing in the engine references** — the
JA/KO/ZH languages only ever load their `@1x` variants, and `Amazon`/`Sonic`
are not referenced at all. They are in the currently shipped
`build-dc/Data/Game/Menu` only because the whole extracted folder was converted
by hand; at 1024×1024 VQ that is ~1.5 MB of disc the console never reads. They
are listed, commented out, at the end of `rsdkv4_filelist.txt` if you want the
disc to match byte for byte anyway.

## Differences from the Sonic CD scripts

| | Sonic CD (v3) | Sonic 1 (v4) | why |
|---|---|---|---|
| music channels | mono | **stereo** | `DC_MusicReadSamples` reads two bytes per frame; the ring is `22050Hz stereo s8`. Mono plays at half speed with channels read as alternating samples. |
| sfx container | RIFF WAV `pcm_u8` | **headerless `s8`** | `DC_LoadRawSfx` sniffs for `RIFF` and *refuses* it, falling back to the packed asset. A RIFF conversion is silently ignored, not rejected loudly. |
| sfx scope | Global + Stage | **Global only** | the loose-file override for stage sfx was measured and reverted — 11–22 extra file opens per stage load cost more than the smaller reads saved. |
| unpacking | walks a directory table | **bootstraps from `GameConfig.bin` + a filename list** | RSDKv3 packs store plaintext paths; RSDKv4 stores only an MD5 of each path, so there is nothing to enumerate. See below. |
| menu art | nothing equivalent | **VQ `.dtex`** | Sonic CD has no native 2013 menu. RSDKv5's `img_step_1_todtex.sh` is the nearest relative; Sonic 2's `menu_step_1_dtex.sh` is the direct one. |

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
- The **menu and model textures** are in the same position as the music, and for
  the same reason: the native UI loads them from string literals in C++, so
  nothing in the pack names them either. They are in the same list, harvested
  from the engine source.

Entries are decrypted with the key schedule from `FileRead()` in
`RSDKv4/Reader.cpp`, keyed on the entry's own size. Verified against the
shipping assets: all 26 music tracks and all 40 global sfx come out
byte-identical to the converted tree already running on hardware, and all 48
menu/model textures come out as real PNGs whose converted `.dtex` headers match
the shipped ones field for field — same format choice, same dimensions, same
byte counts.

By default it writes `Music/`, `SoundFX/Global/`, `Game/Menu/` and
`Game/Models/`, which is exactly what the Dreamcast build reads loose. `--all`
also writes the stage sfx and `GameConfig.bin` — useful for looking around, but
note that a loose `Game/GameConfig.bin` on the disc would *override* the packed
one, so do not ship an `--all` tree.

## Verification

`verify_assets.sh <stage_data> [source_data]` runs automatically at the end of
`generate_assets.sh`. It exists because a headerless file states nothing about
itself — a wrong rate or channel count is not an error anywhere, just a pitch
shift. That has already cost this port a round trip: a global sfx set converted
at 11025 against a `DC_RAWSFX_RATE` of 22050 came out exactly an octave high,
and every file was exactly 0.5× the expected size.

Given the source tree it checks `bytes == rate * channels * seconds` per file,
to 1%. The menu textures get the same treatment for the same reason — a PNG
left in `Game/` is invisible on console and logs nothing. Tested against every
way this goes wrong:

```
FAIL Jump.wav       4785 bytes, expected ~9569  (0.50x -- wrong rate or channel count?)
FAIL odd byte count (not whole stereo frames): Ending.ogg
FAIL still a real Ogg container: GreenHill.ogg
FAIL RIFF header -- DC_LoadRawSfx will refuse this and use the packed asset: Ring.wav
FAIL still a real PNG -- the Dreamcast cannot decode it, the menu will be missing it: Game/Menu/Circle.png
FAIL Data/Game/GameConfig.bin is staged loose -- it overrides the packed one. Delete it.
```

A clean run prints the texture tally alongside the audio:

```
menu art:    48 checked, 0 wrong (48 VQ, 0 plain 16bpp)
music:       26 checked, 0 wrong size
global sfx:  40 checked, 0 wrong size
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
