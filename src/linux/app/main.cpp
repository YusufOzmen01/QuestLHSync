// questlhsync: QuestLHSync for Monado and WiVRn on Linux. What the SteamVR driver does on Windows (driver_main.cpp),
// as an app beside the runtime: it keeps the lighthouse universe aligned to a headset's own tracking, a rooted
// Quest's or a Steam Frame's, from base station laser flashes the headset's cameras see (lhsyncd on the headset;
// sync.cpp solves the 4-DOF lighthouse -> headset transform).
//   - the lighthouse side comes from the lighthouse shim inside the runtime (shim.cpp, through bridge.h): base
//     stations, the raw poses of controllers and trackers, the hiding of devices not placed yet
//   - the headset's poses come from Monado, and the alignment goes back to it as the lighthouse tracking origin's
//     offset (monado_link.h)
//   - status and commands for the dashboard (questlhsync-dashboard, started from here) go through the shared memory
//     in qlhs_status.h, as on Windows
// Settings: ~/.config/questlhsync/settings.json (the keys of steamvr.vrsettings' driver_questlhsync). Data:
// ~/.local/share/QuestLHSync.
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../../common/qlhs_status.h"
#include "../../driver/gravity.h"
#include "../../driver/json.h"
#include "../../driver/net.h"
#include "../../driver/platform.h"
#include "../../driver/sync.h"
#include "../bridge.h"
#include "../openhouse_room.h"
#include "monado_link.h"
#include "steamvr_tree.h"

namespace {

constexpr double kShimStale = 3;  // s: a shim beat this old, and the runtime runs without it

// ---------------------------------------------------------------- logging
std::mutex g_log_m;
FILE *g_logf;
std::unique_ptr<Sync> g_sync;
std::unique_ptr<Gravity> g_gravity;
std::mutex g_stlog_m;
std::vector<std::string> g_pending_log;
std::atomic<bool> g_run{true};

void PushStatusLog(const std::string &s) {
  std::lock_guard<std::mutex> g(g_stlog_m);
  tm t;
  plat::LocalTime(time(nullptr), t);
  char b[16];
  strftime(b, sizeof b, "%H:%M:%S", &t);
  g_pending_log.push_back(std::string(b) + "  " + s);
  if (g_pending_log.size() > 16) g_pending_log.erase(g_pending_log.begin());
}

void Log(const std::string &s) {
  {
    std::lock_guard<std::mutex> g(g_log_m);
    fprintf(stderr, "questlhsync: %s\n", s.c_str());
    if (g_logf) {
      tm t;
      plat::LocalTime(time(nullptr), t);
      char b[32];
      strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &t);
      fprintf(g_logf, "%s %s\n", b, s.c_str());
      fflush(g_logf);
    }
  }
  PushStatusLog(s);
}

std::string Fmt(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return buf;
}

void Copy(char *dst, size_t n, const std::string &s) {
  strncpy(dst, s.c_str(), n - 1);
  dst[n - 1] = 0;
}

template <class T>
T *MapShm(const std::string &name, bool &fresh) {
  int fd = shm_open(name.c_str(), O_RDWR | O_CREAT, 0600);
  if (fd < 0) return nullptr;
  struct stat st {};
  fresh = fstat(fd, &st) == 0 && (size_t)st.st_size < sizeof(T);
  if (fresh && ftruncate(fd, sizeof(T)) != 0) { close(fd); return nullptr; }
  void *m = mmap(nullptr, sizeof(T), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  return m == MAP_FAILED ? nullptr : (T *)m;
}

std::string ExeDir() {
  std::error_code ec;
  return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path().string();
}

// ---------------------------------------------------------------- settings (steamvr.vrsettings' keys)
struct Settings {
  bool enable = true, any_hmd = false, record = false, gravity = true, steady = true;
  std::vector<std::string> hosts;
  std::string headset;
};

Settings ReadSettings() {
  Settings s;
  std::string path = plat::Join(plat::ConfigDir(), "settings.json"), text;
  std::ifstream f(path, std::ios::binary);
  if (!f) return s;
  std::stringstream ss;
  ss << f.rdbuf();
  JVal root;
  if (!JParse(ss.str(), root) || root.t != JVal::Obj) {
    Log(path + " doesn't parse: its defaults used");
    return s;
  }
  auto flag = [&](const char *k, bool &v) {
    if (const JVal *j = root.get(k))
      if (j->t == JVal::Bool) v = j->b;
  };
  flag("enable", s.enable);
  flag("anyHmd", s.any_hmd);
  flag("record", s.record);
  flag("gravity", s.gravity);
  flag("steadyStations", s.steady);
  if (const JVal *j = root.get("host"))
    if (j->t == JVal::Str) {
      std::string cur;
      for (char c : j->s + ",") {
        if (c == ',' || c == ' ' || c == ';') { if (!cur.empty()) s.hosts.push_back(cur); cur.clear(); }
        else cur += c;
      }
    }
  if (const JVal *j = root.get("headset"))
    if (j->t == JVal::Str) s.headset = j->s;
  return s;
}

// ---------------------------------------------------------------- the app
class App {
 public:
  int Run(bool dashboard) {
    want_dashboard_ = dashboard;
    dir_ = plat::DataDir();
    {
      std::lock_guard<std::mutex> g(g_log_m);
      std::string lp = plat::Join(dir_, "questlhsync.log");
      std::error_code ec;
      if (std::filesystem::file_size(lp, ec) > (2 << 20)) std::filesystem::rename(lp, lp + ".old", ec);
      g_logf = fopen(lp.c_str(), "a");
    }
    bool fresh = false;
    st_ = MapShm<QlhsStatus>(QLHS_SHM_PREFIX + std::to_string(getuid()), fresh);
    if (st_) {
      memset((void *)st_, 0, sizeof(QlhsStatus));
      st_->version = QLHS_VERSION;
      st_->magic = QLHS_MAGIC;
    }
    br_ = MapShm<bridge::Shm>(bridge::Path(), fresh);
    if (br_ && (br_->magic != QLHS_BRIDGE_MAGIC || br_->version != QLHS_BRIDGE_VERSION)) {
      if (br_->magic == QLHS_BRIDGE_MAGIC)
        Log("the lighthouse shim in the runtime is another version of QuestLHSync's: restart WiVRn / Monado");
      memset((void *)br_, 0, sizeof *br_);
      br_->version = QLHS_BRIDGE_VERSION;
      br_->magic = QLHS_BRIDGE_MAGIC;
    }
    if (br_) {
      br_->app_pid = getpid();
      ring_r_ = br_->ring_w.load();
      log_r_ = br_->log_w.load();
    }
    Log("QuestLHSync " QLHS_RELEASE " for Monado / WiVRn starting, data in " + dir_);
    {  // SteamVR's files may have changed since: the tree the runtime loads the shim from, again
      std::string why;
      if (!MakeSteamVRTree(plat::Join(dir_, "steamvr"), plat::Join(ExeDir(), "libquestlhsync_shim.so"), why))
        Log("QuestLHSync's SteamVR tree: " + why);
    }
    set_ = ReadSettings();
    if (!set_.enable) {
      Log("enable is false in settings.json: QuestLHSync is off");
      if (br_) br_->off = 1;
      while (g_run) {
        Publish(QpcNow());
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
      return 0;
    }
    SyncConfig cfg;
    cfg.dir = dir_;
    cfg.per_headset = true;
    g_sync = std::make_unique<Sync>(cfg, [](const std::string &s) { Log(s); });
    g_gravity = std::make_unique<Gravity>(dir_, [](const std::string &s) { Log(s); });
    g_gravity->SetRecord([](double t, const std::string &s) { if (g_sync) g_sync->Rec(t, "%s", s.c_str()); });
    g_gravity->SetEnabled(set_.gravity);
    link_ = std::make_unique<HeadsetLink>(
        g_sync.get(), [](const std::string &s) { Log(s); },
        [](double t, const std::string &s) { if (g_sync) g_sync->Rec(t, "%s", s.c_str()); });
    link_->SetMemory(plat::Join(dir_, "headset.txt"));
    link_->SetHosts(set_.hosts);
    if (!set_.headset.empty()) link_->SetPreferred(set_.headset);
    if (br_) {
      br_->steady = set_.steady;
      br_->off = 0;
    }
    SetRecording(set_.record);
    monado_ = std::make_unique<MonadoLink>(
        [](const std::string &s) { Log(s); },
        [this](double t, const Quat &q, V3 p) {
          if (hmd_ok_ && g_sync) g_sync->OnHmdPose(t, q, p);
        });
    monado_->Start();
    link_->Start();
    reader_ = std::thread(&App::Reader, this);
    Worker();
    // stopping
    if (reader_.joinable()) reader_.join();
    g_gravity->Stop();
    link_->Stop();
    monado_->Stop();
    g_sync->SetRecord(nullptr);
    if (br_) { br_->app_beat = 0; br_->hide = 0; }
    Log("QuestLHSync stopped");
    return 0;
  }

 private:
  std::string dir_;
  Settings set_;
  QlhsStatus *st_ = nullptr;
  bridge::Shm *br_ = nullptr;
  bool room_ours_ = false;
  std::unique_ptr<HeadsetLink> link_;
  std::unique_ptr<MonadoLink> monado_;
  std::thread reader_;
  uint64_t ring_r_ = 0;
  uint32_t log_r_ = 0;
  // the devices as last seen in the bridge
  struct Known { int cls = 0; std::string serial, receiver; int channel = -1; bool said = false; };
  Known known_[bridge::kMaxDev];
  int dev_gen_ = -1;
  std::atomic<bool> hmd_ok_{false};  // an eligible headset: its poses feed the solver
  bool wired_ = false, recording_ = false, dashboard_started_ = false, want_dashboard_ = true;
  bool hmd_said_ = false, was_up_ = false;
  std::string hmd_model_, hmd_system_;
  std::mutex hmd_m_;
  int cmd_seen_ = 0;
  double started_ = 0, last_status_ = 0, last_gravity_ = 0, last_classify_ = 0;
  Sync::Spots rec_spots_;
  pid_t dash_pid_ = 0;

  // the shim's poses and log lines, as they come (the pose hook's part of driver_main.cpp)
  void Reader() {
    while (g_run) {
      if (!br_) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
      uint64_t w = br_->ring_w.load(std::memory_order_acquire);
      if (w - ring_r_ > (uint64_t)bridge::kRing) ring_r_ = w - bridge::kRing / 2;  // fell behind: the newest half
      while (ring_r_ < w) {
        const bridge::Pose &s = br_->ring[ring_r_ % bridge::kRing];
        uint64_t s1 = s.seq.load(std::memory_order_acquire);
        if (s1 < ring_r_ + 1) break;  // still being written
        if (s1 != ring_r_ + 1) { ring_r_++; continue; }  // overwritten
        int dev = s.dev;
        double t = s.t;
        V3 p{s.p[0], s.p[1], s.p[2]};
        Quat q{s.q[0], s.q[1], s.q[2], s.q[3]};
        std::atomic_thread_fence(std::memory_order_acquire);
        bool ok = s.seq.load(std::memory_order_acquire) == s1;
        ring_r_++;
        if (!ok || dev < 0 || dev >= bridge::kMaxDev || wired_) continue;
        // controllers and trackers: worn or held, they stay near the head, which tells a fit from its mirror
        g_sync->OnBodyPose(dev, t, p);
        g_gravity->OnPose(dev, t, p, q);
      }
      uint32_t lw = br_->log_w.load(std::memory_order_acquire);
      if (lw - log_r_ > (uint32_t)bridge::kLogs) log_r_ = lw - bridge::kLogs;
      for (; log_r_ != lw; log_r_++) {
        char line[sizeof br_->log[0]];
        memcpy(line, br_->log[log_r_ % bridge::kLogs], sizeof line);
        line[sizeof line - 1] = 0;
        Log(std::string("lighthouse: ") + line);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }

  bool ReadDev(int i, bridge::Dev &out) {
    const bridge::Dev &d = br_->dev[i];
    for (int tries = 0; tries < 8; tries++) {
      uint32_t s = d.seq.load(std::memory_order_acquire);
      if (s & 1) { std::this_thread::yield(); continue; }
      memcpy((void *)&out.cls, (const void *)&d.cls, sizeof d - offsetof(bridge::Dev, cls));
      std::atomic_thread_fence(std::memory_order_acquire);
      if (d.seq.load(std::memory_order_acquire) == s) return true;
    }
    return false;
  }

  // the lighthouse devices (RunFrame's classification on Windows), the headset, and the base stations for the solver
  void Classify() {
    if (!br_) return;
    std::map<std::string, std::pair<V3, M3>> raw;
    std::map<std::string, int> chans;
    bool up = ShimUp();  // what a runtime that has gone left in the bridge isn't used
    for (int i = 0; i < bridge::kMaxDev && up; i++) {
      bridge::Dev d;
      if (!ReadDev(i, d)) continue;
      Known &k = known_[i];
      std::string serial(d.serial, strnlen(d.serial, sizeof d.serial));
      std::string model(d.model, strnlen(d.model, sizeof d.model));
      std::string receiver(d.receiver, strnlen(d.receiver, sizeof d.receiver));
      if (d.cls != k.cls || serial != k.serial) {
        k = Known{};
        k.cls = d.cls;
        k.serial = serial;
      }
      if (d.cls == bridge::kStation) {
        if (!k.said && !serial.empty()) { Log(Fmt("device %d: lighthouse %s (base station)", i, serial.c_str())); k.said = true; }
        // A 1.0 base station ("HTC V2-XD/XE") has no 2.0 channel: openhouse labels its multi-station
        // slots "1".."4", and taken for 2.0 channels their rotor periods mixed the stations up
        const bool v1 = model.rfind("HTC", 0) == 0;
        const int channel = v1 ? 0 : d.channel;
        if (channel != k.channel) {
          if (channel) Log(Fmt("base station %s: channel %d", serial.c_str(), channel));
          k.channel = channel;
        }
        if (channel) chans[serial] = channel;
        if (d.has_pose) raw[serial] = {V3{d.p[0], d.p[1], d.p[2]}, ToM3(Quat{d.q[0], d.q[1], d.q[2], d.q[3]})};
      } else if (d.cls == bridge::kController || d.cls == bridge::kTracker) {
        if (!k.said && !serial.empty()) {
          Log(Fmt("device %d: lighthouse %s (%s) -> moved", i, serial.c_str(), model.c_str()));
          k.said = true;
        }
        if (receiver != k.receiver) {  // its wireless receiver: gravity.h reads its accelerometer through it
          k.receiver = receiver;
          g_gravity->SetDevice(i, serial, receiver);
        }
      }
    }
    OurRoom(raw);
    g_sync->SetStationsRaw(raw);
    g_sync->SetChannels(chans);
  }

  // openhouse (a replacement lighthouse driver) publishes its room: the stations as it has
  // them now, whether they've stopped moving, and when they moved (openhouse_room.h). With it, the stations are its
  // own and the solver waits while they settle; without (the stock driver), the runtime's devices as before.
  void OurRoom(std::map<std::string, std::pair<V3, M3>> &raw) {
    openhouse_room r;
    bool ours = ReadOurRoom(r);
    if (ours != room_ours_) {
      room_ours_ = ours;
      Log(ours ? Fmt("base stations: openhouse's room %s", r.room) : "base stations: the runtime's (no openhouse room)");
    }
    if (!ours) {
      g_sync->SetRoomSource(false, true, 0);
      return;
    }
    std::map<std::string, std::pair<V3, M3>> own;
    for (int i = 0; i < r.count && i < OPENHOUSE_ROOM_MAX_BASES; i++) {
      const openhouse_room_base &b = r.bases[i];
      if (!b.visible || !b.placed) continue;
      own[std::string(b.serial, strnlen(b.serial, sizeof b.serial))] = {V3{b.p[0], b.p[1], b.p[2]},
                                                                        ToM3(Quat{b.q[0], b.q[1], b.q[2], b.q[3]})};
    }
    raw = own;
    g_sync->SetRoomSource(true, r.settled != 0, r.epoch);
  }

  static bool ReadOurRoom(openhouse_room &out) {
    std::string name = OPENHOUSE_ROOM_PATH_PREFIX + std::to_string((unsigned)getuid());
    int fd = shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0) return false;
    struct stat st {};
    bool sized = fstat(fd, &st) == 0 && (size_t)st.st_size >= sizeof(openhouse_room);
    void *m = sized ? mmap(nullptr, sizeof(openhouse_room), PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    if (m == MAP_FAILED) return false;
    const openhouse_room *r = (const openhouse_room *)m;
    bool ok = false;
    for (int tries = 0; tries < 8 && !ok; tries++) {
      uint32_t s = __atomic_load_n(&r->seq, __ATOMIC_ACQUIRE);
      if (s & 1) { std::this_thread::yield(); continue; }
      memcpy(&out, r, sizeof out);
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      ok = __atomic_load_n(&r->seq, __ATOMIC_ACQUIRE) == s;
    }
    munmap(m, sizeof(openhouse_room));
    if (!ok || out.magic != OPENHOUSE_ROOM_MAGIC || out.version != OPENHOUSE_ROOM_VERSION) return false;
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double mono = ts.tv_sec + 1e-9 * ts.tv_nsec;
    out.room[sizeof out.room - 1] = 0;
    return mono - out.beat < 2.0 && out.pid > 0 && kill(out.pid, 0) == 0;
  }

  // the runtime's headset: a Quest Pro, 3, 3S or a Steam Frame as WiVRn streams it (WiVRn names no model, so whichever
  // headset answers is used, as with Virtual Desktop and ALVR on Windows)
  void Headset() {
    MonadoLink::Head h = monado_->head();
    std::string rt = monado_->runtime();
    // the app outlives the runtime: what one session of it was (a wired headset's) the next needn't be
    bool up = monado_->state() == MonadoLink::kConnected;
    if (was_up_ && !up) Unwire();
    was_up_ = up;
    if (wired_ || !up) return;
    // a lighthouse headset next to a streamed one (a Vive left plugged in) is just another device: only the runtime's
    // head being a lighthouse one means wired
    if (!h.present || rt.empty()) return;
    if (h.lighthouse) { SetWired("lighthouse", h.name); return; }
    if (hmd_ok_ || hmd_said_) return;
    std::string family;
    for (const std::string &p : {h.name, h.serial})
      if (family.empty()) family = QuestFamily(p);
    std::string lower = rt;
    for (char &c : lower) c = (char)tolower((unsigned char)c);
    bool streamer = lower.find("wivrn") != std::string::npos;
    bool any = set_.any_hmd || streamer;
    {
      std::lock_guard<std::mutex> g(hmd_m_);
      hmd_model_ = h.name.empty() ? "headset" : h.name;
      hmd_system_ = rt;
    }
    if (family.empty() && !any) {  // a headset Monado drives itself: one on a cable
      SetWired(rt, h.name);
      return;
    }
    hmd_said_ = true;
    link_->SetFamily(family.empty() ? "" : family);
    g_sync->SetStreamer(lower);
    hmd_ok_ = true;
    Log(Fmt("HMD: %s via %s%s", hmd_model_.c_str(), rt.c_str(),
            !family.empty() || !set_.any_hmd || streamer ? "" :
            " - not named a Quest Pro, 3, 3S or Steam Frame, used because anyHmd is set"));
  }

  // the runtime has gone: whatever headset comes next is looked at afresh
  void Unwire() {
    if (wired_) {
      Log("the runtime has stopped: QuestLHSync waits for the next one");
      dashboard_started_ = false;  // a wired headset closed it
    }
    wired_ = false;
    hmd_ok_ = false;
    hmd_said_ = false;
    {
      std::lock_guard<std::mutex> g(hmd_m_);
      hmd_model_.clear();
      hmd_system_.clear();
    }
    if (br_) br_->off = 0;
    monado_->SetOff(false);
    g_gravity->SetEnabled(set_.gravity);
  }

  // a headset that isn't streamed: nothing of QuestLHSync applies, the worker stands down and the dashboard closes
  void SetWired(const std::string &sys, const std::string &model) {
    if (wired_) return;
    wired_ = true;
    hmd_ok_ = false;
    {
      std::lock_guard<std::mutex> g(hmd_m_);
      hmd_model_ = model.empty() ? "headset" : model;
      hmd_system_ = sys;
    }
    if (br_) br_->off = 1;
    monado_->SetOff(true);
    g_gravity->SetEnabled(false);
    Log(Fmt("HMD: %s (%s) isn't streamed, it's a wired headset: QuestLHSync is off", model.empty() ? "headset" :
            model.c_str(), sys.c_str()));
  }

  void SetRecording(bool on) {
    if (on == recording_) return;
    recording_ = on;
    if (!on) {
      g_sync->SetRecord(nullptr);
      Log("recording off: " + Sync::Describe(rec_spots_, g_sync->spots()));
      return;
    }
    namespace fs = std::filesystem;
    fs::path rd = fs::path(dir_) / "recordings";
    std::vector<fs::path> old;  // keep the newest 9 + this one
    std::error_code ec;
    fs::create_directories(rd, ec);
    for (auto &e : fs::directory_iterator(rd, ec))
      if (e.path().extension() == ".txt") old.push_back(e.path());
    std::sort(old.begin(), old.end());
    while (old.size() > 9) { fs::remove(old.front(), ec); old.erase(old.begin()); }
    tm t;
    plat::LocalTime(time(nullptr), t);
    char b[64];
    strftime(b, sizeof b, "qlhs-%Y%m%d-%H%M%S.txt", &t);
    FILE *f = fopen((rd / b).string().c_str(), "w");
    if (!f) { Log("can't open a recording file"); recording_ = false; return; }
    setvbuf(f, nullptr, _IOFBF, 1 << 16);
    g_sync->SetRecord(f);
    rec_spots_ = g_sync->spots();
    g_sync->Rec(QpcNow(), "I QuestLHSync recording (qlhs_replay reads it)");
    Log(std::string("recording to recordings/") + b);
  }

  void Commands() {
    if (!st_) return;
    int s = __atomic_load_n(&st_->cmd_seq, __ATOMIC_ACQUIRE);
    if (s == cmd_seen_) return;
    cmd_seen_ = s;
    switch (st_->cmd) {
      case QLHS_CMD_PAUSE: g_sync->SetPaused(true); Log("corrections paused"); break;
      case QLHS_CMD_RESUME: g_sync->SetPaused(false); Log("corrections resumed"); break;
      case QLHS_CMD_RECORD_ON: SetRecording(true); break;
      case QLHS_CMD_RECORD_OFF: SetRecording(false); break;
      case QLHS_CMD_RECALIBRATE: g_sync->ForceRecalibrate(); break;
    }
  }

  bool ShimUp() const {
    if (!br_) return false;
    double b = br_->shim_beat.load();
    return b > 0 && QpcNow() - b < kShimStale;
  }

  void Publish(double now) {
    if (!st_) return;
    Sync::Status s;
    if (g_sync) s = g_sync->GetStatus(now);
    std::string model, system;
    {
      std::lock_guard<std::mutex> g(hmd_m_);
      model = hmd_model_;
      system = hmd_system_;
    }
    int state;
    auto ls = link_ ? link_->state() : HeadsetLink::kIdle;
    if (!set_.enable) state = QLHS_DISABLED;
    else if (wired_) state = QLHS_WIRED;
    else if (!monado_ || monado_->state() != MonadoLink::kConnected) state = QLHS_NO_RUNTIME;
    else if (!hmd_ok_) state = QLHS_NO_HMD;
    else if (!ShimUp()) state = QLHS_NO_SHIM;
    else if (ls == HeadsetLink::kSearching || ls == HeadsetLink::kIdle) state = QLHS_SEARCHING;
    else if (ls == HeadsetLink::kConnecting) state = QLHS_CONNECTING;
    else if (now - link_->last_frame() > 3 && now - link_->connected_at() > 5) state = QLHS_NO_CAMERAS;
    else if (s.nstations < 2) state = QLHS_NO_STATIONS;
    else if (s.locked) state = QLHS_LOCKED;
    else state = QLHS_ACQUIRING;
    QlhsStatus *t = st_;
    __atomic_fetch_add(&t->seq, 1, __ATOMIC_SEQ_CST);  // odd: writing
    t->state = state;
    t->updated = now;
    Copy(t->hmd, sizeof t->hmd, model);
    Copy(t->hmd_system, sizeof t->hmd_system, system);
    Copy(t->headset, sizeof t->headset, link_ ? link_->model() : "");
    Copy(t->headset_addr, sizeof t->headset_addr, ls == HeadsetLink::kConnected ? link_->addr() : "");
    Copy(t->headset_fw, sizeof t->headset_fw, link_ ? link_->fw() : "");
    t->cam_fps = s.cam_fps;
    t->sight_rate = s.sight_rate;
    t->spot_rate = s.spot_rate;
    t->head_still = s.head_still;
    t->rtt_ms = s.rtt * 1000;
    t->expo_ms = s.expo * 1000;
    t->expo_learned = s.timing_learned;
    t->nst = (int)std::min<size_t>(s.st.size(), 8);
    for (int i = 0; i < t->nst; i++) {
      auto &e = s.st[i];
      Copy(t->st[i].serial, sizeof t->st[i].serial, e.serial);
      t->st[i].support = e.support;
      t->st[i].anchor = e.anchor;
      t->st[i].measured = e.measured;
      t->st[i].last_seen = e.last_seen;
      t->st[i].dist = e.dist;
    }
    t->locked = s.locked;
    t->cond = s.cond;
    t->yaw_deg = s.has_x ? s.x[0] * kDeg : 0;
    for (int i = 0; i < 3; i++) t->t[i] = s.has_x ? s.x[i + 1] : 0;
    t->med_deg = s.med;
    t->nfit = s.n;
    t->paused = g_sync && g_sync->paused();
    t->locked_for = s.locked_for;
    t->lag_cm = s.lag_cm;
    t->recording = recording_;
    {
      std::lock_guard<std::mutex> g(g_stlog_m);
      for (auto &l : g_pending_log) {
        Copy(t->log[t->nlog % 8], sizeof t->log[0], l);
        t->nlog++;
      }
      g_pending_log.clear();
    }
    __atomic_fetch_add(&t->seq, 1, __ATOMIC_SEQ_CST);
  }

  void LaunchDashboard() {
    if (!getenv("WAYLAND_DISPLAY") && !getenv("DISPLAY")) {
      Log("no desktop session: the dashboard isn't started (questlhsync-dashboard shows it)");
      return;
    }
    std::string exe = plat::Join(ExeDir(), "questlhsync-dashboard");
    if (access(exe.c_str(), X_OK) != 0) { Log("dashboard app missing: no questlhsync-dashboard next to questlhsync"); return; }
    pid_t p = fork();
    if (p == 0) {
      setsid();
      int null = open("/dev/null", O_RDWR);
      if (null >= 0) { dup2(null, 0); dup2(null, 1); }
      execl(exe.c_str(), exe.c_str(), (char *)nullptr);
      _exit(127);
    }
    if (p > 0) { dash_pid_ = p; Log("dashboard app started"); }
    else Log("dashboard app didn't start");
  }

  void Worker() {
    started_ = QpcNow();
    while (g_run) {
      double now = QpcNow();
      if (dash_pid_ > 0 && waitpid(dash_pid_, nullptr, WNOHANG) == dash_pid_) dash_pid_ = 0;
      if (br_) br_->app_beat = now;
      if (now - last_classify_ >= 0.5) {  // about twice a second, as RunFrame classifies on Windows
        last_classify_ = now;
        Headset();
        if (!wired_) Classify();
      }
      if (wired_) {  // only the status goes on, which closes a dashboard already open
        link_->SetWanted(false);  // the headset search and levelling stop (SetWired)
        if (br_) br_->hide = 0;
        if (now - last_status_ >= 0.25) { last_status_ = now; Publish(now); }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      link_->SetWanted(hmd_ok_);
      Commands();
      Transform xf = g_sync->Tick(now);
      monado_->SetTransform(xf);
      if (br_) br_->hide = xf.active ? 2 : link_->state() == HeadsetLink::kConnected ? 1 : 0;
      if (now - last_gravity_ >= 1) {
        last_gravity_ = now;
        Gravity::Ref ref;
        ref.ok = g_sync->GravityFrame(ref.C, ref.turn, ref.key);
        bool on;
        M3 tilt;
        if (g_gravity->Step(ref, on, tilt)) g_sync->SetGravity(on, tilt);
      }
      if (now - last_status_ >= 0.25) { last_status_ = now; Publish(now); }
      if (!dashboard_started_ && want_dashboard_ && now - started_ > 2) {
        dashboard_started_ = true;
        LaunchDashboard();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
};

void OnSignal(int) { g_run = false; }

}  // namespace

int main(int argc, char **argv) {
  bool dashboard = true;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--setup")) {  // the SteamVR tree, and what to point the runtime at
      std::string tree = plat::Join(plat::DataDir(), "steamvr"), why;
      if (!MakeSteamVRTree(tree, plat::Join(ExeDir(), "libquestlhsync_shim.so"), why)) {
        fprintf(stderr, "questlhsync: %s\n", why.c_str());
        return 1;
      }
      printf("%s\n", tree.c_str());
      return 0;
    }
    if (!strcmp(argv[i], "--no-dashboard")) dashboard = false;
    else if (!strcmp(argv[i], "--version")) { printf("QuestLHSync %s\n", QLHS_RELEASE); return 0; }
    else {
      fprintf(stderr, "usage: questlhsync [--no-dashboard] | --setup\n"
                      "  Keeps the lighthouse devices aligned to a Quest's or Steam Frame's own tracking under\n"
                      "  WiVRn or Monado. Settings: ~/.config/questlhsync/settings.json\n"
                      "  --setup: (re)makes the SteamVR tree the runtime loads the lighthouse shim from, and prints\n"
                      "  it: start WiVRn / Monado with STEAMVR_PATH set to it\n");
      return 2;
    }
  }
  const char *rt = getenv("XDG_RUNTIME_DIR");
  std::string lock = std::string(rt && *rt ? rt : "/tmp") + "/questlhsync.lock";
  int lf = open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lf < 0 || flock(lf, LOCK_EX | LOCK_NB) != 0) {
    fprintf(stderr, "questlhsync is already running\n");
    return 1;
  }
  struct sigaction sa {};
  sa.sa_handler = OnSignal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);
  App app;
  return app.Run(dashboard);
}
