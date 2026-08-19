/**
 ******************************************************************************
 * Xenia-AE : Xbox 360 Emulator Research Project                              *
 ******************************************************************************
 * Self-contained frame-rate counter.                                          *
 ******************************************************************************
 */

#ifndef XENIA_BASE_AE_FPS_H_
#define XENIA_BASE_AE_FPS_H_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#include "xenia/base/ae_fix_toggle.h"
#include "xenia/base/logging.h"

// Frame-rate counter, kept deliberately standalone.
//
// Scope: it owns ALL of its own state, windowing, logging and its own toggle.
// The only thing the rest of the emulator does is call OnFrame() once per
// presented frame - one line at the frame boundary. Nothing else in the GPU
// backend knows this exists, so it can be deleted or ported by moving this one
// file plus that single call site.
//
// Toggle: debug.canary.fps (its own property, independent of every other
//   diagnostic). Off by default. Re-sampled at runtime, so it can be switched
//   on mid-session without restarting the game:
//     adb shell setprop debug.canary.fps 1
//
// Two consumers, one source of truth:
//   * XELOGI("XEFPS <frames> <ms>")  -> scripts/fps_bench.sh aggregates these
//   * Current()                      -> the on-screen counter reads this via
//                                       JNI, so the overlay never touches the
//                                       filesystem to display a number
//
// Why it publishes raw (frames, milliseconds) rather than a pre-divided rate:
// the sampling window is never exactly 1000 ms, and rounding at this end would
// quietly bias the aggregate the benchmark computes.
//
// IMPORTANT for anyone using this to judge an optimisation: scene variation on
// NFS Carbon is 8.9-13.9 FPS. A single reading cannot distinguish a real change
// from noise - this project has already produced one false "10% win" that way.
// Compare distributions with scripts/fps_bench.sh, never two numbers.

namespace xe {
namespace ae {

class FpsCounter {
 public:
  // Call once per presented frame, from the frame boundary (IssueSwap).
  // Costs one increment and one steady_clock read per frame when enabled, and
  // a cached property check when not.
  static void OnFrame() {
    // Drive the diagnostic-toggle epoch from here: once per FRAME, not once
    // per gate check. XE_AE_DIAG_ENABLED used to read the clock on every
    // invocation and there are ~10 of those inside WriteRegister alone, which
    // put __kernel_clock_gettime at 74.6% of the GPU Commands thread. One
    // clock read per frame keeps the same ~500 ms re-sample responsiveness for
    // a millionth of the cost. Must sit ABOVE the gate below, or turning the
    // FPS counter off would freeze every other diagnostic toggle.
    {
      static std::atomic<uint64_t> next_ms{0};
      const uint64_t now_ms =
          uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count());
      if (now_ms >= next_ms.load(std::memory_order_relaxed)) {
        next_ms.store(now_ms + 500, std::memory_order_relaxed);
        xe::AeDiagTick();
      }
    }
    if (!XE_AE_DIAG_ENABLED("debug.canary.fps")) {
      // Reset so that toggling on mid-session starts a clean window rather
      // than reporting a huge frame count against a stale start time.
      if (state().frames) {
        state().frames = 0;
        state().window_start = Clock::now();
        current_fps_x100.store(0, std::memory_order_relaxed);
      }
      return;
    }
    State& s = state();
    ++s.frames;
    auto now = Clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - s.window_start)
                          .count();
    // FRAME TIME, not just FPS.
    //
    // vsync is on and the panel is 60 Hz, so presented FPS can only ever land
    // on 60/n - measured: 83% of samples are exactly 14.9 (60/4) or 19.9
    // (60/3), with almost nothing in between. That makes FPS a QUANTIZED
    // metric and a bad optimisation target: a real 10% frame-time improvement
    // shows as 0% until it crosses a vblank boundary, then jumps 33% at once.
    // Frame time is continuous and shows progress as it happens.
    if (s.last_frame.time_since_epoch().count() != 0) {
      const int64_t dt_us =
          std::chrono::duration_cast<std::chrono::microseconds>(now -
                                                                s.last_frame)
              .count();
      if (dt_us > 0 && s.sample_count < State::kMaxSamples) {
        s.samples[s.sample_count++] = static_cast<uint32_t>(dt_us);
      }
    }
    s.last_frame = now;

    if (elapsed_ms >= kWindowMs) {
      if (s.sample_count) {
        std::sort(s.samples, s.samples + s.sample_count);
        const auto pct = [&s](double q) {
          size_t i = static_cast<size_t>(s.sample_count * q);
          if (i >= s.sample_count) i = s.sample_count - 1;
          return s.samples[i];
        };
        // Microseconds, so a sub-millisecond change is still visible.
        XELOGI("XEFRAME n={} p50={}us p90={}us p99={}us min={}us",
               s.sample_count, pct(0.50), pct(0.90), pct(0.99), s.samples[0]);
        s.sample_count = 0;
      }
      XELOGI("XEFPS {} {}", s.frames, elapsed_ms);
      current_fps_x100.store(
          static_cast<uint32_t>(s.frames * 100000.0 / double(elapsed_ms) + 0.5),
          std::memory_order_relaxed);
      s.frames = 0;
      s.window_start = now;
    }
  }

  // Last published rate, or 0 when the counter is off / no window has closed.
  static float Current() {
    return current_fps_x100.load(std::memory_order_relaxed) / 100.0f;
  }

 private:
  using Clock = std::chrono::steady_clock;
  static constexpr int64_t kWindowMs = 1000;

  struct State {
    uint32_t frames = 0;
    Clock::time_point window_start = Clock::now();
    // Per-frame deltas for this window. At 15-20 FPS a 1 s window holds ~20
    // samples; the cap is generous so an unlocked/fast scene cannot overflow
    // it, and overflow simply stops sampling rather than looping.
    static constexpr size_t kMaxSamples = 512;
    Clock::time_point last_frame{};
    uint32_t samples[kMaxSamples] = {};
    size_t sample_count = 0;
  };

  // Only ever touched from the GPU command thread, so no synchronisation.
  static State& state() {
    static State s;
    return s;
  }

  // Written by the GPU command thread, read by the Android UI thread. Stored as
  // FPS * 100 in a uint32 because integer atomics are lock-free on every ABI
  // while float atomics are not guaranteed to be.
  inline static std::atomic<uint32_t> current_fps_x100{0};
};

}  // namespace ae
}  // namespace xe

#endif  // XENIA_BASE_AE_FPS_H_
