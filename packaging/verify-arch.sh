#!/usr/bin/env bash
# Run in a fresh Arch container with only release assets mounted at /packages.
set -euo pipefail
expected_version=${1:?Expected CMake version}
cd /packages
sha256sum --check SHA256SUMS
packages=(./trans-*.pkg.tar.zst)
[[ ${#packages[@]} == 1 && -f ${packages[0]} ]]

pacman -Syu --noconfirm --needed desktop-file-utils
pacman -U --noconfirm "${packages[0]}"
pacman -Qi trans
[[ $(pacman -Q trans) == "trans $expected_version-1" ]]
for path in /usr/bin/trans /usr/share/applications/io.github.trans.Trans.desktop \
    /usr/share/icons/hicolor/scalable/apps/trans.svg; do
    test -f "$path"
done
# Fail if packaging starts including unintended source files or user configuration.
while IFS= read -r path; do
    [[ $path == */ ]] && continue
    case "$path" in
        /usr/bin/trans|/usr/share/applications/io.github.trans.Trans.desktop|/usr/share/icons/hicolor/scalable/apps/trans.svg) ;;
        *) echo "Unexpected package file: $path" >&2; exit 1 ;;
    esac
done < <(pacman -Qql trans)
desktop-file-validate /usr/share/applications/io.github.trans.Trans.desktop

useradd --create-home smoke
install -d -m700 -o smoke -g smoke /tmp/trans-runtime
runtime_env=(QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software XDG_RUNTIME_DIR=/tmp/trans-runtime)
actual_version=$(runuser -u smoke -- env "${runtime_env[@]}" /usr/bin/trans --version)
[[ $actual_version == "trans $expected_version" ]]
runuser -u smoke -- env "${runtime_env[@]}" /usr/bin/trans --smoke-test
