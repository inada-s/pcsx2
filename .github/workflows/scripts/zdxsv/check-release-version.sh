#!/bin/bash
# SPDX-FileCopyrightText: 2026 zdxsv contributors
# SPDX-License-Identifier: GPL-3.0+
# Usage: check-release-version.sh <svnrev.h> <pushed tag>
# Fails unless the build carries the pushed tag. A release tag (zdxsv-X.Y.Z) must also
# be built as a tagged commit: the auto updater compares only those versions.
set -euo pipefail
header=$1
tag=$2
built=$(tr -d '\r' < "$header" | sed -n 's/^#define GIT_TAG "\(.*\)" *$/\1/p')
if [ "$built" != "$tag" ]; then
	echo "::error::the build carries tag '$built', the pushed tag is '$tag'"
	exit 1
fi
if [[ $tag =~ ^zdxsv-[0-9]+\.[0-9]+\.[0-9]+$ ]] && ! tr -d '\r' < "$header" | grep -qx '#define GIT_TAGGED_COMMIT 1 *'; then
	echo "::error::$tag is not built as a tagged release (no auto update)"
	exit 1
fi
echo "Version: $tag"
