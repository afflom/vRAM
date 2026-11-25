#!/usr/bin/env bash
# Assemble verified source for compilation and signing by the host's DKMS.
set -euo pipefail
src=$(cd "$(dirname "$0")" && pwd)
out=$(realpath -m "${1:?expected output deb path}")
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
version=$(sed -n 's/^PACKAGE_VERSION="\([0-9][0-9A-Za-z.+~-]*\)"$/\1/p' "$src/dkms.conf")
dpkg --validate-version "$version"
target="$stage/usr/src/vram-fill-$version"
mkdir -p "$target" "$stage/DEBIAN" "$(dirname "$out")"
install -m 644 "$src"/{Kbuild,dkms.conf,page_io.h,vram_fill.c} "$target/"
mkdir -p "$stage/usr/lib/vram"
node "$src/../../automation/kernel-literal.mjs" "$target/literal_policy.h" "$stage/usr/lib/vram/kernel-size.policy"
install -m755 "$src/packaging/kernel-plan" "$stage/usr/lib/vram/"
install -m644 "$src/../vram_firmware.py" "$stage/usr/lib/vram/kernel-firmware.py"
install -Dm644 "$src/packaging/vram-kernel.service" "$stage/usr/lib/systemd/system/vram-kernel.service"
install -Dm755 "$src/packaging/start-kernel-swap" "$stage/usr/lib/vram/start-kernel-swap"
for name in postinst prerm; do
    sed "s/@VERSION@/$version/g" "$src/packaging/$name" > "$stage/DEBIAN/$name"
    chmod 755 "$stage/DEBIAN/$name"
done
install -Dm644 "$src/../../LICENSE-MIT" "$stage/usr/share/doc/vram-kernel-dkms/copyright-project"
install -m644 /usr/share/common-licenses/GPL-2 "$stage/usr/share/doc/vram-kernel-dkms/copyright"
cat > "$stage/DEBIAN/control" <<CONTROL
Package: vram-kernel-dkms
Version: $version
Architecture: amd64
Maintainer: vRAM maintainers <noreply@github.com>
Depends: python3, dkms, kmod, systemd, util-linux, mount
Description: Prism-controlled Verdant resident swap block device
Homepage: https://afflom.github.io/vRAM/
CONTROL
find "$stage" -type d -exec chmod 755 {} +
chmod 644 "$stage/usr/lib/vram/kernel-size.policy" "$target/literal_policy.h" "$stage/DEBIAN/control"
dpkg-deb --build --root-owner-group "$stage" "$out"
