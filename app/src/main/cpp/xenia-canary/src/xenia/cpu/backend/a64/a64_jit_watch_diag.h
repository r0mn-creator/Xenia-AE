/**
 ******************************************************************************
 * Xenia AE : Xbox 360 Emulator Research Project                              *
 ******************************************************************************
 * Copyright 2026 Xenia AE. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_JIT_WATCH_DIAG_H_
#define XENIA_CPU_BACKEND_A64_A64_JIT_WATCH_DIAG_H_

// DIAG(gpu/camera): see docs/HALO3_VISTA_46_VS_64.md section 46.
//
// An always-on (once compiled in), address-independent alternative to
// Memory::EnableCamwatchDiag's page-fault watch (section 45), which proved
// unreliable for this investigation - the guest RAM buffer it watches is
// effectively single-use, so "wait for a repeat write to addr X" almost
// never pays off (section 45.3).
//
// This instead bakes an inline check into every JIT-compiled 32-bit guest
// store (STORE_I32, a64_seq_memory.cc), gated at COMPILE TIME by
// debug.canary.jit_store_watch so it costs nothing when off - must be set
// BEFORE the game launches to affect functions compiled during boot. When
// on, every store checks its VALUE (not address) against a fixed sign+
// exponent bracket (negative, exponent byte 126 -> magnitude in [0.5, 1.0))
// matching the observed mirrored camera constants, and if it matches,
// records {guest_addr, value, guest_lr} into a small ring buffer with no
// function call (all scratch registers used are x0-x18, which the a64
// register allocator never assigns to guest values - see
// a64_backend.cc's "GPR set: x22-x28" comment - so this cannot corrupt a
// live guest register).
//
// Value-based, not address-based: sidesteps the entire "will this address
// ever be written again" problem section 45.3 ran into three times, at the
// cost of also catching unrelated negative floats in the same magnitude
// bracket if any exist elsewhere in the game. Cross-reference hits by
// guest_lr for a consistent culprit, same as section 44's approach - but
// now backed by guaranteed-correct live-register data instead of a
// coincidental page-fault.

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

// Prints any new JITWATCH ring buffer entries via XELOGI. Cheap to call
// often (single relaxed atomic load when there is nothing new) - meant to
// be polled from an already-frequent host hook (e.g. WriteRegister).
void DumpAeJitStoreWatch();

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_JIT_WATCH_DIAG_H_
