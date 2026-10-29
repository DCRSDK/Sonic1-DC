#!/usr/bin/env bash
# Sonic 1 / RSDKv4 Dreamcast — global sound effects.
#
# TARGET FORMAT, fixed by DC_LoadRawSfx in DCAudio.cpp:
#
#   HEADERLESS signed 8-bit PCM, MONO, 22050 Hz   (DC_RAWSFX_RATE / _CHANNELS)
#
# NOT RIFF. DC_LoadRawSfx sniffs the first four bytes and REFUSES anything
# starting "RIFF", falling back to the packed asset and logging
#   [DCSfx] raw: '<file>' is a RIFF wav, not headerless PCM - using the packed asset
# So a pcm_u8 RIFF conversion -- which is what the Sonic CD script produces --
# does not fail loudly here, it is simply ignored. Check the log if a sound
# seems not to have changed.
#
# FORMAT IS A CONTRACT. A headerless file states nothing about itself, so these
# settings and DC_RAWSFX_RATE have to move together. Re-encode at another rate
# without changing the constant and every sound plays at the wrong pitch,
# silently -- which has already happened once on this port: a set converted at
# 11025 against a 22050 constant came out exactly an octave high.
# verify_assets.sh exists to catch precisely that.
#
# WHY ONLY Global/, AND NOT Stage/
#
# Unlike Sonic CD, this port does not read stage sfx loose. The loose-file
# override for them was implemented, measured and REVERTED: the 11-22 extra
# file opens per stage load cost more than the smaller reads saved. Stage sfx
# stay inside Data.rsdk at 44100/16-bit and are decimated to 22050/8-bit at
# load time by the AICA path.
#
# That decimation is ~17% of hardware load time and those entries are 4x larger
# on disc than they need to be -- for Scrap Brain, ~1,284 KB of its 2,279 KB
# read. The fix is to REPACK them into Data.rsdk at 22050/8-bit, not to stage
# them loose here. Note the gotcha before doing so: the AICA's 65,504-frame cap
# forces long sounds to decimate harder, and pre-halving the source gives the
# loader less room -- a 3.7s sound currently lands at 14700 Hz but would land
# at 11025 Hz if the pack were already 22050.
set -euo pipefail
IFS=$'\n\t'

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ $# -eq 1 ]] || die "Usage: ${0##*/} <stage_dir>"
stage=$1
[[ -d "$stage" ]] || die "not a directory: $stage"
[[ -d "$stage/SoundFX/Global" ]] || die "missing directory: $stage/SoundFX/Global"

command -v ffmpeg >/dev/null 2>&1 || die "ffmpeg not found in PATH"

SFX_RATE=22050          # must equal DC_RAWSFX_RATE in DCAudio.cpp

# No AICA frame-cap check here, deliberately. Global sfx never reach AICA sound
# RAM: LoadSfx's loose-file branch sets sfxList[].buffer and returns, and
# DC_AicaSfxLoad is gated on isStageSfx. They are software-mixed and stay
# resident as S16 in main RAM for the whole run -- about 1,524 KB across the
# set -- so length costs RAM here, not hardware voices. DC_AICA_MAX_FRAMES
# constrains the PACKED stage sfx only.

orig_dir=$(pwd -P)
restore_dir() { cd -- "$orig_dir" || true; }
trap restore_dir EXIT

stage_abs=$(cd -P -- "$stage" && pwd -P)
cd -- "$stage_abs/SoundFX/Global"

n=0; bytes=0
while IFS= read -r -d '' file; do
  # Headerless output cannot be written over its own input, so stage via .s8
  # and move the original aside only once ffmpeg has succeeded.
  tmp="${file%.*}.s8"
  ffmpeg -nostdin -hide_banner -loglevel error -y -i "$file" \
    -ar "$SFX_RATE" -ac 1 -f s8 -- "$tmp"
  mv -f -- "$tmp" "$file"
  n=$((n+1))
  bytes=$((bytes + $(wc -c < "$file")))
done < <(find . -type f -iname '*.wav' -print0)

# One byte per sample on disc, two bytes per sample resident (the mixer keeps
# them as S16), so the resident cost is simply double.
printf 'global sfx: %d files -> headerless s8 MONO %d (%d KB on disc, ~%d KB resident)\n' \
  "$n" "$SFX_RATE" "$((bytes / 1024))" "$((bytes * 2 / 1024))"
