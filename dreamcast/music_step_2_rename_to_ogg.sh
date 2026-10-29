#!/usr/bin/env bash
# Put the .ogg name back on the raw PCM.
#
# GameConfig.bin and the StageConfigs resolve tracks BY FILENAME, and those
# names end in .ogg. Only the contents changed in step 1, so the extension has
# to go back or nothing resolves. DC_MusicOpen never looks at the extension --
# it opens the path it is given and treats the bytes as raw PCM -- so the name
# is purely what the config tables ask for.
set -euo pipefail
IFS=$'\n\t'

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ $# -eq 1 ]] || die "Usage: ${0##*/} <stage_dir>"
stage=$1
[[ -d "$stage" ]] || die "not a directory: $stage"
[[ -d "$stage/Music" ]] || die "missing directory: $stage/Music"

orig_dir=$(pwd -P)
restore_dir() { cd -- "$orig_dir" || true; }
trap restore_dir EXIT

stage_abs=$(cd -P -- "$stage" && pwd -P)
cd -- "$stage_abs/Music"

n=0
while IFS= read -r -d '' f; do
  mv -f -- "$f" "${f%.*}.ogg"
  n=$((n+1))
done < <(find . -type f -iname '*.s8' -print0)

printf 'music: %d files renamed back to .ogg\n' "$n"
