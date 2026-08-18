/**
 ******************************************************************************
 * Xenia AE : Xbox 360 Emulator Research Project                              *
 ******************************************************************************
 * Copyright 2026 Xenia AE. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_AE_FIX_TOGGLE_H_
#define XENIA_BASE_AE_FIX_TOGGLE_H_

// ============================================================================
// CANARY-AE-ONLY  —  DELETE THIS FILE FOR MAINLINE XENIA AE
// ============================================================================
// Runtime switches for AE-only engine fixes, so a cross-game regression can be
// bisected with `setprop` instead of a 15-minute rebuild per hypothesis.
//
// Why this exists: AE carries fixes upstream does not, they all live in code
// shared by every title, and a fix added for one game has already broken
// another. NFS Carbon reached gameplay on 2026-07-04; the Halo 3 GPU work
// landed 2026-07-10..13; NFS has frozen at its main menu since. Finding which
// of those changes is responsible means running the same scene once per
// candidate, and rebuilding for each one makes that a day's work instead of
// twenty minutes.
//
// Convention:
//   * Every toggle DEFAULTS ON, so an unset property == shipped behaviour and
//     a normal user is never running a different build to the tested one.
//   * Setting the property to "0" or "false" DISABLES that one fix.
//   * Name them `debug.canary.fix_<thing>` and expose them in
//     DebugSettingsActivity so they are discoverable without reading source.
//
// To strip for mainline: delete this header, then delete each
// `if (AeFixEnabled...)` guard while KEEPING the guarded code - the fixes are
// real and stay, it is only the ability to switch them off that is Canary-only.
// ============================================================================

#include <atomic>
#include <chrono>

#include "xenia/base/platform.h"

#if XE_PLATFORM_ANDROID || XE_PLATFORM_AX360E
#include <sys/system_properties.h>

#include <cstdlib>
#include <cstring>
#endif

namespace xe {

// Reads an AE fix toggle. Absent/empty property means ENABLED.
inline bool AeFixEnabled(const char* prop_name) {
#if XE_PLATFORM_ANDROID || XE_PLATFORM_AX360E
  char buf[PROP_VALUE_MAX] = {};
  if (__system_property_get(prop_name, buf) > 0 && buf[0]) {
    return !(buf[0] == '0' || !std::strcmp(buf, "false"));
  }
#endif
  return true;
}

// Experiments default OFF, the mirror image of AeFixEnabled.
//
// Use this for behaviour that is NOT shipped - something being tried out. An
// unset property must mean "behave as released", so a fix defaults ON and an
// experiment defaults OFF; keeping the two helpers separate makes which one a
// call site is means visible at the call site.
inline bool AeExperimentEnabled(const char* prop_name) {
#if XE_PLATFORM_ANDROID || XE_PLATFORM_AX360E
  char buf[PROP_VALUE_MAX] = {};
  if (__system_property_get(prop_name, buf) > 0 && buf[0]) {
    return buf[0] == '1' || !std::strcmp(buf, "true");
  }
#endif
  return false;
}

// Reads a numeric AE diagnostic property (hex with or without "0x", or
// decimal). Returns `fallback` when unset/empty/unparseable. Used by watches
// that need a runtime-supplied guest ADDRESS: the address of a heap object
// is not known until the game has allocated it, so it cannot be baked in at
// JIT-compile time the way the boolean toggles are.
inline uint32_t AeDiagValue(const char* prop_name, uint32_t fallback = 0) {
#if XE_PLATFORM_ANDROID || XE_PLATFORM_AX360E
  char buf[PROP_VALUE_MAX] = {};
  if (__system_property_get(prop_name, buf) > 0 && buf[0]) {
    char* end = nullptr;
    const int base =
        (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) ? 16 : 16;
    const unsigned long v = std::strtoul(buf, &end, base);
    if (end != buf) {
      return static_cast<uint32_t>(v);
    }
  }
#endif
  return fallback;
}

// For hot paths: samples the property once and remembers the answer.
//
// Use this anywhere the check would otherwise run per-draw or per-binding. The
// emulator runs in its own :emu process and each game launch starts a fresh
// one, so a per-process sample still picks up a change made between runs -
// which is what the Debug screen's "restart the game to apply" means.
#define XE_AE_FIX_ENABLED(prop_name)                       \
  ([]() -> bool {                                          \
    static const bool xe_ae_fix_ = xe::AeFixEnabled(prop_name); \
    return xe_ae_fix_;                                     \
  }())

#define XE_AE_EXPERIMENT_ENABLED(prop_name)                            \
  ([]() -> bool {                                                      \
    static const bool xe_ae_exp_ = xe::AeExperimentEnabled(prop_name); \
    return xe_ae_exp_;                                                 \
  }())

// DIAGNOSTIC toggles: default OFF, and re-sampled while the game runs.
//
// The cached forms above are right for BEHAVIOUR switches - a fix must not
// change halfway through a session, or the run means nothing. Diagnostics are
// the opposite: the moment you need them is when a game has already reached a
// state you cannot easily get back to. Caching them meant a stall could only be
// probed by restarting and replaying everything up to it - which for NFS Carbon
// meant driving an entire race again just to read one address.
//
// Re-reads at most ~twice a second, while a cold path still notices quickly.
//
// PERF (2026-08-17): this used to call steady_clock::now() on EVERY
// invocation to decide whether its 500 ms cache had expired - "one clock read
// per call". Profiling Halo 3 showed __kernel_clock_gettime at 74.6% of the
// GPU Commands thread and [vdso] at 22.2% of the WHOLE process: there are ~10
// of these gates inside WriteRegister alone, which runs millions of times per
// frame. Now compares a monotonic epoch bumped by AeDiagTick() instead, so a
// disabled diagnostic costs one relaxed atomic load and a branch.
// Bumped ~2x/second by AeDiagTick() (called from an already-periodic host
// hook). Diagnostic gates compare against it instead of reading the clock.
inline std::atomic<uint32_t>& AeDiagEpoch() {
  static std::atomic<uint32_t> epoch{1};
  return epoch;
}
inline void AeDiagTick() {
  AeDiagEpoch().fetch_add(1, std::memory_order_relaxed);
}

#define XE_AE_DIAG_ENABLED(prop_name)                                       \
  ([]() -> bool {                                                           \
    static std::atomic<uint32_t> xe_diag_epoch_{0};                         \
    static std::atomic<bool> xe_diag_on_{false};                            \
    const uint32_t xe_diag_now_ =                                           \
        xe::AeDiagEpoch().load(std::memory_order_relaxed);                  \
    if (xe_diag_now_ != xe_diag_epoch_.load(std::memory_order_relaxed)) {   \
      xe_diag_epoch_.store(xe_diag_now_, std::memory_order_relaxed);        \
      xe_diag_on_.store(xe::AeExperimentEnabled(prop_name),                 \
                        std::memory_order_relaxed);                         \
    }                                                                       \
    return xe_diag_on_.load(std::memory_order_relaxed);                     \
  }())

}  // namespace xe

#endif  // XENIA_BASE_AE_FIX_TOGGLE_H_
