#!/usr/bin/env bash
# Sonic 1 / RSDKv4 Dreamcast — the 2013 native menu textures.
#
# The 2013 menu is drawn by DCNative.cpp, not by the script engine, and it reads
# its textures through DC_NativeTryLoadDtex — which bypasses LoadFile entirely
# and does a plain fopen on BASE_PATH + the requested path. So these are LOOSE
# files on the disc, and they are the reason a loose Data/Game/ tree exists at
# all.
#
# Why they are not just PNGs: stbi_load fails on the Dreamcast's fragmented heap
# for anything much over 256x256, and almost every one of these is 512x512 or
# 1024x1024, so most of the menu was invisible before the .dtex path existed.
# A .dtex is streamed straight into VRAM in 32 KB chunks with no decode at all.
#
# NAMED .png, HOLDING .dtex DATA. DC_NativeTryLoadDtex identifies the format by
# the 'DTEX' magic, not the extension; it tries the .dtex sibling first and then
# the exact requested path. Keeping the .png name is the same trick the audio
# uses, and it is what the working build does — every file in
# build-dc/Data/Game/Menu is a .png holding DTEX data.
#
# VQ BY DEFAULT, because that is what ships. Verified against the working Sonic 1
# build: all 51 textures in its Data/Game/Menu are VQ at the converter's default
# maxdim of 1024 — fmt 3 (VQ/ARGB4444) for most, fmt 4 (VQ/ARGB1555) for Intro,
# HelpGraphics and TimeAttack1-4. VQ is a 256-entry codebook of 2x2 texel blocks
# plus one index byte per block: ~1/8 the VRAM of plain 16bpp, at full
# resolution. Without it the set does not fit and you have to drop to
# --maxdim 512, which visibly softens the artwork.
#
# Resident cost if every texture were loaded at once is well over the
# Dreamcast's 8 MB of VRAM. That is fine and expected: the menu never needs all
# of them together, and DC_NativeResetTextures / DC_NativeReloadEvicted stream
# them back on demand.
#
# WHICH FILES. generate_assets.sh extracts the 48 paths that appear as
# LoadTexture() string literals in RSDKv4/NativeObjects (TimeAttack%d.png
# expanded to 1..4) — those are the only ones DC_NativeTryLoadDtex will ever
# fopen. The retail pack also holds Amazon, Sonic, Heading_JA, Label_JA, Text_JA
# and Text_KO@1x, which nothing in the engine references; they are listed,
# commented out, at the end of rsdkv4_filelist.txt.
#
# IDEMPOTENT: a file that is already DTEX is skipped, so re-running over a
# converted tree is a no-op rather than a crash inside PIL.
set -euo pipefail
IFS=$'\n\t'

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ $# -ge 1 ]] || die "Usage: ${0##*/} <stage_data_dir> [--no-vq]"
stage=$1
use_vq=1
[[ "${2:-}" == "--no-vq" ]] && use_vq=0

[[ -d "$stage" ]] || die "not a directory: $stage"

command -v python3 >/dev/null 2>&1 || die "python3 not found in PATH"

script_dir=$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)

# Prefer the converter that lives in the repo, so a fix there is picked up here.
conv=""
for c in "$script_dir/../tools/dtex_convert.py" "$script_dir/dtex_convert.py"; do
  [[ -f "$c" ]] && { conv=$(cd -P -- "$(dirname -- "$c")" && pwd -P)/$(basename -- "$c"); break; }
done
[[ -n "$conv" ]] || die "dtex_convert.py not found next to this script or in ../tools"

python3 - <<'PY' || die "dtex conversion needs Pillow: pip3 install Pillow (or apt install python3-pil)"
import PIL  # noqa: F401
PY
if (( use_vq )); then
  python3 - <<'PY' || die "--vq needs numpy: pip3 install numpy (or apt install python3-numpy), or pass --no-vq"
import numpy  # noqa: F401
PY
fi

# Works IN PLACE on the stage tree, exactly like the music and sfx steps, so the
# whole pipeline is idempotent and the Game/ folder is present in the output
# whether or not anything needed converting.
stage_abs=$(cd -P -- "$stage" && pwd -P)
parent=$(dirname -- "$stage_abs")
dataname=$(basename -- "$stage_abs")

tmp=$(mktemp -d)
orig_dir=$(pwd -P)
cleanup() { cd -- "$orig_dir" || true; rm -rf -- "$tmp"; }
trap cleanup EXIT

# dtex_convert mirrors the source path AS GIVEN under --out, so it has to be run
# from the folder containing Data/ with relative paths, or the output lands in
# the wrong place.
cd -- "$parent"

# Collect only real PNGs. A tree that has already been converted holds DTEX data
# under a .png name, and PIL would throw on it.
sources=()
skipped=0
for sub in Menu Models; do
  d="$dataname/Game/$sub"
  [[ -d "$d" ]] || continue
  while IFS= read -r -d '' f; do
    if [[ "$(od -An -N4 -tx1 -- "$f" | tr -d ' \n')" == "89504e47" ]]; then
      sources+=("$f")
    else
      skipped=$((skipped+1))
    fi
  done < <(find "$d" -type f -iname '*.png' -print0)
done

if (( ${#sources[@]} == 0 )); then
  printf 'menu: no unconverted PNGs under %s/Game/{Menu,Models}' "$dataname"
  (( skipped )) && printf ' (%d already DTEX)' "$skipped"
  printf '\n'
  exit 0
fi

# Convert into a scratch tree and move the results over, rather than writing
# each file back on top of the PIL handle that is still reading it.
args=(--out "$tmp" --ext png)
(( use_vq )) && args+=(--vq)

python3 "$conv" "${args[@]}" "${sources[@]}"

bytes=0
for f in "${sources[@]}"; do
  [[ -f "$tmp/$f" ]] || die "converter produced no output for $f"
  mv -f -- "$tmp/$f" "$f"
  bytes=$((bytes + $(wc -c < "$f")))
done

printf 'menu: %d textures -> %s dtex (%d KB on disc)' \
  "${#sources[@]}" "$( ((use_vq)) && echo VQ || echo '16bpp' )" "$((bytes / 1024))"
(( skipped )) && printf ', %d already DTEX and left alone' "$skipped"
printf '\n'
