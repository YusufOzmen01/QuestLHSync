#!/bin/sh
# Builds the Windows and Linux release files into out/release-desktop-<version>/ (version: QLHS_RELEASE in
# src/common/qlhs_status.h), for a GitHub release:
#   QuestLHSync-steamvr-installer.exe   Windows: installs and updates the SteamVR driver (downloads the zip below)
#   QuestLHSync-steamvr-<version>.zip   Windows: the SteamVR driver folder (questlhsync/)
#   QuestLHSync-linux-<version>.tar.gz  Linux (WiVRn / Monado): prebuilt, extract and run ./install-linux.sh
# The Windows side is cross-built with MinGW (build-mingw.sh). The Quest and Steam Frame modules stay with release.py.
set -e
cd "$(dirname "$0")"
VER=$(sed -n 's/.*QLHS_RELEASE "\([^"]*\)".*/\1/p' src/common/qlhs_status.h)
DEST="out/release-desktop-$VER"

./build-mingw.sh
rm -rf "$DEST"
mkdir -p "$DEST"
cp out/QuestLHSync-Installer.exe "$DEST/QuestLHSync-steamvr-installer.exe"
python3 -c "import sys; sys.path.insert(0, '.'); import release; release.steamvr_zip('$DEST/QuestLHSync-steamvr-$VER.zip')"

# Linux: built here, installed into a staging folder, packed with the installer and notices.
GEN=""
command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"
cmake -S . -B build/linux-release $GEN -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=lib
cmake --build build/linux-release
STAGE=build/linux-stage/QuestLHSync-linux
rm -rf build/linux-stage
cmake --install build/linux-release --prefix "$STAGE" --strip
cp install-linux.sh LICENSE THIRD_PARTY_NOTICES.md "$STAGE/"
tar czf "$DEST/QuestLHSync-linux-$VER.tar.gz" -C build/linux-stage QuestLHSync-linux

ls -l "$DEST"
echo "built $DEST"
