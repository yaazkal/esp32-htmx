#!/bin/sh
# Regenerates src/web_assets.h from src/index.html, src/style.css, and
# src/htmx.min.js.gz via `xxd -i`, then patches the output to add `const` so
# the arrays land in flash (.rodata) instead of being copied into RAM at
# boot (`xxd -i` doesn't emit `const` on its own).
#
# Writes to a temp file and renames it into place rather than using
# `sed -i`, since GNU sed (Linux) and BSD sed (macOS/FreeBSD) take
# incompatible flags for in-place editing — this way works the same on all
# three.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src_dir="$script_dir/../src"
out="$src_dir/web_assets.h"
tmp="$out.tmp"

if ! command -v xxd >/dev/null 2>&1; then
  echo "error: xxd not found (ships with vim / vim-common on most systems)" >&2
  exit 1
fi

cd "$src_dir"
{
  xxd -i index.html
  echo
  xxd -i style.css
  echo
  xxd -i htmx.min.js.gz
} >"$tmp"

sed 's/^unsigned char /const unsigned char /; s/^unsigned int \(.*\)_len/const unsigned int \1_len/' "$tmp" >"$out"
rm -f "$tmp"

echo "regenerated src/web_assets.h"
