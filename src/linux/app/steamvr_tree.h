// QuestLHSync's SteamVR tree, ~/.local/share/QuestLHSync/steamvr: what the runtime is pointed at (STEAMVR_PATH) so
// that its lighthouse driver is the shim. Links to everything of the real SteamVR, but drivers/lighthouse/bin/linux64/
// driver_lighthouse.so links to the shim, and .questlhsync-real next to it to the real driver.
#pragma once
#include <string>

// the real SteamVR install (as Monado's steamvr_lh finds it: Steam's library folders), "" if none
std::string FindSteamVR(const std::string &tree);
// (re)makes the tree; false with why
bool MakeSteamVRTree(const std::string &tree, const std::string &shim, std::string &why);
