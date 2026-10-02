#!/bin/bash
# The shareware's maps for test_bspload: id1/pak0.pak (the same file the full
# game has) from the LHA archive resource.1 inside quake106.zip, Quake 1.06's
# shareware, as the Quake archive keeps it at the commit QUAKE_ARCHIVE names;
# into shareware/id1. Through git, not the raw files' URLs, which answered the
# runners with 504s: the commit's trees alone (blob:none), then the one blob.
# bsdtar reads the zip and the LHA archive: Windows' own tar, macOS's, bsdtar.

set -euo pipefail

ARCHIVE=https://github.com/Jason2Brownlee/QuakeOfficialArchive.git
PAK0_SHA256=35a9c55e5e5a284a159ad2a62e0e8def23d829561fe2f54eb402dbc0a9a946af

case "$(uname -s)" in
	MINGW*|MSYS*)	bsdtar=/c/Windows/System32/tar.exe ;;
	Darwin)			bsdtar=tar ;;
	*)				bsdtar=bsdtar ;;
esac

work=$(mktemp -d)
git -C "$work" init -q
git -C "$work" remote add origin "$ARCHIVE"

# a fetch, tried again on failure, as the runners' network has it
fetch () {
	local attempt
	for attempt in 1 2 3 4 5; do
		git -C "$work" fetch -q --no-tags --depth 1 --filter=blob:none origin "$1" && return 0
		echo "fetching $1 failed (attempt $attempt)" >&2
		sleep 10
	done
	return 1
}

fetch "$QUAKE_ARCHIVE"
blob=$(git -C "$work" rev-parse "FETCH_HEAD:bin/quake106.zip")
fetch "$blob"
git -C "$work" cat-file blob "$blob" > "$work/quake106.zip"
(cd "$work" && "$bsdtar" -xf quake106.zip resource.1 && "$bsdtar" -xf resource.1 ID1/PAK0.PAK)

mkdir -p shareware/id1
mv "$work/ID1/PAK0.PAK" shareware/id1/pak0.pak
rm -rf "$work"
if command -v sha256sum >/dev/null; then
	echo "$PAK0_SHA256  shareware/id1/pak0.pak" | sha256sum -c -
else
	echo "$PAK0_SHA256  shareware/id1/pak0.pak" | shasum -a 256 -c -
fi
