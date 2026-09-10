#!/usr/bin/env bash
# Run in a fresh Arch container with only release assets mounted at /packages.
set -euo pipefail
expected_version=${1:?Expected CMake version}
cd /packages
sha256sum --check SHA256SUMS
packages=(./tran-*.pkg.tar.zst)
[[ ${#packages[@]} == 1 && -f ${packages[0]} ]]

pacman -Syu --noconfirm --needed desktop-file-utils
pacman -U --noconfirm "${packages[0]}"
pacman -Qi tran
[[ $(pacman -Q tran) == "tran $expected_version-1" ]]
for path in /usr/bin/tran /usr/share/applications/io.github.tran.Tran.desktop \
    /usr/share/icons/hicolor/scalable/apps/tran.svg; do
    test -f "$path"
done
# Fail if packaging starts including unintended source files or user configuration.
while IFS= read -r path; do
    [[ $path == */ ]] && continue
    case "$path" in
        /usr/bin/tran|/usr/share/applications/io.github.tran.Tran.desktop|/usr/share/icons/hicolor/scalable/apps/tran.svg) ;;
        *) echo "Unexpected package file: $path" >&2; exit 1 ;;
    esac
done < <(pacman -Qql tran)
desktop-file-validate /usr/share/applications/io.github.tran.Tran.desktop

useradd --create-home smoke
install -d -m700 -o smoke -g smoke /tmp/tran-runtime
runtime_env=(QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software XDG_RUNTIME_DIR=/tmp/tran-runtime)
actual_version=$(runuser -u smoke -- env "${runtime_env[@]}" /usr/bin/tran --version)
[[ $actual_version == "tran $expected_version" ]]
runuser -u smoke -- env "${runtime_env[@]}" /usr/bin/tran --smoke-test
