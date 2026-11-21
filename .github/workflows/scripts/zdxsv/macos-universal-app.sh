#!/bin/bash
# SPDX-FileCopyrightText: 2026 zdxsv contributors
# SPDX-License-Identifier: GPL-3.0+
# Usage: macos-universal-app.sh <arm64 PCSX2.app> <x86_64 PCSX2.app> <output .app>
# Copies the arm64 bundle and merges every arm64-only Mach-O file in it with the
# x86_64 bundle's file of the same path, then signs the result ad-hoc.

set -euo pipefail

if [ "$#" -ne 3 ]; then
	echo "Syntax: $0 <arm64 app> <x86_64 app> <output app>"
	exit 1
fi

ARM=$1
X64=$2
OUT=$3

rm -rf "$OUT"
cp -R "$ARM" "$OUT"

merged=0
fail=0
while IFS= read -r -d '' f; do
	rel=${f#"$OUT"/}
	archs=$(lipo -archs "$f" 2>/dev/null) || continue
	[ "$archs" = arm64 ] || continue
	other="$X64/$rel"
	if [ ! -f "$other" ] || [ "$(lipo -archs "$other" 2>/dev/null)" != x86_64 ]; then
		echo "::error::$rel: no x86_64 counterpart"
		fail=1
		continue
	fi
	lipo -create "$f" "$other" -output "$f.fat"
	mv "$f.fat" "$f"
	merged=$((merged + 1))
done < <(find "$OUT" -type f -print0)

while IFS= read -r -d '' f; do
	rel=${f#"$X64"/}
	if [ ! -e "$OUT/$rel" ]; then
		echo "::error::$rel: only in the x86_64 bundle"
		fail=1
	fi
done < <(find "$X64" -type f -print0)

[ "$fail" = 0 ]

codesign --force --deep -s - "$OUT"
codesign --verify --deep --strict "$OUT"
echo "Merged $merged files into $OUT ($(lipo -archs "$OUT/Contents/MacOS/PCSX2"))"
