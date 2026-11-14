// QuestLHSync on Linux: the lighthouse shim (inside wivrn-server / monado-service, as its lighthouse driver) <-> the
// questlhsync app, through shared memory /dev/shm/questlhsync-bridge-<uid>.
//
// Monado's SteamVR lighthouse driver drops base stations and keeps the raw lighthouse poses to itself, so the shim
// wraps the driver's host: it adopts the base stations (their poses and channels come here), and passes every
// controller's and tracker's raw pose (SteamVR's lighthouse universe: WorldFromDriver * pose * DriverFromHead) with its
// time (CLOCK_MONOTONIC s, Monado's clock) through a ring. The app solves the alignment and applies it as the
// lighthouse tracking origin's offset (libmonado, as motoc does); the shim hides the devices while they're not placed
// yet (hide), and averages SteamVR's base station relationships (relations.h) when the app says so (steady).
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

#include <unistd.h>

#define QLHS_BRIDGE_MAGIC 0x42484C51u  // "QLHB"
#define QLHS_BRIDGE_VERSION 1

namespace bridge {

constexpr int kMaxDev = 64;
constexpr int kRing = 1 << 14;  // body poses: ~16 s of 4 devices at 250 Hz
constexpr int kLogs = 32;

enum Cls : int32_t { kNone = 0, kHmd = 1, kController = 2, kTracker = 3, kStation = 4 };

struct Dev {
  std::atomic<uint32_t> seq;  // seqlock over the rest: odd while writing
  int32_t cls;                // Cls
  int32_t channel;            // a 2.0 base station's channel 1..16 (its mode label), 0 unknown
  char serial[32];
  char model[64];
  char receiver[32];          // a controller's or tracker's wireless receiver (Prop_ConnectedWirelessDongle_String)
  // a base station's newest valid pose (raw lighthouse universe)
  int32_t has_pose, pad;
  double p[3], q[4];          // q: w x y z
};

struct Pose {
  std::atomic<uint64_t> seq;  // the ring index written + 1, after the rest: 0 or other while being written
  int32_t dev, pad;
  double t;                   // CLOCK_MONOTONIC s (now + poseTimeOffset, as Monado times it)
  double p[3], q[4];          // raw lighthouse universe, q: w x y z
};

struct Shm {
  uint32_t magic, version;
  // shim -> app
  std::atomic<int32_t> shim_pid;
  std::atomic<double> shim_beat;     // CLOCK_MONOTONIC s, every second while the lighthouse driver runs
  std::atomic<int32_t> wired;        // the lighthouse driver has an HMD (Index, Vive, ...); wired only if it's the
                                     // runtime's head (the app tells, from libmonado)
  std::atomic<int32_t> dev_gen;      // bumped when a device appears or changes
  std::atomic<int32_t> rel_state;    // 0 not yet, 1 averaging, 2 not available (logged)
  Dev dev[kMaxDev];
  std::atomic<uint64_t> ring_w;      // poses written
  Pose ring[kRing];
  std::atomic<uint32_t> log_w;       // shim log lines written; log[i % kLogs]
  char log[kLogs][200];
  // app -> shim
  std::atomic<int32_t> app_pid;
  std::atomic<double> app_beat;      // CLOCK_MONOTONIC s, while the app runs: a stale one and the shim hides nothing
  // 1: not placed while the headset is about to place them: controllers and trackers hidden (kHideFor s at most,
  // from the first one), 2: placed (the next 1 starts over), 0: neither
  std::atomic<int32_t> hide;
  std::atomic<int32_t> steady;       // average SteamVR's base station relationships (steadyStations)
  std::atomic<int32_t> off;          // a wired headset, or the app's enable is false: the shim passes everything on
};

inline std::string Path() { return "/questlhsync-bridge-" + std::to_string((unsigned)getuid()); }

}  // namespace bridge
