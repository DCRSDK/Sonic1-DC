#!/usr/bin/env bash
# ---------------------------------------------------------------------
# Sonic 1 / RSDKv4 — Dreamcast asset conversion
#
#   ./generate_assets.sh <path/to/Data.rsdk> [stage_dir] [--menu-no-vq] [--skip-menu]
#
# Takes the game's own datafile, pulls out the three asset classes the Dreamcast
# build reads loose, converts them, and leaves everything else to stream out of
# the untouched Data.rsdk.
#
# WHAT COMES OUT, AND WHERE
#
#   <stage_dir>/Data/Music/*.ogg           headerless signed 8-bit, STEREO, 22050
#   <stage_dir>/Data/SoundFX/Global/*.wav  headerless signed 8-bit, MONO,   22050
#   <stage_dir>/Data/Game/Menu/*.png       VQ .dtex, streamed straight to VRAM
#   <stage_dir>/Data/Game/Models/*.png     ditto (About-screen box art)
#
# Every one keeps its original extension. The audio does because GameConfig.bin
# resolves assets by filename; the textures do because DC_NativeTryLoadDtex
# identifies the format by its 'DTEX' magic, not the extension. Only the
# CONTENTS change -- none of these is a container or a RIFF or a PNG any more.
# The names are a lie the loaders are in on.
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
#   menu art        CD has no native 2013 menu, so the v3 pipeline has nothing
#                   equivalent. RSDKv5's img_step_1_todtex.sh is the nearest
#                   relative.
#
# REQUIREMENTS: bash, python3, ffmpeg, ffprobe, and for the menu textures
# python3-pil plus python3-numpy (numpy only for VQ). --skip-menu drops both.
# ---------------------------------------------------------------------
set -euo pipefail
IFS=$'\n\t'

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
usage() {
  printf 'Usage: %s <path/to/Data.rsdk> [stage_dir] [--menu-no-vq] [--skip-menu] [--reference-layout]\n' \
    "${0##*/}" >&2
  exit 2
}

src_in=""
stage_arg=""
menu_vq=1
skip_menu=0
reference_layout=0

for arg in "$@"; do
  case "$arg" in
    --menu-no-vq) menu_vq=0 ;;
    --skip-menu)  skip_menu=1 ;;
    --reference-layout) reference_layout=1 ;;
    -*)           usage ;;
    *)            if   [[ -z "$src_in"   ]]; then src_in=$arg
                  elif [[ -z "$stage_arg" ]]; then stage_arg=$arg
                  else usage; fi ;;
  esac
done

[[ -n "$src_in" ]] || usage
[[ -e "$src_in" ]] || die "no such file or directory: $src_in"
if (( reference_layout && skip_menu )); then
  die "--reference-layout requires menu textures (remove --skip-menu)"
fi

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
  extract_args=()
  (( reference_layout )) && extract_args+=(--reference-layout)
  python3 -I "$script_dir/rsdkv4_extract.py" "$pack" "$work/extracted" "${extract_args[@]}"
  src=$work/extracted
else
  printf -- '-- using pre-extracted tree %s\n' "$loose"
  src=$loose
fi

[[ -d "$src/Music"          ]] || die "no Music/ came out of the source"
[[ -d "$src/SoundFX/Global" ]] || die "no SoundFX/Global/ came out of the source"

# Default the stage to build-dc, which is where the loose Data/ tree has to end
# up next to Data.rsdk anyway.
stage_in=${stage_arg:-$script_dir/../build-dc}
mkdir -p -- "$stage_in"
stage=$(cd -P -- "$stage_in" && pwd -P)

printf -- '-- staging into %s/Data\n' "$stage"
mkdir -p -- "$stage/Data/SoundFX"
rm -rf -- "$stage/Data/Music" "$stage/Data/SoundFX/Global"
cp -R -- "$src/Music"          "$stage/Data/"
cp -R -- "$src/SoundFX/Global" "$stage/Data/SoundFX/"

# Only the PNGs out of Game/, never the whole folder. The disc carries exactly
# the converted textures under Data/Game and nothing else: a loose
# Game/GameConfig.bin beside Data.rsdk would OVERRIDE the packed one, which is a
# quiet way to break a disc. (The extractor does not write one by default
# either -- belt and braces.)
if (( ! skip_menu )); then
  rm -rf -- "$stage/Data/Game/Menu" "$stage/Data/Game/Models"
  for sub in Menu Models; do
    if [[ -d "$src/Game/$sub" ]]; then
      mkdir -p -- "$stage/Data/Game/$sub"
      find "$src/Game/$sub" -maxdepth 1 -type f -iname '*.png' \
        -exec cp -- {} "$stage/Data/Game/$sub/" \;
    fi
  done
fi

printf -- '-- converting music\n'
"$script_dir/music_step_1_ogg_to_pcm.sh"    "$stage/Data"
"$script_dir/music_step_2_rename_to_ogg.sh" "$stage/Data"

printf -- '-- converting global sound effects\n'
"$script_dir/sfx_step_1_global_to_raw.sh"   "$stage/Data"

if (( reference_layout )); then
  python3 "$script_dir/reference_assets.py" stage-sfx "$src" "$stage/Data"
fi

if (( skip_menu )); then
  printf -- '-- 2013 menu textures skipped\n'
elif [[ ! -d "$stage/Data/Game/Menu" ]]; then
  printf -- '-- no Game/Menu came out of the source, nothing to convert\n'
else
  printf -- '-- converting 2013 menu textures\n'
  if (( menu_vq )); then
    "$script_dir/menu_step_1_dtex.sh" "$stage/Data"
  else
    "$script_dir/menu_step_1_dtex.sh" "$stage/Data" --no-vq
  fi
fi

# The source tree is what makes the size check possible -- bytes must equal
# rate * channels * source duration -- so verify before the temp dir goes.
printf -- '-- verifying\n'
"$script_dir/verify_assets.sh"              "$stage/Data" "$src"
if (( reference_layout )); then
  python3 "$script_dir/reference_assets.py" check "$stage/Data"
fi

printf -- '-- DONE: %s/Data\n' "$stage"
if [[ -n "$pack" ]]; then
  printf -- '   remember Data.rsdk still has to sit beside it on the disc.\n'
fi
exit 0
