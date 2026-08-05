/**
 ******************************************************************************
 * Xenia-AE : Xbox 360 Emulator Research Project                              *
 ******************************************************************************
 * Symbol map for JIT-compiled guest code, for profilers.                      *
 ******************************************************************************
 */

#ifndef XENIA_BASE_AE_PERF_MAP_H_
#define XENIA_BASE_AE_PERF_MAP_H_

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "xenia/base/ae_fix_toggle.h"
#include "xenia/base/logging.h"

// Why this exists
// ---------------
// A 20 s simpleperf capture during NFS Carbon gameplay showed the two guest
// threads are **57.3% of process CPU** - more than twice the GPU command
// thread's 24.6%. But they profile as **97.3% "unknown"**, because JIT'd code
// lives in anonymous memory with no ELF symbols behind it. So the single
// largest consumer of CPU in the emulator is completely opaque.
//
// That matters for a decision, not just curiosity: reaching 30 FPS from ~10
// requires attacking that 57.3%, and we cannot tell whether it is
//   (a) the a64 backend emitting poor code for hot guest functions, or
//   (b) the game genuinely executing that much work.
// Those need opposite responses. This makes the difference visible.
//
// How it works
// ------------
// The perf-map convention: a profiler reads /tmp/perf-<pid>.map, one line per
// code region:
//
//     <start-hex> <size-hex> <name>
//
// Any sample landing inside a range is attributed to that name. We emit one
// line per JIT-compiled guest function, named by its GUEST address, so a hot
// symbol can be traced straight back to guest code and disassembled.
//
// Toggle: debug.canary.perf_map (default OFF - it writes a file and costs a
// lock + fprintf per compiled function, which is not free during load).
//
//     adb shell setprop debug.canary.perf_map 1
//     # launch the game, then profile as usual
//
// Note the file must be written by the process being profiled, and the
// profiler needs to read it as that pid. Android's /tmp is not the usual
// location, so the path is overridable - see PerfMap::Open.

namespace xe {
namespace ae {

class PerfMap {
 public:
  // Called once per JIT-compiled function. Cheap no-op when disabled.
  static void OnFunctionCompiled(uint32_t guest_address, uint64_t host_address,
                                 size_t code_size, const char* name) {
    if (!XE_AE_DIAG_ENABLED("debug.canary.perf_map")) {
      return;
    }
    Instance().Write(guest_address, host_address, code_size, name);
  }

  static void Close() {
    PerfMap& m = Instance();
    std::lock_guard<std::mutex> lock(m.mutex_);
    if (m.file_) {
      fclose(m.file_);
      m.file_ = nullptr;
      XELOGI("perf map: closed after {} entries", m.count_);
    }
  }

 private:
  static PerfMap& Instance() {
    static PerfMap m;
    return m;
  }

  void Write(uint32_t guest_address, uint64_t host_address, size_t code_size,
             const char* name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_ && !opened_) {
      Open();
    }
    if (!file_) {
      return;
    }
    // Name by GUEST address so a hot symbol maps straight back to guest code.
    // A supplied name (known export/import) is far more useful, so prefer it.
    if (name && *name) {
      fprintf(file_, "%llx %zx guest_%08X_%s\n",
              static_cast<unsigned long long>(host_address), code_size,
              guest_address, name);
    } else {
      fprintf(file_, "%llx %zx guest_%08X\n",
              static_cast<unsigned long long>(host_address), code_size,
              guest_address);
    }
    // Flushed per entry deliberately: a profiling run is usually ended by
    // killing the process, and an unflushed buffer would lose exactly the
    // symbols we came for.
    fflush(file_);
    ++count_;
  }

  void Open() {
    opened_ = true;
    // Canonical location first. On Android /tmp is often absent or not
    // writable by the app, so fall back to a path the app definitely owns;
    // the report script copies it into place.
    const int pid = static_cast<int>(getpid());
    char path[256];
    snprintf(path, sizeof(path), "/tmp/perf-%d.map", pid);
    file_ = fopen(path, "w");
    if (!file_) {
      snprintf(path, sizeof(path), "/data/local/tmp/perf-%d.map", pid);
      file_ = fopen(path, "w");
    }
    if (file_) {
      XELOGI("perf map: writing JIT symbols to {}", path);
    } else {
      XELOGW("perf map: could not open a map file (tried /tmp and "
             "/data/local/tmp) - JIT frames will stay unsymbolized");
    }
  }

  std::mutex mutex_;
  FILE* file_ = nullptr;
  bool opened_ = false;
  uint64_t count_ = 0;
};

}  // namespace ae
}  // namespace xe

#endif  // XENIA_BASE_AE_PERF_MAP_H_
