#!/bin/bash
# release.sh <file> -- uploads a build to the GitHub release of the version tag
# the run is for (GITHUB_REF_NAME, as v3.06), which whichever job gets here
# first makes. The tag must be the project's version (CMakeLists.txt).
set -euo pipefail

file=$1
tag=$GITHUB_REF_NAME
repo=$GITHUB_REPOSITORY
version=$(sed -n 's/^project(SoftWorld VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)

if [ "$tag" != "v$version" ]; then
	echo "The tag $tag isn't the project's version, v$version" >&2
	exit 1
fi

# made here, or by another job meanwhile
gh release view "$tag" --repo "$repo" > /dev/null 2>&1 \
	|| gh release create "$tag" --repo "$repo" --title "SoftWorld $version" --generate-notes \
	|| gh release view "$tag" --repo "$repo" > /dev/null
gh release upload "$tag" "$file" --repo "$repo" --clobber
