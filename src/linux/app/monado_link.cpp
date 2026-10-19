#include "monado_link.h"

#include <dlfcn.h>
#include <time.h>

#define XR_USE_TIMESPEC
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "../../driver/json.h"
#include "../../driver/net.h"
#include "../../driver/platform.h"
#include "monado/monado.h"
#include "openxr/XR_MNDX_xdev_space.h"

namespace {

constexpr double kLag = 0.03;       // s: the headset located this far behind now, from Monado's pose history
constexpr double kStep = 0.004;     // s between headset poses
constexpr double kRefresh = 0.25;   // s: the stage offset, the devices, the origins re-read
constexpr double kReassert = 1.0;   // s: the origin offset set again even unchanged (something else may move it)

std::string Fmt(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return buf;
}

struct Pose {
  Quat q{1, 0, 0, 0};
  V3 t{};
};
Pose Mul(const Pose &a, const Pose &b) { return {a.q * b.q, a.t + ToM3(a.q) * b.t}; }
Pose Inv(const Pose &a) {
  Quat c{a.q.w, -a.q.x, -a.q.y, -a.q.z};
  return {c, ToM3(c) * a.t * -1.0};
}
Quat Norm(Quat q) {
  double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  return n > 0 ? Quat{q.w / n, q.x / n, q.y / n, q.z / n} : Quat{1, 0, 0, 0};
}
Pose FromMnd(const mnd_pose_t &p) {
  return {Norm({p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z}),
          {p.position.x, p.position.y, p.position.z}};
}
mnd_pose_t ToMnd(const Pose &p) {
  mnd_pose_t m;
  m.orientation.x = (float)p.q.x; m.orientation.y = (float)p.q.y; m.orientation.z = (float)p.q.z;
  m.orientation.w = (float)p.q.w;
  m.position.x = (float)p.t.x; m.position.y = (float)p.t.y; m.position.z = (float)p.t.z;
  return m;
}
Pose FromXr(const XrPosef &p) {
  return {Norm({p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z}),
          {p.position.x, p.position.y, p.position.z}};
}

bool ReadText(const std::string &path, std::string &out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

std::string Str(const JVal *v) {
  if (!v) return "";
  if (v->t == JVal::Str) return v->s;
  if (v->t == JVal::Num) return Fmt("%.0f", v->n);
  return "";
}

// steamvr_lh's chaperone: the standing play area of the first known universe that has one, a turn by its yaw about
// -Y and its translation turned with it (device.cpp, Device::init_chaperone); identity without one
Pose Chaperone(std::string &said) {
  std::string root = plat::Home() + "/.steam/root", text;
  JVal db, chap;
  if (!ReadText(root + "/config/lighthouse/lighthousedb.json", text) || !JParse(text, db)) {
    said = "no lighthousedb.json: none";
    return {};
  }
  if (!ReadText(root + "/config/chaperone_info.vrchap", text) || !JParse(text, chap)) {
    said = "no chaperone_info.vrchap: none";
    return {};
  }
  const JVal *known = db.get("known_universes"), *unis = chap.get("universes");
  if (!known || known->t != JVal::Arr || !unis || unis->t != JVal::Arr) {
    said = "no universes: none";
    return {};
  }
  for (auto &k : known->a) {
    std::string id = Str(k.get("id"));
    for (auto &u : unis->a) {
      if (Str(u.get("universeID")) != id) continue;
      const JVal *st = u.get("standing");
      if (!st) continue;
      std::vector<double> tr = st->get("translation") ? st->get("translation")->nums() : std::vector<double>{};
      tr.resize(3, 0.0);
      double yaw = st->get("yaw") ? st->get("yaw")->num() : 0;
      Quat q{std::cos(yaw / 2), 0, -std::sin(yaw / 2), 0};  // yaw about (0, -1, 0)
      Pose c{q, ToM3(q) * V3{tr[0], tr[1], tr[2]}};
      said = Fmt("universe %s: yaw %.2f deg, [%.3f, %.3f, %.3f] m", id.c_str(), yaw * kDeg, tr[0], tr[1], tr[2]);
      return c;
    }
  }
  said = "no play area for a known universe: none";
  return {};
}

// libmonado, from the active runtime's manifest (MND_libmonado_path) as libmonado-rs finds it, or WiVRn's / Monado's
// usual places
struct Mnd {
  void *lib = nullptr;
  mnd_root_t *root = nullptr;
#define MND_FN(name) decltype(&::name) name = nullptr;
  MND_FN(mnd_root_create)
  MND_FN(mnd_root_destroy)
  MND_FN(mnd_root_update_client_list)
  MND_FN(mnd_root_get_device_count)
  MND_FN(mnd_root_get_device_info_string)
  MND_FN(mnd_root_get_device_info_u32)
  MND_FN(mnd_root_get_device_from_role)
  MND_FN(mnd_root_get_reference_space_offset)
  MND_FN(mnd_root_get_tracking_origin_count)
  MND_FN(mnd_root_get_tracking_origin_name)
  MND_FN(mnd_root_get_tracking_origin_offset)
  MND_FN(mnd_root_set_tracking_origin_offset)
#undef MND_FN

  static std::vector<std::string> Candidates() {
    std::vector<std::string> out;
    if (const char *e = getenv("QLHS_LIBMONADO")) out.push_back(e);
    if (const char *e = getenv("LIBMONADO_PATH")) out.push_back(e);
    std::vector<std::string> manifests;
    if (const char *e = getenv("XR_RUNTIME_JSON")) manifests.push_back(e);
    const char *xc = getenv("XDG_CONFIG_HOME");
    manifests.push_back((xc && *xc ? std::string(xc) : plat::Home() + "/.config") + "/openxr/1/active_runtime.json");
    manifests.push_back("/etc/xdg/openxr/1/active_runtime.json");
    for (auto &m : manifests) {
      std::error_code ec;
      std::filesystem::path real = std::filesystem::canonical(m, ec);
      std::string text;
      JVal j;
      if (ec || !ReadText(real.string(), text) || !JParse(text, j)) continue;
      const JVal *rt = j.get("runtime");
      std::string p = rt ? Str(rt->get("MND_libmonado_path")) : "";
      if (p.empty()) continue;
      std::filesystem::path lp = p;
      if (lp.is_relative()) lp = real.parent_path() / lp;
      out.push_back(lp.lexically_normal().string());
    }
    for (const char *p : {"/usr/lib/wivrn/libmonado_wivrn.so", "/usr/lib64/wivrn/libmonado_wivrn.so",
                          "/usr/local/lib/wivrn/libmonado_wivrn.so", "libmonado.so.25", "libmonado.so"})
      out.push_back(p);
    return out;
  }

  bool Load(std::string &why) {
    for (auto &c : Candidates()) {
      lib = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (lib) break;
    }
    if (!lib) { why = "no libmonado"; return false; }
#define MND_FN(name) if (!(name = (decltype(name))dlsym(lib, #name))) { why = "libmonado without " #name; return false; }
    MND_FN(mnd_root_create)
    MND_FN(mnd_root_destroy)
    MND_FN(mnd_root_update_client_list)
    MND_FN(mnd_root_get_device_count)
    MND_FN(mnd_root_get_device_info_string)
    MND_FN(mnd_root_get_device_info_u32)
    MND_FN(mnd_root_get_device_from_role)
    MND_FN(mnd_root_get_reference_space_offset)
    MND_FN(mnd_root_get_tracking_origin_count)
    MND_FN(mnd_root_get_tracking_origin_name)
    MND_FN(mnd_root_get_tracking_origin_offset)
    MND_FN(mnd_root_set_tracking_origin_offset)
#undef MND_FN
    return true;
  }
  bool Connect() { return lib && (root || mnd_root_create(&root) == MND_SUCCESS); }
  void Disconnect() {
    if (root) mnd_root_destroy(&root);
    root = nullptr;
  }
};

struct Xr {
  XrInstance inst = XR_NULL_HANDLE;
  XrSystemId sys = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace stage = XR_NULL_HANDLE;
  XrXDevListMNDX list = XR_NULL_HANDLE;
  uint64_t gen = ~0ull;
  std::map<std::string, XrSpace> spaces;  // serial -> its xdev space
  std::map<std::string, std::string> names;
  bool running = false;
  std::string runtime;
  PFN_xrCreateXDevListMNDX createList = nullptr;
  PFN_xrGetXDevListGenerationNumberMNDX listGen = nullptr;
  PFN_xrEnumerateXDevsMNDX enumerate = nullptr;
  PFN_xrGetXDevPropertiesMNDX props = nullptr;
  PFN_xrDestroyXDevListMNDX destroyList = nullptr;
  PFN_xrCreateXDevSpaceMNDX createSpace = nullptr;
  PFN_xrConvertTimespecTimeToTimeKHR toTime = nullptr;

  template <class F>
  bool Fn(const char *name, F &f) {
    return xrGetInstanceProcAddr(inst, name, (PFN_xrVoidFunction *)&f) == XR_SUCCESS && f;
  }

  bool Create(std::string &why) {
    uint32_t n = 0;
    if (xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr) != XR_SUCCESS) {
      why = "no OpenXR runtime";
      return false;
    }
    std::vector<XrExtensionProperties> ext(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, ext.data());
    auto has = [&](const char *e) {
      for (auto &x : ext)
        if (!strcmp(x.extensionName, e)) return true;
      return false;
    };
    const char *need[] = {XR_MND_HEADLESS_EXTENSION_NAME, XR_MNDX_XDEV_SPACE_EXTENSION_NAME,
                          XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME};
    for (const char *e : need)
      if (!has(e)) { why = std::string("the OpenXR runtime has no ") + e + " (not Monado or WiVRn?)"; return false; }
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "QuestLHSync");
    strcpy(ci.applicationInfo.engineName, "QuestLHSync");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = 3;
    ci.enabledExtensionNames = need;
    if (xrCreateInstance(&ci, &inst) != XR_SUCCESS) { why = "OpenXR instance refused"; inst = XR_NULL_HANDLE; return false; }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (xrGetInstanceProperties(inst, &ip) == XR_SUCCESS) {
      // "WiVRn" or "Monado": the streamer's name, as the pose timing learned per streamer is kept under it
      std::string n = ip.runtimeName, l = n;
      for (char &c : l) c = (char)tolower((unsigned char)c);
      runtime = l.find("wivrn") != std::string::npos ? "WiVRn" : l.find("monado") != std::string::npos ? "Monado"
                                                                                                       : n.substr(0, n.find(' '));
    }
    if (!Fn("xrCreateXDevListMNDX", createList) || !Fn("xrGetXDevListGenerationNumberMNDX", listGen) ||
        !Fn("xrEnumerateXDevsMNDX", enumerate) || !Fn("xrGetXDevPropertiesMNDX", props) ||
        !Fn("xrDestroyXDevListMNDX", destroyList) || !Fn("xrCreateXDevSpaceMNDX", createSpace) ||
        !Fn("xrConvertTimespecTimeToTimeKHR", toTime)) {
      why = "the OpenXR runtime's extension functions are missing";
      return false;
    }
    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (xrGetSystem(inst, &sg, &sys) != XR_SUCCESS) { why = "no headset in the OpenXR runtime"; return false; }
    XrSessionCreateInfo sc{XR_TYPE_SESSION_CREATE_INFO};  // headless: no graphics
    sc.systemId = sys;
    if (xrCreateSession(inst, &sc, &session) != XR_SUCCESS) { why = "OpenXR session refused"; session = XR_NULL_HANDLE; return false; }
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    rs.poseInReferenceSpace.orientation.w = 1;
    if (xrCreateReferenceSpace(session, &rs, &stage) != XR_SUCCESS) { why = "no STAGE space"; return false; }
    return true;
  }

  void Destroy() {
    DropDevices();
    if (stage) xrDestroySpace(stage);
    if (session) xrDestroySession(session);  // ends it too, whatever its state
    if (inst) xrDestroyInstance(inst);
    *this = Xr{};
  }

  void DropDevices() {
    for (auto &kv : spaces) xrDestroySpace(kv.second);
    spaces.clear();
    names.clear();
    if (list) destroyList(list);
    list = XR_NULL_HANDLE;
    gen = ~0ull;
  }

  // the devices' spaces, again when Monado's device list changes
  void Devices() {
    if (!list) {
      XrCreateXDevListInfoMNDX li{(XrStructureType)XR_TYPE_CREATE_XDEV_LIST_INFO_MNDX};
      if (createList(session, &li, &list) != XR_SUCCESS) { list = XR_NULL_HANDLE; return; }
    }
    uint64_t g = 0;
    if (listGen(list, &g) != XR_SUCCESS) return;
    if (g == gen) return;
    DropDevices();
    XrCreateXDevListInfoMNDX li{(XrStructureType)XR_TYPE_CREATE_XDEV_LIST_INFO_MNDX};
    if (createList(session, &li, &list) != XR_SUCCESS) { list = XR_NULL_HANDLE; return; }
    listGen(list, &gen);
    uint32_t n = 0;
    enumerate(list, 0, &n, nullptr);
    std::vector<XrXDevIdMNDX> ids(n);
    enumerate(list, n, &n, ids.data());
    for (auto id : ids) {
      XrGetXDevInfoMNDX gi{(XrStructureType)XR_TYPE_GET_XDEV_INFO_MNDX};
      gi.id = id;
      XrXDevPropertiesMNDX pr{(XrStructureType)XR_TYPE_XDEV_PROPERTIES_MNDX};
      if (props(list, &gi, &pr) != XR_SUCCESS || !pr.canCreateSpace || spaces.count(pr.serial)) continue;
      XrCreateXDevSpaceInfoMNDX si{(XrStructureType)XR_TYPE_CREATE_XDEV_SPACE_INFO_MNDX};
      si.xdevList = list;
      si.id = id;
      si.offset.orientation.w = 1;
      XrSpace sp;
      if (createSpace(session, &si, &sp) != XR_SUCCESS) continue;
      spaces[pr.serial] = sp;
      names[pr.serial] = pr.name;
    }
  }

  XrTime Time(double mono) {
    timespec ts;
    ts.tv_sec = (time_t)mono;
    ts.tv_nsec = (long)((mono - (double)ts.tv_sec) * 1e9);
    XrTime t = 0;
    return toTime(inst, &ts, &t) == XR_SUCCESS ? t : 0;
  }

  // a device's pose in the STAGE space at a CLOCK_MONOTONIC time: false unless tracked
  bool Locate(const std::string &serial, double mono, Pose &out) {
    auto it = spaces.find(serial);
    if (it == spaces.end()) return false;
    XrTime t = Time(mono);
    if (!t) return false;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (xrLocateSpace(it->second, stage, t, &loc) != XR_SUCCESS) return false;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                      XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    if ((loc.locationFlags & need) != need) return false;
    out = FromXr(loc.pose);
    return std::isfinite(out.t.x + out.t.y + out.t.z);
  }

  // false: the session is over (the runtime went away)
  bool Events() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    for (;;) {
      ev = {XR_TYPE_EVENT_DATA_BUFFER};
      XrResult r = xrPollEvent(inst, &ev);
      if (r == XR_EVENT_UNAVAILABLE) return true;
      if (r != XR_SUCCESS) return false;
      if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) return false;
      if (ev.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
      auto &sc = *(XrEventDataSessionStateChanged *)&ev;
      switch (sc.state) {
        case XR_SESSION_STATE_READY: {
          XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
          bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          running = xrBeginSession(session, &bi) == XR_SUCCESS;
          break;
        }
        case XR_SESSION_STATE_STOPPING:
          xrEndSession(session);
          running = false;
          break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING: return false;
        default: break;
      }
    }
  }
};

double Mono() { return QpcNow(); }

// the runtime's IPC socket is there (WiVRn's, Monado's): libmonado and OpenXR are tried only then, as each failed try
// prints a page of errors
bool RuntimeUp() {
  if (getenv("QLHS_LIBMONADO")) return true;
  const char *rt = getenv("XDG_RUNTIME_DIR");
  std::string d = rt && *rt ? rt : "/tmp";
  std::error_code ec;
  for (const char *s : {"/wivrn/comp_ipc", "/monado_comp_ipc"})
    if (std::filesystem::exists(d + s, ec)) return true;
  return false;
}

}  // namespace

void MonadoLink::Start() {
  if (run_) return;
  run_ = true;
  th_ = std::thread(&MonadoLink::Loop, this);
}

void MonadoLink::Stop() {
  run_ = false;
  if (th_.joinable()) th_.join();
}

void MonadoLink::SetTransform(const Transform &x) {
  std::lock_guard<std::mutex> g(m_);
  if (x.active == want_.active && (!x.active || (QuatDeg(x.q, want_.q) < 1e-5 && norm(x.t - want_.t) < 1e-6))) return;
  want_ = x;
  want_new_ = true;
}

void MonadoLink::Loop() {
  Mnd mnd;
  std::string why;
  bool loaded = mnd.Load(why);
  if (!loaded) log_("monado: " + why + " (is WiVRn or Monado installed?): the alignment can't be applied");
  std::string said;
  Xr xr;
  Pose chap;
  double next_try = 0, next_refresh = 0, next_pose = 0, next_assert = 0;
  Pose stage;
  std::string head_serial;
  bool was_up = false, off_set = false;
  bool have_applied = false;

  auto down = [&](const char *what) {
    if (was_up) log_(std::string("monado: ") + what);
    xr.Destroy();
    mnd.Disconnect();
    state_ = kNoRuntime;
    origin_ = -1;
    {
      std::lock_guard<std::mutex> g(m_);
      head_ = Head{};
      want_new_ = true;
    }
    was_up = false;
    have_applied = false;
  };

  while (run_) {
    double now = Mono();
    if (state_ == kNoRuntime) {
      if (now < next_try) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
      next_try = now + 3;
      const char *wait = !loaded ? nullptr : !RuntimeUp() ? "waiting for WiVRn or Monado to start"
                         : !mnd.Connect() ? "the runtime doesn't take clients yet (WiVRn: until a headset connects)"
                                          : "";
      if (!wait || *wait) {
        if (wait && said != wait) log_(std::string("monado: ") + wait);
        if (wait) said = wait;
        continue;
      }
      if (!xr.Create(why)) {
        if (why == "OpenXR instance refused") why = "the runtime doesn't take clients yet (WiVRn: until a headset connects)";
        if (why != said) log_("monado: " + why);
        said = why;
        xr.Destroy();
        mnd.Disconnect();
        continue;
      }
      said.clear();
      std::string cs;
      chap = Chaperone(cs);
      {
        std::lock_guard<std::mutex> g(m_);
        runtime_ = xr.runtime;
      }
      log_("monado: connected to " + xr.runtime + "; lighthouse play area (chaperone) " + cs);
      state_ = kConnected;
      was_up = true;
      next_refresh = next_pose = 0;
    }

    if (!xr.Events()) { down("the runtime went away"); continue; }

    if (now >= next_refresh) {
      next_refresh = now + kRefresh;
      mnd_pose_t sp;
      if (mnd.mnd_root_get_reference_space_offset(mnd.root, MND_SPACE_REFERENCE_TYPE_STAGE, &sp) != MND_SUCCESS) {
        down("libmonado lost");
        continue;
      }
      stage = FromMnd(sp);
      if (xr.running) xr.Devices();
      // the lighthouse tracking origin, by its name (steamvr_lh's), else a lighthouse device's
      mnd.mnd_root_update_client_list(mnd.root);
      uint32_t no = 0;
      int origin = -1;
      mnd.mnd_root_get_tracking_origin_count(mnd.root, &no);
      for (uint32_t i = 0; i < no && origin < 0; i++) {
        const char *name = nullptr;
        if (mnd.mnd_root_get_tracking_origin_name(mnd.root, i, &name) == MND_SUCCESS && name &&
            strstr(name, "Lighthouse"))
          origin = (int)i;
      }
      uint32_t nd = 0;
      mnd.mnd_root_get_device_count(mnd.root, &nd);
      Head h;
      int32_t head_idx = -1;
      mnd.mnd_root_get_device_from_role(mnd.root, "head", &head_idx);
      for (uint32_t i = 0; i < nd; i++) {
        const char *serial = nullptr, *name = nullptr;
        uint32_t o = 0;
        mnd.mnd_root_get_device_info_string(mnd.root, i, MND_PROPERTY_SERIAL_STRING, &serial);
        mnd.mnd_root_get_device_info_string(mnd.root, i, MND_PROPERTY_NAME_STRING, &name);
        mnd.mnd_root_get_device_info_u32(mnd.root, i, MND_PROPERTY_TRACKING_ORIGIN_U32, &o);
        if (origin < 0 && serial && !strncmp(serial, "LHR-", 4)) origin = (int)o;
        if ((int32_t)i == head_idx) {
          h.present = true;
          h.serial = serial ? serial : "";
          h.name = name ? name : "";
          h.lighthouse = origin >= 0 && (int)o == origin;
        }
      }
      if (h.present && h.lighthouse == false && origin >= 0) {
        // the origin may only be known from a device listed after the headset
        for (uint32_t i = 0; i < nd; i++) {
          uint32_t o = 0;
          if ((int32_t)i == head_idx && mnd.mnd_root_get_device_info_u32(mnd.root, i, MND_PROPERTY_TRACKING_ORIGIN_U32,
                                                                          &o) == MND_SUCCESS)
            h.lighthouse = (int)o == origin;
        }
      }
      if (origin != origin_) {
        if (origin >= 0) log_(Fmt("monado: lighthouse tracking origin %d", origin));
        origin_ = origin;
        have_applied = false;
      }
      head_serial = h.serial;
      std::lock_guard<std::mutex> g(m_);
      head_ = h;
    }

    // the headset's pose, a little behind now: what Monado's history holds rather than its prediction
    if (xr.running && !head_serial.empty() && now >= next_pose) {
      next_pose = now + kStep;
      double t = now - kLag;
      Pose p;
      if (xr.Locate(head_serial, t, p)) {
        Pose w = Mul(stage, p);
        hmd_(t, w.q, w.t);
        if (last_pose_ == 0 || now - last_pose_ > 5)
          log_(Fmt("monado: headset poses from %s (at [%.2f, %.2f, %.2f] m)", head_serial.c_str(), w.t.x, w.t.y, w.t.z));
        last_pose_ = now;
      }
    }

    // the alignment, as the lighthouse origin's offset: X * chaperone^-1, none while there's no alignment
    int origin = origin_;
    if (origin >= 0) {
      Transform want;
      bool fresh;
      {
        std::lock_guard<std::mutex> g(m_);
        want = want_;
        fresh = want_new_;
        want_new_ = false;
      }
      bool off = off_;
      if (off && off_set) fresh = false;
      if (fresh || now >= next_assert || !have_applied) {
        next_assert = now + kReassert;
        Pose o;
        if (want.active && !off) o = Mul(Pose{want.q, want.t}, Inv(chap));
        mnd_pose_t mp = ToMnd(o);
        if (mnd.mnd_root_set_tracking_origin_offset(mnd.root, (uint32_t)origin, &mp) == MND_SUCCESS) {
          have_applied = true;
          off_set = off;
        }
      }
    }

    double wait = std::max(0.0, std::min(next_pose, next_refresh) - Mono());
    std::this_thread::sleep_for(std::chrono::microseconds((long)(std::min(wait, 0.004) * 1e6)));
  }
  if (state_ == kConnected) xr.Destroy();
  mnd.Disconnect();
}
