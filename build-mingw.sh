#!/bin/sh
# Cross-builds the Windows side on Linux with MinGW-w64, as build.bat does with Visual Studio: the SteamVR driver
# (driver/questlhsync/bin/win64/driver_questlhsync.dll), the dashboard app (QuestLHSync.exe next to it, with
# openvr_api.dll) and the installer (out/QuestLHSync-Installer.exe), plus out/QuestLHSync-steamvr-mingw.zip of the
# driver folder. Everything is linked statically: only Windows system DLLs and the UCRT are needed.
#   ./build-mingw.sh        needs x86_64-w64-mingw32-g++ (mingw-w64-gcc)
set -e
cd "$(dirname "$0")"
CXX=${CXX:-x86_64-w64-mingw32-g++}
CC=${CC:-x86_64-w64-mingw32-gcc}
STRIP=${STRIP:-x86_64-w64-mingw32-strip}
OUT=driver/questlhsync/bin/win64
MH=third_party/minhook
OVR=third_party/openvr
OBJ=build/mingw
mkdir -p "$OBJ" "$OUT" out
DEFS="-DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS"
STATIC="-static -static-libgcc -static-libstdc++"

for c in buffer hook trampoline hde/hde64; do
  $CC -O2 -c -I$MH/include "$MH/src/$c.c" -o "$OBJ/$(basename $c).o"
done
$CXX -shared -O2 -std=c++17 $DEFS -I$OVR/headers -I$MH/include \
  src/driver/driver_main.cpp src/driver/sync.cpp src/driver/net.cpp src/driver/gravity.cpp src/driver/relations.cpp \
  "$OBJ"/buffer.o "$OBJ"/hook.o "$OBJ"/trampoline.o "$OBJ"/hde64.o \
  -o $OUT/driver_questlhsync.dll $STATIC -lws2_32 -liphlpapi -lshell32 -lole32 -lsetupapi -lhid -luuid
echo "built $OUT/driver_questlhsync.dll"

$CXX -O2 -std=c++17 -municode -mwindows $DEFS -DUNICODE -D_UNICODE -I$OVR/headers src/overlay/overlay_main.cpp \
  -o $OUT/QuestLHSync.exe $STATIC $OVR/bin/win64/openvr_api.dll -lgdi32 -luser32 -lshell32 -ld3d11 -ldxgi -luuid
cp $OVR/bin/win64/openvr_api.dll $OUT/
echo "built $OUT/QuestLHSync.exe"

$CXX -O2 -std=c++17 -municode -mwindows $DEFS -DUNICODE -D_UNICODE src/installer/installer_main.cpp \
  -o out/QuestLHSync-Installer.exe $STATIC -lwinhttp -lgdi32 -luser32 -ldwmapi -lshell32 -lole32 -luuid
echo "built out/QuestLHSync-Installer.exe"

$STRIP $OUT/driver_questlhsync.dll $OUT/QuestLHSync.exe out/QuestLHSync-Installer.exe
rm -f out/QuestLHSync-steamvr-mingw.zip
(cd driver && zip -qr ../out/QuestLHSync-steamvr-mingw.zip questlhsync)
echo "packed out/QuestLHSync-steamvr-mingw.zip (register the extracted questlhsync folder with vrpathreg adddriver)"
