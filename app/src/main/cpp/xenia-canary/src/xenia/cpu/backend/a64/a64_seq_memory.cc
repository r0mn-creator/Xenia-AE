/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/ae_fix_toggle.h"
#include "xenia/cpu/backend/a64/a64_sequences.h"

#include <atomic>
#include <chrono>
#include <cstddef>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_jit_watch_diag.h"
#include "xenia/cpu/backend/a64/a64_op.h"
#include "xenia/cpu/backend/a64/a64_seq_util.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"

DECLARE_bool(emit_mmio_aware_stores_for_recorded_exception_addresses);
DECLARE_bool(emit_inline_mmio_checks);

DECLARE_bool(guest_scheduler);

DEFINE_bool(a64_park_spin_backoff, true,
            "For collapsed guest spin-backoff loops, spin cheaply for the first "
            "few iterations then park the thread with a short real sleep "
            "(adaptive) instead of a fixed isb sled - reclaims CPU on long "
            "guest spin-waits while short waits (resolved during the cheap "
            "spin) stay latency-unaffected.\n"
            "Only has any effect when collapse_ctr_spin_loops is on.",
            "CPU");

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

volatile int anchor_memory = 0;

static bool IsPossibleMMIOInstruction(A64Emitter& e, const hir::Instr* i) {
  if (!cvars::emit_mmio_aware_stores_for_recorded_exception_addresses) {
    return false;
  }
  uint32_t guest_address = i->GuestAddressFor();
  if (!guest_address) {
    return false;
  }

  auto* guest_module = e.GuestModule();
  if (!guest_module) {
    return false;
  }
  auto* flags = guest_module->GetInstructionAddressFlags(guest_address);
  return flags && flags->accessed_mmio;
}

// ============================================================================
// OPCODE_DELAY_EXECUTION
// ============================================================================
struct DELAY_EXECUTION
    : Sequence<DELAY_EXECUTION, I<OPCODE_DELAY_EXECUTION, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // The guest's "pause in a spin loop" instruction.
    //
    // This used to emit YIELD. Per the ARM Architecture Reference Manual:
    //   "The YIELD instruction is a NOP hint instruction."
    //   "The YIELD instruction has no effect in a single-threaded system."
    // It only does anything on hardware that can actually swap threads on the
    // hint - i.e. SMT. Consumer ARM is SMP without SMT, so on this device YIELD
    // is literally a no-op and the guest's throttle does nothing.
    //
    // That matters here: guest_826DEFD0 is a polling loop and 14% of ALL
    // process CPU, and its pause primitive is eight consecutive YIELDs - eight
    // no-ops. The loop spins at full rate.
    //
    // ISB flushes the pipeline ("a context synchronization event", per the
    // manual), which is a real stall and the replacement RPCS3 adopted for the
    // same problem. It is not a power-saving instruction and ARM notes it costs
    // a little more energy than a proper wait - but it is the closest thing to
    // x86's PAUSE that works without SMT.
    //
    // WFE would be better still (a genuine wait on an event) but needs an SEV
    // from the waker, which the guest does not provide.
    //
    // MEASURED 2026-08-09: emitting ISB here is a 21.5% REGRESSION
    // (NFS Carbon, 180s, 9.67 -> 7.59 FPS, p=0.0000). Reverted to YIELD.
    //
    // Why the "fix" was wrong: this opcode is emitted INSIDE translated guest
    // code, so it runs every time that guest instruction executes - eight times
    // per iteration of guest_826DEFD0's polling loop, millions of times a
    // second. ISB is the most expensive barrier ARM has; eight free no-ops
    // became eight full pipeline flushes.
    //
    // YIELD being architecturally a no-op (per the ARM manual) is NOT a defect
    // here. In a hot inner spin, free is exactly what is wanted. Do not
    // "fix" this again - see docs/ENGINE_CHANGE_LOG.md 2026-08-09.
    e.yield();
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DELAY_EXECUTION, DELAY_EXECUTION);

// Adaptive park helper for OPCODE_SPIN_BACKOFF, called once per outer poll
// iteration of a collapsed guest spin-wait. A young wait spins cheap (a few
// isb); once it proves long it sleeps briefly so the core stops burning
// cycles. A gap since the previous call starts a fresh episode, so an
// unrelated later wait spins cheap again. The sleep timeout guarantees forward
// progress, so no wake plumbing is needed. Never host-blocks under the
// cooperative scheduler - blocking a dispatch thread there can stall the
// sibling fiber that releases the polled word.
static void SpinBackoffParkThunk(void* /*ppc_context*/) {
  static constexpr uint32_t kSpinIters = 24;
  static constexpr int64_t kParkNs = 30000;   // 30us bounded park
  static constexpr int64_t kGapNs = 200000;   // >200us idle -> new episode
  thread_local uint32_t consec = 0;
  thread_local int64_t last_ns = 0;
  if (cvars::guest_scheduler) {
    // Cooperative path: host-parking would stall co-resident fibers and
    // host-spinning never runs the producer, which may be a fiber queued
    // behind this one. Yield instead - ready-tail requeue guarantees
    // co-resident progress.
    if (auto* yield_handler = xe::cpu::backend::spin_backoff_yield_handler) {
      yield_handler(nullptr);
      return;
    }
    // Scheduler enabled but not started yet (early init), or a non-fiber
    // caller: fall back to the cheap spin.
    for (uint32_t n = 0; n < 8; ++n) {
      __asm__ __volatile__("isb sy" ::: "memory");
    }
    return;
  }
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  if (now_ns - last_ns > kGapNs) {
    consec = 0;
  }
  last_ns = now_ns;
  if (++consec < kSpinIters) {
    for (uint32_t n = 0; n < 8; ++n) {
      __asm__ __volatile__("isb sy" ::: "memory");
    }
    return;
  }
  xe::threading::NanoSleep(kParkNs);
}

// ============================================================================
// OPCODE_SPIN_BACKOFF
// ============================================================================
// Bounded host-side wait emitted in place of a proven constant-trip-count guest
// spin-backoff loop. src1.offset is the iteration count, already clamped by the
// pass that emits this op.
//
// The fallback loop is held entirely in w16, an emitter-scratch register (the
// register allocator only hands out x22-x28). It uses sub+cbnz rather than
// subs+b.ne so NZCV is never written - no host state a surrounding sequence
// could observe is disturbed, and there is no guest context or memory traffic.
struct SPIN_BACKOFF
    : Sequence<SPIN_BACKOFF, I<OPCODE_SPIN_BACKOFF, VoidOp, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const uint32_t count = static_cast<uint32_t>(i.src1.value);
    if (!count) {
      return;
    }
    if (cvars::a64_park_spin_backoff) {
      // Adaptive spin-then-park: cheap for short waits, a real short sleep for
      // long ones. CallNativeSafe preserves guest context across the
      // (possibly sleeping) helper.
      //
      // Under the cooperative scheduler the helper only yields the fiber, a
      // no-op unless something else is runnable, but the call is a full
      // guest->host thunk plus a thread_local lookup. Gate it on the
      // scheduler's own give-way flag so the common case is two instructions.
      if (cvars::guest_scheduler) {
        static_assert(offsetof(ppc::PPCContext, preempt_requested) < 4096);
        auto& skip = e.NewCachedLabel();
        e.ldrb(e.w16,
               Xbyak_aarch64::ptr(e.GetContextReg(),
                                  static_cast<uint32_t>(offsetof(
                                      ppc::PPCContext, preempt_requested))));
        e.cbz(e.w16, skip);
        e.CallNativeSafe(reinterpret_cast<void*>(&SpinBackoffParkThunk));
        e.L(skip);
        return;
      }
      e.CallNativeSafe(reinterpret_cast<void*>(&SpinBackoffParkThunk));
      return;
    }
    auto& loop = e.NewCachedLabel();
    e.mov(e.w16, count);
    e.L(loop);
    e.isb(Xbyak_aarch64::SY);
    e.sub(e.w16, e.w16, 1);
    e.cbnz(e.w16, loop);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SPIN_BACKOFF, SPIN_BACKOFF);

// ============================================================================
// OPCODE_MEMORY_BARRIER
// ============================================================================
struct MEMORY_BARRIER
    : Sequence<MEMORY_BARRIER, I<OPCODE_MEMORY_BARRIER, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.dmb(Xbyak_aarch64::ISH);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MEMORY_BARRIER, MEMORY_BARRIER);

// ============================================================================
// OPCODE_CACHE_CONTROL
// ============================================================================
struct CACHE_CONTROL
    : Sequence<CACHE_CONTROL,
               I<OPCODE_CACHE_CONTROL, VoidOp, I64Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    bool is_clflush = false, is_prefetch = false, is_prefetchw = false;
    switch (CacheControlType(i.instr->flags)) {
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_TOUCH:
        is_prefetch = true;
        break;
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_TOUCH_FOR_STORE:
        is_prefetchw = true;
        break;
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_STORE:
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_STORE_AND_FLUSH:
        is_clflush = true;
        break;
      default:
        return;
    }
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.add(e.x0, e.GetMembaseReg(), addr);
    size_t cache_line_size = i.src2.value;
    if (is_clflush) {
      // dc civac, x0
      e.sys(0b011, 0b0111, 0b1110, 0b001, e.x0);
    }
    if (is_prefetch) {
      e.prfm(Xbyak_aarch64::PLDL1KEEP, ptr(e.x0));
    } else if (is_prefetchw) {
      e.prfm(Xbyak_aarch64::PSTL1KEEP, ptr(e.x0));
    }
    if (cache_line_size >= 128) {
      e.eor(e.x0, e.x0, 64);
      if (is_clflush) {
        // dc civac, x0
        e.sys(0b011, 0b0111, 0b1110, 0b001, e.x0);
      }
      if (is_prefetch) {
        e.prfm(Xbyak_aarch64::PLDL1KEEP, ptr(e.x0));
      } else if (is_prefetchw) {
        e.prfm(Xbyak_aarch64::PSTL1KEEP, ptr(e.x0));
      }
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CACHE_CONTROL, CACHE_CONTROL);

// DIAG(gpu/camera): see a64_jit_watch_diag.h for the full rationale. Struct
// and ring buffer are written directly by JIT'd code (STORE_I32::Emit
// below), no function call - only x0-x18 touched, never allocated to guest
// values (a64_backend.cc: "GPR set: x22-x28"), so this cannot corrupt a
// live guest register even while active.
// Definitions for the ring declared in a64_jit_watch_diag.h.
AeJitWatchEntry g_ae_jit_watch_ring[kAeJitWatchRingSize];
std::atomic<uint32_t> g_ae_jit_watch_ring_index{0};
std::atomic<uint32_t> g_ae_jit_watch_addr{0};
uint32_t g_ae_jit_watch_ring_dumped = 0;

// Defined in a64_seq_vector.cc - records a vector store whose aligned
// 16-byte line covers the watched address, with all four lanes.
void EmitAeJitVecWatch(A64Emitter& e, uint32_t addr_reg_idx,
                       uint32_t src_vreg_idx, uint32_t kind);

void DumpAeJitStoreWatch() {
  // Refresh the exact-address target from its property. Rate-limited the
  // same way XE_AE_DIAG_ENABLED samples its own toggle (500 ms), because
  // this runs from WriteRegister, which is extremely hot.
  {
    static std::atomic<uint64_t> next_ms{0};
    const uint64_t now_ms =
        uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count());
    if (now_ms >= next_ms.load(std::memory_order_relaxed)) {
      next_ms.store(now_ms + 500, std::memory_order_relaxed);
      g_ae_jit_watch_addr.store(
          xe::AeDiagValue("debug.canary.jit_watch_addr"),
          std::memory_order_relaxed);
    }
  }
  uint32_t written = g_ae_jit_watch_ring_index.load(std::memory_order_acquire);
  if (written <= g_ae_jit_watch_ring_dumped) {
    return;
  }
  // If the ring wrapped more than once between polls, only the last
  // kAeJitWatchRingSize entries are still valid - skip ahead to those.
  uint32_t start = written > kAeJitWatchRingSize
                       ? written - kAeJitWatchRingSize
                       : g_ae_jit_watch_ring_dumped;
  for (uint32_t seq = start; seq < written; ++seq) {
    const AeJitWatchEntry& entry =
        g_ae_jit_watch_ring[seq % kAeJitWatchRingSize];
    const char* kind_name = entry.kind == 0   ? "STORE"
                            : entry.kind == 1 ? "LOAD "
                            : entry.kind == 2 ? "STVLX"
                            : entry.kind == 3 ? "STVRX"
                                              : entry.kind == 4 ? "STV128"
                            : entry.kind == 5 ? "STR64"
                            : entry.kind == 6 ? "STRF32"
                                              : "STROFF";
    XELOGI(
        "JITWATCH {} seq={} guest_addr=0x{:08X} value=0x{:08X} "
        "guest_lr=0x{:08X} caller=0x{:08X} grandcaller=0x{:08X} "
        "ctx328=0x{:016X} ctx568=0x{:016X} r24=0x{:016X} r26=0x{:016X} "
        "r28=0x{:016X} lanes=[0x{:08X},0x{:08X},0x{:08X},0x{:08X}]",
        kind_name, seq, entry.guest_addr, entry.value,
        entry.guest_lr, entry.caller_guest_addr, entry.grandcaller_guest_addr,
        entry.arg_ctx328, entry.result_ctx568, entry.r24, entry.r26,
        entry.r28, entry.lane[0], entry.lane[1], entry.lane[2],
        entry.lane[3]);
  }
  g_ae_jit_watch_ring_dumped = written;
}

// DIAG(gpu/camera): shared emit body for the store watch (46.1) and the
// load watch (51). Both need byte-identical filtering and capture, and
// duplicating ~60 lines of hand-written codegen twice is exactly how the
// two would silently drift apart and make a comparison meaningless.
//
// Registers: only x0-x2, w9-w16 - all inside the a64 backend's reserved
// scratch range (x0-x18), never allocated to guest values
// (a64_backend.cc: "GPR set: x22-x28"), so this is safe to drop in front
// of any real load/store without disturbing it. addr_reg_idx and
// value_reg_idx may name any register including a guest one; they are
// only ever READ.
static void EmitAeJitWatchBody(A64Emitter& e, uint32_t addr_reg_idx,
                               uint32_t value_reg_idx, uint32_t kind,
                               bool exact_addr) {
  auto& watch_skip = e.NewCachedLabel();
  if (exact_addr) {
    // Exact-address mode (51): match one runtime-supplied guest address and
    // record whatever is written to it, sign included. Unlike the value
    // filter below this can report a CORRECT value, which is what makes it
    // able to answer "is this field ever positive here".
    e.mov(e.x1, reinterpret_cast<uint64_t>(&g_ae_jit_watch_addr));
    e.ldr(e.w11, ptr(e.x1));
    e.cmp(WReg(addr_reg_idx), e.w11);
    e.b(Xbyak_aarch64::NE, watch_skip);
    e.mov(e.w16, WReg(value_reg_idx));
  } else {
    // Address floor first - see 46.1: unfiltered, a 0x400F41xx math-library
    // cluster produced 594997 hits in one 50s run. Everything this
    // investigation cares about is in the physical-alias range.
    e.mov(e.w11, 0xA0000000u);
    e.cmp(WReg(addr_reg_idx), e.w11);
    e.b(Xbyak_aarch64::LO, watch_skip);
    e.mov(e.w16, WReg(value_reg_idx));
    // exponent byte == 126 (magnitude in [0.5, 1.0)) and sign set - matches
    // every mirrored camera value sampled so far (0xBF7E5FB4, 0xBF268EE8,
    // 0xBF2979F4). NOTE (47): this also matches the constant -0.5 exactly,
    // which is written extremely often by an unrelated helper - rank
    // candidates by VALUE VARIANCE, never by hit count. NOTE (51): this
    // filter can only ever report negatives - see g_ae_jit_watch_addr.
    e.lsr(e.w15, e.w16, 23);
    e.and_(e.w15, e.w15, 0xFF);
    e.cmp(e.w15, 126);
    e.b(Xbyak_aarch64::NE, watch_skip);
    e.tbz(e.w16, 31, watch_skip);
  }
  // Match - append to the ring buffer. Non-atomic index increment: a lost
  // entry under contention is acceptable for a diagnostic, an extra
  // load/store/branch in the hot path is not.
  e.mov(e.x1, reinterpret_cast<uint64_t>(&g_ae_jit_watch_ring_index));
  e.ldr(e.w13, ptr(e.x1));
  e.add(e.w14, e.w13, 1);
  e.str(e.w14, ptr(e.x1));
  e.and_(e.w13, e.w13, kAeJitWatchRingSize - 1);
  e.mov(e.x2, reinterpret_cast<uint64_t>(&g_ae_jit_watch_ring[0]));
  e.mov(e.w14, static_cast<uint32_t>(sizeof(AeJitWatchEntry)));
  e.umull(e.x12, e.w13, e.w14);
  e.add(e.x2, e.x2, e.x12);
  e.str(WReg(addr_reg_idx),
        ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry,
                                                 guest_addr))));
  e.str(e.w16,
        ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry, value))));
  e.mov(e.w14, kind);
  e.str(e.w14,
        ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry, kind))));
  // Scalar kinds carry no lanes - zero them so the log can't be misread.
  for (uint32_t li = 0; li < 4; ++li) {
    e.str(e.wzr, ptr(e.x2, static_cast<uint32_t>(
                               offsetof(AeJitWatchEntry, lane) + li * 4)));
  }
  e.ldr(e.w15,
        ptr(e.x20, static_cast<uint32_t>(offsetof(ppc::PPCContext, lr))));
  e.str(e.w15,
        ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry, guest_lr))));
  // ctx328/ctx568: 49.4's node-to-node data channel. 50.1 measured
  // ctx328 as a constant 4.0 (a fixed handler selector, NOT camera data)
  // - kept because it cheaply confirms which dispatch slot is running.
  e.ldr(e.x13, ptr(e.x20, 328));
  e.str(e.x13,
        ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry,
                                                 arg_ctx328))));
  e.ldr(e.x13, ptr(e.x20, 568));
  e.str(e.x13,
        ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry,
                                                 result_ctx568))));
  e.ldr(e.x13, ptr(e.x20, 224));
  e.str(e.x13,
        ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry, r24))));
  e.ldr(e.x13, ptr(e.x20, 240));
  e.str(e.x13,
        ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry, r26))));
  e.ldr(e.x13, ptr(e.x20, 256));
  e.str(e.x13,
        ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry, r28))));
  // caller/grandcaller: A64BackendStackpoint - x19's own fields, offsets
  // confirmed against a64_backend.h (152=stackpoints,
  // 172=current_stackpoint_depth), the same struct
  // A64Backend::PopulatePseudoStacktrace reads from host C++. depth-1 is
  // the innermost pushed frame, depth-2 its caller. guest_lr alone is
  // usually stale by the time a load/store deep in a function fires.
  e.ldr(e.w9, ptr(e.x19, 172));
  auto& no_caller = e.NewCachedLabel();
  auto& no_grandcaller = e.NewCachedLabel();
  auto& grandcaller_done = e.NewCachedLabel();
  e.cbz(e.w9, no_caller);
  e.mov(e.w11, static_cast<uint32_t>(sizeof(A64BackendStackpoint)));
  e.sub(e.w9, e.w9, 1);
  e.ldr(e.x10, ptr(e.x19, 152));
  e.umull(e.x12, e.w9, e.w11);
  e.add(e.x12, e.x10, e.x12);
  e.ldr(e.w14,
        ptr(e.x12, static_cast<uint32_t>(offsetof(
                       A64BackendStackpoint, guest_return_address_))));
  e.str(e.w14, ptr(e.x2, static_cast<uint32_t>(offsetof(
                             AeJitWatchEntry, caller_guest_addr))));
  e.cbz(e.w9, no_grandcaller);
  e.sub(e.w9, e.w9, 1);
  e.umull(e.x12, e.w9, e.w11);
  e.add(e.x12, e.x10, e.x12);
  e.ldr(e.w14,
        ptr(e.x12, static_cast<uint32_t>(offsetof(
                       A64BackendStackpoint, guest_return_address_))));
  e.str(e.w14, ptr(e.x2, static_cast<uint32_t>(offsetof(
                             AeJitWatchEntry, grandcaller_guest_addr))));
  e.b(grandcaller_done);
  e.L(no_grandcaller);
  e.str(e.wzr, ptr(e.x2, static_cast<uint32_t>(offsetof(
                             AeJitWatchEntry, grandcaller_guest_addr))));
  e.L(grandcaller_done);
  e.b(watch_skip);
  e.L(no_caller);
  e.str(e.wzr, ptr(e.x2, static_cast<uint32_t>(offsetof(
                             AeJitWatchEntry, caller_guest_addr))));
  e.str(e.wzr, ptr(e.x2, static_cast<uint32_t>(offsetof(
                             AeJitWatchEntry, grandcaller_guest_addr))));
  e.L(watch_skip);
}

template <typename T, bool swap>
static void MMIOAwareStore(void* _ctx, unsigned int guestaddr, T value) {
  if (swap) {
    value = xe::byte_swap(value);
  }
  if (guestaddr >= 0xE0000000) {
    guestaddr += 0x1000;
  }
  auto ctx = reinterpret_cast<ppc::PPCContext*>(_ctx);
  auto gaddr = ctx->processor->memory()->LookupVirtualMappedRange(guestaddr);
  if (!gaddr) {
    *reinterpret_cast<T*>(ctx->virtual_membase + guestaddr) = value;
  } else {
    value = xe::byte_swap(value);
    gaddr->write(nullptr, gaddr->callback_context, guestaddr, value);
  }
}

template <typename T, bool swap>
static T MMIOAwareLoad(void* _ctx, unsigned int guestaddr) {
  T value;
  if (guestaddr >= 0xE0000000) {
    guestaddr += 0x1000;
  }
  auto ctx = reinterpret_cast<ppc::PPCContext*>(_ctx);
  auto gaddr = ctx->processor->memory()->LookupVirtualMappedRange(guestaddr);
  if (!gaddr) {
    value = *reinterpret_cast<T*>(ctx->virtual_membase + guestaddr);
    if (swap) {
      value = xe::byte_swap(value);
    }
  } else {
    value = gaddr->read(nullptr, gaddr->callback_context, guestaddr);
  }
  return value;
}

// ============================================================================
// OPCODE_LOAD
// ============================================================================
struct LOAD_I8 : Sequence<LOAD_I8, I<OPCODE_LOAD, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.ldrb(i.dest, ptr(e.GetMembaseReg(), addr));
  }
};
struct LOAD_I16 : Sequence<LOAD_I16, I<OPCODE_LOAD, I16Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.ldrh(i.dest, ptr(e.GetMembaseReg(), addr));
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.rev16(i.dest, i.dest);
    }
  }
};
struct LOAD_I32 : Sequence<LOAD_I32, I<OPCODE_LOAD, I32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (IsPossibleMMIOInstruction(e, i.instr)) {
      void* mmio_fn = (void*)&MMIOAwareLoad<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareLoad<uint32_t, true>;
      }
      if (i.src1.is_constant) {
        e.mov(e.w1,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w1, WReg(i.src1.reg().getIdx()));
      }
      e.CallNativeSafe(mmio_fn);
      e.mov(i.dest, e.w0);
      return;
    }
    if (cvars::emit_inline_mmio_checks) {
      if (i.src1.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w17, WReg(i.src1.reg().getIdx()));
      }
      auto& normal_access = e.NewCachedLabel();
      auto& done = e.NewCachedLabel();
      e.mov(e.w0, 0x7FC00000u);
      e.cmp(e.w17, e.w0);
      e.b(LO, normal_access);
      e.mov(e.w0, 0x7FFFFFFFu);
      e.cmp(e.w17, e.w0);
      e.b(HI, normal_access);
      // MMIO path
      void* mmio_fn = (void*)&MMIOAwareLoad<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareLoad<uint32_t, true>;
      }
      e.mov(e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.mov(i.dest, e.w0);
      e.b(done);
      e.L(normal_access);
      {
        auto addr = ComputeMemoryAddress(e, i.src1);
        e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
        if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
          e.rev(i.dest, i.dest);
        }
      }
      e.L(done);
    } else {
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        e.rev(i.dest, i.dest);
      }
      // DIAG(gpu/camera): section 51. The store watch can only say "this
      // wrong value was written HERE"; every backward step then needs a
      // guess about who produced it. The load watch says "this wrong value
      // was READ FROM there" - a concrete guest address to trace to next,
      // with no guessing. Placed after the byte swap so the captured value
      // is the guest-visible one, matching what the store watch records.
      //
      // Guest float loads DO come through here: 8212BCE0's own
      // disassembly shows "ldr w23,[x21,x0]; rev w23,w23; fmov s5,w23" -
      // the a64 backend loads floats as I32 and moves them across, so the
      // same sign+exponent filter that works for stores works here.
      const bool exact_addr_mode =
          XE_AE_DIAG_ENABLED("debug.canary.jit_watch_exact");
      if (exact_addr_mode ||
          XE_AE_DIAG_ENABLED("debug.canary.jit_load_watch")) {
        EmitAeJitWatchBody(e, addr.getIdx(), i.dest.reg().getIdx(), /*kind=*/1,
                           exact_addr_mode);
      }
    }
  }
};
struct LOAD_I64 : Sequence<LOAD_I64, I<OPCODE_LOAD, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.rev(i.dest, i.dest);
    }
  }
};
struct LOAD_F32 : Sequence<LOAD_F32, I<OPCODE_LOAD, F32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.ldr(e.w0, ptr(e.GetMembaseReg(), addr));
      e.rev(e.w0, e.w0);
      e.fmov(i.dest, e.w0);
    } else {
      e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
    }
  }
};
struct LOAD_F64 : Sequence<LOAD_F64, I<OPCODE_LOAD, F64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.ldr(e.x0, ptr(e.GetMembaseReg(), addr));
      e.rev(e.x0, e.x0);
      e.fmov(i.dest, e.x0);
    } else {
      e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
    }
  }
};
struct LOAD_V128 : Sequence<LOAD_V128, I<OPCODE_LOAD, V128Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      // Reverse bytes within each 32-bit word (PPC BE -> ARM64 LE).
      auto idx = i.dest.reg().getIdx();
      e.rev32(VReg16B(idx), VReg16B(idx));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD, LOAD_I8, LOAD_I16, LOAD_I32, LOAD_I64,
                     LOAD_F32, LOAD_F64, LOAD_V128);

// ============================================================================
// OPCODE_STORE
// ============================================================================
struct STORE_I8 : Sequence<STORE_I8, I<OPCODE_STORE, VoidOp, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (i.src2.is_constant) {
      e.mov(e.w17, static_cast<uint64_t>(i.src2.constant() & 0xFF));
      e.strb(e.w17, ptr(e.GetMembaseReg(), addr));
    } else {
      e.strb(i.src2, ptr(e.GetMembaseReg(), addr));
    }
  }
};
struct STORE_I16 : Sequence<STORE_I16, I<OPCODE_STORE, VoidOp, I64Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src2.is_constant) {
        uint16_t val = xe::byte_swap(static_cast<uint16_t>(i.src2.constant()));
        e.mov(e.w17, static_cast<uint64_t>(val));
      } else {
        e.rev16(e.w17, i.src2);
      }
      e.strh(e.w17, ptr(e.GetMembaseReg(), addr));
    } else {
      if (i.src2.is_constant) {
        e.mov(e.w17, static_cast<uint64_t>(i.src2.constant() & 0xFFFF));
        e.strh(e.w17, ptr(e.GetMembaseReg(), addr));
      } else {
        e.strh(i.src2, ptr(e.GetMembaseReg(), addr));
      }
    }
  }
};
struct STORE_I32 : Sequence<STORE_I32, I<OPCODE_STORE, VoidOp, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (IsPossibleMMIOInstruction(e, i.instr)) {
      void* mmio_fn = (void*)&MMIOAwareStore<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareStore<uint32_t, true>;
      }
      if (i.src1.is_constant) {
        e.mov(e.w1,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w1, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        e.mov(e.w2,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
      } else {
        e.mov(e.w2, i.src2);
      }
      e.CallNativeSafe(mmio_fn);
      return;
    }
    if (cvars::emit_inline_mmio_checks) {
      if (i.src1.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w17, WReg(i.src1.reg().getIdx()));
      }
      auto& normal_access = e.NewCachedLabel();
      auto& done = e.NewCachedLabel();
      e.mov(e.w0, 0x7FC00000u);
      e.cmp(e.w17, e.w0);
      e.b(LO, normal_access);
      e.mov(e.w0, 0x7FFFFFFFu);
      e.cmp(e.w17, e.w0);
      e.b(HI, normal_access);
      // MMIO path — copy value to w2 before w1 in case src2 is in w1
      void* mmio_fn = (void*)&MMIOAwareStore<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareStore<uint32_t, true>;
      }
      if (i.src2.is_constant) {
        e.mov(e.w2,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
      } else {
        e.mov(e.w2, i.src2);
      }
      e.mov(e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.b(done);
      e.L(normal_access);
      {
        auto addr = ComputeMemoryAddress(e, i.src1);
        if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
          if (i.src2.is_constant) {
            uint32_t val =
                xe::byte_swap(static_cast<uint32_t>(i.src2.constant()));
            e.mov(e.w17, static_cast<uint64_t>(val));
          } else {
            e.rev(e.w17, i.src2);
          }
          e.str(e.w17, ptr(e.GetMembaseReg(), addr));
        } else {
          if (i.src2.is_constant) {
            e.mov(e.w17, static_cast<uint64_t>(
                             static_cast<uint32_t>(i.src2.constant())));
            e.str(e.w17, ptr(e.GetMembaseReg(), addr));
          } else {
            e.str(i.src2, ptr(e.GetMembaseReg(), addr));
          }
        }
      }
      e.L(done);
    } else {
      auto addr = ComputeMemoryAddress(e, i.src1);
      // DIAG(gpu/camera): see a64_jit_watch_diag.h. Compile-time gated
      // (checked once per Emit call, not per execution) so this costs
      // nothing when off. Only touches w12-w16/x1-x2/x12 - all in the a64
      // backend's reserved scratch range (x0-x18), never allocated to guest
      // values (a64_backend.cc: "GPR set: x22-x28") - so it cannot disturb
      // the real store that follows, constant or register.
      const bool exact_addr_mode =
          XE_AE_DIAG_ENABLED("debug.canary.jit_watch_exact");
      if (exact_addr_mode ||
          XE_AE_DIAG_ENABLED("debug.canary.jit_store_watch")) {
        // Materialize the stored value into w16 first (it may be a
        // constant), then run the shared filter/capture body.
        if (i.src2.is_constant) {
          e.mov(e.w16, static_cast<uint64_t>(
                           static_cast<uint32_t>(i.src2.constant())));
        } else {
          e.mov(e.w16, i.src2);
        }
        EmitAeJitWatchBody(e, addr.getIdx(), 16, /*kind=*/0,
                           exact_addr_mode);
      }
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        if (i.src2.is_constant) {
          uint32_t val =
              xe::byte_swap(static_cast<uint32_t>(i.src2.constant()));
          e.mov(e.w17, static_cast<uint64_t>(val));
        } else {
          e.rev(e.w17, i.src2);
        }
        e.str(e.w17, ptr(e.GetMembaseReg(), addr));
      } else {
        if (i.src2.is_constant) {
          e.mov(e.w17, static_cast<uint64_t>(
                           static_cast<uint32_t>(i.src2.constant())));
          e.str(e.w17, ptr(e.GetMembaseReg(), addr));
        } else {
          e.str(i.src2, ptr(e.GetMembaseReg(), addr));
        }
      }
    }
  }
};
// DIAG(gpu/camera): section 53. 64-bit span variant of the exact-address
// watch. A guest memcpy moves data with `std`, which lands here rather than in
// STORE_I32 - section 52 wrongly concluded "no guest code writes this" purely
// because this opcode class was never instrumented. Matches any 8-byte store
// whose span covers the watched address and records the whole doubleword.
static void EmitAeJitWatch64(A64Emitter& e, uint32_t addr_reg_idx,
                             uint32_t value_reg_idx, uint32_t kind) {
  auto& skip = e.NewCachedLabel();
  e.mov(e.x1, reinterpret_cast<uint64_t>(&g_ae_jit_watch_addr));
  e.ldr(e.w11, ptr(e.x1));
  e.and_(e.w11, e.w11, ~7u);
  e.and_(e.w12, WReg(addr_reg_idx), ~7u);
  e.cmp(e.w12, e.w11);
  e.b(Xbyak_aarch64::NE, skip);

  e.mov(e.x1, reinterpret_cast<uint64_t>(&g_ae_jit_watch_ring_index));
  e.ldr(e.w13, ptr(e.x1));
  e.add(e.w14, e.w13, 1);
  e.str(e.w14, ptr(e.x1));
  e.and_(e.w13, e.w13, kAeJitWatchRingSize - 1);
  e.mov(e.x2, reinterpret_cast<uint64_t>(&g_ae_jit_watch_ring[0]));
  e.mov(e.w14, static_cast<uint32_t>(sizeof(AeJitWatchEntry)));
  e.umull(e.x12, e.w13, e.w14);
  e.add(e.x2, e.x2, e.x12);

  e.str(WReg(addr_reg_idx),
        ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry,
                                                 guest_addr))));
  e.mov(e.w14, kind);
  e.str(e.w14,
        ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry, kind))));
  // Whole doubleword, exactly as it goes to memory (guest byte order).
  e.str(XReg(value_reg_idx),
        ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry,
                                                 arg_ctx328))));
  e.ldr(e.w15,
        ptr(e.x20, static_cast<uint32_t>(offsetof(ppc::PPCContext, lr))));
  e.str(e.w15,
        ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry, guest_lr))));
  // DIAG(gpu/camera): section 54 - the storing function is a block copy, so
  // its ARGUMENTS name the source buffer the negative w was copied FROM.
  // PPC ABI: r3/r4/r5 = first three args. PPCContext::r[n] is at 32 + 8n.
  // Reuses the r24/r26/r28 slots (logged as r3/r4/r5 for this kind).
  e.ldr(e.x13, ptr(e.x20, 32 + 3 * 8));
  e.str(e.x13, ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry, r24))));
  e.ldr(e.x13, ptr(e.x20, 32 + 4 * 8));
  e.str(e.x13, ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry, r26))));
  e.ldr(e.x13, ptr(e.x20, 32 + 5 * 8));
  e.str(e.x13, ptr(e.x2, static_cast<uint32_t>(offsetof(AeJitWatchEntry, r28))));

  e.ldr(e.w9, ptr(e.x19, 172));
  auto& no_caller = e.NewCachedLabel();
  auto& no_gc = e.NewCachedLabel();
  auto& gc_done = e.NewCachedLabel();
  e.cbz(e.w9, no_caller);
  e.mov(e.w11, static_cast<uint32_t>(sizeof(A64BackendStackpoint)));
  e.sub(e.w9, e.w9, 1);
  e.ldr(e.x10, ptr(e.x19, 152));
  e.umull(e.x12, e.w9, e.w11);
  e.add(e.x12, e.x10, e.x12);
  e.ldr(e.w14, ptr(e.x12, static_cast<uint32_t>(offsetof(
                              A64BackendStackpoint, guest_return_address_))));
  e.str(e.w14, ptr(e.x2, static_cast<uint32_t>(offsetof(
                             AeJitWatchEntry, caller_guest_addr))));
  e.cbz(e.w9, no_gc);
  e.sub(e.w9, e.w9, 1);
  e.umull(e.x12, e.w9, e.w11);
  e.add(e.x12, e.x10, e.x12);
  e.ldr(e.w14, ptr(e.x12, static_cast<uint32_t>(offsetof(
                              A64BackendStackpoint, guest_return_address_))));
  e.str(e.w14, ptr(e.x2, static_cast<uint32_t>(offsetof(
                             AeJitWatchEntry, grandcaller_guest_addr))));
  e.b(gc_done);
  e.L(no_gc);
  e.str(e.wzr, ptr(e.x2, static_cast<uint32_t>(offsetof(
                             AeJitWatchEntry, grandcaller_guest_addr))));
  e.L(gc_done);
  e.b(skip);
  e.L(no_caller);
  e.str(e.wzr, ptr(e.x2, static_cast<uint32_t>(offsetof(
                             AeJitWatchEntry, caller_guest_addr))));
  e.str(e.wzr, ptr(e.x2, static_cast<uint32_t>(offsetof(
                             AeJitWatchEntry, grandcaller_guest_addr))));
  e.L(skip);
}

struct STORE_I64 : Sequence<STORE_I64, I<OPCODE_STORE, VoidOp, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    // NOTE: gated separately from jit_watch_exact, and OFF by default.
    // STORE_I64 is far hotter than the other store classes, and enabling this
    // watch crashed the emulator during boot - the fault is in this emit path,
    // not yet diagnosed. Do not turn it on until that is understood; the
    // host-side CAMWATCH (section 53) answers the same question and works.
    if (!i.src2.is_constant &&
        XE_AE_DIAG_ENABLED("debug.canary.jit_watch_i64")) {
      EmitAeJitWatch64(e, addr.getIdx(), i.src2.reg().getIdx(), /*kind=*/5);
    }
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src2.is_constant) {
        uint64_t val = xe::byte_swap(static_cast<uint64_t>(i.src2.constant()));
        e.mov(e.x17, val);
      } else {
        e.rev(e.x17, i.src2);
      }
      e.str(e.x17, ptr(e.GetMembaseReg(), addr));
    } else {
      if (i.src2.is_constant) {
        e.mov(e.x17, static_cast<uint64_t>(i.src2.constant()));
        e.str(e.x17, ptr(e.GetMembaseReg(), addr));
      } else {
        e.str(i.src2, ptr(e.GetMembaseReg(), addr));
      }
    }
  }
};
struct STORE_F32 : Sequence<STORE_F32, I<OPCODE_STORE, VoidOp, I64Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    // DIAG(gpu/camera): section 53. A float store is its OWN sequence, not
    // STORE_I32 - sections 51/52 measured "zero stores" to the camera
    // quaternion without ever having instrumented this class. Value is
    // recorded pre-byteswap (host order); only the address match matters.
    if (!i.src2.is_constant &&
        XE_AE_DIAG_ENABLED("debug.canary.jit_watch_exact")) {
      e.fmov(e.w16, i.src2);
      EmitAeJitWatchBody(e, addr.getIdx(), 16, /*kind=*/6, /*exact_addr=*/true);
    }
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src2.is_constant) {
        uint32_t val =
            xe::byte_swap(static_cast<uint32_t>(i.src2.value->constant.i32));
        e.mov(e.w17, static_cast<uint64_t>(val));
      } else {
        e.fmov(e.w17, i.src2);
        e.rev(e.w17, e.w17);
      }
      e.str(e.w17, ptr(e.GetMembaseReg(), addr));
    } else {
      if (i.src2.is_constant) {
        e.mov(e.w17, static_cast<uint64_t>(i.src2.value->constant.i32));
        e.str(e.w17, ptr(e.GetMembaseReg(), addr));
      } else {
        e.str(i.src2, ptr(e.GetMembaseReg(), addr));
      }
    }
  }
};
struct STORE_F64 : Sequence<STORE_F64, I<OPCODE_STORE, VoidOp, I64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src2.is_constant) {
        uint64_t val =
            xe::byte_swap(static_cast<uint64_t>(i.src2.value->constant.i64));
        e.mov(e.x17, val);
      } else {
        e.fmov(e.x17, i.src2);
        e.rev(e.x17, e.x17);
      }
      e.str(e.x17, ptr(e.GetMembaseReg(), addr));
    } else {
      if (i.src2.is_constant) {
        e.mov(e.x17, static_cast<uint64_t>(i.src2.value->constant.i64));
        e.str(e.x17, ptr(e.GetMembaseReg(), addr));
      } else {
        e.str(i.src2, ptr(e.GetMembaseReg(), addr));
      }
    }
  }
};
struct STORE_V128
    : Sequence<STORE_V128, I<OPCODE_STORE, VoidOp, I64Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // ComputeMemoryAddress may return x0, and LoadV128Const/SrcVReg clobber
    // x0, so save the address to x17 when we need to load a constant source.
    bool need_src_load =
        i.src2.is_constant ||
        (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP);
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (need_src_load) {
      e.mov(e.x17, addr);
      addr = e.x17;
    }
    // DIAG(gpu/camera): section 51.8 - aligned vector stores. The camera
    // quaternion is 16 bytes at an unaligned address, so it straddles two
    // aligned lines; match on the line, not the exact address.
    if (!i.src2.is_constant &&
        XE_AE_DIAG_ENABLED("debug.canary.jit_watch_exact")) {
      EmitAeJitVecWatch(e, addr.getIdx(), i.src2.reg().getIdx(),
                        /*kind=*/4);
    }
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      // Reverse bytes within each 32-bit word, store via scratch v0.
      int idx = SrcVReg(e, i.src2, 0);
      e.rev32(VReg16B(0), VReg16B(idx));
      e.str(QReg(0), ptr(e.GetMembaseReg(), addr));
    } else {
      if (i.src2.is_constant) {
        LoadV128Const(e, 0, i.src2.constant());
        e.str(QReg(0), ptr(e.GetMembaseReg(), addr));
      } else {
        e.str(i.src2, ptr(e.GetMembaseReg(), addr));
      }
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE, STORE_I8, STORE_I16, STORE_I32, STORE_I64,
                     STORE_F32, STORE_F64, STORE_V128);

// ============================================================================
// OPCODE_LOAD_CLOCK
// ============================================================================
struct LOAD_CLOCK : Sequence<LOAD_CLOCK, I<OPCODE_LOAD_CLOCK, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Call QueryGuestTickCount which updates the clock from host ticks.
    // Reading the cached pointer directly would return stale values for
    // consecutive mftb instructions.
    e.CallNative(reinterpret_cast<void*>(LoadClock));
    e.mov(i.dest, e.x0);
  }
  static uint64_t LoadClock(void* raw_context) {
    return Clock::QueryGuestTickCount();
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_CLOCK, LOAD_CLOCK);

// ============================================================================
// OPCODE_LOAD_OFFSET / OPCODE_STORE_OFFSET
// ============================================================================
struct LOAD_OFFSET_I8
    : Sequence<LOAD_OFFSET_I8, I<OPCODE_LOAD_OFFSET, I8Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
    e.ldrb(i.dest, ptr(e.GetMembaseReg(), e.x0));
  }
};
struct LOAD_OFFSET_I16
    : Sequence<LOAD_OFFSET_I16, I<OPCODE_LOAD_OFFSET, I16Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
    e.ldrh(i.dest, ptr(e.GetMembaseReg(), e.x0));
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.rev16(i.dest, i.dest);
    }
  }
};
struct LOAD_OFFSET_I32
    : Sequence<LOAD_OFFSET_I32, I<OPCODE_LOAD_OFFSET, I32Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (IsPossibleMMIOInstruction(e, i.instr)) {
      void* mmio_fn = (void*)&MMIOAwareLoad<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareLoad<uint32_t, true>;
      }
      if (i.src1.is_constant) {
        e.mov(e.w1,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w1, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
      } else {
        e.mov(e.w17, WReg(i.src2.reg().getIdx()));
      }
      e.add(e.w1, e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.mov(i.dest, e.w0);
      return;
    }
    if (cvars::emit_inline_mmio_checks) {
      // Compute raw guest address (src1 + src2) in w17 for range check.
      if (i.src1.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w17, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        uint32_t offset = static_cast<uint32_t>(i.src2.constant());
        if (offset != 0) {
          e.mov(e.w0, static_cast<uint64_t>(offset));
          e.add(e.w17, e.w17, e.w0);
        }
      } else {
        e.add(e.w17, e.w17, WReg(i.src2.reg().getIdx()));
      }
      auto& normal_access = e.NewCachedLabel();
      auto& done = e.NewCachedLabel();
      e.mov(e.w0, 0x7FC00000u);
      e.cmp(e.w17, e.w0);
      e.b(LO, normal_access);
      e.mov(e.w0, 0x7FFFFFFFu);
      e.cmp(e.w17, e.w0);
      e.b(HI, normal_access);
      // MMIO path
      void* mmio_fn = (void*)&MMIOAwareLoad<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareLoad<uint32_t, true>;
      }
      e.mov(e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.mov(i.dest, e.w0);
      e.b(done);
      e.L(normal_access);
      {
        AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
        e.ldr(i.dest, ptr(e.GetMembaseReg(), e.x0));
        if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
          e.rev(i.dest, i.dest);
        }
      }
      e.L(done);
    } else {
      AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
      e.ldr(i.dest, ptr(e.GetMembaseReg(), e.x0));
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        e.rev(i.dest, i.dest);
      }
    }
  }
};
struct LOAD_OFFSET_I64
    : Sequence<LOAD_OFFSET_I64, I<OPCODE_LOAD_OFFSET, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
    e.ldr(i.dest, ptr(e.GetMembaseReg(), e.x0));
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.rev(i.dest, i.dest);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_OFFSET, LOAD_OFFSET_I8, LOAD_OFFSET_I16,
                     LOAD_OFFSET_I32, LOAD_OFFSET_I64);

struct STORE_OFFSET_I8
    : Sequence<STORE_OFFSET_I8,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
    if (i.src3.is_constant) {
      e.mov(e.w17, static_cast<uint64_t>(i.src3.constant() & 0xFF));
      e.strb(e.w17, ptr(e.GetMembaseReg(), e.x0));
    } else {
      e.strb(i.src3, ptr(e.GetMembaseReg(), e.x0));
    }
  }
};
struct STORE_OFFSET_I16
    : Sequence<STORE_OFFSET_I16,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src3.is_constant) {
        uint16_t val = xe::byte_swap(static_cast<uint16_t>(i.src3.constant()));
        e.mov(e.w17, static_cast<uint64_t>(val));
      } else {
        e.rev16(e.w17, i.src3);
      }
      e.strh(e.w17, ptr(e.GetMembaseReg(), e.x0));
    } else {
      if (i.src3.is_constant) {
        e.mov(e.w17, static_cast<uint64_t>(i.src3.constant() & 0xFFFF));
        e.strh(e.w17, ptr(e.GetMembaseReg(), e.x0));
      } else {
        e.strh(i.src3, ptr(e.GetMembaseReg(), e.x0));
      }
    }
  }
};
struct STORE_OFFSET_I32
    : Sequence<STORE_OFFSET_I32,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (IsPossibleMMIOInstruction(e, i.instr)) {
      void* mmio_fn = (void*)&MMIOAwareStore<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareStore<uint32_t, true>;
      }
      if (i.src1.is_constant) {
        e.mov(e.w1,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w1, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
      } else {
        e.mov(e.w17, WReg(i.src2.reg().getIdx()));
      }
      e.add(e.w1, e.w1, e.w17);
      if (i.src3.is_constant) {
        e.mov(e.w2,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src3.constant())));
      } else {
        e.mov(e.w2, i.src3);
      }
      e.CallNativeSafe(mmio_fn);
      return;
    }
    if (cvars::emit_inline_mmio_checks) {
      // Compute raw guest address (src1 + src2) in w17 for range check.
      if (i.src1.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w17, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        uint32_t offset = static_cast<uint32_t>(i.src2.constant());
        if (offset != 0) {
          e.mov(e.w0, static_cast<uint64_t>(offset));
          e.add(e.w17, e.w17, e.w0);
        }
      } else {
        e.add(e.w17, e.w17, WReg(i.src2.reg().getIdx()));
      }
      auto& normal_access = e.NewCachedLabel();
      auto& done = e.NewCachedLabel();
      e.mov(e.w0, 0x7FC00000u);
      e.cmp(e.w17, e.w0);
      e.b(LO, normal_access);
      e.mov(e.w0, 0x7FFFFFFFu);
      e.cmp(e.w17, e.w0);
      e.b(HI, normal_access);
      // MMIO path — copy value to w2 before w1 in case src3 is in w1
      void* mmio_fn = (void*)&MMIOAwareStore<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareStore<uint32_t, true>;
      }
      if (i.src3.is_constant) {
        e.mov(e.w2,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src3.constant())));
      } else {
        e.mov(e.w2, i.src3);
      }
      e.mov(e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.b(done);
      e.L(normal_access);
      {
        AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
        // DIAG(gpu/camera): section 53 - OPCODE_STORE_OFFSET is a DIFFERENT
        // opcode from OPCODE_STORE, with its own sequences. AddGuestMemoryOffset
        // leaves the guest address in x0, which the watch never writes.
        if (!i.src3.is_constant &&
            XE_AE_DIAG_ENABLED("debug.canary.jit_watch_exact")) {
          e.mov(e.w16, i.src3);
          EmitAeJitWatchBody(e, 0, 16, /*kind=*/7, /*exact_addr=*/true);
        }
        if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
          if (i.src3.is_constant) {
            uint32_t val =
                xe::byte_swap(static_cast<uint32_t>(i.src3.constant()));
            e.mov(e.w17, static_cast<uint64_t>(val));
          } else {
            e.rev(e.w17, i.src3);
          }
          e.str(e.w17, ptr(e.GetMembaseReg(), e.x0));
        } else {
          if (i.src3.is_constant) {
            e.mov(e.w17, static_cast<uint64_t>(
                             static_cast<uint32_t>(i.src3.constant())));
            e.str(e.w17, ptr(e.GetMembaseReg(), e.x0));
          } else {
            e.str(i.src3, ptr(e.GetMembaseReg(), e.x0));
          }
        }
      }
      e.L(done);
    } else {
      AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        if (i.src3.is_constant) {
          uint32_t val =
              xe::byte_swap(static_cast<uint32_t>(i.src3.constant()));
          e.mov(e.w17, static_cast<uint64_t>(val));
        } else {
          e.rev(e.w17, i.src3);
        }
        e.str(e.w17, ptr(e.GetMembaseReg(), e.x0));
      } else {
        if (i.src3.is_constant) {
          e.mov(e.w17, static_cast<uint64_t>(
                           static_cast<uint32_t>(i.src3.constant())));
          e.str(e.w17, ptr(e.GetMembaseReg(), e.x0));
        } else {
          e.str(i.src3, ptr(e.GetMembaseReg(), e.x0));
        }
      }
    }
  }
};
struct STORE_OFFSET_I64
    : Sequence<STORE_OFFSET_I64,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    AddGuestMemoryOffset(e, ComputeMemoryAddress(e, i.src1), i.src2);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src3.is_constant) {
        uint64_t val = xe::byte_swap(static_cast<uint64_t>(i.src3.constant()));
        e.mov(e.x17, val);
      } else {
        e.rev(e.x17, i.src3);
      }
      e.str(e.x17, ptr(e.GetMembaseReg(), e.x0));
    } else {
      if (i.src3.is_constant) {
        e.mov(e.x17, static_cast<uint64_t>(i.src3.constant()));
        e.str(e.x17, ptr(e.GetMembaseReg(), e.x0));
      } else {
        e.str(i.src3, ptr(e.GetMembaseReg(), e.x0));
      }
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE_OFFSET, STORE_OFFSET_I8, STORE_OFFSET_I16,
                     STORE_OFFSET_I32, STORE_OFFSET_I64);

// ============================================================================
// OPCODE_MEMSET
// ============================================================================
static const bool zva_enable = (xe_cpu_mrs(DCZID_EL0) & 0b1'0000) == 0;
static const uint64_t zva_length = (4ULL << (xe_cpu_mrs(DCZID_EL0) & 0b0'1111));

struct MEMSET_I64
    : Sequence<MEMSET_I64, I<OPCODE_MEMSET, VoidOp, I64Op, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.is_constant);
    assert_true(i.src3.is_constant);
    assert_true(i.src2.constant() == 0);
    // memset(membase + guest_addr, 0, length)
    // Only used by dcbz/dcbz128: constant zero value, constant aligned size.
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.add(e.x0, e.GetMembaseReg(), addr);
    const uint64_t len = i.src3.constant();
    uint64_t off = 0;

    // Use `dc zva` if it writes more bytes at a time than STP
    if (zva_enable && len >= zva_length && zva_length > 16) {
      for (; off + zva_length <= len; off += zva_length) {
        // dc zva, x0
        e.sys(0b011, 0b0111, 0b0100, 0b001, e.x0);
        if (off + zva_length < len) {
          e.add(e.x0, e.x0, zva_length);
        }
      }
    }

    // Inline with STP xzr, xzr pairs (16 bytes each)
    for (; off + 16 <= len; off += 16) {
      e.stp(e.xzr, e.xzr, AdrPostImm(e.x0, 16));
    }
    // Handle remaining bytes (0-15)
    if (off + 8 <= len) {
      e.str(e.xzr, AdrPostImm(e.x0, 8));
      off += 8;
    }
    if (off + 4 <= len) {
      e.str(e.wzr, AdrPostImm(e.x0, 4));
      off += 4;
    }
    // Byte loop for any remaining 0-3 bytes
    for (; off + 1 <= len; off += 1) {
      e.strb(e.wzr, AdrPostImm(e.x0, 1));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MEMSET, MEMSET_I64);

// ============================================================================
// ============================================================================
// OPCODE_ATOMIC_COMPARE_EXCHANGE
// ============================================================================
struct ATOMIC_COMPARE_EXCHANGE_I32
    : Sequence<ATOMIC_COMPARE_EXCHANGE_I32,
               I<OPCODE_ATOMIC_COMPARE_EXCHANGE, I8Op, I64Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Compute full host address (ldxr/stxr need base-only [Xn] addressing).
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.add(e.x4, e.GetMembaseReg(), addr);
    // src2 = expected (use w5), src3 = desired (use w6).
    if (i.src2.is_constant) {
      e.mov(e.w5,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
    } else {
      e.mov(e.w5, i.src2);
    }
    if (i.src3.is_constant) {
      e.mov(e.w6,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src3.constant())));
    } else {
      e.mov(e.w6, i.src3);
    }

    if (e.IsFeatureEnabled(kA64EmitLSE)) {
      e.mov(e.w0, e.w5);
      e.casal(e.w5, e.w6, ptr(e.x4));
      e.cmp(e.w5, e.w0);
      e.cset(i.dest, Xbyak_aarch64::EQ);
      return;
    }

    auto& retry = e.NewCachedLabel();
    auto& fail = e.NewCachedLabel();
    auto& done = e.NewCachedLabel();
    e.L(retry);
    e.ldaxr(e.w2, ptr(e.x4));
    e.cmp(e.w2, e.w5);
    e.b(Xbyak_aarch64::NE, fail);
    e.stlxr(e.w3, e.w6, ptr(e.x4));
    e.cbnz(e.w3, retry);
    e.mov(i.dest, 1);
    e.b(done);
    e.L(fail);
    e.clrex(15);
    e.mov(i.dest, 0);
    e.L(done);
  }
};
struct ATOMIC_COMPARE_EXCHANGE_I64
    : Sequence<ATOMIC_COMPARE_EXCHANGE_I64,
               I<OPCODE_ATOMIC_COMPARE_EXCHANGE, I8Op, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.add(e.x4, e.GetMembaseReg(), addr);
    if (i.src2.is_constant) {
      e.mov(e.x5, static_cast<uint64_t>(i.src2.constant()));
    } else {
      e.mov(e.x5, i.src2);
    }
    if (i.src3.is_constant) {
      e.mov(e.x6, static_cast<uint64_t>(i.src3.constant()));
    } else {
      e.mov(e.x6, i.src3);
    }

    if (e.IsFeatureEnabled(kA64EmitLSE)) {
      e.mov(e.x0, e.x5);
      e.casal(e.x5, e.x6, ptr(e.x4));
      e.cmp(e.x5, e.x0);
      e.cset(i.dest, Xbyak_aarch64::EQ);
      return;
    }

    auto& retry = e.NewCachedLabel();
    auto& fail = e.NewCachedLabel();
    auto& done = e.NewCachedLabel();
    e.L(retry);
    e.ldaxr(e.x2, ptr(e.x4));
    e.cmp(e.x2, e.x5);
    e.b(Xbyak_aarch64::NE, fail);
    e.stlxr(e.w3, e.x6, ptr(e.x4));
    e.cbnz(e.w3, retry);
    e.mov(i.dest, 1);
    e.b(done);
    e.L(fail);
    e.clrex(15);
    e.mov(i.dest, 0);
    e.L(done);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ATOMIC_COMPARE_EXCHANGE,
                     ATOMIC_COMPARE_EXCHANGE_I32, ATOMIC_COMPARE_EXCHANGE_I64);

// ============================================================================
// OPCODE_LOAD_MMIO / OPCODE_STORE_MMIO
// ============================================================================
struct LOAD_MMIO_I32
    : Sequence<LOAD_MMIO_I32, I<OPCODE_LOAD_MMIO, I32Op, OffsetOp, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto mmio_range = reinterpret_cast<MMIORange*>(i.src1.value);
    auto read_address = uint32_t(i.src2.value);
    // CallNativeSafe: thunk sets x0=PPCContext*, x1/x2/x3 pass through.
    // MMIOReadCallback(void* ppc_ctx, void* callback_ctx, uint32_t addr).
    e.mov(e.x1, uint64_t(mmio_range->callback_context));
    e.mov(e.w2, static_cast<uint64_t>(read_address));
    e.CallNativeSafe(reinterpret_cast<void*>(mmio_range->read));
    e.rev(e.w0, e.w0);
    e.mov(i.dest, e.w0);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_MMIO, LOAD_MMIO_I32);

struct STORE_MMIO_I32
    : Sequence<STORE_MMIO_I32,
               I<OPCODE_STORE_MMIO, VoidOp, OffsetOp, OffsetOp, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto mmio_range = reinterpret_cast<MMIORange*>(i.src1.value);
    auto write_address = uint32_t(i.src2.value);
    // CallNativeSafe: thunk sets x0=PPCContext*, x1/x2/x3 pass through.
    // MMIOWriteCallback(void* ppc_ctx, void* callback_ctx, uint32_t addr,
    //                   uint32_t value).
    e.mov(e.x1, uint64_t(mmio_range->callback_context));
    e.mov(e.w2, static_cast<uint64_t>(write_address));
    if (i.src3.is_constant) {
      e.mov(e.w3, static_cast<uint64_t>(
                      xe::byte_swap(static_cast<uint32_t>(i.src3.constant()))));
    } else {
      e.mov(e.w3, i.src3);
      e.rev(e.w3, e.w3);
    }
    e.CallNativeSafe(reinterpret_cast<void*>(mmio_range->write));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE_MMIO, STORE_MMIO_I32);

// ============================================================================
// OPCODE_RESERVED_LOAD / OPCODE_RESERVED_STORE
// ============================================================================
// Helper: get pointer to A64BackendContext.
// x19 is the dedicated backend context register, so this is a no-op
// accessor for readability. The returned register is x19.
static const Xbyak_aarch64::XReg& LoadBackendCtxPtr(A64Emitter& e) {
  return e.GetBackendCtxReg();
}

struct RESERVED_LOAD_I32
    : Sequence<RESERVED_LOAD_I32, I<OPCODE_RESERVED_LOAD, I32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    // Save guest address before load — dest may alias addr register.
    e.mov(e.w0, WReg(addr.getIdx()));
    // Load the value (may clobber addr if dest == addr).
    e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
    // Save reservation: address and value in backend context.
    auto bctx = LoadBackendCtxPtr(e);
    // Store the guest address (already saved in x0).
    e.str(e.x0, ptr(bctx, static_cast<uint32_t>(offsetof(
                              A64BackendContext, cached_reserve_offset))));
    // Store the loaded value (zero-extended to 64-bit).
    e.mov(e.w1, i.dest);
    e.str(e.x1, ptr(bctx, static_cast<uint32_t>(offsetof(
                              A64BackendContext, cached_reserve_value_))));
    // Set the "has reserve" flag (bit 1).
    e.ldr(e.w1,
          ptr(bctx, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
    e.orr(e.w1, e.w1, static_cast<uint32_t>(1u << kA64BackendHasReserveBit));
    e.str(e.w1,
          ptr(bctx, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
  }
};
struct RESERVED_LOAD_I64
    : Sequence<RESERVED_LOAD_I64, I<OPCODE_RESERVED_LOAD, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    // Save guest address before load — dest may alias addr register.
    e.mov(e.w0, WReg(addr.getIdx()));
    // Load the value (may clobber addr if dest == addr).
    e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
    // Save reservation in backend context.
    auto bctx = LoadBackendCtxPtr(e);
    e.str(e.x0, ptr(bctx, static_cast<uint32_t>(offsetof(
                              A64BackendContext, cached_reserve_offset))));
    e.str(i.dest, ptr(bctx, static_cast<uint32_t>(offsetof(
                                A64BackendContext, cached_reserve_value_))));
    e.ldr(e.w1,
          ptr(bctx, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
    e.orr(e.w1, e.w1, static_cast<uint32_t>(1u << kA64BackendHasReserveBit));
    e.str(e.w1,
          ptr(bctx, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_RESERVED_LOAD, RESERVED_LOAD_I32,
                     RESERVED_LOAD_I64);

struct RESERVED_STORE_I32
    : Sequence<RESERVED_STORE_I32,
               I<OPCODE_RESERVED_STORE, I8Op, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    auto& no_reserve = e.NewCachedLabel();
    auto& done = e.NewCachedLabel();
    // Check if we have a reservation.
    auto bctx = LoadBackendCtxPtr(e);
    e.ldr(e.w4,
          ptr(bctx, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
    e.tbz(e.w4, kA64BackendHasReserveBit, no_reserve);
    // Clear the reserve flag.
    e.and_(e.w4, e.w4,
           static_cast<uint32_t>(~(1u << kA64BackendHasReserveBit)));
    e.str(e.w4,
          ptr(bctx, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
    // Check if address matches.
    e.ldr(e.x4, ptr(bctx, static_cast<uint32_t>(offsetof(
                              A64BackendContext, cached_reserve_offset))));
    e.mov(e.w5, WReg(addr.getIdx()));
    e.cmp(e.x4, e.x5);
    e.b(Xbyak_aarch64::NE, no_reserve);
    // Address matches. Do atomic compare-exchange.
    // Expected value from cached_reserve_value_.
    e.ldr(e.w5, ptr(bctx, static_cast<uint32_t>(offsetof(
                              A64BackendContext, cached_reserve_value_))));
    // Desired value.
    if (i.src2.is_constant) {
      e.mov(e.w6,
            static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
    } else {
      e.mov(e.w6, WReg(i.src2.reg().getIdx()));
    }
    // Compute host address.
    e.add(e.x4, e.GetMembaseReg(), addr);

    if (e.IsFeatureEnabled(kA64EmitLSE)) {
      e.mov(e.w0, e.w5);
      e.casal(e.w5, e.w6, ptr(e.x4));
      e.cmp(e.w5, e.w0);
      e.cset(i.dest, Xbyak_aarch64::EQ);
      e.b(done);
    } else {
      // LDXR/STXR loop.
      auto& cas_loop = e.NewCachedLabel();
      auto& cas_fail = e.NewCachedLabel();
      e.L(cas_loop);
      e.ldaxr(e.w7, ptr(e.x4));
      e.cmp(e.w7, e.w5);
      e.b(Xbyak_aarch64::NE, cas_fail);
      e.stlxr(e.w7, e.w6, ptr(e.x4));
      e.cbnz(e.w7, cas_loop);
      // Success.
      e.mov(i.dest, 1);
      e.b(done);
      e.L(cas_fail);
      e.clrex(15);
    }
    e.L(no_reserve);
    e.mov(i.dest, 0);
    e.L(done);
  }
};
struct RESERVED_STORE_I64
    : Sequence<RESERVED_STORE_I64,
               I<OPCODE_RESERVED_STORE, I8Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr = ComputeMemoryAddress(e, i.src1);
    auto& no_reserve = e.NewCachedLabel();
    auto& done = e.NewCachedLabel();
    auto bctx = LoadBackendCtxPtr(e);
    e.ldr(e.w4,
          ptr(bctx, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
    e.tbz(e.w4, kA64BackendHasReserveBit, no_reserve);
    e.and_(e.w4, e.w4,
           static_cast<uint32_t>(~(1u << kA64BackendHasReserveBit)));
    e.str(e.w4,
          ptr(bctx, static_cast<uint32_t>(offsetof(A64BackendContext, flags))));
    e.ldr(e.x4, ptr(bctx, static_cast<uint32_t>(offsetof(
                              A64BackendContext, cached_reserve_offset))));
    e.mov(e.w5, WReg(addr.getIdx()));
    e.cmp(e.x4, e.x5);
    e.b(Xbyak_aarch64::NE, no_reserve);
    // 64-bit compare-exchange.
    e.ldr(e.x5, ptr(bctx, static_cast<uint32_t>(offsetof(
                              A64BackendContext, cached_reserve_value_))));
    if (i.src2.is_constant) {
      e.mov(e.x6, static_cast<uint64_t>(i.src2.constant()));
    } else {
      e.mov(e.x6, XReg(i.src2.reg().getIdx()));
    }
    e.add(e.x4, e.GetMembaseReg(), addr);

    if (e.IsFeatureEnabled(kA64EmitLSE)) {
      e.mov(e.x0, e.x5);
      e.casal(e.x5, e.x6, ptr(e.x4));
      e.cmp(e.x5, e.x0);
      e.cset(i.dest, Xbyak_aarch64::EQ);
      e.b(done);
    } else {
      auto& cas_loop = e.NewCachedLabel();
      auto& cas_fail = e.NewCachedLabel();
      e.L(cas_loop);
      e.ldaxr(e.x7, ptr(e.x4));
      e.cmp(e.x7, e.x5);
      e.b(Xbyak_aarch64::NE, cas_fail);
      e.stlxr(e.w7, e.x6, ptr(e.x4));
      e.cbnz(e.w7, cas_loop);
      e.mov(i.dest, 1);
      e.b(done);
      e.L(cas_fail);
      e.clrex(15);
    }
    e.L(no_reserve);
    e.mov(i.dest, 0);
    e.L(done);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_RESERVED_STORE, RESERVED_STORE_I32,
                     RESERVED_STORE_I64);

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
