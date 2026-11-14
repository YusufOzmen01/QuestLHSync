// QuestLHSync's lighthouse shim: what Monado's steamvr_lh (in wivrn-server or monado-service) loads as SteamVR's
// lighthouse driver, from QuestLHSync's SteamVR tree (STEAMVR_PATH=~/.local/share/QuestLHSync/steamvr: links to the
// real SteamVR, but this in place of driver_lighthouse.so). It loads the real driver and wraps it, the way the Windows
// driver hooks vrserver's pose updates:
//   - base stations, which steamvr_lh drops, are adopted (activated at ids kStationId0 on, past Monado's): their poses
//     and channels go to the app (bridge.h)
//   - every lighthouse controller's and tracker's raw pose goes to the app with its time; while the app says the
//     headset is about to place them they're hidden (poseIsValid false) rather than shown in the raw lighthouse space
//   - SteamVR's base station relationships are averaged (relations.h), as on Windows
// Everything else passes through untouched. The alignment itself is the lighthouse tracking origin's offset, which
// the app sets through libmonado.
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../driver/json.h"
#include "../../driver/platform.h"
#include "../../driver/relations.h"
#include "../bridge.h"
#include "openvr_driver.h"

namespace {

constexpr uint32_t kStationId0 = 40;  // our ids for base stations: Monado's are 0 (HMD) and 1..16 (controllers)
constexpr vr::PropertyContainerHandle_t kStationProps = 0x514C485300000000ull;  // "QLHS" + id: their containers
constexpr double kHideFor = 20;     // s: at most, from the first device that came while not placed
constexpr double kAppStale = 2;     // s: an app beat this old and the shim hides nothing

double Mono() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

std::string Fmt(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return buf;
}

bridge::Shm *g_br;

void Log(const std::string &s) {
  fprintf(stderr, "questlhsync: %s\n", s.c_str());
  if (!g_br) return;
  uint32_t i = g_br->log_w.load(std::memory_order_relaxed);
  char *dst = g_br->log[i % bridge::kLogs];
  strncpy(dst, s.c_str(), sizeof g_br->log[0] - 1);
  dst[sizeof g_br->log[0] - 1] = 0;
  g_br->log_w.store(i + 1, std::memory_order_release);
}

bridge::Shm *OpenBridge() {
  std::string name = bridge::Path();
  int fd = shm_open(name.c_str(), O_RDWR | O_CREAT, 0600);
  if (fd < 0) return nullptr;
  struct stat st {};
  if (fstat(fd, &st) == 0 && (size_t)st.st_size < sizeof(bridge::Shm) && ftruncate(fd, sizeof(bridge::Shm)) != 0) {
    close(fd);
    return nullptr;
  }
  void *m = mmap(nullptr, sizeof(bridge::Shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) return nullptr;
  auto *b = (bridge::Shm *)m;
  if (b->magic != QLHS_BRIDGE_MAGIC || b->version != QLHS_BRIDGE_VERSION) {
    memset((void *)b, 0, sizeof *b);  // fresh, or an older shim's: the app (re)reads it from here
    b->version = QLHS_BRIDGE_VERSION;
    b->magic = QLHS_BRIDGE_MAGIC;
  }
  // a new lighthouse driver: what an earlier run of it left is gone
  for (auto &d : b->dev) {
    d.seq.fetch_add(1);
    d.cls = bridge::kNone;
    d.has_pose = 0;
    d.channel = 0;
    d.serial[0] = d.model[0] = d.receiver[0] = 0;
    d.seq.fetch_add(1);
  }
  b->wired = 0;
  b->rel_state = 0;
  b->dev_gen.fetch_add(1);
  b->shim_pid = getpid();
  b->shim_beat = Mono();
  return b;
}

void Copy(char *dst, size_t n, const std::string &s) {
  strncpy(dst, s.c_str(), n - 1);
  dst[n - 1] = 0;
}

// a device's entry in the bridge, under its seqlock
template <class F>
void EditDev(uint32_t id, F f) {
  if (!g_br || id >= bridge::kMaxDev) return;
  bridge::Dev &d = g_br->dev[id];
  d.seq.fetch_add(1, std::memory_order_acq_rel);
  f(d);
  d.seq.fetch_add(1, std::memory_order_acq_rel);
  g_br->dev_gen.fetch_add(1, std::memory_order_release);
}

vr::HmdQuaternion_t Mul(const vr::HmdQuaternion_t &a, const vr::HmdQuaternion_t &b) {
  return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

void Rotate(const vr::HmdQuaternion_t &q, const double v[3], double out[3]) {
  double ux = q.x, uy = q.y, uz = q.z;  // v + 2w(u x v) + 2 u x (u x v)
  double cx = uy * v[2] - uz * v[1], cy = uz * v[0] - ux * v[2], cz = ux * v[1] - uy * v[0];
  double dx = uy * cz - uz * cy, dy = uz * cx - ux * cz, dz = ux * cy - uy * cx;
  out[0] = v[0] + 2 * (q.w * cx + dx);
  out[1] = v[1] + 2 * (q.w * cy + dy);
  out[2] = v[2] + 2 * (q.w * cz + dz);
}

// the pose in the raw lighthouse universe: WorldFromDriver * pose * DriverFromHead (Monado puts the chaperone and the
// tracking origin's offset on top)
void RawPose(const vr::DriverPose_t &p, double pos[3], vr::HmdQuaternion_t &q) {
  double head[3], loc[3], raw[3];
  Rotate(p.qRotation, p.vecDriverFromHeadTranslation, head);
  for (int i = 0; i < 3; i++) loc[i] = p.vecPosition[i] + head[i];
  Rotate(p.qWorldFromDriverRotation, loc, raw);
  for (int i = 0; i < 3; i++) pos[i] = raw[i] + p.vecWorldFromDriverTranslation[i];
  q = Mul(Mul(p.qWorldFromDriverRotation, p.qRotation), p.qDriverFromHeadRotation);
}

std::unique_ptr<Relations> g_rel;
std::atomic<double> g_unplaced{0};  // when a controller or tracker first came while not placed (0: none)
std::atomic<bool> g_on{true};       // settings' enable
std::atomic<int> g_cls[vr::k_unMaxTrackedDeviceCount];
std::atomic<long long> g_poses{0};

bool Off() { return !g_on.load(std::memory_order_relaxed) || (g_br && g_br->off.load(std::memory_order_relaxed)); }

void PushPose(uint32_t id, double t, const double p[3], const vr::HmdQuaternion_t &q) {
  if (!g_br) return;
  uint64_t i = g_br->ring_w.fetch_add(1, std::memory_order_acq_rel);
  bridge::Pose &s = g_br->ring[i % bridge::kRing];
  s.seq.store(0, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  s.dev = (int)id;
  s.t = t;
  memcpy(s.p, p, sizeof s.p);
  s.q[0] = q.w; s.q[1] = q.x; s.q[2] = q.y; s.q[3] = q.z;
  s.seq.store(i + 1, std::memory_order_release);
}

// ---------------------------------------------------------------- properties
// The lighthouse driver writes each device's properties as it activates it; Monado keeps those of its devices, ours
// (base stations) are kept here. What the app needs of them (model, channel, receiver) goes to the bridge.
class Props : public vr::IVRProperties {
 public:
  vr::IVRProperties *real = nullptr;

  vr::ETrackedPropertyError ReadPropertyBatch(vr::PropertyContainerHandle_t h, vr::PropertyRead_t *batch,
                                              uint32_t n) override {
    if (!Ours(h)) return real->ReadPropertyBatch(h, batch, n);
    if (!batch) return vr::TrackedProp_InvalidOperation;
    std::lock_guard<std::mutex> g(m_);
    auto &store = kept_[h];
    for (uint32_t i = 0; i < n; i++) {
      vr::PropertyRead_t &r = batch[i];
      auto it = store.find(r.prop);
      if (it == store.end()) { r.eError = vr::TrackedProp_UnknownProperty; continue; }
      r.unTag = it->second.tag;
      r.unRequiredBufferSize = (uint32_t)it->second.data.size();
      if (r.unBufferSize < it->second.data.size()) { r.eError = vr::TrackedProp_BufferTooSmall; continue; }
      if (!it->second.data.empty()) memcpy(r.pvBuffer, it->second.data.data(), it->second.data.size());
      r.eError = vr::TrackedProp_Success;
    }
    return vr::TrackedProp_Success;
  }

  vr::ETrackedPropertyError WritePropertyBatch(vr::PropertyContainerHandle_t h, vr::PropertyWrite_t *batch,
                                               uint32_t n) override {
    if (batch) Note(h, batch, n);
    if (!Ours(h)) return real->WritePropertyBatch(h, batch, n);
    if (!batch) return vr::TrackedProp_InvalidOperation;
    std::lock_guard<std::mutex> g(m_);
    auto &store = kept_[h];
    for (uint32_t i = 0; i < n; i++) {
      vr::PropertyWrite_t &w = batch[i];
      if (w.writeType == vr::PropertyWrite_Set) {
        Kept &k = store[w.prop];
        k.tag = w.unTag;
        k.data.assign((const char *)w.pvBuffer, (const char *)w.pvBuffer + (w.pvBuffer ? w.unBufferSize : 0));
      } else {
        store.erase(w.prop);
      }
      w.eError = vr::TrackedProp_Success;
    }
    return vr::TrackedProp_Success;
  }

  const char *GetPropErrorNameFromEnum(vr::ETrackedPropertyError e) override { return real->GetPropErrorNameFromEnum(e); }

  vr::PropertyContainerHandle_t TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t id) override {
    if (id >= kStationId0 && id < vr::k_unMaxTrackedDeviceCount) return kStationProps | id;
    vr::PropertyContainerHandle_t h = real->TrackedDeviceToPropertyContainer(id);
    if (h != vr::k_ulInvalidPropertyContainer) {
      std::lock_guard<std::mutex> g(m_);
      id_of_[h] = id;
    }
    return h;
  }

 private:
  struct Kept { vr::PropertyTypeTag_t tag; std::string data; };
  std::mutex m_;
  std::map<vr::PropertyContainerHandle_t, std::map<int, Kept>> kept_;
  std::map<vr::PropertyContainerHandle_t, uint32_t> id_of_;

  static bool Ours(vr::PropertyContainerHandle_t h) { return (h & ~0xFFull) == kStationProps; }

  void Note(vr::PropertyContainerHandle_t h, const vr::PropertyWrite_t *batch, uint32_t n) {
    uint32_t id;
    if (Ours(h)) {
      id = (uint32_t)(h & 0xFF);
    } else {
      std::lock_guard<std::mutex> g(m_);
      auto it = id_of_.find(h);
      if (it == id_of_.end()) return;
      id = it->second;
    }
    for (uint32_t i = 0; i < n; i++) {
      const vr::PropertyWrite_t &w = batch[i];
      if (w.writeType != vr::PropertyWrite_Set || w.unTag != vr::k_unStringPropertyTag || !w.pvBuffer) continue;
      std::string v((const char *)w.pvBuffer, strnlen((const char *)w.pvBuffer, w.unBufferSize));
      if (w.prop == vr::Prop_ModelNumber_String) {
        EditDev(id, [&](bridge::Dev &d) { Copy(d.model, sizeof d.model, v); });
      } else if (w.prop == vr::Prop_ConnectedWirelessDongle_String) {
        EditDev(id, [&](bridge::Dev &d) { Copy(d.receiver, sizeof d.receiver, v); });
      } else if (w.prop == vr::Prop_ModeLabel_String) {  // a 2.0 base station's channel, "1".."16"
        int ch = 0;
        bool digits = !v.empty();
        for (char c : v) digits = digits && isdigit((unsigned char)c);
        if (digits) ch = atoi(v.c_str());
        if (ch < 1 || ch > 16) ch = 0;
        EditDev(id, [&](bridge::Dev &d) { d.channel = ch; });
      }
    }
  }
};
Props g_props;

// ---------------------------------------------------------------- devices
// a device the lighthouse driver adds: its id, as Monado activates it, says whose poses are whose
class Device : public vr::ITrackedDeviceServerDriver {
 public:
  Device(vr::ITrackedDeviceServerDriver *real, std::string serial, int cls)
      : real_(real), serial_(std::move(serial)), cls_(cls) {}
  vr::EVRInitError Activate(uint32_t id) override {
    Register(id);
    return real_->Activate(id);
  }
  void Register(uint32_t id) {
    if (id >= vr::k_unMaxTrackedDeviceCount) return;
    g_cls[id] = cls_;
    EditDev(id, [&](bridge::Dev &d) {
      d.cls = cls_;
      Copy(d.serial, sizeof d.serial, serial_);
    });
  }
  void Deactivate() override { real_->Deactivate(); }
  void EnterStandby() override { real_->EnterStandby(); }
  void *GetComponent(const char *name) override { return real_->GetComponent(name); }
  void DebugRequest(const char *req, char *buf, uint32_t n) override { real_->DebugRequest(req, buf, n); }
  vr::DriverPose_t GetPose() override { return real_->GetPose(); }
  vr::ITrackedDeviceServerDriver *real() const { return real_; }

 private:
  vr::ITrackedDeviceServerDriver *real_;
  std::string serial_;
  int cls_;
};

std::mutex g_dev_m;
std::vector<Device *> g_stations;  // ours to activate and deactivate
std::map<vr::ITrackedDeviceServerDriver *, Device *> g_wrapped;  // never freed: Monado keeps the pointers

bridge::Cls ClsOf(vr::ETrackedDeviceClass c) {
  switch (c) {
    case vr::TrackedDeviceClass_HMD: return bridge::kHmd;
    case vr::TrackedDeviceClass_Controller: return bridge::kController;
    case vr::TrackedDeviceClass_GenericTracker: return bridge::kTracker;
    case vr::TrackedDeviceClass_TrackingReference: return bridge::kStation;
    default: return bridge::kNone;
  }
}

// ---------------------------------------------------------------- the host
class Host : public vr::IVRServerDriverHost {
 public:
  vr::IVRServerDriverHost *real = nullptr;

  bool TrackedDeviceAdded(const char *serial, vr::ETrackedDeviceClass cls, vr::ITrackedDeviceServerDriver *drv) override {
    bridge::Cls c = ClsOf(cls);
    if (c == bridge::kHmd && g_br && !g_br->wired.exchange(1))
      Log(Fmt("the lighthouse driver has a headset (%s): QuestLHSync goes by whether it's the runtime's", serial ? serial : ""));
    if (c != bridge::kStation || !drv) {
      Device *d;
      {
        std::lock_guard<std::mutex> g(g_dev_m);
        Device *&w = g_wrapped[drv];
        if (!w && drv) w = new Device(drv, serial ? serial : "", c);
        d = w;
      }
      return real->TrackedDeviceAdded(serial, cls, d ? d : drv);
    }
    // a base station: Monado's steamvr_lh only logs it; adopted here
    real->TrackedDeviceAdded(serial, cls, drv);
    uint32_t id;
    Device *d;
    {
      std::lock_guard<std::mutex> g(g_dev_m);
      for (Device *s : g_stations)
        if (s->real() == drv) return true;  // added again
      id = kStationId0 + (uint32_t)g_stations.size();
      if (id >= vr::k_unMaxTrackedDeviceCount) {
        Log(Fmt("base station %s: no room for it", serial ? serial : ""));
        return false;
      }
      d = new Device(drv, serial ? serial : "", c);
      g_stations.push_back(d);
    }
    d->Register(id);
    vr::EVRInitError e = drv->Activate(id);
    Log(Fmt("base station %s: %s", serial ? serial : "", e == vr::VRInitError_None ? "kept for QuestLHSync"
                                                                                 : "didn't activate"));
    return e == vr::VRInitError_None;
  }

  void TrackedDevicePoseUpdated(uint32_t id, const vr::DriverPose_t &pose, uint32_t size) override {
    if (size != sizeof(vr::DriverPose_t) || id >= vr::k_unMaxTrackedDeviceCount) {
      real->TrackedDevicePoseUpdated(id, pose, size);
      return;
    }
    int cls = g_cls[id].load(std::memory_order_relaxed);
    if (cls == bridge::kStation) {  // ours: Monado has no such device
      if (!Off() && pose.poseIsValid && pose.deviceIsConnected && pose.result == vr::TrackingResult_Running_OK) {
        double p[3];
        vr::HmdQuaternion_t q;
        RawPose(pose, p, q);
        double qq = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
        // SteamVR hands out zero poses while it starts and stops
        if (qq > 0.9 && qq < 1.1 && std::isfinite(p[0] + p[1] + p[2]))
          EditDev(id, [&](bridge::Dev &d) {
            memcpy(d.p, p, sizeof p);
            d.q[0] = q.w; d.q[1] = q.x; d.q[2] = q.y; d.q[3] = q.z;
            d.has_pose = 1;
          });
      }
      return;
    }
    if ((cls != bridge::kController && cls != bridge::kTracker) || Off()) {
      real->TrackedDevicePoseUpdated(id, pose, size);
      return;
    }
    g_poses.fetch_add(1, std::memory_order_relaxed);
    if (pose.poseIsValid && pose.result == vr::TrackingResult_Running_OK) {
      double p[3];
      vr::HmdQuaternion_t q;
      RawPose(pose, p, q);
      if (std::isfinite(p[0] + p[1] + p[2])) PushPose(id, Mono() + pose.poseTimeOffset, p, q);
    }
    // not placed yet while the headset's cameras are about to: hidden, rather than shown metres away in SteamVR's own
    // lighthouse universe and then snapped into place
    int hide = g_br ? g_br->hide.load(std::memory_order_relaxed) : 0;
    double now = Mono();
    if (hide == 2) g_unplaced.store(0, std::memory_order_relaxed);
    if (hide == 1 && now - g_br->app_beat.load(std::memory_order_relaxed) < kAppStale) {
      double since = g_unplaced.load(std::memory_order_relaxed);
      if (since == 0) g_unplaced.compare_exchange_strong(since, now);
      if (since == 0 || now - since <= kHideFor) {
        vr::DriverPose_t p = pose;
        p.poseIsValid = false;
        p.result = vr::TrackingResult_Running_OutOfRange;
        real->TrackedDevicePoseUpdated(id, p, size);
        return;
      }
    }
    real->TrackedDevicePoseUpdated(id, pose, size);
  }

  void VsyncEvent(double t) override { real->VsyncEvent(t); }
  void VendorSpecificEvent(uint32_t id, vr::EVREventType type, const vr::VREvent_Data_t &data, double t) override {
    if (id >= kStationId0 && id < vr::k_unMaxTrackedDeviceCount) return;  // Monado has no device there
    real->VendorSpecificEvent(id, type, data, t);
  }
  bool IsExiting() override { return real->IsExiting(); }
  bool PollNextEvent(vr::VREvent_t *e, uint32_t n) override { return real->PollNextEvent(e, n); }
  void GetRawTrackedDevicePoses(float s, vr::TrackedDevicePose_t *p, uint32_t n) override {
    real->GetRawTrackedDevicePoses(s, p, n);
  }
  void RequestRestart(const char *a, const char *b, const char *c, const char *d) override {
    real->RequestRestart(a, b, c, d);
  }
  uint32_t GetFrameTimings(vr::Compositor_FrameTiming *t, uint32_t n) override { return real->GetFrameTimings(t, n); }
  void SetDisplayEyeToHead(uint32_t id, const vr::HmdMatrix34_t &l, const vr::HmdMatrix34_t &r) override {
    real->SetDisplayEyeToHead(id, l, r);
  }
  void SetDisplayProjectionRaw(uint32_t id, const vr::HmdRect2_t &l, const vr::HmdRect2_t &r) override {
    real->SetDisplayProjectionRaw(id, l, r);
  }
  void SetRecommendedRenderTargetSize(uint32_t id, uint32_t w, uint32_t h) override {
    real->SetRecommendedRenderTargetSize(id, w, h);
  }
};
Host g_host;

class Context : public vr::IVRDriverContext {
 public:
  vr::IVRDriverContext *real = nullptr;
  void *GetGenericInterface(const char *version, vr::EVRInitError *err) override {
    void *r = real->GetGenericInterface(version, err);
    if (!r || !version) return r;
    if (!strcmp(version, vr::IVRServerDriverHost_Version)) {
      g_host.real = (vr::IVRServerDriverHost *)r;
      return &g_host;
    }
    if (!strcmp(version, vr::IVRProperties_Version)) {
      g_props.real = (vr::IVRProperties *)r;
      return &g_props;
    }
    return r;
  }
  vr::DriverHandle_t GetDriverHandle() override { return real->GetDriverHandle(); }
};
Context g_ctx;

// settings.json: "enable" and "steadyStations" (what the shim reads; the app reads the rest)
void ReadSettings(bool &enable, bool &steady) {
  enable = steady = true;
  FILE *f = fopen(plat::Join(plat::ConfigDir(), "settings.json").c_str(), "rb");
  if (!f) return;
  std::string text;
  char buf[4096];
  for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
  fclose(f);
  JVal root;
  if (!JParse(text, root)) {
    Log("settings.json doesn't parse: its defaults used");
    return;
  }
  if (const JVal *v = root.get("enable")) enable = v->t != JVal::Bool || v->b;
  if (const JVal *v = root.get("steadyStations")) steady = v->t != JVal::Bool || v->b;
}

class Provider : public vr::IServerTrackedDeviceProvider {
 public:
  vr::IServerTrackedDeviceProvider *real = nullptr;

  vr::EVRInitError Init(vr::IVRDriverContext *ctx) override {
    g_ctx.real = ctx;
    g_br = OpenBridge();
    bool enable, steady;
    ReadSettings(enable, steady);
    g_on = enable;
    Log(std::string("lighthouse shim in ") + program_invocation_short_name +
        (g_br ? "" : ": no shared memory for the app") + (enable ? "" : " (off: enable is false)"));
    std::string dir = plat::DataDir();
    g_rel = std::make_unique<Relations>(dir, [](const std::string &s) { Log(s); });
    g_rel->SetEnabled(enable && steady);
    if (enable && steady) {
      g_rel->Hook();
      if (g_br) g_br->rel_state = 1;
    }
    run_ = true;
    worker_ = std::thread(&Provider::Worker, this);
    return real->Init(&g_ctx);
  }

  void Cleanup() override {
    run_ = false;
    if (worker_.joinable()) worker_.join();
    if (g_rel) { g_rel->Unhook(); g_rel->Flush(true); }
    std::vector<Device *> st;
    {
      std::lock_guard<std::mutex> g(g_dev_m);
      st = g_stations;
    }
    for (Device *d : st) d->Deactivate();
    if (g_br) g_br->shim_beat = 0;
    Log("lighthouse shim stopped");
    real->Cleanup();
  }

  const char *const *GetInterfaceVersions() override { return real->GetInterfaceVersions(); }
  void RunFrame() override { real->RunFrame(); }
  bool ShouldBlockStandbyMode() override { return real->ShouldBlockStandbyMode(); }
  void EnterStandby() override { real->EnterStandby(); }
  void LeaveStandby() override { real->LeaveStandby(); }

 private:
  std::atomic<bool> run_{false};
  std::thread worker_;

  void Worker() {
    while (run_) {
      for (int i = 0; i < 10 && run_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (g_br) {
        g_br->shim_beat = Mono();
        bool steady = g_br->app_beat.load() == 0 || Mono() - g_br->app_beat.load() > kAppStale || g_br->steady.load();
        if (g_rel) g_rel->SetEnabled(g_on && steady && !Off());
      }
      if (g_rel) g_rel->Flush();
    }
  }
};
Provider g_provider;

using Factory = void *(*)(const char *, int *);

// The real lighthouse driver, next to this one in QuestLHSync's SteamVR tree as a link to it (.questlhsync-real), or
// QLHS_REAL_LIGHTHOUSE. It's opened at its own place, so it finds its own files as it always does
Factory RealFactory() {
  static Factory f = [] {
    std::string path;
    if (const char *e = getenv("QLHS_REAL_LIGHTHOUSE")) path = e;
    Dl_info me;
    if (path.empty() && dladdr((void *)&RealFactory, &me) && me.dli_fname) {
      std::string dir = me.dli_fname;
      dir = dir.substr(0, dir.rfind('/') + 1);
      char real[4096];
      ssize_t n = readlink((dir + ".questlhsync-real").c_str(), real, sizeof real - 1);
      if (n > 0) path.assign(real, n);
    }
    if (path.empty()) { Log("no link to SteamVR's lighthouse driver next to the shim: run questlhsync --setup"); return (Factory) nullptr; }
    void *lib = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!lib) { Log(std::string("SteamVR's lighthouse driver didn't load: ") + dlerror()); return (Factory) nullptr; }
    auto r = (Factory)dlsym(lib, "HmdDriverFactory");
    if (!r) Log("SteamVR's lighthouse driver has no HmdDriverFactory");
    return r;
  }();
  return f;
}

}  // namespace

// What Monado's steamvr_lh loads as SteamVR's lighthouse driver (STEAMVR_PATH: QuestLHSync's SteamVR tree): the real
// one's provider, wrapped
extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *name, int *ret) {
  Factory real = RealFactory();
  if (!real) {
    if (ret) *ret = vr::VRInitError_Init_FileNotFound;
    return nullptr;
  }
  void *r = real(name, ret);
  if (!r || !name || strcmp(name, vr::IServerTrackedDeviceProvider_Version)) return r;
  g_provider.real = (vr::IServerTrackedDeviceProvider *)r;
  return &g_provider;
}
