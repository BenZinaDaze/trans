#!/usr/bin/env bash
# Run as root in a disposable official Arch container; makepkg runs as builder.
set -euo pipefail

pacman -Syu --noconfirm --needed base-devel cmake ninja \
    qt6-base qt6-declarative qt6-svg kglobalaccel kwindowsystem \
    ca-certificates hicolor-icon-theme dbus desktop-file-utils ttf-dejavu noto-fonts-cjk
useradd --create-home builder
install -d -o builder -g builder /build
cp /input/PKGBUILD /input/trans-*.tar.gz /build/
chown -R builder:builder /build
cd /build
runuser -u builder -- env CMAKE_BUILD_PARALLEL_LEVEL=2 makepkg --cleanbuild --noconfirm

packages=(/build/trans-*.pkg.tar.zst)
if [[ ${#packages[@]} != 1 || ! -f ${packages[0]} ]]; then
    echo 'Expected exactly one Arch package' >&2
    exit 1
fi
install -Dm644 "${packages[0]}" "/output/$(basename "${packages[0]}")"
cd /output
sha256sum ./*.pkg.tar.zst > SHA256SUMS
