/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_BASE_TESTRIG_DEBUG_SERVER_H_
#define XENIA_BASE_TESTRIG_DEBUG_SERVER_H_

// TESTRIG(infra): Canary AE only (see docs/TEST_HARNESS.md at the repo root).
// This is the "monitor ports" mechanism - a minimal localhost-only TCP debug
// server so any subsystem can expose live text state for external inspection
// while a game runs. Not present in Xenia AE (main / gpu-backport branches).
//
// Usage from any subsystem:
//   xe::testrig::Expose(xe::testrig::kPortAudio, "audio", [this]() {
//     return FormatDebugSnapshot();
//   });
//
// From a host machine:
//   adb forward tcp:9932 tcp:9932
//   nc 127.0.0.1 9932        # or: telnet 127.0.0.1 9932
// The connection stays open and receives a fresh snapshot every
// kPushIntervalMs, separated by a "---" line.
//
// One connection at a time per port - this is a debug tool, not a production
// server. A new connection simply takes over from the previous one.
//
// TOGGLING (so you can play a game with zero overhead, then flip a subsystem
// on to look inside without reinstalling anything):
//   adb shell setprop debug.canary.testrig.master 1   # enable the harness
//   adb shell setprop debug.canary.testrig.gpu 1      # enable just GPU
//   adb shell setprop debug.canary.testrig.master 0   # back to off (default)
// Any subsystem name matches the string passed to Expose() (gpu, audio, jit,
// mem, kernel, ...).
//
// ★ DEFAULT IS OFF. Nothing set means NO instrumentation runs.
//
// This used to default ON, which meant 18 hot-path GPU probes and 5 TCP debug
// servers were live in every normal play session - and because "debug.*"
// properties do not survive a reboot, turning them off was undone by the next
// restart. Diagnostics must never be the default: a frame-rate measurement has
// to measure emulation, not instrumentation.
//
// "debug.*" properties can be set by `adb shell setprop` on any device without
// root - no special permissions needed.
// A port being "disabled" means: (a) its live-data push loop stops calling the
// snapshot function and instead tells a connected client it's off, and (b) any
// hot-path instrumentation gated with HotPathEnabledCached() (see below) stops
// running - this is what actually removes overhead from the render loop, not
// just the socket.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "xenia/base/logging.h"

namespace xe {
namespace testrig {

// Port assignment - document any new subsystem's port here so they don't
// collide.
//   9931 - GPU (command processor / memexport state)
//   9932 - Audio (AAudio output stream state)
//   9933 - CPU / JIT (arm64 code cache state)
//   9934 - Memory (per-heap page usage)
//   9935 - Kernel / guest threads (live thread listing)
constexpr uint16_t kPortGpu = 9931;
constexpr uint16_t kPortAudio = 9932;
constexpr uint16_t kPortCpu = 9933;
constexpr uint16_t kPortMemory = 9934;
constexpr uint16_t kPortKernel = 9935;

using SnapshotFn = std::function<std::string()>;

namespace internal {

constexpr int kPushIntervalMs = 500;
// How long a hot-path HotPathEnabledCached() check trusts its cached answer
// before re-reading the system property. Keeps toggling responsive (flip a
// property, the render loop notices within this window) without paying a
// property lookup on every single call.
constexpr int64_t kHotPathCacheMs = 250;

inline int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Reads Android system property `prop_name` as a tri-state boolean:
//   "1"/"true"  -> true
//   "0"/"false" -> false
//   unset/other -> default_value
// Unlike PropertyEnabled below (which can only DISABLE a default-on feature),
// this can also ENABLE a default-off one, so experimental engine switches can be
// driven from the Debug menu instead of requiring a rebuild.
// TESTRIG(core): part of the Canary-AE debug module.
inline bool PropertyBool(const char* prop_name, bool default_value) {
  char value[PROP_VALUE_MAX] = {0};
  int len = __system_property_get(prop_name, value);
  if (len <= 0) {
    return default_value;
  }
  const std::string v(value);
  if (v == "1" || v == "true") {
    return true;
  }
  if (v == "0" || v == "false") {
    return false;
  }
  return default_value;
}

// Reads Android system property `prop_name`; "0" or "false" means explicitly
// disabled, anything else (including unset) means `default_value`.
inline bool PropertyEnabled(const std::string& prop_name,
                            bool default_value) {
  char value[PROP_VALUE_MAX] = {0};
  int len = __system_property_get(prop_name.c_str(), value);
  if (len <= 0) {
    return default_value;
  }
  if ((value[0] == '0' && value[1] == '\0') ||
      std::string(value) == "false") {
    return false;
  }
  if ((value[0] == '1' && value[1] == '\0') ||
      std::string(value) == "true") {
    return true;
  }
  return default_value;
}

struct PortServer {
  uint16_t port;
  std::string subsystem_name;
  SnapshotFn snapshot_fn;
  std::atomic<bool> running{false};
};

inline std::mutex& registry_mutex() {
  static std::mutex m;
  return m;
}
inline std::unordered_map<uint16_t, PortServer*>& registry() {
  static std::unordered_map<uint16_t, PortServer*> r;
  return r;
}

inline void ClientLoop(int client_fd, PortServer* server) {
  while (server->running.load(std::memory_order_relaxed)) {
    std::string snapshot;
    if (PropertyEnabled("debug.canary.testrig.master", false) &&
        PropertyEnabled("debug.canary.testrig." + server->subsystem_name,
                        true)) {
      snapshot = server->snapshot_fn ? server->snapshot_fn() : std::string();
    } else {
      snapshot = fmt::format(
          "testrig({}) is DISABLED - re-enable with:\n"
          "  adb shell setprop debug.canary.testrig.{} 1\n"
          "  adb shell setprop debug.canary.testrig.master 1",
          server->subsystem_name, server->subsystem_name);
    }
    snapshot += "\n---\n";
    ssize_t sent =
        send(client_fd, snapshot.data(), snapshot.size(), MSG_NOSIGNAL);
    if (sent < 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPushIntervalMs));
  }
  close(client_fd);
}

inline void AcceptLoop(PortServer* server) {
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    XELOGE("testrig({}): socket() failed", server->subsystem_name);
    return;
  }
  int reuse = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  // Loopback only - never expose the debug port beyond the device itself.
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(server->port);

  if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    XELOGE("testrig({}): bind() to 127.0.0.1:{} failed", server->subsystem_name,
           server->port);
    close(listen_fd);
    return;
  }
  if (listen(listen_fd, 1) != 0) {
    XELOGE("testrig({}): listen() failed", server->subsystem_name);
    close(listen_fd);
    return;
  }

  XELOGI("testrig({}): debug port listening on 127.0.0.1:{}",
         server->subsystem_name, server->port);

  while (server->running.load(std::memory_order_relaxed)) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr),
                           &client_len);
    if (client_fd < 0) {
      continue;
    }
    ClientLoop(client_fd, server);
  }
  close(listen_fd);
}

}  // namespace internal

// Registers `snapshot_fn` to be served as live text on 127.0.0.1:`port`,
// starting the accept thread the first time a given port is exposed. Safe to
// call again for the same port later (e.g. after a stream is recreated) - the
// snapshot function is simply replaced.
inline void Expose(uint16_t port, const std::string& subsystem_name,
                    SnapshotFn snapshot_fn) {
  std::lock_guard<std::mutex> guard(internal::registry_mutex());
  auto it = internal::registry().find(port);
  if (it != internal::registry().end()) {
    it->second->snapshot_fn = std::move(snapshot_fn);
    return;
  }
  auto* server = new internal::PortServer();
  server->port = port;
  server->subsystem_name = subsystem_name;
  server->snapshot_fn = std::move(snapshot_fn);
  server->running.store(true, std::memory_order_relaxed);
  std::thread accept_thread(internal::AcceptLoop, server);
  accept_thread.detach();
  internal::registry()[port] = server;
}

// Uncached enable check - fine to call from anything that already runs at
// most a few times a second (e.g. a port's own push loop). For a real render
// hot path, use HotPathEnabledCached() instead so toggling doesn't cost a
// property lookup on every call.
inline bool IsEnabled(const std::string& subsystem_name) {
  return internal::PropertyEnabled("debug.canary.testrig.master", false) &&
         internal::PropertyEnabled(
             "debug.canary.testrig." + subsystem_name, true);
}

// Cheap hot-path gate: caches the enabled state for kHotPathCacheMs so a tight
// loop (e.g. once per draw call) can check this every iteration for close to
// free (an atomic load plus an infrequent clock comparison - no mutex, no
// property lookup, no allocation on the common path). `cached_enabled` and
// `next_check_ms` should be call-site-local statics (one pair per hot path,
// not shared across subsystems) so different call sites don't invalidate each
// other's cache. Example:
//   static std::atomic<bool> enabled{true};
//   static std::atomic<int64_t> next_check_ms{0};
//   if (xe::testrig::HotPathEnabledCached("gpu", enabled, next_check_ms)) {
//     ++counter_;
//   }
inline bool HotPathEnabledCached(const char* subsystem_name,
                                  std::atomic<bool>& cached_enabled,
                                  std::atomic<int64_t>& next_check_ms) {
  int64_t now_ms = internal::NowMs();
  if (now_ms >= next_check_ms.load(std::memory_order_relaxed)) {
    bool enabled = IsEnabled(subsystem_name);
    cached_enabled.store(enabled, std::memory_order_relaxed);
    next_check_ms.store(now_ms + internal::kHotPathCacheMs,
                        std::memory_order_relaxed);
    return enabled;
  }
  return cached_enabled.load(std::memory_order_relaxed);
}

}  // namespace testrig
}  // namespace xe

#endif  // XENIA_BASE_TESTRIG_DEBUG_SERVER_H_
