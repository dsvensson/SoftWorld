#!/bin/bash
# The shareware's maps for test_bspload: id1/pak0.pak (the same file the full
# game has) from the LHA archive resource.1 inside quake106.zip, Quake 1.06's
# shareware, as the Quake archive QUAKE_ARCHIVE names (a commit) keeps it; into
# shareware/id1. bsdtar reads both: Windows' own tar, macOS's, Arch's bsdtar.

set -euo pipefail

PAK0_SHA256=35a9c55e5e5a284a159ad2a62e0e8def23d829561fe2f54eb402dbc0a9a946af

case "$(uname -s)" in
	MINGW*|MSYS*)	bsdtar=/c/Windows/System32/tar.exe ;;
	Darwin)			bsdtar=tar ;;
	*)				bsdtar=bsdtar ;;
esac

work=$(mktemp -d)
curl -sSfL --retry 5 --retry-all-errors --retry-delay 10 -o "$work/quake106.zip" \
	"https://github.com/Jason2Brownlee/QuakeOfficialArchive/raw/$QUAKE_ARCHIVE/bin/quake106.zip"
(cd "$work" && "$bsdtar" -xf quake106.zip resource.1 && "$bsdtar" -xf resource.1 ID1/PAK0.PAK)

mkdir -p shareware/id1
mv "$work/ID1/PAK0.PAK" shareware/id1/pak0.pak
rm -rf "$work"
if command -v sha256sum >/dev/null; then
	echo "$PAK0_SHA256  shareware/id1/pak0.pak" | sha256sum -c -
else
	echo "$PAK0_SHA256  shareware/id1/pak0.pak" | shasum -a 256 -c -
fi
