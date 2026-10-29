#!/usr/bin/env bash
# Sonic 1 / RSDKv4 Dreamcast — music.
#
# No Vorbis decoder is linked into the Dreamcast build (libtremor and libogg
# were dropped once nothing decoded any more), so nothing on the disc may still
# be a container. Music becomes HEADERLESS SIGNED 8-BIT PCM, STEREO, 22050 Hz,
# streamed straight off the GD-ROM by DC_MusicOpen.
#
# STEREO, unlike the Sonic CD port. This is not a preference, it is what the
# reader expects: DC_MUS_SRC_RATE is 22050 and the ring is documented as
# "~1.5s of 22050Hz stereo s8", with the mixer taking two bytes per frame and
# head/tail kept even to stay on whole stereo frames. Convert to mono and every
# track plays at half speed with the channels read as alternating samples.
#
# 22050 and 8-bit is the whole budget: 22050 * 2ch * 1 byte = 44100 B/s, which
# is what the credits timing and the dcload chunk sizing were measured against.
# Changing either silently invalidates both.
#
# SANITY CHECK: output bytes must equal 44100 * duration. Credits.ogg is
# 5,351,202 bytes -> 121.3s, which is the figure the credits analysis uses.
set -euo pipefail
IFS=$'\n\t'

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ $# -eq 1 ]] || die "Usage: ${0##*/} <stage_dir>"
stage=$1
[[ -d "$stage" ]] || die "not a directory: $stage"
[[ -d "$stage/Music" ]] || die "missing directory: $stage/Music"

command -v ffmpeg >/dev/null 2>&1 || die "ffmpeg not found in PATH"

orig_dir=$(pwd -P)
restore_dir() { cd -- "$orig_dir" || true; }
trap restore_dir EXIT

stage_abs=$(cd -P -- "$stage" && pwd -P)
cd -- "$stage_abs/Music"

n=0
while IFS= read -r -d '' in_file; do
  out_file="${in_file%.*}.s8"
  ffmpeg -nostdin -hide_banner -loglevel error -y -i "$in_file" \
    -ar 22050 -ac 2 -f s8 -- "$out_file"
  rm -f -- "$in_file"
  n=$((n+1))
done < <(find . -type f -iname '*.ogg' -print0)

printf 'music: %d tracks -> headerless s8 STEREO 22050\n' "$n"
