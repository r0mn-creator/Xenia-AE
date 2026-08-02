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
