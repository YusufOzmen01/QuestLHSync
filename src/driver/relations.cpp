#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#endif

#include "relations.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "json.h"
#include "platform.h"
#ifdef _WIN32
#include "MinHook.h"
#else
#include "../linux/shim/inline_hook.h"
#endif

namespace {

// a relationship this far from the pair's average isn't averaged in, and a second one like it means the station moved
// (one device's view is off by up to ~3 cm and 1.5 deg, rarely more)
constexpr double kFarM = 0.15, kFarDeg = 2.5;
constexpr double kAgreeM = 0.06, kAgreeDeg = 1.0;  // two far ones this close: the station's new place
constexpr size_t kKeepRel = 30;                     // the newest relationships a pair keeps
constexpr int kSayPerMin = 10;                      // log lines a minute at most
constexpr uint64_t kSaveMs = 10000;                 // relations.json written at most this often

// the relationship step's log line
const char kMoving[] = "Moving base %08X %.0fmm and %.1f deg because of relationship with %08X";
Relations *g_self;

#ifdef _WIN32
// its arguments as it takes them: mov r13, r9 (the pose); mov r15d, r8d and mov r12d, edx (the two stations' serials)
const uint8_t kArgs[] = {0x4D, 0x8B, 0xE9, 0x45, 0x8B, 0xF8, 0x44, 0x8B, 0xE2};

// (this, a, b, rel, variance) as it takes them, with room for arguments a later version might add: each one, and what
// it returns, passed on bit for bit
using RelFn = uint64_t (*)(void *, uint64_t, uint64_t, const float *, uint64_t, uint64_t, uint64_t, uint64_t);
RelFn g_orig;

uint64_t Detour(void *self, uint64_t a, uint64_t b, const float *rel, uint64_t s5, uint64_t s6, uint64_t s7,
                uint64_t s8) {
  float avg[7];
  bool use = false;
  try {
    use = g_self && rel && g_self->Average((uint32_t)a, (uint32_t)b, rel, avg);
  } catch (...) {
  }
  return g_orig(self, a, b, use ? avg : rel, s5, s6, s7, s8);
}
#else
// The Linux build (System V ABI) takes (this, a, b, variance, rel): this, a, b in rdi, esi, edx, the variance in
// xmm0 and the pose, 7 floats, by value on the stack; what it returns isn't used. The integer registers it doesn't
// take, and the pose, are passed on bit for bit
struct Rel7 { float f[7]; };
using RelFn = void (*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, float, Rel7);
RelFn g_orig;
InlineHook g_hook;

void Detour(void *self, uint64_t a, uint64_t b, uint64_t r3, uint64_t r4, uint64_t r5, float var, Rel7 rel) {
  Rel7 avg;
  bool use = false;
  try {
    use = g_self && g_self->Average((uint32_t)a, (uint32_t)b, rel.f, avg.f);
  } catch (...) {
  }
  g_orig(self, a, b, r3, r4, r5, var, use ? avg : rel);
}

// the loaded driver_lighthouse.so: its base, program headers and name
struct Image { uintptr_t base = 0; const ElfW(Phdr) *ph = nullptr; int nph = 0; };

int FindImage(dl_phdr_info *info, size_t, void *data) {
  auto *img = (Image *)data;
  const char *n = info->dlpi_name;
  if (!n || !strstr(n, "driver_lighthouse.so")) return 0;
  Dl_info me;
  link_map *lm = nullptr;  // the shim takes driver_lighthouse.so's name in QuestLHSync's SteamVR tree: not it
  if (dladdr1((void *)&FindImage, &me, (void **)&lm, RTLD_DL_LINKMAP) && lm && lm->l_addr == info->dlpi_addr) return 0;
  img->base = info->dlpi_addr;
  img->ph = info->dlpi_phdr;
  img->nph = info->dlpi_phnum;
  return 1;
}

// a DWARF pointer (.eh_frame's encodings: absptr, udata/sdata 4 and 8; pcrel and datarel)
bool ReadEnc(const uint8_t *&p, uint8_t enc, uintptr_t datarel, uintptr_t &out) {
  if (enc == 0xFF) return false;
  const uint8_t *at = p;
  int64_t v;
  switch (enc & 0x0F) {
    case 0x00: { uint64_t u; memcpy(&u, p, 8); v = (int64_t)u; p += 8; break; }
    case 0x03: { uint32_t u; memcpy(&u, p, 4); v = u; p += 4; break; }
    case 0x0B: { int32_t u; memcpy(&u, p, 4); v = u; p += 4; break; }
    case 0x04: case 0x0C: { int64_t u; memcpy(&u, p, 8); v = u; p += 8; break; }
    default: return false;
  }
  switch (enc & 0x70) {
    case 0x00: break;
    case 0x10: v += (int64_t)(uintptr_t)at; break;
    case 0x30: v += (int64_t)datarel; break;
    default: return false;
  }
  out = (uintptr_t)v;
  return true;
}

uint64_t Uleb(const uint8_t *&p) {
  uint64_t r = 0;
  for (int s = 0;; s += 7) { uint8_t b = *p++; r |= (uint64_t)(b & 0x7F) << s; if (!(b & 0x80)) break; }
  return r;
}

// the FDE's function range, its pc_begin encoded as its CIE says (augmentation 'R')
bool FdeRange(const uint8_t *fde, uintptr_t &begin, uintptr_t &end) {
  uint32_t len;
  memcpy(&len, fde, 4);
  if (len == 0 || len == 0xFFFFFFFFu) return false;
  int32_t cie_off;
  memcpy(&cie_off, fde + 4, 4);
  const uint8_t *cie = fde + 4 - cie_off, *c = cie + 8;
  uint8_t ver = *c++;
  const char *aug = (const char *)c;
  c += strlen(aug) + 1;
  Uleb(c);  // code alignment
  Uleb(c);  // data alignment (sleb: only skipped)
  if (ver == 1) c++; else Uleb(c);  // return register
  uint8_t enc = 0;  // absptr
  if (aug[0] == 'z') {
    Uleb(c);
    for (const char *a = aug + 1; *a; a++) {
      if (*a == 'R') enc = *c++;
      else if (*a == 'L') c++;
      else if (*a == 'P') { uint8_t pe = *c++; uintptr_t skip; if (!ReadEnc(c, pe & 0x7F, 0, skip)) return false; }
      else if (*a != 'S' && *a != 'B') return false;
    }
  }
  const uint8_t *q = fde + 8;
  uintptr_t range;
  if (!ReadEnc(q, enc, 0, begin) || !ReadEnc(q, enc & 0x0F, 0, range)) return false;
  end = begin + range;
  return true;
}
#endif

std::string Fmt(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return buf;
}

V3 Rot(const Quat &q, V3 v) { return ToM3(q) * v; }
Quat Conj(const Quat &q) { return {q.w, -q.x, -q.y, -q.z}; }
double Dot(const Quat &a, const Quat &b) { return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z; }

Relations::Pose FromF(const float f[7]) { return {{f[3], f[0], f[1], f[2]}, {f[4], f[5], f[6]}}; }
void ToF(const Relations::Pose &p, float f[7]) {
  f[0] = (float)p.q.x; f[1] = (float)p.q.y; f[2] = (float)p.q.z; f[3] = (float)p.q.w;
  f[4] = (float)p.t.x; f[5] = (float)p.t.y; f[6] = (float)p.t.z;
}

bool Far(const Relations::Pose &a, const Relations::Pose &b, double m, double deg) {
  return norm(a.t - b.t) > m || QuatDeg(a.q, b.q) > deg;
}

std::string Name(uint32_t s) { return Fmt("LHB-%08X", s); }

}  // namespace

Relations::Relations(std::string dir, LogFn log) : dir_(std::move(dir)), log_(std::move(log)) { Load(); }

Relations::Pose Relations::Inv(const Pose &p) {
  Quat c = Conj(p.q);
  return {c, Rot(c, p.t) * -1.0};
}

Relations::Pose Relations::Mean(const std::vector<Pose> &v) {
  Quat s{0, 0, 0, 0};
  V3 t{};
  for (auto &p : v) {
    double k = Dot(p.q, v[0].q) < 0 ? -1 : 1;  // q and -q are the same turn
    s = {s.w + k * p.q.w, s.x + k * p.q.x, s.y + k * p.q.y, s.z + k * p.q.z};
    t += p.t;
  }
  double n = std::sqrt(Dot(s, s));
  return {{s.w / n, s.x / n, s.y / n, s.z / n}, t * (1.0 / v.size())};
}

#ifdef _WIN32
void *Relations::Find(void *module, std::string &why) {
  auto *base = (uint8_t *)module;
  auto *dos = (IMAGE_DOS_HEADER *)base;
  auto *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
  auto *sec = IMAGE_FIRST_SECTION(nt);
  int nsec = nt->FileHeader.NumberOfSections;
  // the log line, then the code that loads it
  const uint8_t *str = nullptr;
  for (int i = 0; i < nsec && !str; i++) {
    if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
    const uint8_t *p = base + sec[i].VirtualAddress, *e = p + sec[i].Misc.VirtualSize;
    for (; p + sizeof kMoving <= e; p++)
      if (*p == 'M' && !memcmp(p, kMoving, sizeof kMoving - 1)) { str = p; break; }
  }
  if (!str) { why = "no base station move in it"; return nullptr; }
  const uint8_t *ref = nullptr;
  for (int i = 0; i < nsec && !ref; i++) {
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
    const uint8_t *p = base + sec[i].VirtualAddress, *e = p + sec[i].Misc.VirtualSize;
    for (; p + 7 <= e; p++) {
      if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) continue;  // lea r, [rip+d]
      int32_t d;
      memcpy(&d, p + 3, 4);
      if (p + 7 + d == str) { ref = p; break; }
    }
  }
  if (!ref) { why = "nothing loads its move line"; return nullptr; }
  // the function around it, from the image's unwind table (a chained entry points back to the function's own)
  auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  auto *rf = (RUNTIME_FUNCTION *)(base + dir.VirtualAddress);
  size_t n = dir.Size / sizeof(RUNTIME_FUNCTION);
  DWORD rva = (DWORD)(ref - base);
  const RUNTIME_FUNCTION *f = nullptr;
  for (size_t lo = 0, hi = n; lo < hi;) {
    size_t mid = (lo + hi) / 2;
    if (rva < rf[mid].BeginAddress) hi = mid;
    else if (rva >= rf[mid].EndAddress) lo = mid + 1;
    else { f = &rf[mid]; break; }
  }
  for (int i = 0; f && i < 8; i++) {
    const uint8_t *ui = base + f->UnwindData;
    if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
    f = (const RUNTIME_FUNCTION *)(ui + 4 + 2 * ((ui[2] + 1) & ~1));
  }
  if (!f) { why = "no unwind entry for its move"; return nullptr; }
  uint8_t *fn = base + f->BeginAddress;
  for (int k = 0; k < 0x40; k++)
    if (!memcmp(fn + k, kArgs, sizeof kArgs)) return fn;
  why = "its relationship step takes other arguments";
  return nullptr;
}

bool Relations::Hook() {
  if (target_) return true;
  HMODULE mod = GetModuleHandleW(L"driver_lighthouse.dll");
  if (!mod) return false;
  std::string why;
  void *fn = Find(mod, why);
  if (!fn) {
    log_("SteamVR's lighthouse driver isn't one this version knows (" + why +
         "): base stations placed as SteamVR places them");
    return true;
  }
  g_self = this;
  if (MH_CreateHook(fn, (void *)&Detour, (void **)&g_orig) != MH_OK || MH_EnableHook(fn) != MH_OK) {
    log_("hooking SteamVR's base station placement failed: base stations placed as SteamVR places them");
    return true;
  }
  target_ = fn;
  size_t known = 0;
  {
    std::lock_guard<std::mutex> g(m_);
    for (auto &kv : pairs_) known += kv.second.size();
  }
  log_(Fmt("base stations: SteamVR places them by the average of its measurements (%zu kept)", known));
  return true;
}

void Relations::Unhook() {
  if (target_) MH_DisableHook(target_);
}
#else
void *Relations::Find(void *module, std::string &why) {
  const Image &img = *(const Image *)module;
  // the log line in a segment that isn't code, then the code that loads it (lea r, [rip+d])
  const uint8_t *str = nullptr, *ref = nullptr, *ehh = nullptr;
  for (int i = 0; i < img.nph && !str; i++) {
    const ElfW(Phdr) &ph = img.ph[i];
    if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X)) continue;
    const uint8_t *p = (const uint8_t *)(img.base + ph.p_vaddr), *e = p + ph.p_memsz;
    for (; p + sizeof kMoving <= e; p++)
      if (*p == 'M' && !memcmp(p, kMoving, sizeof kMoving - 1)) { str = p; break; }
  }
  if (!str) { why = "no base station move in it"; return nullptr; }
  for (int i = 0; i < img.nph && !ref; i++) {
    const ElfW(Phdr) &ph = img.ph[i];
    if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X)) continue;
    const uint8_t *p = (const uint8_t *)(img.base + ph.p_vaddr), *e = p + ph.p_memsz;
    for (; p + 7 <= e; p++) {
      if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) continue;
      int32_t d;
      memcpy(&d, p + 3, 4);
      if (p + 7 + d == str) { ref = p; break; }
    }
  }
  if (!ref) { why = "nothing loads its move line"; return nullptr; }
  // the function around it, from the image's unwind table (.eh_frame_hdr: functions' starts, sorted)
  for (int i = 0; i < img.nph; i++)
    if (img.ph[i].p_type == PT_GNU_EH_FRAME) ehh = (const uint8_t *)(img.base + img.ph[i].p_vaddr);
  if (!ehh || ehh[0] != 1 || ehh[3] != 0x3B) { why = "no unwind table to find its move's function"; return nullptr; }
  const uint8_t *q = ehh + 4;
  uintptr_t frame, count;
  if (!ReadEnc(q, ehh[1], (uintptr_t)ehh, frame) || !ReadEnc(q, ehh[2], (uintptr_t)ehh, count)) {
    why = "an unwind table it can't read";
    return nullptr;
  }
  auto *tab = (const int32_t *)q;  // (start, fde) pairs, relative to .eh_frame_hdr
  uintptr_t at = (uintptr_t)ref, fn = 0;
  size_t lo = 0, hi = count;
  while (lo < hi) {  // the last start <= at
    size_t mid = (lo + hi) / 2;
    if ((uintptr_t)ehh + tab[2 * mid] <= at) lo = mid + 1; else hi = mid;
  }
  if (lo) {
    uintptr_t begin, end;
    const uint8_t *fde = ehh + tab[2 * (lo - 1) + 1];
    if (FdeRange(fde, begin, end) && begin <= at && at < end) fn = begin;
  }
  if (!fn) { why = "no unwind entry for its move"; return nullptr; }
  // (this, a, b, variance, rel): its first instructions take the pose's 7 floats off the stack, movss xmm,
  // [rbp+0x10] .. [rbp+0x28]
  auto *code = (const uint8_t *)fn;
  int got = 0;
  for (int off = 0x10; off <= 0x28; off += 4)
    for (int k = 0; k < 0xA0; k++) {
      const uint8_t *m = code + k;
      if (m[0] != 0xF3) continue;
      const uint8_t *o = m + 1;
      if ((*o & 0xF0) == 0x40) o++;  // REX
      if (o[0] == 0x0F && o[1] == 0x10 && (o[2] & 0xC7) == 0x45 && o[3] == off) { got++; break; }
    }
  if (got != 7) { why = "its relationship step takes other arguments"; return nullptr; }
  return (void *)fn;
}

bool Relations::Hook() {
  if (target_) return true;
  Image img;
  dl_iterate_phdr(&FindImage, &img);
  if (!img.base) return false;
  std::string why;
  void *fn = Find(&img, why);
  if (!fn) {
    log_("SteamVR's lighthouse driver isn't one this version knows (" + why +
         "): base stations placed as SteamVR places them");
    return true;
  }
  g_self = this;
  if (!HookCreate(fn, (void *)&Detour, g_hook, why)) {
    log_("hooking SteamVR's base station placement failed (" + why +
         "): base stations placed as SteamVR places them");
    return true;
  }
  g_orig = (RelFn)g_hook.tramp;
  target_ = fn;
  size_t known = 0;
  {
    std::lock_guard<std::mutex> g(m_);
    for (auto &kv : pairs_) known += kv.second.size();
  }
  log_(Fmt("base stations: SteamVR places them by the average of its measurements (%zu kept)", known));
  return true;
}

void Relations::Unhook() {
  // the hook stays in, passing each measurement on untouched: SteamVR's tracking thread may be in it right now
  enabled_ = false;
}

#endif

bool Relations::Average(uint32_t a, uint32_t b, const float rel[7], float out[7]) {
  if (!enabled_ || a == b || !a || !b) return false;
  Pose p = FromF(rel);
  double qn = Dot(p.q, p.q), d = norm(p.t);
  if (!std::isfinite(qn + d) || std::fabs(qn - 1) > 0.02 || d < 0.2 || d > 50) {
    std::lock_guard<std::mutex> g(m_);
    if (!odd_said_)
      Say(Fmt("base stations %s and %s: SteamVR's measurement of them doesn't read as a pose here (%.3g %.3g %.3g %.3g "
              "%.3g %.3g %.3g): passed on as it is", Name(a).c_str(), Name(b).c_str(), rel[0], rel[1], rel[2], rel[3],
              rel[4], rel[5], rel[6]));
    odd_said_ = true;
    return false;
  }
  p.q = {p.q.w / std::sqrt(qn), p.q.x / std::sqrt(qn), p.q.y / std::sqrt(qn), p.q.z / std::sqrt(qn)};
  bool flip = a > b;  // kept as the lower serial's: a = rel * b
  auto key = flip ? std::make_pair(b, a) : std::make_pair(a, b);
  if (flip) p = Inv(p);
  std::string pair = Name(key.first) + " and " + Name(key.second);
  std::lock_guard<std::mutex> g(m_);
  std::vector<Pose> &v = pairs_[key];
  std::string said;
  if (v.empty()) {
    v.push_back(p);
    said = Fmt("base stations %s: SteamVR's first measurement of them, kept for averaging", pair.c_str());
  } else {
    Pose m = Mean(v);
    double cm = norm(p.t - m.t) * 100, deg = QuatDeg(p.q, m.q);
    auto pend = pending_.find(key);
    if (!Far(p, m, kFarM, kFarDeg)) {
      v.push_back(p);
      if (v.size() > kKeepRel) v.erase(v.begin());
      if (pend != pending_.end()) pending_.erase(pend);
      said = Fmt("base stations %s: SteamVR measured them %.1f cm and %.2f deg from the average of %zu, which it "
                 "gets instead",
                 pair.c_str(), cm, deg, v.size());
    } else if (pend != pending_.end() && !Far(p, pend->second, kAgreeM, kAgreeDeg)) {
      v = {pend->second, p};
      pending_.erase(pend);
      said = Fmt("base stations %s: SteamVR measured them %.1f cm and %.2f deg from the average twice: moved, "
                 "averaging from the new place",
                 pair.c_str(), cm, deg);
    } else {
      pending_[key] = p;
      said = Fmt("base stations %s: SteamVR measured them %.1f cm and %.2f deg from the average of %zu: kept the "
                 "average (another like it means a station moved)",
                 pair.c_str(), cm, deg, v.size());
    }
  }
  dirty_ = true;
  Say(said);
  Pose m = Mean(v);
  if (flip) m = Inv(m);
  ToF(m, out);
  return true;
}

void Relations::Roll(uint64_t now) {
  if (now - say_t0_ < 60000) return;
  if (say_skipped_) said_.push_back(Fmt("base stations: %d more lines like these not logged", say_skipped_));
  say_t0_ = now;
  say_n_ = say_skipped_ = 0;
}

void Relations::Say(const std::string &s) {
  Roll(plat::TickMs());
  if (say_n_ < kSayPerMin) {
    said_.push_back(s);
    say_n_++;
  } else {
    say_skipped_++;
  }
}

void Relations::Flush(bool now) {
  std::string s;
  std::vector<std::string> said;
  {
    std::lock_guard<std::mutex> g(m_);
    uint64_t t = plat::TickMs();
    Roll(t);
    if (dirty_ && (now || t - saved_t_ >= kSaveMs)) {
      s = Json();
      dirty_ = false;
      saved_t_ = t;
    }
    said.swap(said_);
  }
  for (auto &l : said) log_(l);
  if (s.empty()) return;
  std::string path = plat::Join(dir_, "relations.json"), tmp = path + ".tmp";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) return;
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
  plat::Replace(tmp, path);
}

void Relations::Load() {
  std::ifstream f(plat::Join(dir_, "relations.json"), std::ios::binary);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  JVal root;
  if (!JParse(ss.str(), root)) return;
  const JVal *pairs = root.get("pairs");
  if (!pairs || pairs->t != JVal::Obj) return;
  for (auto &kv : pairs->o) {
    unsigned a = 0, b = 0;
    if (sscanf(kv.first.c_str(), "LHB-%8X LHB-%8X", &a, &b) != 2 || a >= b || kv.second.t != JVal::Arr) continue;
    std::vector<Pose> v;
    for (auto &e : kv.second.a) {
      auto x = e.nums();
      if (x.size() != 7) continue;
      float r[7];
      for (int i = 0; i < 7; i++) r[i] = (float)x[i];
      v.push_back(FromF(r));
    }
    if (v.size() > kKeepRel) v.erase(v.begin(), v.end() - kKeepRel);
    if (!v.empty()) pairs_[{a, b}] = v;
  }
}

std::string Relations::Json() const {
  std::string s ="{\n \"note\": \"SteamVR's measurements of where its base stations sit relative to each other "
                  "(qx qy qz qw tx ty tz): QuestLHSync has SteamVR place them by their average. Delete to start "
                  "over.\",\n \"pairs\": {";
  bool first = true;
  for (auto &kv : pairs_) {
    s += Fmt("%s\n  \"%s %s\": [", first ? "" : ",", Name(kv.first.first).c_str(), Name(kv.first.second).c_str());
    for (size_t i = 0; i < kv.second.size(); i++) {
      const Pose &p = kv.second[i];
      s += Fmt("%s\n   [%.6f, %.6f, %.6f, %.6f, %.5f, %.5f, %.5f]", i ? "," : "", p.q.x, p.q.y, p.q.z, p.q.w, p.t.x,
               p.t.y, p.t.z);
    }
    s += "\n  ]";
    first = false;
  }
  s += "\n }\n}\n";
  return s;
}
