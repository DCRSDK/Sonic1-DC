#!/usr/bin/env bash
# ---------------------------------------------------------------------
# Sonic 1 / RSDKv4 — Dreamcast asset conversion
#
#   ./generate_assets.sh <path/to/Data.rsdk> [stage_dir]
#
# Takes the game's own datafile, pulls out the two asset classes the Dreamcast
# build reads loose, converts them, and leaves everything else to stream out of
# the untouched Data.rsdk.
#
# WHAT COMES OUT, AND WHERE
#
#   <stage_dir>/Data/Music/*.ogg           headerless signed 8-bit, STEREO, 22050
#   <stage_dir>/Data/SoundFX/Global/*.wav  headerless signed 8-bit, MONO,   22050
#
# Both keep their original extensions because GameConfig.bin resolves them by
# filename; only the CONTENTS change. Neither is a container or a RIFF file any
# more -- the names are a lie the loader is in on.
#
# Data.rsdk itself is never modified, and the copy you point this at does not
# have to be the one you ship. The disc needs BOTH: the loose Data/ tree built
# here, and Data.rsdk beside it for everything else (sprites, tilesets, scripts,
# stage sfx).
#
# SOURCE IS THE PACK, NOT A PRE-EXTRACTED TREE
#
# Extraction is built in (rsdkv4_extract.py), so nothing has to be unpacked by
# hand first. That also removes the trap the old loose-tree flow had: a tree
# that had already been converted once looked exactly like a pristine one, and
# re-running the script fed 8-bit headerless PCM back into ffmpeg, which failed
# with "Invalid data found when processing input". Extracting fresh every run
# means the input is pristine by construction.
#
# RSDKv3 packs store plaintext paths, so Sonic CD's unpacker can walk a
# directory table. RSDKv4 stores only an MD5 of each lowercased path, so names
# cannot be enumerated -- they have to be known in advance. The extractor
# bootstraps from Data/Game/GameConfig.bin for the sound effects and uses a
# filename list for music, which RSDKv4 names only in script bytecode. See
# rsdkv4_extract.py.
#
# A loose tree is still accepted as the first argument, for anyone who already
# has one extracted and wants the old behaviour.
#
# HOW THIS DIFFERS FROM THE SONIC CD (RSDKv3) SCRIPTS
#
# Do not copy those settings over. Three of them are wrong here:
#
#   music channels  CD is MONO (DC_MUSIC_CHANNELS=1). Sonic 1 is STEREO --
#                   DC_MusicReadSamples reads two bytes per frame and the ring
#                   is documented as "22050Hz stereo s8". Mono music would play
#                   at half speed with the channels interleaved as garbage.
#
#   sfx container   CD emits RIFF WAV (pcm_u8) so a per-file rate can ride in
#                   the header. Sonic 1's DC_LoadRawSfx has an explicit RIFF
#                   guard: a real RIFF file is REFUSED and the packed asset is
#                   used instead, so a RIFF conversion here is silently ignored.
#                   Sonic 1 takes headerless SIGNED 8-bit at a fixed rate.
#
#   sfx scope       CD stages Global AND Stage sfx loose. Sonic 1 stages ONLY
#                   Global -- see the note in sfx_step_1_global_to_raw.sh.
#
# REQUIREMENTS: bash, python3, ffmpeg, ffprobe.
# ---------------------------------------------------------------------
set -euo pipefail
IFS=$'\n\t'

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
usage() { printf 'Usage: %s <path/to/Data.rsdk> [stage_dir]\n' "${0##*/}" >&2; exit 2; }

[[ $# -ge 1 && $# -le 2 ]] || usage

src_in=$1
[[ -e "$src_in" ]] || die "no such file or directory: $src_in"

for tool in ffmpeg ffprobe; do
  command -v "$tool" >/dev/null 2>&1 || die "$tool not found in PATH"
done

script_dir=$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)

# ---- resolve the input --------------------------------------------------
# Three things are accepted, in this order: the pack itself, a directory
# holding it, or an already-extracted loose tree.
pack=
loose=
if [[ -f "$src_in" ]]; then
  pack=$(cd -P -- "$(dirname -- "$src_in")" && pwd -P)/$(basename -- "$src_in")
elif [[ -d "$src_in" ]]; then
  d=$(cd -P -- "$src_in" && pwd -P)
  for cand in "$d/Data.rsdk" "$d/data.rsdk"; do
    [[ -f "$cand" ]] && { pack=$cand; break; }
  done
  if [[ -z "$pack" ]]; then
    [[ -d "$d/Music" && -d "$d/SoundFX/Global" ]] \
      || die "$d holds neither Data.rsdk nor a Music/ + SoundFX/Global/ tree"
    loose=$d
  fi
fi

work=
cleanup() { [[ -n "$work" && -d "$work" ]] && rm -rf -- "$work"; }
trap cleanup EXIT

if [[ -n "$pack" ]]; then
  command -v python3 >/dev/null 2>&1 || die "python3 not found in PATH (needed to read Data.rsdk)"
  [[ -f "$script_dir/rsdkv4_extract.py" ]] \
    || die "missing $script_dir/rsdkv4_extract.py"

  work=$(mktemp -d)
  printf -- '-- extracting from %s\n' "$pack"
  # -I so the extractor cannot pick up a stray module from the data directory.
  python3 -I "$script_dir/rsdkv4_extract.py" "$pack" "$work/extracted"
  src=$work/extracted
else
  printf -- '-- using pre-extracted tree %s\n' "$loose"
  src=$loose
fi

[[ -d "$src/Music"          ]] || die "no Music/ came out of the source"
[[ -d "$src/SoundFX/Global" ]] || die "no SoundFX/Global/ came out of the source"

# Default the stage to build-dc, which is where the loose Data/ tree has to end
# up next to Data.rsdk anyway.
stage_in=${2:-$script_dir/../build-dc}
mkdir -p -- "$stage_in"
stage=$(cd -P -- "$stage_in" && pwd -P)

printf -- '-- staging into %s/Data\n' "$stage"
mkdir -p -- "$stage/Data/SoundFX"
rm -rf -- "$stage/Data/Music" "$stage/Data/SoundFX/Global"
cp -R -- "$src/Music"          "$stage/Data/"
cp -R -- "$src/SoundFX/Global" "$stage/Data/SoundFX/"

printf -- '-- converting music\n'
"$script_dir/music_step_1_ogg_to_pcm.sh"    "$stage/Data"
"$script_dir/music_step_2_rename_to_ogg.sh" "$stage/Data"

printf -- '-- converting global sound effects\n'
"$script_dir/sfx_step_1_global_to_raw.sh"   "$stage/Data"

# The source tree is what makes the size check possible -- bytes must equal
# rate * channels * source duration -- so verify before the temp dir goes.
printf -- '-- verifying\n'
"$script_dir/verify_assets.sh"              "$stage/Data" "$src"

printf -- '-- DONE: %s/Data\n' "$stage"
if [[ -n "$pack" ]]; then
  printf -- '   remember Data.rsdk still has to sit beside it on the disc.\n'
fi
exit 0
