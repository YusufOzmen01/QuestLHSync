#!/bin/sh
# QuestLHSync for Linux (WiVRn / Monado): builds and installs it for your user. Needs cmake, a C++17 compiler, the
# OpenXR loader (openxr) and GTK 3 (for the dashboard). SteamVR must be installed (its lighthouse driver is used).
#   ./install-linux.sh             build, install to ~/.local/lib/questlhsync, start the questlhsync user service
#   ./install-linux.sh uninstall   remove all of that (your data in ~/.local/share/QuestLHSync stays)
set -e
cd "$(dirname "$0")"
LIB="$HOME/.local/lib/questlhsync"
CFG="${XDG_CONFIG_HOME:-$HOME/.config}"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
TREE="$DATA/QuestLHSync/steamvr"
UNIT="$CFG/systemd/user"

if [ "$1" = uninstall ]; then
  systemctl --user disable --now questlhsync.service 2>/dev/null || true
  rm -f "$UNIT/questlhsync.service" "$UNIT/wivrn.service.d/questlhsync.conf" "$CFG/environment.d/questlhsync.conf" \
        "$DATA/applications/questlhsync-dashboard.desktop" "$HOME/.local/bin/questlhsync" \
        "$HOME/.local/bin/questlhsync-dashboard"
  rmdir "$UNIT/wivrn.service.d" 2>/dev/null || true
  systemctl --user unset-environment STEAMVR_PATH 2>/dev/null || true
  systemctl --user daemon-reload 2>/dev/null || true
  rm -rf "$LIB" "$TREE"
  echo "QuestLHSync removed. Restart WiVRn so it loads SteamVR's own lighthouse driver again."
  exit 0
fi

GEN=""
command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"
cmake -S . -B build/linux $GEN -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux
cmake --install build/linux --prefix "$HOME/.local"

# the SteamVR tree the runtime loads the lighthouse shim from
"$LIB/questlhsync" --setup >/dev/null

mkdir -p "$HOME/.local/bin" "$CFG/environment.d" "$UNIT/wivrn.service.d" "$DATA/applications"
ln -sf "$LIB/questlhsync" "$HOME/.local/bin/questlhsync"
ln -sf "$LIB/questlhsync-dashboard" "$HOME/.local/bin/questlhsync-dashboard"

# STEAMVR_PATH for the runtime: the session (WiVRn's dashboard, Envision, a terminal) after the next login, the user
# services (WiVRn's own unit) now
echo "STEAMVR_PATH=$TREE" > "$CFG/environment.d/questlhsync.conf"
printf '[Service]\nEnvironment=STEAMVR_PATH=%s\n' "$TREE" > "$UNIT/wivrn.service.d/questlhsync.conf"
systemctl --user set-environment STEAMVR_PATH="$TREE"

cat > "$UNIT/questlhsync.service" <<EOF
[Unit]
Description=QuestLHSync: lighthouse devices aligned to the headset's own tracking (WiVRn / Monado)

[Service]
ExecStart=$LIB/questlhsync
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
EOF

cat > "$DATA/applications/questlhsync-dashboard.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=QuestLHSync
Comment=Lighthouse tracking aligned to your headset, from its own cameras
Exec=$LIB/questlhsync-dashboard
Icon=input-gaming
Categories=Utility;
EOF

systemctl --user daemon-reload
systemctl --user enable questlhsync.service >/dev/null 2>&1
systemctl --user restart questlhsync.service

echo
echo "QuestLHSync installed, its service started (journalctl --user -u questlhsync)."
echo "WiVRn must start with STEAMVR_PATH=$TREE to load QuestLHSync's lighthouse shim:"
echo "  - started by its user service (systemctl --user start wivrn): set already"
echo "  - started from WiVRn's dashboard: log out and in once (or start the dashboard from a terminal"
echo "    with STEAMVR_PATH=$TREE)"
echo "  - Envision: add STEAMVR_PATH=$TREE to the profile's environment variables"
echo "Restart WiVRn if it's running."
