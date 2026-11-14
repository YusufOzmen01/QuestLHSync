// QuestLHSync's side of Monado / WiVRn: what SteamVR's driver host gives the Windows driver.
//   - the headset's poses, with their times: an OpenXR session (XR_MND_headless) locates its device
//     (XR_MNDX_xdev_space) every few ms, in Monado's world space (the STAGE space with its offset put back, as motoc
//     does), at a time just behind now (from history rather than prediction)
//   - the alignment applied: the lighthouse tracking origin's offset (libmonado), as motoc applies a calibration.
//     Monado places a lighthouse device at offset * chaperone * raw (steamvr_lh: SteamVR's standing play area, read
//     from chaperone_info.vrchap), so for raw -> headset space X the offset is X * chaperone^-1. The chaperone is
//     read as steamvr_lh reads it (checked against Monado's own placement of a lighthouse headset: exact). Not against
//     its controllers': their spaces are their grip poses, a model's turn and offset away from the raw pose
// Everything runs on its own thread: libmonado and OpenXR are used from it alone.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "../../driver/mathx.h"
#include "../../driver/sync.h"

class MonadoLink {
 public:
  enum State { kNoRuntime, kConnected };
  struct Head {
    bool present = false;
    std::string name, serial;
    bool lighthouse = false;  // tracked by the lighthouse origin (an Index, a Vive): a wired headset
  };
  using PoseFn = std::function<void(double t, const Quat &q, V3 p)>;

  MonadoLink(LogFn log, PoseFn hmd) : log_(std::move(log)), hmd_(std::move(hmd)) {}
  ~MonadoLink() { Stop(); }
  void Start();
  void Stop();
  // the alignment (raw lighthouse -> headset space); inactive: Monado's own place for lighthouse devices
  void SetTransform(const Transform &x);
  void SetOff(bool off) { off_ = off; }  // a wired headset: the offset back to none, and left alone

  State state() const { return state_; }
  std::string runtime() const { std::lock_guard<std::mutex> g(m_); return runtime_; }
  Head head() const { std::lock_guard<std::mutex> g(m_); return head_; }
  bool has_origin() const { return origin_ >= 0; }
  double last_pose() const { return last_pose_; }

 private:
  LogFn log_;
  PoseFn hmd_;
  std::thread th_;
  std::atomic<bool> run_{false}, off_{false};
  std::atomic<State> state_{kNoRuntime};
  std::atomic<int> origin_{-1};
  std::atomic<double> last_pose_{0};
  mutable std::mutex m_;
  std::string runtime_;
  Head head_;
  Transform want_{};
  bool want_new_ = true;

  void Loop();
};
