#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
deps="build/deps"
archive="$deps/lingeling-1.0.0.tar.gz"
source="$deps/lingeling-rel-1.0.0"
mkdir -p "$deps"
if [ ! -f "$archive" ]; then
    tmp=$(mktemp "$deps/lingeling.XXXXXX")
    trap 'rm -f "$tmp"' EXIT
    curl --fail --location --retry 2 --output "$tmp" \
        "https://codeload.github.com/arminbiere/lingeling/tar.gz/refs/tags/rel-1.0.0"
    printf "%s  %s\n" "99f35f14091a495ca22e4bf7394f645bff7161ea4e0294f73cf4d5114ac33ac3" "$tmp" | sha256sum -c -
    mv "$tmp" "$archive"
fi
printf "%s  %s\n" "99f35f14091a495ca22e4bf7394f645bff7161ea4e0294f73cf4d5114ac33ac3" "$archive" | sha256sum -c -
tar -xzf "$archive" -C "$deps"
(cd "$source" && sh configure.sh -O3 && make liblgl.a)
make lingeling
