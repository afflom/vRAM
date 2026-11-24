#!/usr/bin/env bash
# Build solely against the disposable benchmark guest's pinned Debian kernel.
set -euo pipefail
out=$(realpath -m "${1:?expected output directory}")
src=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out/debs"
cp "$src/Kbuild" "$src/vram_literal.c" "$out/"
node "$src/../../../automation/kernel-literal.mjs" "$out/literal_policy.h"
while read -r filename sha256 snapshot; do
    pkg="$out/debs/$filename"
    curl --retry 3 -fsSL "https://deb.debian.org/debian/pool/main/l/linux/$filename" -o "$pkg" ||
        curl --retry 3 -fsSL "https://snapshot.debian.org/file/$snapshot" -o "$pkg"
    echo "$sha256  $pkg" | sha256sum -c
done <<'PACKAGES'
linux-headers-6.12.117+deb13-amd64_6.12.117-1_amd64.deb e6b8a30d6c834fad8e5bd83d7bbe319279ed45f01d663926f886e353e004871a 328b8aa6194b9ac0a4d0c59056a5db1712a794ad
linux-headers-6.12.117+deb13-common_6.12.117-1_all.deb ac1203a8778a01e09c0ef94d87a3607672c0fc3d5369540c3f1580269b277611 8fd6b99a61f8d65d4d95898b0bc47271b1dab5cb
linux-kbuild-6.12.117+deb13_6.12.117-1_amd64.deb 821ae105a5e2d04c68ddc1a446b8b3554cb4e4581bf009c4165f94612047e3eb f8a567d17aeb0ebca11b0e70a47589769f963d8e
PACKAGES
docker run --rm -v "$out:/build" debian:trixie-slim@sha256:8aa5a7bf5a1955bddbe7d74fcdd0b91ab30e74362a8b73af672b0481b35b44a0 sh -ec '
    apt-get update -qq
    apt-get install -y --no-install-recommends build-essential pahole libelf-dev
    for pkg in /build/debs/*.deb; do dpkg-deb -x "$pkg" /; done
    make -C /usr/src/linux-headers-6.12.117+deb13-amd64 M=/build modules
'
