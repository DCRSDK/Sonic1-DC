# Building RSDKv4 (Sonic 1) for SEGA Dreamcast

Dreamcast port based on the KallistiOS techniques from the RSDKv5 Dreamcast
port by SF94 / Michael Fadely (`RSDKv5-Decompilation`, branch
`sf94/dreamcast-kallistios-pvr`).

## What this port does

- **Hardware rendering through the PVR.** Tile layers and sprites are
  submitted as textured quads. Per-line parallax is preserved exactly by
  splitting the screen into bands wherever the layer's `lineScroll` index
  changes, and per-line water deformation is handled the same way — a band
  also ends wherever the deformation offset changes and at the water line.
  A layer whose wobble is too fine to afford falls back to software, and
  3D stages (the Special Stage half-pipe) stay software throughout because
  they rely on paint-over compositing.
- **Sprites are palette-split at the water line**, so a sprite straddling the
  surface is drawn with the right bank either side of it rather than taking
  one bank for the whole sprite.
- **Audio: no Vorbis decoding on-console.** libtremor and libogg are gone.
  Music is raw PCM streamed from disc by a reader thread; global sound effects
  are raw PCM loaded loose; stage sound effects come out of `Data.rsdk` and are
  decimated into the AICA's own sound RAM so they play on hardware voices.
  The engine's software mixer drives a KOS `snd_stream` at 44.1 kHz S16 stereo.
- **Input:** maple controller (D-pad/analog, A/B/X/Y, triggers, Start;
  **L+R+Start = Select/dev menu**)
- **Saves are persistent on VMU**, including a custom BIOS icon. Writes are
  deferred to safe flush points (stage loads, the pause menu) rather than
  taken at the end of an act.
- **Game data** is read from `/cd/` by default, or `/pc/` over dcload — see
  "Running over dcload" below.
- Mod loader, networking (2P VS), haptics: disabled.

## Prerequisites (in the KOS Ubuntu VM)

1. KallistiOS environment installed and working (`kos-cc` in PATH after
   `source /opt/toolchains/dc/kos/environ.sh`). Built and tested against
   **KOS 2.3.0 / sh-elf-gcc 14.4.1**.

   `2.3.0` is the version **master** reports, not a release — there is no
   `v2.3.0` tag and the newest tag is `v2.2.2`, so cloning `v2.3.0` fails.
   The reference build is master at commit `14bc62b6` (2026-07-11). Note that
   on master the toolchain builder is `utils/kos-chain`, configured by
   `Makefile.cfg`; it was `utils/dc-chain` with a `config.mk` up to 2.2.x.
   `sh-elf-gcc 14.4.1` is the `14.4.1-dev` kos-chain profile (GCC 14 from
   git); the `14.4.0` release profile builds this port fine too.

   The ARM toolchain is optional: KOS ships a precompiled AICA driver
   (`stream.drv.prebuilt`) and falls back to it when `DC_ARM_CC` is absent.
2. `ffmpeg` and `python3` for asset generation, plus `python3-pil` and
   `python3-numpy` for the menu textures (`--skip-menu` drops both).
3. `mkdcdisc` for .cdi packaging: https://gitlab.com/simulant/mkdcdisc
4. For testing: Flycast or Redream (or dcload/dc-tool for hardware dev)

No kos-ports packages are required. libtremor was dropped once nothing on the
console decoded Vorbis any more.

## Build

```sh
source /opt/toolchains/dc/kos/environ.sh   # if not already done in .bashrc

cd RSDKv4-Decompilation-Sonic1
mkdir build-dc && cd build-dc
kos-cmake -DPLATFORM=KallistiOS -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

Produces `RSDKv4.elf` (statically linked KOS ELF).

Debug build (dev menu on, logs over dcload console):

```sh
kos-cmake -DPLATFORM=KallistiOS -DCMAKE_BUILD_TYPE=Release -DRSDK_DEBUG=ON ..
```

## Generate the assets

**Do this before packaging.** Three asset classes have to be converted and left
loose on the disc; a disc built without them boots, but with silent music, no
global sound effects and an invisible 2013 menu. Everything else (sprites,
stages, scripts, palettes, bitmap fonts, and stage sfx) streams out of
`Data.rsdk` untouched and needs no conversion.

- **Music and global sfx** — the build links no Vorbis decoder, so these become
  raw PCM.
- **The 2013 menu and model textures** — `DC_NativeTryLoadDtex` reads these with
  a plain `fopen` on `BASE_PATH + path`, bypassing `LoadFile`, so they cannot
  come from the pack at all. They also cannot stay PNGs: `stbi_load` fails on
  the DC's fragmented heap above ~256×256 and all of these are 512×512 or
  larger, so they become VQ `.dtex` streamed straight into VRAM.

All you need is your own `Data.rsdk` from the 2013 Sonic 1 mobile remaster.
Extraction is built in, and the pack is only read, never modified.

```sh
cd dreamcast
./generate_assets.sh /path/to/Data.rsdk ../build-dc
```

`rsdkv4_extract.py` does the unpacking. RSDKv4 stores only an MD5 of each asset
path, so there is no directory table to walk and every name has to be known in
advance — it bootstraps from `Data/Game/GameConfig.bin` for the sound effects
and from the bundled `rsdkv4_filelist.txt` for the music, which RSDKv4 names
only in script bytecode. An already-extracted loose tree is still accepted in
place of the pack.

Requires `python3` as well as `ffmpeg`.

Takes about five minutes, nearly all of it the menu VQ pass. `--skip-menu` or
`--menu-no-vq` are there if you need it faster; `--menu-no-vq` will not fit in
VRAM at full resolution, so it is a diagnostic, not a shipping option.

That stages and converts:

| output | format |
|---|---|
| `build-dc/Data/Music/*.ogg` | headerless signed 8-bit, **stereo**, 22050 Hz |
| `build-dc/Data/SoundFX/Global/*.wav` | headerless signed 8-bit, **mono**, 22050 Hz |
| `build-dc/Data/Game/Menu/*.png` | VQ `.dtex` (45 textures, ~8.4 MB) |
| `build-dc/Data/Game/Models/*.png` | VQ `.dtex` (3 textures, ~0.8 MB) |

Everything keeps its original extension. The audio does because
`GameConfig.bin` resolves assets by filename; the textures do because
`DC_NativeTryLoadDtex` identifies the format by its `DTEX` magic rather than
the extension. Only the contents change — none of them is a container, a RIFF or
a PNG any more.

`rsdkv4_extract.py` deliberately does **not** write a loose
`Data/Game/GameConfig.bin`: one next to `Data.rsdk` would override the packed
one. `verify_assets.sh` fails if it finds it.

`verify_assets.sh` runs automatically at the end and cross-checks every output
against its source duration. This matters more than it sounds: a headerless
file carries no sample rate, so converting at the wrong one is not an error
anywhere — it is a silent pitch shift. See `dreamcast/README.md` for the format
contract and which script setting pairs with which constant in `DCAudio.cpp`.

## Package a .cdi (GDEMU / emulator)

You need your own `Data.rsdk` from the 2013 Sonic 1 mobile remaster.

```sh
mkdcdisc -e RSDKv4.elf \
         -f Data.rsdk \
         -f settings.ini \
         -d Data \
         -o sonic1.cdi -n "SONIC 1"
```

Run this from `build-dc`, so the converted `Data/` tree generated above goes on
the disc next to the untouched `Data.rsdk`. `settings.ini` overrides engine
defaults (the port reads `/cd/settings.ini`).

Test in Flycast or Redream first; then drop `sonic1.cdi` on the GDEMU SD card.

## Running over dcload

dcload-ip is supported as a way to play, not just a dev transport. Configure
with `/pc/` as the data root, **in its own build directory** — `DC_DATA_ROOT`
is a cached variable, so reconfiguring `build-dc` will not pick it up:

```sh
cd RSDKv4-Decompilation-Sonic1
mkdir build-dcload && cd build-dcload
kos-cmake -DPLATFORM=KallistiOS -DCMAKE_BUILD_TYPE=Release -DDC_DATA_ROOT=/pc/ ..
make -j$(nproc)
```

Watch the configure output for both of these:

```
-- RSDKv4/DC: game data root is /pc/
-- RSDKv4/DC: dcload transport - music reads in small chunks
```

The second line confirms the port knows it is on dcload. Every dcload call runs
with interrupts disabled for the whole transfer, so a large read halts the
machine rather than merely occupying the link; on `/pc/` the music reader uses
small, frame-sized reads instead so the stalls stay inside a frame's slack.

Then serve the `build-dc` contents (`Data.rsdk`, `Data/`, `settings.ini`) as
`/pc/` and upload:

```sh
dc-tool-ip -t <dreamcast-ip> -x RSDKv4.elf
```

You can also boot over dcload while the game reads `/cd/` from a disc or GDEMU
in the drive — build normally, upload the ELF, and you keep the console output
without putting game data on the link. Load times measured this way are the
real ones; `/pc/` timings are not comparable.

## Known limitations

- 2P VS and anything else needing networking is disabled
- Mod loader is disabled
- Leaderboard uploads have nowhere to go (local scores are still kept)
- The end credits music runs out before the sequence finishes. The credits are
  frame-timed while the music is wall-clock, so the stage loads between demos
  stretch a 128s sequence to around 156s against a 121s track. `Ending.ogg` is
  chained onto `Credits.ogg` to cover the final demo; the closing screens still
  play out quiet.
- Special stages are the heaviest scenes (software 3D) and do not hold 60fps
  throughout

## Files added/changed vs upstream RSDKv4-Decompilation

- `platforms/KallistiOS.cmake` (new)
- `dreamcast/` — asset extraction and conversion scripts (new), including
  `rsdkv4_extract.py`, a standalone RSDKv4 datapack reader
- `RSDKv4/Dreamcast/DCCommon.hpp, DCGraphics.cpp, DCAudio.cpp, DCInput.cpp,
  DCNative.cpp, DCSave.cpp, DCSystem.cpp` (new)
- `RSDKv4/RetroEngine.hpp` — RETRO_DC platform, KOS includes, SDL-type typedefs
- `RSDKv4/Drawing.cpp` — KOS init/flip/release hooks, hardware tile band
  renderer with per-line deformation
- `RSDKv4/Sprite.cpp` — faster GIF decode; `RemoveGraphicsFile` compacts the
  sprite pool with one `memmove` over live data instead of a byte loop over 3MB
- `RSDKv4/Scene.cpp` — stage-load phase timing, tileset cache
- `RSDKv4/Reader.cpp`, `Reader.hpp` — GD-ROM stdio buffer tuning, entry-end
  clamp, sector-aligned seeking
- `RSDKv4/Audio.cpp`, `Audio.hpp` — KOS audio device, raw-PCM music and sfx
  paths, AICA stage sfx, lock macros
- `RSDKv4/Input.cpp` — KOS input hook + non-SDL guard fix in `controllerClose`
- `RSDKv4/RetroEngine.cpp` — KOS timer mapping, yield in frame cap
- `RSDKv4/main.cpp` — KOS_INIT_FLAGS, SDL hint guard, debug defaults
- `RSDKv4/Debug.cpp` — missing `<cstdarg>` include
