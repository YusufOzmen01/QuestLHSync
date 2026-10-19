#include "steamvr_tree.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include "../../driver/platform.h"

namespace fs = std::filesystem;

std::string FindSteamVR(const std::string &tree) {
  std::error_code ec;
  auto ok = [&](const fs::path &p) {
    return fs::exists(p / "drivers" / "lighthouse" / "bin" / "linux64" / "driver_lighthouse.so", ec) &&
           fs::canonical(p, ec) != fs::canonical(tree, ec);
  };
  std::vector<fs::path> cand;
  if (const char *e = getenv("QLHS_STEAMVR")) cand.push_back(e);
  if (const char *e = getenv("STEAMVR_PATH")) cand.push_back(e);
  // Steam's library folders, as steamvr_lh reads them
  std::string home = plat::Home();
  for (const char *root : {"/.steam/root", "/.steam/steam", "/.local/share/Steam"}) {
    std::ifstream f(home + root + "/steamapps/libraryfolders.vdf");
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    std::regex path_re("\"path\"\\s+\"([^\"]+)\"");
    for (std::sregex_iterator it(text.begin(), text.end(), path_re), end; it != end; ++it)
      cand.push_back(fs::path((*it)[1].str()) / "steamapps" / "common" / "SteamVR");
    cand.push_back(fs::path(home + root) / "steamapps" / "common" / "SteamVR");
  }
  for (auto &c : cand)
    if (ok(c)) return fs::canonical(c, ec).string();
  return "";
}

namespace {

// dir: a link to each entry of real, but those in skip
bool LinkAll(const fs::path &real, const fs::path &dir, const std::set<std::string> &skip, std::string &why) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  // links whose entry has gone go too
  for (auto &e : fs::directory_iterator(dir, ec))
    if (fs::is_symlink(e.symlink_status(ec)) && !skip.count(e.path().filename().string()) &&
        !fs::exists(real / e.path().filename(), ec))
      fs::remove(e.path(), ec);
  for (auto &e : fs::directory_iterator(real, ec)) {
    std::string name = e.path().filename().string();
    if (skip.count(name)) continue;
    fs::path at = dir / name;
    std::error_code e2;
    if (fs::is_symlink(fs::symlink_status(at, e2)) && fs::read_symlink(at, e2) == e.path()) continue;
    fs::remove(at, e2);
    fs::create_symlink(e.path(), at, e2);
    if (e2) { why = "can't link " + at.string() + ": " + e2.message(); return false; }
  }
  if (ec) { why = "can't read " + real.string() + ": " + ec.message(); return false; }
  return true;
}

bool Link(const fs::path &to, const fs::path &at, std::string &why) {
  std::error_code ec;
  if (fs::is_symlink(fs::symlink_status(at, ec)) && fs::read_symlink(at, ec) == to) return true;
  fs::remove(at, ec);
  fs::create_symlink(to, at, ec);
  if (ec) { why = "can't link " + at.string() + ": " + ec.message(); return false; }
  return true;
}

}  // namespace

bool MakeSteamVRTree(const std::string &tree, const std::string &shim, std::string &why) {
  std::string real = FindSteamVR(tree);
  if (real.empty()) { why = "SteamVR isn't installed (Steam's lighthouse driver comes with it)"; return false; }
  std::error_code ec;
  if (!fs::exists(shim, ec)) { why = "no " + shim; return false; }
  fs::path r = real, t = tree;
  fs::path lh = "drivers/lighthouse", bin = lh / "bin" / "linux64";
  return LinkAll(r, t, {"drivers"}, why) && LinkAll(r / "drivers", t / "drivers", {"lighthouse"}, why) &&
         LinkAll(r / lh, t / lh, {"bin"}, why) && LinkAll(r / lh / "bin", t / lh / "bin", {"linux64"}, why) &&
         LinkAll(r / bin, t / bin, {"driver_lighthouse.so", ".questlhsync-real"}, why) &&
         Link(fs::canonical(shim, ec), t / bin / "driver_lighthouse.so", why) &&
         Link(r / bin / "driver_lighthouse.so", t / bin / ".questlhsync-real", why);
}
