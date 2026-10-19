// The few things the shared code needs from the OS, for Windows (the SteamVR driver) and Linux (the Monado / WiVRn
// app and its lighthouse shim).
#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#include <time.h>
#endif

namespace plat {

inline void LocalTime(time_t t, tm &out) {
#ifdef _WIN32
  localtime_s(&out, &t);
#else
  localtime_r(&t, &out);
#endif
}

// tmp over path, in one step
inline bool Replace(const std::string &tmp, const std::string &path) {
#ifdef _WIN32
  return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
  return std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

inline uint64_t TickMs() {
#ifdef _WIN32
  return GetTickCount64();
#else
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

inline std::string Join(const std::string &dir, const std::string &name) {
  return (std::filesystem::path(dir) / name).string();
}

#ifndef _WIN32
inline std::string Home() {
  const char *h = getenv("HOME");
  return h && *h ? h : ".";
}

// $XDG_DATA_HOME/QuestLHSync (~/.local/share/QuestLHSync): what %LOCALAPPDATA%\QuestLHSync is on Windows. The app and
// the lighthouse shim (inside wivrn-server / monado-service) both keep their files there
inline std::string DataDir() {
  const char *x = getenv("XDG_DATA_HOME");
  std::filesystem::path d = x && *x ? std::filesystem::path(x) : std::filesystem::path(Home()) / ".local" / "share";
  d /= "QuestLHSync";
  std::error_code ec;
  std::filesystem::create_directories(d, ec);
  return d.string();
}

// $XDG_CONFIG_HOME/questlhsync (~/.config/questlhsync): settings.json, in place of steamvr.vrsettings
inline std::string ConfigDir() {
  const char *x = getenv("XDG_CONFIG_HOME");
  std::filesystem::path d = x && *x ? std::filesystem::path(x) : std::filesystem::path(Home()) / ".config";
  return (d / "questlhsync").string();
}
#endif

}  // namespace plat
