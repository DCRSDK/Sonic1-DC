# Building RSDKv4 (Sonic 1) for SEGA Dreamcast

Dreamcast port based on the KallistiOS techniques from the RSDKv5 Dreamcast
port by SF94 / Michael Fadely (`RSDKv5-Decompilation`, branch
`sf94/dreamcast-kallistios-pvr`).

## Prerequisites 

1. KallistiOS environment installed and working (`kos-cc` in PATH after
   `source /opt/toolchains/dc/kos/environ.sh`). Built and tested against
   **KOS 2.3.0 & 2.2.0/ sh-elf-gcc 14.4.1**.
2. `ffmpeg` for asset generation.
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

**Do this before packaging.** The build links no Vorbis decoder, so music and
global sound effects have to be converted to raw PCM first — a disc built from
unconverted assets will boot, but every track and every global sfx will be
silent or refused. Everything else (sprites, stages, scripts, palettes, and
stage sfx) streams out of `Data.rsdk` untouched and needs no conversion.

You need an extracted Sonic 1 data tree (`Sonic1Data`) alongside your
`Data.rsdk`. RSDKv4 packs address their entries by MD5 hash rather than by
path, so the scripts cannot unpack the datafile themselves — point them at the
loose tree.

```sh
cd dreamcast
./generate_assets.sh /path/to/Sonic1Data ../build-dc
```

That stages and converts:

| output | format |
|---|---|
| `build-dc/Data/Music/*.ogg` | headerless signed 8-bit, **stereo**, 22050 Hz |
| `build-dc/Data/SoundFX/Global/*.wav` | headerless signed 8-bit, **mono**, 22050 Hz |

Both keep their original extensions because `GameConfig.bin` resolves assets by
filename; only the contents change. Neither is a container or a RIFF file any
more.

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
