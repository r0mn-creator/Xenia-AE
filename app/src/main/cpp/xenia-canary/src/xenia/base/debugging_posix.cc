/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/debugging.h"

#include <csignal>
#include <cstdarg>
#include <fstream>
#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>

#include "xenia/base/string_buffer.h"

namespace xe {
namespace debugging {

bool IsDebuggerAttached() {
  // PERF: this is called from Emulator::ExceptionCallback, i.e. on EVERY host
  // exception - and xenia uses protected-page access violations to track guest
  // writes, so exceptions are a hot path, not a rare event. Opening and parsing
  // /proc/self/status each time put proc_pid_status, num_to_str,
  // seq_put_decimal_ull_width, format_decode and strlen at the top of
  // MAIN_THREAD's profile while Halo 4 sat on a black screen: the thread was
  // spending its time in the kernel formatting a text file instead of
  // servicing faults.
  //
  // Sample it once every 4096 calls instead. A debugger attaching mid-run is a
  // developer action and still gets noticed promptly, while the steady-state
  // cost of the check drops to nothing. Same idiom as XeRefreshLiveLogLevel in
  // base/logging.cc. Xenia's OWN debugger is tracked separately via
  // processor()->is_debugger_attached(), which this does not affect.
  static std::atomic<uint32_t> poll_counter{0};
  static std::atomic<bool> cached_attached{false};
  if ((poll_counter.fetch_add(1, std::memory_order_relaxed) & 0xFFF) != 0) {
    return cached_attached.load(std::memory_order_relaxed);
  }

  std::ifstream proc_status_stream("/proc/self/status");
  if (!proc_status_stream.is_open()) {
    cached_attached.store(false, std::memory_order_relaxed);
    return false;
  }
  std::string line;
  while (std::getline(proc_status_stream, line)) {
    std::istringstream line_stream(line);
    std::string key;
    line_stream >> key;
    if (key == "TracerPid:") {
      uint32_t tracer_pid;
      line_stream >> tracer_pid;
      const bool attached = tracer_pid != 0;
      cached_attached.store(attached, std::memory_order_relaxed);
      return attached;
    }
  }
  cached_attached.store(false, std::memory_order_relaxed);
  return false;
}

void Break() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    // Install handler for sigtrap only once
    std::signal(SIGTRAP, [](int) {
      // Forward signal to default handler after being caught
      std::signal(SIGTRAP, SIG_DFL);
    });
  });
  std::raise(SIGTRAP);
}

namespace internal {
void DebugPrint(const char* s) { std::clog << s << std::endl; }
}  // namespace internal

}  // namespace debugging
}  // namespace xe
