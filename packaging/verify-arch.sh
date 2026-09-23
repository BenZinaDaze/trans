#!/usr/bin/env bash
# Run in a fresh Arch container with only release assets mounted at /packages.
set -euo pipefail
expected_version=${1:?Expected CMake version}
cd /packages
sha256sum --check SHA256SUMS
packages=(./trans-*.pkg.tar.zst)
[[ ${#packages[@]} == 1 && -f ${packages[0]} ]]

# Xvfb + Mesa exercises the shipped Winit/FemtoVG renderer; Slint's software
# renderer is deliberately not compiled into the application.
pacman -Syu --noconfirm --needed desktop-file-utils xorg-server-xvfb xorg-xauth \
    mesa ttf-dejavu noto-fonts-cjk
pacman -U --noconfirm "${packages[0]}"
pacman -Qi trans
[[ $(pacman -Q trans) == "trans $expected_version-1" ]]
for path in /usr/bin/trans /usr/lib/libslint_cpp.so \
    /usr/share/applications/io.github.trans.Trans.desktop \
    /usr/share/icons/hicolor/scalable/apps/trans.svg \
    /usr/share/licenses/trans/nlohmann-json-LICENSE.MIT \
    /usr/share/licenses/trans/third-party/THIRD-PARTY-NOTICES.txt \
    /usr/share/licenses/trans/third-party/slint/LICENSE.md; do
    test -s "$path"
done
# Reject SDK/compiler payloads, source files, and accidental user configuration.
while IFS= read -r path; do
    [[ $path == */ ]] && continue
    case "$path" in
        /usr/bin/trans|/usr/lib/libslint_cpp.so|/usr/share/applications/io.github.trans.Trans.desktop|/usr/share/icons/hicolor/scalable/apps/trans.svg|/usr/share/licenses/trans/*) ;;
        *) echo "Unexpected package file: $path" >&2; exit 1 ;;
    esac
done < <(pacman -Qql trans)
desktop-file-validate /usr/share/applications/io.github.trans.Trans.desktop

# ldd resolves the ELF transitive dependency closure, not only direct NEEDED
# entries. Fail rather than installing a missing development package as fallback.
dependencies=$(ldd /usr/bin/trans /usr/lib/libslint_cpp.so)
printf '%s\n' "$dependencies"
if [[ $dependencies == *'not found'* || $dependencies =~ lib(Qt[56]|KF[56]) ]]; then
    echo 'Missing runtime dependency or forbidden Qt/KF dependency' >&2
    exit 1
fi
while IFS= read -r installed; do
    case "$installed" in
        qt5-*|qt6-*|kglobalaccel|kwindowsystem)
            echo "Unexpected GUI framework installed in clean runtime: $installed" >&2
            exit 1 ;;
    esac
done < <(pacman -Qq)

useradd --create-home smoke
install -d -m700 -o smoke -g smoke /tmp/trans-runtime /tmp/trans-config
actual_version=$(runuser -u smoke -- /usr/bin/trans --version)
[[ $actual_version == "Trans $expected_version" ]]
output=$(runuser -u smoke -- timeout 30s dbus-run-session -- xvfb-run -a \
    -s '-screen 0 1600x1000x24 -nolisten tcp' \
    env -u WAYLAND_DISPLAY XDG_SESSION_TYPE=x11 SLINT_BACKEND=winit-femtovg \
    LIBGL_ALWAYS_SOFTWARE=1 XDG_RUNTIME_DIR=/tmp/trans-runtime \
    XDG_CONFIG_HOME=/tmp/trans-config /usr/bin/trans --smoke-test)
printf '%s\n' "$output"
[[ $output == *TRANS_SMOKE_PASS* ]]
