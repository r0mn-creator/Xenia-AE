/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/base/platform.h"
#include "xenia/kernel/kernel_flags.h"

#if XE_PLATFORM_AX360E
DEFINE_bool(headless, true,
            "Don't display any UI, using defaults for prompts as needed.",
            "UI");
#else
DEFINE_bool(headless, false,
            "Don't display any UI, using defaults for prompts as needed.",
            "UI");
#endif
// TESTRIG(kernel-call-trace): see shim_utils.h - without this, an export that
// lacks the kLog tag is invisible no matter what other logging is enabled.
DEFINE_bool(log_all_kernel_calls, false,
            "Log every kernel call, ignoring the per-export kLog tag. Very "
            "verbose, but the only way to prove a subsystem is NOT being "
            "called.",
            "Kernel");

DEFINE_bool(log_high_frequency_kernel_calls, false,
            "Log kernel calls with the kHighFrequency tag.", "Kernel");

// Ported from XenDroid (Canary AEX overhaul, docs/AEX_OVERHAUL.md step 1).
// Defaults OFF here: AEX must boot identically to Canary AE until the whole
// scheduler chain (steps 1-4) is in place. Partial enablement wedges the guest.
DEFINE_bool(
    guest_scheduler, false,
    "Run guest threads as cooperative fibers driven by an in-kernel scheduler "
    "instead of mapping each to its own host OS thread. Requires a restart to "
    "take effect.",
    "Kernel");
DEFINE_uint32(
    guest_scheduler_quantum_us, 1000,
    "Cooperative-scheduler timeslice in microseconds. A guest fiber running "
    "this long yields at its next JIT safepoint so co-resident fibers on the "
    "same dispatch thread make progress. Lower is fairer but switches more.",
    "Kernel");
