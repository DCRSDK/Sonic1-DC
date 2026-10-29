#!/usr/bin/env bash
# Sonic 1 / RSDKv4 Dreamcast — verify a staged Data/ tree.
#
#   ./verify_assets.sh <stage_data_dir> [source_data_dir]
#
# A headerless file says nothing about itself, so a wrong sample rate or
# channel count is not an error anywhere -- it is a pitch shift you only notice
# by ear, if you notice it at all. That has already cost this port one round
# trip (a global sfx set converted at 11025 against a 22050 constant played
# exactly an octave high, and every file was exactly 0.5x the expected size).
#
# So: structural checks always, and if the ORIGINAL tree is given, an exact
# size check against each source duration. bytes = rate * channels * seconds,
# and nothing else can be true.
set -euo pipefail
IFS=$'\n\t'

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ $# -ge 1 && $# -le 2 ]] || die "Usage: ${0##*/} <stage_data_dir> [source_data_dir]"
stage=$1
[[ -d "$stage" ]] || die "not a directory: $stage"
src=${2:-}

MUS_RATE=22050; MUS_CH=2     # DC_MUS_SRC_RATE, stereo
SFX_RATE=22050; SFX_CH=1     # DC_RAWSFX_RATE, DC_RAWSFX_CHANNELS

magic4() { head -c4 -- "$1" | tr -d '\0'; }
fsize()  { wc -c < "$1" | tr -d ' '; }

fail=0

# ---- structural: nothing may still be a container -------------------------
if [[ -d "$stage/Music" ]]; then
  while IFS= read -r -d '' f; do
    [[ "$(magic4 "$f")" == "OggS" ]] && { printf 'FAIL still a real Ogg container: %s\n' "$f"; fail=1; }
    (( $(fsize "$f") % 2 )) && { printf 'FAIL odd byte count (not whole stereo frames): %s\n' "$f"; fail=1; }
  done < <(find "$stage/Music" -type f -iname '*.ogg' -print0)
fi

if [[ -d "$stage/SoundFX/Global" ]]; then
  while IFS= read -r -d '' f; do
    [[ "$(magic4 "$f")" == "RIFF" ]] && {
      printf 'FAIL RIFF header -- DC_LoadRawSfx will refuse this and use the packed asset: %s\n' "$f"; fail=1; }
  done < <(find "$stage/SoundFX/Global" -type f -iname '*.wav' -print0)
fi

# ---- the 2013 menu textures must be DTEX, not PNG -------------------------
# A real PNG left here is the silent failure the .dtex path was written to
# avoid: DC_NativeTryLoadDtex sees no 'DTEX' magic, hands back to stbi_load,
# and stbi OOMs on the DC heap for anything over ~256x256 -- so the texture is
# simply missing on screen with nothing logged. Every one of these is 512x512
# or larger, so a PNG here means an invisible menu element, not a slow one.
#
# The header is also checked for sanity, since a 16-byte head that says DTEX
# but carries nonsense dimensions is rejected at load and lands in the same
# place.
if [[ -d "$stage/Game" ]]; then
  n=0; bad=0; vq=0; plain=0
  while IFS= read -r -d '' f; do
    n=$((n+1))
    if [[ "$(magic4 "$f")" != "DTEX" ]]; then
      printf 'FAIL still a real PNG -- the Dreamcast cannot decode it, the menu will be missing it: %s\n' "$f"
      bad=$((bad+1)); fail=1
      continue
    fi
    # Own IFS: this script sets IFS to newline+tab, so a bare `read` would put
    # the whole awk line into the first variable.
    IFS=' ' read -r fmt potw poth < <(od -An -j5 -N9 -tu1 -- "$f" \
      | awk '{print $1, $6 + $7 * 256, $8 + $9 * 256}')
    case "$fmt" in
      3|4) vq=$((vq+1)) ;;
      1|2) plain=$((plain+1)) ;;
      *)   printf 'FAIL unknown dtex format %s: %s\n' "$fmt" "$f"; bad=$((bad+1)); fail=1; continue ;;
    esac
    if (( potw < 8 || poth < 8 || potw > 1024 || poth > 1024 )); then
      printf 'FAIL dtex pot size %sx%s is outside what the loader accepts: %s\n' "$potw" "$poth" "$f"
      bad=$((bad+1)); fail=1
    fi
  done < <(find "$stage/Game" -type f -iname '*.png' -print0)
  if (( n )); then
    printf 'menu art:    %d checked, %d wrong (%d VQ, %d plain 16bpp)\n' "$n" "$bad" "$vq" "$plain"
  fi
fi

# A loose GameConfig.bin next to Data.rsdk OVERRIDES the packed one. Nothing in
# this pipeline writes it, so finding one means it was copied in by hand.
if [[ -f "$stage/Game/GameConfig.bin" ]]; then
  printf 'FAIL Data/Game/GameConfig.bin is staged loose -- it overrides the packed one. Delete it.\n'
  fail=1
fi

# ---- exact size check against the source durations ------------------------
check_against_source() {
  local sub=$1 pattern=$2 rate=$3 ch=$4 label=$5
  [[ -d "$stage/$sub" && -d "$src/$sub" ]] || return 0
  command -v ffprobe >/dev/null 2>&1 || { printf 'note: ffprobe missing, skipping %s size check\n' "$label"; return 0; }

  local n=0 bad=0
  while IFS= read -r -d '' out; do
    local rel=${out#"$stage/$sub/"}
    local in="$src/$sub/$rel"
    [[ -f "$in" ]] || continue
    local dur
    dur=$(ffprobe -v error -select_streams a:0 -show_entries stream=duration \
          -of default=nokey=1:noprint_wrappers=1 -- "$in" | head -n1 || true)
    [[ -n "$dur" && "$dur" != "N/A" ]] || continue
    local actual expect
    actual=$(fsize "$out")
    expect=$(awk -v d="$dur" -v r="$rate" -v c="$ch" 'BEGIN{printf "%d", d*r*c}')
    n=$((n+1))
    # 1% tolerance absorbs encoder frame padding; a wrong rate or channel count
    # is off by 2x or more and cannot hide inside that.
    if awk -v a="$actual" -v e="$expect" 'BEGIN{exit !(e>0 && (a<e*0.99 || a>e*1.01))}'; then
      bad=$((bad+1)); fail=1
      awk -v f="$rel" -v a="$actual" -v e="$expect" \
        'BEGIN{printf "FAIL %-28s %d bytes, expected ~%d  (%.2fx -- wrong rate or channel count?)\n", f, a, e, a/e}'
    fi
  done < <(find "$stage/$sub" -type f \( -iname "$pattern" \) -print0)
  printf '%-12s %d checked, %d wrong size\n' "$label" "$n" "$bad"
}

if [[ -n "$src" ]]; then
  [[ -d "$src" ]] || die "not a directory: $src"
  check_against_source "Music"          '*.ogg' "$MUS_RATE" "$MUS_CH" "music:"
  check_against_source "SoundFX/Global" '*.wav' "$SFX_RATE" "$SFX_CH" "global sfx:"
else
  printf 'note: no source tree given -- structural checks only\n'
fi

# ---- leftovers: a Stage/ folder nothing reads ------------------------------
# LoadSfx gates the loose-file path on the sfx ID being OUTSIDE the stage range
# (Audio.cpp: "GLOBAL sfx only, deliberately"), so a stage sfx never reaches
# DC_LoadRawSfx -- it goes to LoadFile, out of the pack, into AICA sound RAM.
# A Data/SoundFX/Stage/ tree is therefore never opened. It is usually left over
# from the reverted loose-file override. Warn, do not delete: it is the user's
# disc space to spend and a stray folder is not a failure.
if [[ -d "$stage/SoundFX/Stage" ]]; then
  n=$(find "$stage/SoundFX/Stage" -type f | wc -l)
  kb=$(find "$stage/SoundFX/Stage" -type f -printf '%s\n' 2>/dev/null | paste -sd+ - | bc)
  kb=$(( ${kb:-0} / 1024 ))
  printf 'warn: Data/SoundFX/Stage/ exists (%d files, %d KB) but NOTHING READS IT.\n' "$n" "$kb"
  printf '      Stage sfx load from Data.rsdk into AICA sound RAM; the loose path\n'
  printf '      is global-only. Safe to delete -- it is dead weight on the disc.\n'
fi

# ---- summary ---------------------------------------------------------------
for sub in Music SoundFX/Global Game/Menu Game/Models; do
  [[ -d "$stage/$sub" ]] || continue
  printf '%-16s %4d files, %8d KB\n' "$sub" \
    "$(find "$stage/$sub" -type f | wc -l)" \
    "$(( $(find "$stage/$sub" -type f -printf '%s+' 2>/dev/null | sed 's/+$//' | paste -sd+ - | bc) / 1024 ))"
done

[[ $fail -eq 0 ]] && printf 'verify: OK\n' || { printf 'verify: FAILED\n'; exit 1; }
