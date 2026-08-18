/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/memory.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <random>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/ae_fix_toggle.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/host_thread_context.h"
#include "xenia/base/logging.h"
#include "xenia/base/testrig_debug_server.h"  // TESTRIG(mem)
#include "xenia/base/math.h"
#include "xenia/base/threading.h"

#include "xenia/cpu/mmio_handler.h"
#include "xenia/cpu/ppc/ppc_context.h"

// TODO(benvanik): move xbox.h out
#include "xenia/xbox.h"

DEFINE_bool(protect_zero, true, "Protect the zero page from reads and writes.",
            "Memory");
DEFINE_bool(emit_inline_mmio_checks, false,
            "Emit inline MMIO range checks for all I32 loads/stores instead "
            "of relying on exception-based MMIO detection.",
            "CPU");
DEFINE_bool(emit_mmio_aware_stores_for_recorded_exception_addresses, true,
            "Uses info gathered via record_mmio_access_exceptions to emit "
            "special stores that are faster than trapping the exception",
            "CPU");
DEFINE_bool(record_mmio_access_exceptions, true,
            "For guest addresses records whether we caught any mmio accesses "
            "for them. This info can then be used on a subsequent run to "
            "instruct the recompiler to emit checks",
            "CPU");
DEFINE_bool(protect_on_release, false,
            "Protect released memory to prevent accesses.", "Memory");
DEFINE_bool(scribble_heap, false,
            "Scribble specific or random value into all allocated heap memory.",
            "Memory");
DEFINE_int32(scribble_heap_value, 0,
             "Value used to fill all allocated heap memory. 0 - Random value. "
             "Valid range: [1-255]",
             "Memory");
DEFINE_uint32(mmap_address_high,8,
              "1-124",
              "Memory");
namespace xe {
uint32_t get_page_count(uint32_t value, uint32_t page_size) {
  return xe::round_up(value, page_size) / page_size;
}

/**
 * Memory map:
 * 0x00000000 - 0x3FFFFFFF (1024mb) - virtual 4k pages
 * 0x40000000 - 0x7FFFFFFF (1024mb) - virtual 64k pages
 * 0x80000000 - 0x8BFFFFFF ( 192mb) - xex 64k pages
 * 0x8C000000 - 0x8FFFFFFF (  64mb) - xex 64k pages (encrypted)
 * 0x90000000 - 0x9FFFFFFF ( 256mb) - xex 4k pages
 * 0xA0000000 - 0xBFFFFFFF ( 512mb) - physical 64k pages
 * 0xC0000000 - 0xDFFFFFFF          - physical 16mb pages
 * 0xE0000000 - 0xFFFFFFFF          - physical 4k pages
 *
 * We use the host OS to create an entire addressable range for this. That way
 * we don't have to emulate a TLB. It'd be really cool to pass through page
 * sizes or use madvice to let the OS know what to expect.
 *
 * We create our own heap of committed memory that lives at
 * memory_HEAP_LOW to memory_HEAP_HIGH - all normal user allocations
 * come from there. Since the Xbox has no paging, we know that the size of
 * this heap will never need to be larger than ~512MB (realistically, smaller
 * than that). We place it far away from the XEX data and keep the memory
 * around it uncommitted so that we have some warning if things go astray.
 *
 * For XEX/GPU/etc data we allow placement allocations (base_address != 0) and
 * commit the requested memory as needed. This bypasses the standard heap, but
 * XEXs should never be overwriting anything so that's fine. We can also query
 * for previous commits and assert that we really isn't committing twice.
 *
 * GPU memory is mapped onto the lower 512mb of the virtual 4k range (0).
 * So 0xA0000000 = 0x00000000. A more sophisticated allocator could handle
 * this.
 */

static Memory* active_memory_ = nullptr;

void CrashDump() {
  static std::atomic<int> in_crash_dump(0);
  if (in_crash_dump.fetch_add(1)) {
    xe::FatalError(
        "Hard crash: the memory system crashed while dumping a crash dump.");
    return;
  }
  active_memory_->DumpMap();
  --in_crash_dump;
}

static inline bool ShouldSkipHostCommit(const BaseHeap& heap) {
  // When the host page size is larger than 4 KB (e.g. 16 KB on macOS ARM64,
  // 64 KB on some ARM64 Linux kernels), mprotect on 4 KB guest page boundaries
  // fails with EINVAL. All heaps are backed by a shared file mapping
  // (MapFileView) that is already mapped RW, so the commit is a no-op — skip
  // it.
  if (xe::memory::page_size() > 0x1000) {
    return true;
  }
  return false;
}

xe::memory::PageAccess ToPageAccess(uint32_t protect) {
  // Write-combine memory is CPU-writable (for GPU uploads)
  bool is_writable =
      (protect & kMemoryProtectWrite) || (protect & kMemoryProtectWriteCombine);

  if ((protect & kMemoryProtectRead) && !is_writable) {
    return xe::memory::PageAccess::kReadOnly;
  } else if ((protect & kMemoryProtectRead) && is_writable) {
    return xe::memory::PageAccess::kReadWrite;
  } else {
    return xe::memory::PageAccess::kNoAccess;
  }
}

void RandomizeMemory(void* range_start, uint32_t size) {
  if (!cvars::scribble_heap) {
    return;
  }

  if (!cvars::scribble_heap_value) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, std::numeric_limits<uint8_t>::max());

    std::generate(static_cast<char*>(range_start),
                  static_cast<char*>(range_start) + size,
                  [&]() { return dis(gen); });
  } else {
    std::memset(range_start, cvars::scribble_heap_value, size);
  }
}

Memory::Memory() {
  system_page_size_ = uint32_t(xe::memory::page_size());
  system_allocation_granularity_ =
      uint32_t(xe::memory::allocation_granularity());
  assert_zero(active_memory_);
  active_memory_ = this;
}

Memory::~Memory() {
  assert_true(active_memory_ == this);
  active_memory_ = nullptr;

  // Uninstall the MMIO handler, as we won't be able to service more
  // requests.
  mmio_handler_.reset();

  for (auto invalidation_callback : physical_memory_invalidation_callbacks_) {
    delete invalidation_callback;
  }

  heaps_.v00000000.Dispose();
  heaps_.v40000000.Dispose();
  heaps_.v80000000.Dispose();
  heaps_.v90000000.Dispose();
  heaps_.vA0000000.Dispose();
  heaps_.vC0000000.Dispose();
  heaps_.vE0000000.Dispose();
  heaps_.physical.Dispose();

  // Unmap all views and close mapping.
  if (mapping_ != xe::memory::kFileMappingHandleInvalid) {
    UnmapViews();
    xe::memory::CloseFileMappingHandle(mapping_, file_name_);
    mapping_base_ = nullptr;
    mapping_ = xe::memory::kFileMappingHandleInvalid;
  }

  virtual_membase_ = nullptr;
  physical_membase_ = nullptr;
}

bool Memory::Initialize() {
  file_name_ = fmt::format("xenia_memory_{}", Clock::QueryHostTickCount());

  // Create main page file-backed mapping. This is all reserved but
  // uncommitted (so it shouldn't expand page file).
  mapping_ = xe::memory::CreateFileMappingHandle(
      file_name_,
      // entire 4gb space + 512mb physical:
      0x11FFFFFFF, xe::memory::PageAccess::kReadWrite, false);
  if (mapping_ == xe::memory::kFileMappingHandleInvalid) {
    XELOGE("Unable to reserve the 4gb guest address space.");
    assert_always();
    return false;
  }

  // Attempt to create our views. This may fail at the first address
  // we pick, so try a few times.
  mapping_base_ = 0;
#if !XE_PLATFORM_AX360E
    for (size_t n = 32; n < 64; n++) {
      auto mapping_base = reinterpret_cast<uint8_t*>(1ull << n);
      if (!MapViews(mapping_base)) {
        mapping_base_ = mapping_base;
        break;
      }
    }
#else
    assert(cvars::mmap_address_high>=1&&cvars::mmap_address_high<=124);
    auto mapping_base = reinterpret_cast<uint8_t*>(uint64_t(cvars::mmap_address_high+0) << 32);
    if (!MapViews(mapping_base)) {
        mapping_base_ = mapping_base;
    } else {
        assert_always();
        return false;
    }
#endif
  if (!mapping_base_) {
    XELOGE("Unable to find a continuous block in the 64bit address space.");
    assert_always();
    return false;
  }
  virtual_membase_ = mapping_base_;
  physical_membase_ = mapping_base_ + 0x100000000ull;

  // Prepare virtual heaps.
  heaps_.v00000000.Initialize(this, virtual_membase_, HeapType::kGuestVirtual,
                              0x00000000, 0x40000000, 4096);
  heaps_.v40000000.Initialize(this, virtual_membase_, HeapType::kGuestVirtual,
                              0x40000000, 0x40000000 - 0x01000000, 64 * 1024);
  heaps_.v80000000.Initialize(this, virtual_membase_, HeapType::kGuestXex,
                              0x80000000, 0x10000000, 64 * 1024);
  heaps_.v90000000.Initialize(this, virtual_membase_, HeapType::kGuestXex,
                              0x90000000, 0x10000000, 4096);

  // Prepare physical heaps.
  heaps_.physical.Initialize(this, physical_membase_, HeapType::kGuestPhysical,
                             0x00000000, 0x20000000, 4096);
  heaps_.vA0000000.Initialize(this, virtual_membase_, HeapType::kGuestPhysical,
                              0xA0000000, 0x20000000, 64 * 1024,
                              &heaps_.physical);
  heaps_.vC0000000.Initialize(this, virtual_membase_, HeapType::kGuestPhysical,
                              0xC0000000, 0x20000000, 16 * 1024 * 1024,
                              &heaps_.physical);
  heaps_.vE0000000.Initialize(this, virtual_membase_, HeapType::kGuestPhysical,
                              0xE0000000, 0x1FD00000, 4096, &heaps_.physical);

  // Protect the first and last 64kb of memory.
  heaps_.v00000000.AllocFixed(
      0x00000000, 0x10000, 0x10000,
      kMemoryAllocationReserve | kMemoryAllocationCommit,
      !cvars::protect_zero ? kMemoryProtectRead | kMemoryProtectWrite
                           : kMemoryProtectNoAccess);
  // The last 64 KB of PHYSICAL memory was reserved as a guard. Halo 4 sizes
  // its big allocation as "everything from the first free page up to
  // 0x20000000" and so asks for this range too - it was the final 16 pages
  // standing between the title and a successful boot. The virtual guard at
  // 0x00000000 (above) is untouched; this only releases the physical top.
  // Toggle: debug.canary.fix_free_physical_top_guard (default ON).
  if (!XE_AE_FIX_ENABLED("debug.canary.fix_free_physical_top_guard")) {
    heaps_.physical.AllocFixed(0x1FFF0000, 0x10000, 0x10000,
                               kMemoryAllocationReserve,
                               kMemoryProtectNoAccess);
  }

  // GPU writeback.
  // 0xC... is physical, 0x7F... is virtual. We may need to overlay these.
  //
  // This reserves the first 16 MB of GUEST PHYSICAL memory, and that is what
  // stopped Halo 4 booting. The title asks for ~492 MB of contiguous physical
  // memory in the window [0x00FE0000, 0x1FC00000] - a window exactly the size
  // of the request, so it has to start at page 4064. The reservation owns
  // pages 0..4095, so the free block begins at 4096 and every one of the
  // eight retries came up short by exactly 32 pages (128 KB). It then called
  // RtlRaiseException and the process died.
  //
  // The reservation cannot be shrunk through vC0000000: that heap has 16 MB
  // pages, so AllocFixed rounds any size up to the full 16 MB. Reserve in the
  // parent physical heap instead, which has 4 KB pages.
  //
  // Why shrinking is safe: nothing writes to physical 0 unconditionally. GPU
  // writeback is redirected to virtual 0x7F000000 + offset and bounded by the
  // guest's WRITEBACK_SIZE register (see
  // pm4_command_processor_implement.h, ExecutePacketType3 write-back path).
  // That is the command-processor read-pointer writeback - kilobytes at most.
  // Reserving all 16 MB was maximally defensive; keeping 15 MB leaves an
  // enormous margin over anything a read-pointer writeback can use, while
  // handing the top 1 MB back as ordinary guest RAM, which is what Halo 4
  // needs 128 KB of.
  //
  // Toggle: debug.canary.fix_gpu_writeback_reserve (default ON; set 0 to
  // restore the original full-16 MB reservation and bisect).
  static constexpr uint32_t kGpuWritebackRegionSize = 0x01000000;  // 16 MB
  // 14 MB. Sized so that after this reserve, the 3.4 MB "?" reservation and
  // xenia's own system allocations, the guest's free block still begins low
  // enough that rounding it up to the next 64 KB boundary (which is the
  // granularity titles allocate in) leaves Halo 4's ~493 MB request room to
  // fit. 15 MB left it 16 pages short. GPU writeback is bounded by the guest's
  // WRITEBACK_SIZE register and is kilobytes, so 14 MB is still an enormous
  // margin over anything that region can actually be used for.
  static constexpr uint32_t kGpuWritebackReserveSize = 0x00E00000;  // 14 MB
  if (XE_AE_FIX_ENABLED("debug.canary.fix_gpu_writeback_reserve")) {
    heaps_.physical.AllocFixed(
        0x00000000, kGpuWritebackReserveSize, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite);
    // The reservation above is what used to host-commit the whole first 16 MB
    // (BaseHeap::AllocFixed commits when kMemoryAllocationCommit is set). The
    // pages we just handed back to the guest still have to be backed, or the
    // first guest write to them faults - so commit the remainder explicitly,
    // exactly as the loop below does for the rest of physical memory.
    xe::memory::AllocFixed(
        heaps_.physical.TranslateRelative(kGpuWritebackReserveSize),
        kGpuWritebackRegionSize - kGpuWritebackReserveSize,
        xe::memory::AllocationType::kCommit,
        xe::memory::PageAccess::kReadWrite);
  } else {
    heaps_.vC0000000.AllocFixed(
        0xC0000000, kGpuWritebackRegionSize, 32,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite);
  }

  // TODO(Gliniak): Seems like GPU has access to whole physical memory range
  // without any restriction. This however needs some form of validation.
  // That's why we're commiting whole physical memory range and deal with
  // allocations issues on custom page protection level.
  for (size_t i = 1; i <= 16; i++) {
    xe::memory::AllocFixed(heaps_.physical.TranslateRelative(i << 24),
                           heaps_.physical.page_size() * 0x10000,
                           xe::memory::AllocationType::kCommit,
                           xe::memory::PageAccess::kReadWrite);
  }

  // Add handlers for MMIO.
  mmio_handler_ = cpu::MMIOHandler::Install(
      virtual_membase_, physical_membase_, physical_membase_ + 0x1FFFFFFF,
      HostToGuestVirtualThunk, this, AccessViolationCallbackThunk, this,
      nullptr, nullptr);
  if (!mmio_handler_) {
    XELOGE("Unable to install MMIO handlers");
    assert_always();
    return false;
  }

  // ?
  //
  // Upstream does not know what this 3.4 MB reservation is for either - hence
  // the comment. What matters here is that it is allocated TOP-DOWN, so it
  // lands at the very top of guest physical memory (0x1FCB0000-0x1FFF0000),
  // and everything xenia itself later puts in physical memory is pushed BELOW
  // it: the XMA context array (xma_decoder.cc, 320 * 64 = 20480 bytes) and a
  // handful of single-page SystemHeapAlloc(kSystemHeapPhysical) allocations.
  // Those nine pages land at 0x1FCA7000, i.e. inside the region a title
  // expects to be free.
  //
  // That is what stops Halo 4 booting. It binary-searches for the largest
  // contiguous physical block and asks for exactly [free_start, 0x1FCB0000) -
  // the whole span up to this reservation - which our own nine pages sit
  // inside, so every attempt fails and the title raises an exception and dies.
  //
  // Allocate it bottom-up instead. This is safe by construction: the returned
  // address goes into a local that is never read, so nothing can depend on
  // WHERE it lands - the reservation's only effect is to consume 3.4 MB. Doing
  // it bottom-up leaves the top of physical memory to xenia's own system
  // allocations and keeps the rest of the heap as one unbroken block.
  //
  // Toggle: debug.canary.fix_unk_phys_bottom_up (default ON; set 0 for the
  // original top-down placement).
  uint32_t unk_phys_alloc;
  const bool unk_phys_top_down =
      !XE_AE_FIX_ENABLED("debug.canary.fix_unk_phys_bottom_up");
  heaps_.vA0000000.Alloc(0x340000, 64 * 1024, kMemoryAllocationReserve,
                         kMemoryProtectNoAccess, unk_phys_top_down,
                         &unk_phys_alloc);

  uint32_t unknown_xex_range;  // Probably hypervisor?
  heaps_.v80000000.Alloc(0x40000, 4 * 1024, kMemoryAllocationCommit,
                         kMemoryProtectRead | kMemoryProtectWrite, false,
                         &unknown_xex_range);

  // Value taken from 544307D5. Title explicitly access this address and this is
  // a value underneath it (It's constant between multiple runs)
  uint32_t value_to_write = xe::byte_swap(0x2a6e3f38);
  memcpy(TranslateVirtual(0x80000000 + 0x1C), &value_to_write,
         sizeof(uint32_t));

  // TESTRIG(mem): expose live per-heap page usage - see docs/TEST_HARNESS.md.
  xe::testrig::Expose(xe::testrig::kPortMemory, "mem",
                       [this]() { return TestrigFormatSnapshot(); });

  return true;
}

void Memory::SetMMIOExceptionRecordingCallback(
    cpu::MmioAccessRecordCallback callback, void* context) {
  mmio_handler_->SetMMIOExceptionRecordingCallback(callback, context);
}

static const struct {
  uint64_t virtual_address_start;
  uint64_t virtual_address_end;
  uint64_t target_address;
} map_info[] = {
    // (1024mb) - virtual 4k pages
    {
        0x00000000,
        0x3FFFFFFF,
        0x0000000000000000ull,
    },
    // (1024mb) - virtual 64k pages (cont)
    {
        0x40000000,
        0x7EFFFFFF,
        0x0000000040000000ull,
    },
    //   (16mb) - GPU writeback + 15mb of XPS?
    {
        0x7F000000,
        0x7FFFFFFF,
        0x0000000100000000ull,
    },
    //  (256mb) - xex 64k pages
    {
        0x80000000,
        0x8FFFFFFF,
        0x0000000080000000ull,
    },
    //  (256mb) - xex 4k pages
    {
        0x90000000,
        0x9FFFFFFF,
        0x0000000080000000ull,
    },
    //  (512mb) - physical 64k pages
    {
        0xA0000000,
        0xBFFFFFFF,
        0x0000000100000000ull,
    },
    //          - physical 16mb pages
    {
        0xC0000000,
        0xDFFFFFFF,
        0x0000000100000000ull,
    },
    //          - physical 4k pages
    {
        0xE0000000,
        0xFFFFFFFF,
        0x0000000100001000ull,
    },
    //          - physical raw
    {
        0x100000000,
        0x11FFFFFFF,
        0x0000000100000000ull,
    },
};
int Memory::MapViews(uint8_t* mapping_base) {
  assert_true(xe::countof(map_info) == xe::countof(views_.all_views));
  // 0xE0000000 4 KB offset is emulated via host_address_offset and on the CPU
  // side if system allocation granularity is bigger than 4 KB.
  uint64_t granularity_mask = ~uint64_t(system_allocation_granularity_ - 1);
  for (size_t n = 0; n < xe::countof(map_info); n++) {
    views_.all_views[n] = reinterpret_cast<uint8_t*>(xe::memory::MapFileView(
        mapping_, mapping_base + map_info[n].virtual_address_start,
        map_info[n].virtual_address_end - map_info[n].virtual_address_start + 1,
        xe::memory::PageAccess::kReadWrite,
        map_info[n].target_address & granularity_mask));
    if (!views_.all_views[n]) {
      // Failed, so bail and try again.
      UnmapViews();
      return 1;
    }
  }
  return 0;
}

void Memory::UnmapViews() {
  for (size_t n = 0; n < xe::countof(views_.all_views); n++) {
    if (views_.all_views[n]) {
      size_t length = map_info[n].virtual_address_end -
                      map_info[n].virtual_address_start + 1;
      xe::memory::UnmapFileView(mapping_, views_.all_views[n], length);
    }
  }
}

void Memory::Reset() {
  heaps_.v00000000.Reset();
  heaps_.v40000000.Reset();
  heaps_.v80000000.Reset();
  heaps_.v90000000.Reset();
  heaps_.physical.Reset();
}
// clang does not like non-standard layout offsetof
#if XE_COMPILER_MSVC == 1 && XE_COMPILER_CLANG_CL == 0
XE_NOALIAS
const BaseHeap* Memory::LookupHeap(uint32_t address) const {
#define HEAP_INDEX(name) \
  offsetof(Memory, heaps_.name) - offsetof(Memory, heaps_)

  const char* heap_select = (const char*)&this->heaps_;

  unsigned selected_heap_offset = 0;
  unsigned high_nibble = address >> 28;

  if (high_nibble < 0x4) {
    selected_heap_offset = HEAP_INDEX(v00000000);
  } else if (address < 0x7F000000) {
    selected_heap_offset = HEAP_INDEX(v40000000);
  } else if (high_nibble < 0x8) {
    heap_select = nullptr;
    // return nullptr;
  } else if (high_nibble < 0x9) {
    selected_heap_offset = HEAP_INDEX(v80000000);
    // return &heaps_.v80000000;
  } else if (high_nibble < 0xA) {
    // return &heaps_.v90000000;
    selected_heap_offset = HEAP_INDEX(v90000000);
  } else if (high_nibble < 0xC) {
    // return &heaps_.vA0000000;
    selected_heap_offset = HEAP_INDEX(vA0000000);
  } else if (high_nibble < 0xE) {
    // return &heaps_.vC0000000;
    selected_heap_offset = HEAP_INDEX(vC0000000);
  } else if (address < 0xFFD00000) {
    // return &heaps_.vE0000000;
    selected_heap_offset = HEAP_INDEX(vE0000000);
  } else {
    //  return nullptr;
    heap_select = nullptr;
  }
  return reinterpret_cast<const BaseHeap*>(selected_heap_offset + heap_select);
}
#else
XE_NOALIAS
const BaseHeap* Memory::LookupHeap(uint32_t address) const {
  if (address < 0x40000000) {
    return &heaps_.v00000000;
  } else if (address < 0x7F000000) {
    return &heaps_.v40000000;
  } else if (address < 0x80000000) {
    return nullptr;
  } else if (address < 0x90000000) {
    return &heaps_.v80000000;
  } else if (address < 0xA0000000) {
    return &heaps_.v90000000;
  } else if (address < 0xC0000000) {
    return &heaps_.vA0000000;
  } else if (address < 0xE0000000) {
    return &heaps_.vC0000000;
  } else if (address < 0xFFD00000) {
    return &heaps_.vE0000000;
  } else {
    return nullptr;
  }
}
#endif
BaseHeap* Memory::LookupHeapByType(bool physical, uint32_t page_size) {
  if (physical) {
    if (page_size <= 4096) {
      return &heaps_.vE0000000;
    } else if (page_size <= 64 * 1024) {
      return &heaps_.vA0000000;
    } else {
      return &heaps_.vC0000000;
    }
  } else {
    if (page_size <= 4096) {
      return &heaps_.v00000000;
    } else {
      return &heaps_.v40000000;
    }
  }
}

VirtualHeap* Memory::GetPhysicalHeap() { return &heaps_.physical; }

void Memory::GetHeapsPageStatsSummary(const BaseHeap* const* provided_heaps,
                                      size_t heaps_count,
                                      uint32_t& unreserved_pages,
                                      uint32_t& reserved_pages,
                                      uint32_t& used_pages,
                                      uint32_t& reserved_bytes) {
  auto lock = global_critical_region_.Acquire();
  for (size_t i = 0; i < heaps_count; i++) {
    const BaseHeap* heap = provided_heaps[i];
    uint32_t heap_unreserved_pages = heap->unreserved_page_count();
    uint32_t heap_reserved_pages = heap->reserved_page_count();

    unreserved_pages += heap_unreserved_pages;
    reserved_pages += heap_reserved_pages;
    used_pages += ((heap->total_page_count() - heap_unreserved_pages) *
                   heap->page_size()) /
                  4096;
    reserved_bytes += heap_reserved_pages * heap->page_size();
  }
}

uint32_t Memory::HostToGuestVirtual(const void* host_address) const {
  size_t virtual_address = reinterpret_cast<size_t>(host_address) -
                           reinterpret_cast<size_t>(virtual_membase_);
  uint32_t vE0000000_host_offset = heaps_.vE0000000.host_address_offset();
  size_t vE0000000_host_base =
      size_t(heaps_.vE0000000.heap_base()) + vE0000000_host_offset;
  if (virtual_address >= vE0000000_host_base &&
      virtual_address <=
          (vE0000000_host_base + (heaps_.vE0000000.heap_size() - 1))) {
    virtual_address -= vE0000000_host_offset;
  }
  return uint32_t(virtual_address);
}

uint32_t Memory::HostToGuestVirtualThunk(const void* context,
                                         const void* host_address) {
  return reinterpret_cast<const Memory*>(context)->HostToGuestVirtual(
      host_address);
}

uint32_t Memory::GetPhysicalAddress(uint32_t address) const {
  const BaseHeap* heap = LookupHeap(address);
  if (!heap) {
    return UINT32_MAX;
  }

  // Assumption that we already received physical address, so just return it.
  if (heap->heap_type() != HeapType::kGuestPhysical && address < 0x1FFFFFFF) {
    return address;
  }

  return static_cast<const PhysicalHeap*>(heap)->GetPhysicalAddress(address);
}

void Memory::Zero(uint32_t address, uint32_t size) {
  std::memset(TranslateVirtual(address), 0, size);
}

void Memory::Fill(uint32_t address, uint32_t size, uint8_t value) {
  std::memset(TranslateVirtual(address), value, size);
}

void Memory::Copy(uint32_t dest, uint32_t src, uint32_t size) {
  uint8_t* pdest = TranslateVirtual(dest);
  const uint8_t* psrc = TranslateVirtual(src);
  std::memcpy(pdest, psrc, size);
}

uint32_t Memory::SearchAligned(uint32_t start, uint32_t end,
                               const uint32_t* values, size_t value_count) {
  assert_true(start <= end);
  auto p = TranslateVirtual<const uint32_t*>(start);
  auto pe = TranslateVirtual<const uint32_t*>(end);
  while (p != pe) {
    if (*p == values[0]) {
      const uint32_t* pc = p + 1;
      size_t matched = 1;
      for (size_t n = 1; n < value_count; n++, pc++) {
        if (*pc != values[n]) {
          break;
        }
        matched++;
      }
      if (matched == value_count) {
        return HostToGuestVirtual(p);
      }
    }
    p++;
  }
  return 0;
}

bool Memory::AddVirtualMappedRange(uint32_t virtual_address, uint32_t mask,
                                   uint32_t size, void* context,
                                   cpu::MMIOReadCallback read_callback,
                                   cpu::MMIOWriteCallback write_callback) {
  if (!cvars::emit_inline_mmio_checks) {
    if (!xe::memory::AllocFixed(TranslateVirtual(virtual_address), size,
                                xe::memory::AllocationType::kCommit,
                                xe::memory::PageAccess::kNoAccess)) {
      XELOGE("Unable to map range; commit/protect failed");
      return false;
    }
  }
  return mmio_handler_->RegisterRange(virtual_address, mask, size, context,
                                      read_callback, write_callback);
}

cpu::MMIORange* Memory::LookupVirtualMappedRange(uint32_t virtual_address) {
  return mmio_handler_->LookupRange(virtual_address);
}

bool Memory::AccessViolationCallback(
    global_unique_lock_type global_lock_locked_once, void* host_address,
    bool is_write) {
  // Access via physical_membase_ is special, when need to bypass everything
  // (for instance, for a data provider to actually write the data) so only
  // triggering callbacks on virtual memory regions.
  if (reinterpret_cast<size_t>(host_address) <
          reinterpret_cast<size_t>(virtual_membase_) ||
      reinterpret_cast<size_t>(host_address) >=
          reinterpret_cast<size_t>(physical_membase_)) {
    return false;
  }
  uint32_t virtual_address = HostToGuestVirtual(host_address);
  BaseHeap* heap = LookupHeap(virtual_address);
  if (heap->heap_type() != HeapType::kGuestPhysical) {
    return false;
  }

  // Access violation callbacks from the guest are triggered when the global
  // critical region mutex is locked once.
  //
  // Will be rounded to physical page boundaries internally, so just pass 1 as
  // the length - guranteed not to cross page boundaries also.
  auto physical_heap = static_cast<PhysicalHeap*>(heap);
  return physical_heap->TriggerCallbacks(std::move(global_lock_locked_once),
                                         virtual_address, 1, is_write, false);
}

bool Memory::AccessViolationCallbackThunk(
    global_unique_lock_type global_lock_locked_once, void* context,
    void* host_address, bool is_write) {
  return reinterpret_cast<Memory*>(context)->AccessViolationCallback(
      std::move(global_lock_locked_once), host_address, is_write);
}

bool Memory::TriggerPhysicalMemoryCallbacks(
    global_unique_lock_type global_lock_locked_once, uint32_t virtual_address,
    uint32_t length, bool is_write, bool unwatch_exact_range, bool unprotect) {
  BaseHeap* heap = LookupHeap(virtual_address);
  if (heap->heap_type() == HeapType::kGuestPhysical) {
    auto physical_heap = static_cast<PhysicalHeap*>(heap);
    return physical_heap->TriggerCallbacks(std::move(global_lock_locked_once),
                                           virtual_address, length, is_write,
                                           unwatch_exact_range, unprotect);
  }
  return false;
}

void* Memory::RegisterPhysicalMemoryInvalidationCallback(
    PhysicalMemoryInvalidationCallback callback, void* callback_context) {
  auto entry = new std::pair<PhysicalMemoryInvalidationCallback, void*>(
      callback, callback_context);
  auto lock = global_critical_region_.Acquire();
  physical_memory_invalidation_callbacks_.push_back(entry);
  return entry;
}

void Memory::UnregisterPhysicalMemoryInvalidationCallback(
    void* callback_handle) {
  auto entry =
      reinterpret_cast<std::pair<PhysicalMemoryInvalidationCallback, void*>*>(
          callback_handle);
  {
    auto lock = global_critical_region_.Acquire();
    auto it = std::find(physical_memory_invalidation_callbacks_.begin(),
                        physical_memory_invalidation_callbacks_.end(), entry);
    assert_true(it != physical_memory_invalidation_callbacks_.end());
    if (it != physical_memory_invalidation_callbacks_.end()) {
      physical_memory_invalidation_callbacks_.erase(it);
    }
  }
  delete entry;
}

void Memory::EnablePhysicalMemoryAccessCallbacks(
    uint32_t physical_address, uint32_t length,
    bool enable_invalidation_notifications, bool enable_data_providers) {
  heaps_.vA0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications,
                                         enable_data_providers);
  heaps_.vC0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications,
                                         enable_data_providers);
  heaps_.vE0000000.EnableAccessCallbacks(physical_address, length,
                                         enable_invalidation_notifications,
                                         enable_data_providers);
}

namespace {
// DIAG(gpu/camera): see Memory::EnableCamwatchDiag's declaration.
//
// RegisterPhysicalMemoryInvalidationCallback is GLOBAL - every registered
// callback fires for every watched page anyone invalidates (shared memory,
// texture cache, ...), not just the page we armed. The first run without
// this filter produced 7700+ hits sweeping unrelated 256 KB-strided ranges
// within a couple of seconds. g_ae_camwatch_pages tracks the (at most two,
// since the source buffer double-buffers) pages we actually armed, and the
// callback drops anything else before it even counts as a hit.
// Raised from 12: with page-granular watching, the first fault after arming
// is whatever field happens to be written FIRST on that page, not
// necessarily the tracked byte (confirmed - every one of the first 12 hits
// had exact_match=false, landing at varying offsets under 0xD00 on each
// page). The callback below now re-arms itself immediately on every hit
// (SharedMemory's own MemoryInvalidationCallback re-acquires
// global_critical_region_ from inside itself, so this is a proven-safe
// pattern, not a new one) so one page's writes are traced back-to-back
// until the exact byte is hit or the cap is reached.
constexpr int kAeCamwatchMaxHits = 2000;
std::atomic<uint32_t> g_ae_camwatch_pages[2]{{0}, {0}};
// Most recently armed EXACT address (not page-rounded) - lets the callback
// report whether a hit's fault_phys matches the byte we actually meant to
// track, versus some other field sharing the same 4 KB page.
std::atomic<uint32_t> g_ae_camwatch_exact_target{0};
std::atomic<bool> g_ae_camwatch_registered{false};
std::atomic<int> g_ae_camwatch_hits{0};
std::atomic<bool> g_ae_camwatch_found_exact{false};

bool AeCamwatchPageIsOurs(uint32_t page) {
  return page != 0 &&
        (g_ae_camwatch_pages[0].load(std::memory_order_relaxed) == page ||
         g_ae_camwatch_pages[1].load(std::memory_order_relaxed) == page);
}

std::pair<uint32_t, uint32_t> AeCamwatchInvalidationCallback(
    void* context_ptr, uint32_t physical_address_start, uint32_t length,
    bool exact_range) {
  uint32_t page = physical_address_start & ~uint32_t(0xFFF);
  if (!AeCamwatchPageIsOurs(page)) {
    return std::make_pair(uint32_t(0), UINT32_MAX);
  }
  int hit = g_ae_camwatch_hits.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t guest_lr = 0;
  uint64_t ppc_ctx_ptr = 0;
  bool had_context = false;
#if XE_ARCH_ARM64
  const HostThreadContext* tc = cpu::g_ae_camwatch_fault_context;
  if (tc) {
    had_context = true;
    // PPCContext pointer lives in x20 (the context register - see
    // a64_emitter.h and PreemptCurrentFiber).
    //
    // BUG FIXED (2026-08-16): this used to read a hardcoded offset 0x10 for
    // lr, copied from a comment in ppc_context.h ("uint64_t lr; // 0x10").
    // That comment is STALE - the struct was reordered ("most frequently
    // used registers first") and never re-annotated. The real layout has
    // cr0..cr7 (32B) + fpscr (4B) + 4B padding to 8-align + r[32] (256B) +
    // ctr (8B) THEN lr, landing lr at offset 304 - which is exactly the
    // address a disassembly of guest_825AD9F0 showed a guest `bl`-equivalent
    // writing its return address to. Confirmed by including the real struct
    // and using offsetof instead of trusting the comment a second time.
    ppc_ctx_ptr = tc->x[20];
    if (ppc_ctx_ptr) {
      static_assert(offsetof(cpu::ppc::PPCContext, lr) == 304,
                    "PPCContext::lr moved - camwatch's guest_lr read is now "
                    "wrong again, fix the assumption, not the assert");
      guest_lr = uint32_t(
          reinterpret_cast<const cpu::ppc::PPCContext*>(ppc_ctx_ptr)->lr);
    }
  }
#endif
  uint64_t host_pc = 0;
  uint64_t host_lr = 0;
  uint32_t caller = 0, grandcaller = 0, great_caller = 0;
  uint32_t chain[8] = {};
#if XE_ARCH_ARM64
  if (tc) {
    host_pc = tc->pc;
    host_lr = tc->x[30];
    // DIAG(gpu/ball): section 62 - walk Xenia's own guest stack from the fault
    // context, exactly as A64Backend::PopulatePseudoStacktrace does, so a ring
    // buffer write names the whole GUEST call chain above it rather than just
    // the faulting function. x19 is the backend context register; the
    // stackpoints array is at +152 and the depth at +172 (confirmed in
    // section 48 against a64_backend.h and the prologue bytes).
    const uint64_t backend_ctx = tc->x[19];
    if (backend_ctx) {
      auto sp_base = *reinterpret_cast<uint8_t* const*>(backend_ctx + 152);
      uint32_t depth =
          *reinterpret_cast<const uint32_t*>(backend_ctx + 172);
      // The watch fires for ANY write to the page, including from threads
      // that are not running guest code at all - there x19 is not a backend
      // context and both fields are garbage. Walking that crashed the
      // emulator once (an 8-frame walk read far out of bounds), so bound the
      // depth by the same 0x10000 cap the JIT prologue enforces before
      // dereferencing anything.
      const bool sp_usable =
          sp_base != nullptr && depth != 0 && depth <= 0x10000;
      auto frame_ret = [&](uint32_t back) -> uint32_t {
        if (!sp_usable || depth <= back) return 0u;
        return *reinterpret_cast<const uint32_t*>(
            sp_base + size_t(depth - 1 - back) * 16 + 12);
      };
      caller = frame_ret(0);
      grandcaller = frame_ret(1);
      great_caller = frame_ret(2);
      // 3 frames only. An 8-frame walk crashed the emulator TWICE even with
      // a depth bound: the watch fires on threads where x19 is not a backend
      // context, and sp_base can then be a readable-looking but wrong
      // pointer. Deeper call chains must be recovered by STATIC disassembly
      // (the section 49 technique), not by walking further here.
      for (int fi = 0; fi < 3; ++fi) chain[fi] = frame_ret(uint32_t(fi));
    }
  }
#endif
  // DIAG(gpu/camera): the EXACT faulting byte, not just the watched page -
  // physical_address_start/length above are the ARMED RANGE (page-rounded),
  // not the fault. A 4 KB page can hold many unrelated fields (confirmed:
  // hits 13-16 on one page in an earlier run resolved to three DIFFERENT
  // guest functions), so this is what actually confirms a hit is the byte
  // being tracked and not a neighbour.
  uint32_t fault_phys = 0;
  const void* fault_host = cpu::g_ae_camwatch_fault_host_address;
  if (fault_host) {
    auto* memory = reinterpret_cast<Memory*>(context_ptr);
    fault_phys = uint32_t((reinterpret_cast<uintptr_t>(fault_host) -
                           reinterpret_cast<uintptr_t>(memory->physical_membase())) &
                          0x1FFFFFFFu);
  }
  uint32_t exact_target =
      g_ae_camwatch_exact_target.load(std::memory_order_relaxed);
  bool exact_match = fault_phys == exact_target;
  XELOGI(
      "CAMWATCH hit={} phys=0x{:08X} len={} fault_phys=0x{:08X} "
      "exact_match={} had_context={} x20=0x{:016X} guest_lr=0x{:08X} "
      "host_pc=0x{:016X} host_lr=0x{:016X} caller=0x{:08X} "
      "chain=[{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}]",
      hit, physical_address_start, length, fault_phys, exact_match,
      had_context, ppc_ctx_ptr, guest_lr, host_pc, host_lr, chain[0],
      chain[1], chain[2], chain[3], chain[4], chain[5], chain[6], chain[7]);
  if (exact_match) {
    g_ae_camwatch_found_exact.store(true, std::memory_order_relaxed);
  }
  // DIAG(gpu/camera): re-arm the SAME page immediately, from inside the
  // callback, so back-to-back writes within one buffer-fill pass are all
  // traced instead of losing coverage after the page's one-shot watch fires
  // once. SharedMemory::MemoryInvalidationCallback re-acquires
  // global_critical_region_ from inside itself in production, so doing
  // real work (not just returning) from inside this callback is a proven
  // pattern here, not a new risk.
  //
  // Skipped in sweep mode: chasing one page converges on whichever writer
  // is fastest to re-trigger (always the same one - see EnableCamwatchDiag)
  // which is the opposite of what a survey needs. Sweep mode wants each
  // external CAMWRITE call to get its own single fault on its own address,
  // for variety across DIFFERENT writers.
  if (!XE_AE_DIAG_ENABLED("debug.canary.camwatch_sweep") &&
      !g_ae_camwatch_found_exact.load(std::memory_order_relaxed) &&
      g_ae_camwatch_hits.load(std::memory_order_relaxed) < kAeCamwatchMaxHits) {
    reinterpret_cast<Memory*>(context_ptr)
        ->EnablePhysicalMemoryAccessCallbacks(page, 4096, true, false);
  }
  return std::make_pair(uint32_t(0), UINT32_MAX);
}
}  // namespace

void Memory::ArmCamwatchExact(uint32_t physical_address) {
  // Guest addresses come in as 0xA5xxxxxx / 0x85xxxxxx aliases; the watch and
  // the callback's fault_phys both work in physical space.
  physical_address &= 0x1FFFFFFFu;
  static std::atomic<uint32_t> armed_target{0};
  uint32_t previous = armed_target.load(std::memory_order_relaxed);
  bool target_changed = previous != physical_address;
  if (target_changed) {
    // New target: clear the latch and the hit budget from the previous one.
    armed_target.store(physical_address, std::memory_order_relaxed);
    g_ae_camwatch_found_exact.store(false, std::memory_order_relaxed);
    g_ae_camwatch_hits.store(0, std::memory_order_relaxed);
    g_ae_camwatch_pages[0].store(0, std::memory_order_relaxed);
    g_ae_camwatch_pages[1].store(0, std::memory_order_relaxed);
  }
  if (g_ae_camwatch_found_exact.load(std::memory_order_relaxed) ||
      g_ae_camwatch_hits.load(std::memory_order_relaxed) >=
          kAeCamwatchMaxHits) {
    return;
  }
  bool expected = false;
  if (g_ae_camwatch_registered.compare_exchange_strong(expected, true)) {
    RegisterPhysicalMemoryInvalidationCallback(AeCamwatchInvalidationCallback,
                                               this);
  }
  g_ae_camwatch_exact_target.store(physical_address, std::memory_order_relaxed);
  uint32_t page = physical_address & ~uint32_t(0xFFF);
  if (g_ae_camwatch_pages[0].load(std::memory_order_relaxed) != page) {
    g_ae_camwatch_pages[0].store(page, std::memory_order_relaxed);
  }
  if (target_changed) {
    XELOGI("CAMWATCH_ARMED_EXACT target=0x{:08X} page=0x{:08X}",
           physical_address, page);
  }
  EnablePhysicalMemoryAccessCallbacks(page, 4096, true, false);
}

void Memory::EnableCamwatchDiag(uint32_t physical_address) {
  // DIAG(gpu/camera): arm ONCE (the very first call), then leave the target
  // alone. WriteALURangeFromMem calls WriteRegister in a tight host loop, so
  // the external CAMWRITE hook re-fires far faster than one page's watch can
  // be re-triggered by a guest write - re-arming the target on every call
  // thrashed it before the callback's own re-arm ever got a second hit on
  // the SAME page (confirmed: 60/60 hits, every one a DIFFERENT page).
  //
  // Tried gating this on page-recurrence first (only arm once a page had
  // already been seen twice) to skip the menu-load linear-allocator phase -
  // REVERTED. Bumping the recurrence-detection ring from 16 to 256 slots
  // fixed it firing at all, but the page it then armed still only produced
  // hits at OTHER offsets (many distinct writers share a page - confirmed:
  // one run got 24 back-to-back hits on the same page, all at just two
  // other offsets, zero at the tracked byte). The target byte looks like
  // it's written once per buffer instance, so waiting for a first-seen
  // buffer to be reused needs patience across many frames, not a smarter
  // gate - arm on the very first sample and let the (large) hit cap and a
  // long test run do the waiting.
  // debug.canary.camwatch_sweep flips this off: instead of chasing one
  // address, re-arm on every call so many DIFFERENT writers get sampled in
  // one run (each still page-filtered, so this doesn't reintroduce the
  // 45's original global-callback flood) - useful for surveying which
  // guest functions touch this buffer class at all, e.g. to find one with
  // FP/vector instructions worth checking as the actual computer.
  static std::atomic<bool> started{false};
  if (!XE_AE_DIAG_ENABLED("debug.canary.camwatch_sweep")) {
    bool expected_start = false;
    if (!started.compare_exchange_strong(expected_start, true)) {
      return;
    }
  }
  if (g_ae_camwatch_found_exact.load(std::memory_order_relaxed) ||
      g_ae_camwatch_hits.load(std::memory_order_relaxed) >=
          kAeCamwatchMaxHits) {
    return;
  }
  bool expected = false;
  if (g_ae_camwatch_registered.compare_exchange_strong(expected, true)) {
    RegisterPhysicalMemoryInvalidationCallback(AeCamwatchInvalidationCallback,
                                               this);
  }
  g_ae_camwatch_exact_target.store(physical_address, std::memory_order_relaxed);
  uint32_t page = physical_address & ~uint32_t(0xFFF);
  if (g_ae_camwatch_pages[0].load(std::memory_order_relaxed) != page &&
      g_ae_camwatch_pages[1].load(std::memory_order_relaxed) != page) {
    // Evict round-robin between the two tracked slots.
    static std::atomic<int> next_slot{0};
    g_ae_camwatch_pages[next_slot.fetch_add(1, std::memory_order_relaxed) & 1]
        .store(page, std::memory_order_relaxed);
  }
  XELOGI("CAMWATCH_ARMED target=0x{:08X} page=0x{:08X}", physical_address,
         page);
  EnablePhysicalMemoryAccessCallbacks(page, 4096, true, false);
}

uint32_t Memory::SystemHeapAlloc(uint32_t size, uint32_t alignment,
                                 uint32_t system_heap_flags) {
  // TODO(benvanik): lightweight pool.
  bool is_physical = !!(system_heap_flags & kSystemHeapPhysical);
  auto heap = LookupHeapByType(is_physical, 4096);
  uint32_t address;
  if (!heap->AllocSystemHeap(
          size, alignment, kMemoryAllocationReserve | kMemoryAllocationCommit,
          kMemoryProtectRead | kMemoryProtectWrite, false, &address)) {
    return 0;
  }
  Zero(address, size);
  return address;
}

void Memory::SystemHeapFree(uint32_t address, uint32_t* out_region_size) {
  if (!address) {
    return;
  }
  // TODO(benvanik): lightweight pool.
  auto heap = LookupHeap(address);
  heap->Release(address, out_region_size);
}

void Memory::DumpMap() {
  XELOGE("==================================================================");
  XELOGE("Memory Dump");
  XELOGE("==================================================================");
  XELOGE("               System Page Size: {0} ({0:08X})", system_page_size_);
  XELOGE("  System Allocation Granularity: {0} ({0:08X})",
         system_allocation_granularity_);
  XELOGE("                Virtual Membase: {}",
         static_cast<void*>(virtual_membase_));
  XELOGE("               Physical Membase: {}",
         static_cast<void*>(physical_membase_));
  XELOGE("");
  XELOGE("------------------------------------------------------------------");
  XELOGE("Virtual Heaps");
  XELOGE("------------------------------------------------------------------");
  XELOGE("");
  heaps_.v00000000.DumpMap();
  heaps_.v40000000.DumpMap();
  heaps_.v80000000.DumpMap();
  heaps_.v90000000.DumpMap();
  XELOGE("");
  XELOGE("------------------------------------------------------------------");
  XELOGE("Physical Heaps");
  XELOGE("------------------------------------------------------------------");
  XELOGE("");
  heaps_.physical.DumpMap();
  heaps_.vA0000000.DumpMap();
  heaps_.vC0000000.DumpMap();
  heaps_.vE0000000.DumpMap();
  XELOGE("");
}

// TESTRIG(mem): summarizes committed/reserved pages per named guest heap - see
// docs/TEST_HARNESS.md.
std::string Memory::TestrigFormatSnapshot() {
  auto heap_line = [](const char* name, BaseHeap& heap) {
    uint32_t total = heap.total_page_count();
    uint32_t reserved = heap.reserved_page_count();
    uint64_t reserved_bytes = uint64_t(reserved) * heap.page_size();
    return fmt::format(
        "{}: {}/{} pages reserved ({:.1f} MB), page_size={}\n", name,
        reserved, total, double(reserved_bytes) / (1024.0 * 1024.0),
        heap.page_size());
  };
  std::string result;
  result += fmt::format("virtual_membase: {}\nphysical_membase: {}\n",
                        static_cast<void*>(virtual_membase_),
                        static_cast<void*>(physical_membase_));
  result += heap_line("v00000000", heaps_.v00000000);
  result += heap_line("v40000000", heaps_.v40000000);
  result += heap_line("v80000000", heaps_.v80000000);
  result += heap_line("v90000000", heaps_.v90000000);
  result += heap_line("physical", heaps_.physical);
  result += heap_line("vA0000000", heaps_.vA0000000);
  result += heap_line("vC0000000", heaps_.vC0000000);
  result += heap_line("vE0000000", heaps_.vE0000000);
  return result;
}

bool Memory::Save(ByteStream* stream) {
  XELOGD("Serializing memory...");
  heaps_.v00000000.Save(stream);
  heaps_.v40000000.Save(stream);
  heaps_.v80000000.Save(stream);
  heaps_.v90000000.Save(stream);
  heaps_.physical.Save(stream);

  return true;
}

bool Memory::Restore(ByteStream* stream) {
  XELOGD("Restoring memory...");
  heaps_.v00000000.Restore(stream);
  heaps_.v40000000.Restore(stream);
  heaps_.v80000000.Restore(stream);
  heaps_.v90000000.Restore(stream);
  heaps_.physical.Restore(stream);

  return true;
}

uint32_t FromPageAccess(xe::memory::PageAccess protect) {
  switch (protect) {
    case memory::PageAccess::kNoAccess:
      return kMemoryProtectNoAccess;
    case memory::PageAccess::kReadOnly:
      return kMemoryProtectRead;
    case memory::PageAccess::kReadWrite:
      return kMemoryProtectRead | kMemoryProtectWrite;
    case memory::PageAccess::kExecuteReadOnly:
      // Guest memory cannot be executable - this should never happen :)
      assert_always();
      return kMemoryProtectRead;
    case memory::PageAccess::kExecuteReadWrite:
      // Guest memory cannot be executable - this should never happen :)
      assert_always();
      return kMemoryProtectRead | kMemoryProtectWrite;
  }

  return kMemoryProtectNoAccess;
}

BaseHeap::BaseHeap()
    : membase_(nullptr), heap_base_(0), heap_size_(0), page_size_(0) {}

BaseHeap::~BaseHeap() = default;

void BaseHeap::Initialize(Memory* memory, uint8_t* membase, HeapType heap_type,
                          uint32_t heap_base, uint32_t heap_size,
                          uint32_t page_size, uint32_t host_address_offset) {
  memory_ = memory;
  membase_ = membase;
  heap_type_ = heap_type;
  heap_base_ = heap_base;
  heap_size_ = heap_size;
  page_size_ = page_size;
  xenia_assert(xe::is_pow2(page_size_));
  page_size_shift_ = xe::log2_floor(page_size_);
  host_address_offset_ = host_address_offset;
  page_table_.resize(heap_size / page_size);
  unreserved_page_count_ = uint32_t(page_table_.size());

  // Initialize free block tracker with a single block covering the entire heap.
  free_blocks_.clear();
  free_blocks_[0] = uint32_t(page_table_.size());
}

void BaseHeap::Dispose() {
  // Walk table and release all regions.
  for (uint32_t page_number = 0; page_number < page_table_.size();
       ++page_number) {
    auto& page_entry = page_table_[page_number];
    if (page_entry.state) {
      xe::memory::DeallocFixed(TranslateRelative(page_number * page_size_), 0,
                               xe::memory::DeallocationType::kRelease);
      page_number += page_entry.region_page_count;
    }
  }
  free_blocks_.clear();
}

void BaseHeap::DumpMap() {
  auto global_lock = global_critical_region_.Acquire();
  XELOGE("------------------------------------------------------------------");
  XELOGE("Heap: {:08X}-{:08X}", heap_base_, heap_base_ + (heap_size_ - 1));
  XELOGE("------------------------------------------------------------------");
  XELOGE("            Heap Base: {:08X}", heap_base_);
  XELOGE("            Heap Size: {0} ({0:08X})", heap_size_);
  XELOGE("            Page Size: {0} ({0:08X})", page_size_);
  XELOGE("           Page Count: {}", page_table_.size());
  XELOGE("  Host Address Offset: {0} ({0:08X})", host_address_offset_);
  bool is_empty_span = false;
  uint32_t empty_span_start = 0;
  for (uint32_t i = 0; i < uint32_t(page_table_.size()); ++i) {
    auto& page = page_table_[i];
    if (!page.state) {
      if (!is_empty_span) {
        is_empty_span = true;
        empty_span_start = i;
      }
      continue;
    }
    if (is_empty_span) {
      XELOGE("  {:08X}-{:08X} {:6d}p {:10d}b unreserved",
             heap_base_ + empty_span_start * page_size_,
             heap_base_ + i * page_size_, i - empty_span_start,
             (i - empty_span_start) * page_size_);
      is_empty_span = false;
    }
    const char* state_name = "   ";
    if (page.state & kMemoryAllocationCommit) {
      state_name = "COM";
    } else if (page.state & kMemoryAllocationReserve) {
      state_name = "RES";
    }
    char access_r = (page.current_protect & kMemoryProtectRead) ? 'R' : ' ';
    char access_w = (page.current_protect & kMemoryProtectWrite) ? 'W' : ' ';
    XELOGE("  {:08X}-{:08X} {:6d}p {:10d}b {} {}{}",
           heap_base_ + i * page_size_,
           heap_base_ + (i + page.region_page_count) * page_size_,
           page.region_page_count, page.region_page_count * page_size_,
           state_name, access_r, access_w);
    i += page.region_page_count - 1;
  }
  if (is_empty_span) {
    XELOGE("  {:08X}-{:08X} - {} unreserved pages)",
           heap_base_ + empty_span_start * page_size_,
           heap_base_ + (heap_size_ - 1),
           page_table_.size() - empty_span_start);
  }
}

bool BaseHeap::Save(ByteStream* stream) {
  XELOGD("Heap {:08X}-{:08X}", heap_base_, heap_base_ + (heap_size_ - 1));

  for (size_t i = 0; i < page_table_.size(); i++) {
    auto& page = page_table_[i];
    stream->Write(page.qword);
    if (!page.state) {
      // Unallocated.
      continue;
    }

    // TODO(DrChat): write compressed with snappy.
    if (page.state & kMemoryAllocationCommit) {
      void* addr = TranslateRelative(i * page_size_);

      memory::PageAccess old_access;
      memory::Protect(addr, page_size_, memory::PageAccess::kReadWrite,
                      &old_access);

      stream->Write(addr, page_size_);

      memory::Protect(addr, page_size_, old_access, nullptr);
    }
  }

  return true;
}

bool BaseHeap::Restore(ByteStream* stream) {
  XELOGD("Heap {:08X}-{:08X}", heap_base_, heap_base_ + (heap_size_ - 1));

  for (size_t i = 0; i < page_table_.size(); i++) {
    auto& page = page_table_[i];
    page.qword = stream->Read<uint64_t>();
    if (!page.state) {
      // Unallocated.
      continue;
    }

    memory::PageAccess page_access = memory::PageAccess::kNoAccess;
    if ((page.current_protect & kMemoryProtectRead) &&
        (page.current_protect & kMemoryProtectWrite)) {
      page_access = memory::PageAccess::kReadWrite;
    } else if (page.current_protect & kMemoryProtectRead) {
      page_access = memory::PageAccess::kReadOnly;
    }

    // Commit the memory if it isn't already. We do not need to reserve any
    // memory, as the mapping has already taken care of that.
    if (page.state & kMemoryAllocationCommit) {
      xe::memory::AllocFixed(TranslateRelative(i * page_size_), page_size_,
                             memory::AllocationType::kCommit,
                             memory::PageAccess::kReadWrite);
    }

    // Now read into memory. We'll set R/W protection first, then set the
    // protection back to its previous state.
    // TODO(DrChat): read compressed with snappy.
    if (page.state & kMemoryAllocationCommit) {
      void* addr = TranslateRelative(i * page_size_);
      xe::memory::Protect(addr, page_size_, memory::PageAccess::kReadWrite,
                          nullptr);

      stream->Read(addr, page_size_);

      xe::memory::Protect(addr, page_size_, page_access, nullptr);
    }
  }

  RebuildFreeBlocks();

  return true;
}

void BaseHeap::RebuildFreeBlocks() {
  free_blocks_.clear();
  uint32_t run_start = UINT32_MAX;
  for (uint32_t i = 0; i < uint32_t(page_table_.size()); ++i) {
    if (page_table_[i].state == 0) {
      if (run_start == UINT32_MAX) {
        run_start = i;
      }
    } else {
      if (run_start != UINT32_MAX) {
        free_blocks_[run_start] = i - run_start;
        run_start = UINT32_MAX;
      }
    }
  }
  if (run_start != UINT32_MAX) {
    free_blocks_[run_start] = uint32_t(page_table_.size()) - run_start;
  }
}

void BaseHeap::RemoveFreeBlock(uint32_t start_page, uint32_t page_count) {
  if (free_blocks_.empty()) {
    return;
  }

  // Find the free block that contains the allocated range.
  auto it = free_blocks_.upper_bound(start_page);
  if (it != free_blocks_.begin()) {
    --it;
  }

  // Verify the block actually contains our range.
  uint32_t block_start = it->first;
  uint32_t block_count = it->second;
  uint32_t block_end = block_start + block_count;
  assert_true(start_page >= block_start &&
              start_page + page_count <= block_end);

  free_blocks_.erase(it);

  // Insert remnant before the allocated range.
  if (block_start < start_page) {
    free_blocks_[block_start] = start_page - block_start;
  }

  // Insert remnant after the allocated range.
  uint32_t alloc_end = start_page + page_count;
  if (alloc_end < block_end) {
    free_blocks_[alloc_end] = block_end - alloc_end;
  }
}

void BaseHeap::InsertFreeBlock(uint32_t start_page, uint32_t page_count) {
  uint32_t new_start = start_page;
  uint32_t new_count = page_count;

  // Try to merge with block immediately after.
  auto it_after = free_blocks_.find(start_page + page_count);
  if (it_after != free_blocks_.end()) {
    new_count += it_after->second;
    free_blocks_.erase(it_after);
  }

  // Try to merge with block immediately before.
  auto it_at = free_blocks_.lower_bound(start_page);
  if (it_at != free_blocks_.begin()) {
    auto it_before = std::prev(it_at);
    if (it_before->first + it_before->second == start_page) {
      new_start = it_before->first;
      new_count += it_before->second;
      free_blocks_.erase(it_before);
    }
  }

  free_blocks_[new_start] = new_count;
}

void BaseHeap::Reset() {
  // TODO(DrChat): protect pages.
  std::memset(page_table_.data(), 0, sizeof(PageEntry) * page_table_.size());
  unreserved_page_count_ = uint32_t(page_table_.size());
  // TODO(Triang3l): Remove access callbacks from pages if this is a physical
  // memory heap.

  // Re-initialize free block tracker.
  free_blocks_.clear();
  free_blocks_[0] = uint32_t(page_table_.size());
}

bool BaseHeap::Alloc(uint32_t size, uint32_t alignment,
                     uint32_t allocation_type, uint32_t protect, bool top_down,
                     uint32_t* out_address) {
  *out_address = 0;
  size = xe::round_up(size, page_size_);
  alignment = xe::round_up(alignment, page_size_);

  // Exclude the top 240MB of the v40000000 heap (64KB guest pages) from
  // general allocation to protect the thread stack region
  // (0x70000000-0x7F000000)
  uint32_t heap_virtual_guest_offset = 0;
  if (heap_type_ == HeapType::kGuestVirtual && page_size_ == 0x10000) {
    heap_virtual_guest_offset = 0x0F000000;
  }

  uint32_t low_address = heap_base_;
  uint32_t high_address =
      heap_base_ + (heap_size_ - 1) - heap_virtual_guest_offset;
  return AllocRange(low_address, high_address, size, alignment, allocation_type,
                    protect, top_down, out_address);
}

bool BaseHeap::AllocFixed(uint32_t base_address, uint32_t size,
                          uint32_t alignment, uint32_t allocation_type,
                          uint32_t protect) {
  alignment = xe::round_up(alignment, page_size_);
  size = xe::align(size, alignment);
  assert_true((base_address + host_address_offset_) % alignment == 0);
  uint32_t page_count = get_page_count(size, page_size_);
  uint32_t start_page_number = (base_address - heap_base_) / page_size_;
  uint32_t end_page_number = start_page_number + page_count - 1;
  if (start_page_number >= page_table_.size() ||
      end_page_number > page_table_.size()) {
    XELOGE("BaseHeap::AllocFixed passed out of range address range");
    return false;
  }

  auto global_lock = global_critical_region_.Acquire();

  // - If we are reserving, the entire range must not be already reserved.
  // - If we are committing it's ok for pages within the range to already be
  //   committed.
  const bool is_pure_reserve = allocation_type == kMemoryAllocationReserve;
  for (uint32_t page_number = start_page_number; page_number <= end_page_number;
       ++page_number) {
    uint32_t state = page_table_[page_number].state;
    if ((allocation_type == kMemoryAllocationReserve) && state) {
      // Already reserved.
      XELOGE(
          "BaseHeap::AllocFixed attempting to reserve an already reserved "
          "range");
      return false;
    }
    if ((allocation_type == kMemoryAllocationCommit) &&
        !(state & kMemoryAllocationReserve)) {
      // Attempting a commit-only op on an unreserved page.
      // This may be OK.
      XELOGW("BaseHeap::AllocFixed attempting commit on unreserved page");
      allocation_type |= kMemoryAllocationReserve;
      break;
    }
  }

  // Allocate from host.
  if (allocation_type == kMemoryAllocationReserve) {
    // Reserve is not needed, as we are mapped already.
  } else {
    if (!ShouldSkipHostCommit(*this)) {
      auto alloc_type = (allocation_type & kMemoryAllocationCommit)
                            ? xe::memory::AllocationType::kCommit
                            : xe::memory::AllocationType::kReserve;
      void* result = xe::memory::AllocFixed(
          TranslateRelative(start_page_number * page_size_),
          page_count * page_size_, alloc_type, ToPageAccess(protect));
      if (!result) {
        XELOGE("BaseHeap::AllocFixed failed to alloc range from host");
        return false;
      }

      if (cvars::scribble_heap && protect & kMemoryProtectWrite) {
        RandomizeMemory(result, page_count * page_size_);
      }
    } else if (cvars::scribble_heap && protect & kMemoryProtectWrite) {
      RandomizeMemory(TranslateRelative(start_page_number * page_size_),
                      page_count * page_size_);
    }
  }

  // Set page state.
  bool had_free_pages = false;
  for (uint32_t page_number = start_page_number; page_number <= end_page_number;
       ++page_number) {
    auto& page_entry = page_table_[page_number];
    if (allocation_type & kMemoryAllocationReserve) {
      // Region is based on reservation.
      page_entry.base_address = start_page_number;
      page_entry.region_page_count = page_count;
    }
    page_entry.allocation_protect = protect;
    page_entry.current_protect = protect;
    if (!(page_entry.state & kMemoryAllocationReserve)) {
      had_free_pages = true;
      unreserved_page_count_--;
    }
    page_entry.state = kMemoryAllocationReserve | allocation_type;
  }

  // Update free block tracker if any pages transitioned from free.
  if (had_free_pages) {
    if (is_pure_reserve) {
      // Pure reserve: validation confirmed all pages were free, so the range
      // is within a single coalesced free block.
      RemoveFreeBlock(start_page_number, page_count);
    } else {
      // Mixed state (commit upgraded to reserve+commit): pages may span
      // multiple free blocks, rebuild from page_table_.
      RebuildFreeBlocks();
    }
  }

  return true;
}
template <typename T>
static inline T QuickMod(T value, uint32_t modv) {
  if (xe::is_pow2(modv)) {
    return value & (modv - 1);
  } else {
    return value % modv;
  }
}

bool BaseHeap::AllocRange(uint32_t low_address, uint32_t high_address,
                          uint32_t size, uint32_t alignment,
                          uint32_t allocation_type, uint32_t protect,
                          bool top_down, uint32_t* out_address) {
  *out_address = 0;

  alignment = xe::round_up(alignment, page_size_);
  uint32_t page_count = get_page_count(size, page_size_);
  low_address = std::max(heap_base_, xe::align(low_address, alignment));
  high_address = std::min(heap_base_ + (heap_size_ - 1),
                          xe::align(high_address, alignment));

  uint32_t low_page_number = (low_address - heap_base_) >> page_size_shift_;
  uint32_t high_page_number = (high_address - heap_base_) >> page_size_shift_;
  low_page_number = std::min(uint32_t(page_table_.size()) - 1, low_page_number);
  high_page_number =
      std::min(uint32_t(page_table_.size()) - 1, high_page_number);

  // high_page_number is an INCLUSIVE bound (high_address is the last valid
  // byte, and BaseHeap::AllocRange clamps it to heap_base_ + heap_size_ - 1),
  // so the number of pages in [low, high] is the difference PLUS ONE. Without
  // the +1 a request that exactly fills the range is rejected - Halo 4 hit
  // this asking for 126640 pages against a computed span of 126639.
  // Toggle: debug.canary.fix_allocrange_inclusive (default ON).
  const uint32_t inclusive_span =
      (high_page_number - low_page_number) +
      (XE_AE_FIX_ENABLED("debug.canary.fix_allocrange_inclusive") ? 1u : 0u);
  if (page_count > inclusive_span) {
    // HEAPDIAG: Halo 4 dies during boot after eight failed
    // MmAllocatePhysicalMemoryEx calls, the last of which asks for only 64 KB
    // while the parent heap reports ~492 MB free. "Not enough room" cannot
    // explain that, so log the actual bounds rather than guessing which of
    // range, alignment or tracker state is at fault.
    XELOGE(
        "BaseHeap::Alloc page count too big for requested range "
        "[HEAPDIAG lo={:08X} hi={:08X} size={} align={} pages={} "
        "lo_pg={} hi_pg={} span_pg={} top_down={}]",
        low_address, high_address, size, alignment, page_count, low_page_number,
        high_page_number, high_page_number - low_page_number, top_down ? 1 : 0);
    return false;
  }

  auto global_lock = global_critical_region_.Acquire();

  // Find a free page range using the free block tracker.
  // The base page must match the requested alignment.
  uint32_t start_page_number = UINT_MAX;
  uint32_t end_page_number = UINT_MAX;
  uint32_t page_scan_stride = alignment >> page_size_shift_;

  if (top_down) {
    // Search free blocks from high addresses downward.
    // Find the first block that could overlap our range.
    auto it = free_blocks_.upper_bound(high_page_number);
    while (it != free_blocks_.begin()) {
      --it;
      uint32_t block_start = it->first;
      uint32_t block_count = it->second;
      uint32_t block_end = block_start + block_count;

      // Block is entirely below our search range — stop.
      if (block_end <= low_page_number) {
        break;
      }

      // Skip blocks too small to possibly fit.
      if (block_count < page_count) {
        continue;
      }

      // Compute the highest aligned start within this block and range.
      // high_page_number is exclusive and rounded down to the stride, so
      // the top stride of pages is never returned.
      // Same inclusive-bound correction as the span guard above: the top
      // page of the range is usable, so convert to an exclusive end before
      // aligning down. Treating it as exclusive threw away up to a full
      // stride (64 KB) at the top of the heap.
      const uint32_t high_exclusive =
          high_page_number +
          (XE_AE_FIX_ENABLED("debug.canary.fix_allocrange_inclusive") ? 1u
                                                                      : 0u);
      uint32_t high_aligned =
          high_exclusive - QuickMod(high_exclusive, page_scan_stride);
      uint32_t usable_end = std::min(block_end, high_aligned);
      if (usable_end < page_count) {
        continue;
      }
      uint32_t latest_start = usable_end - page_count;
      // Align down to stride.
      latest_start -= QuickMod(latest_start, page_scan_stride);
      uint32_t usable_start = std::max(block_start, low_page_number);
      if (latest_start >= usable_start &&
          latest_start + page_count <= block_end) {
        start_page_number = latest_start;
        end_page_number = latest_start + page_count - 1;
        break;
      }
    }
  } else {
    // Search free blocks from low addresses upward.
    auto it = free_blocks_.lower_bound(low_page_number);
    // Check if the previous block extends into our range.
    if (it != free_blocks_.begin()) {
      auto prev = std::prev(it);
      if (prev->first + prev->second > low_page_number) {
        it = prev;
      }
    }
    for (; it != free_blocks_.end(); ++it) {
      uint32_t block_start = it->first;
      uint32_t block_count = it->second;
      uint32_t block_end = block_start + block_count;

      // Block is entirely above our search range — stop.
      if (block_start > high_page_number) {
        break;
      }

      // Skip blocks too small to possibly fit.
      if (block_count < page_count) {
        continue;
      }

      // Compute the lowest aligned start within this block and range.
      // high_page_number is treated as exclusive — the page at
      // high_page_number itself is never returned.
      uint32_t earliest = std::max(block_start, low_page_number);
      uint32_t aligned_start = xe::round_up(earliest, page_scan_stride, false);
      const uint32_t up_high_exclusive =
          high_page_number +
          (XE_AE_FIX_ENABLED("debug.canary.fix_allocrange_inclusive") ? 1u
                                                                      : 0u);
      if (aligned_start + page_count <= block_end &&
          aligned_start + page_count <= up_high_exclusive) {
        start_page_number = aligned_start;
        end_page_number = aligned_start + page_count - 1;
        break;
      }
    }
  }

  if (start_page_number == UINT_MAX || end_page_number == UINT_MAX) {
    // Out of memory.
    // HEAPDIAG: report the bounds AND what the tracker actually holds. A
    // 64 KB request failing against ~492 MB free means either the search
    // range is wrong or the free-block tracker disagrees with reality, and
    // the largest free block distinguishes the two immediately.
    uint32_t largest_block = 0, largest_at = 0, blocks_in_range = 0;
    for (const auto& b : free_blocks_) {
      if (b.second > largest_block) {
        largest_block = b.second;
        largest_at = b.first;
      }
      if (b.first + b.second > low_page_number && b.first <= high_page_number) {
        ++blocks_in_range;
      }
    }
    XELOGE(
        "BaseHeap::Alloc failed to find contiguous range "
        "[HEAPDIAG lo={:08X} hi={:08X} size={} align={} pages={} stride={} "
        "lo_pg={} hi_pg={} top_down={} blocks={} in_range={} "
        "largest={}pg@{} ]",
        low_address, high_address, size, alignment, page_count,
        page_scan_stride, low_page_number, high_page_number, top_down ? 1 : 0,
        uint32_t(free_blocks_.size()), blocks_in_range, largest_block,
        largest_at);
    // HEAPDIAG: the free block does not reach the top of the heap either, and
    // whatever holds those pages now bounds the largest possible allocation.
    // Dump the map once so the occupants are named rather than guessed at.
    static std::atomic<bool> dumped_once{false};
    bool expected = false;
    if (dumped_once.compare_exchange_strong(expected, true)) {
      XELOGE("HEAPDIAG dumping heap map (heap_base={:08X} pages={})",
             heap_base_, uint32_t(page_table_.size()));
      DumpMap();
    }
    // assert_always("Heap exhausted!");
    return false;
  }

  // Update free block tracker.
  RemoveFreeBlock(start_page_number, page_count);

  // Allocate from host.
  if (allocation_type == kMemoryAllocationReserve) {
    // Reserve is not needed, as we are mapped already.
  } else {
    if (!ShouldSkipHostCommit(*this)) {
      auto alloc_type = (allocation_type & kMemoryAllocationCommit)
                            ? xe::memory::AllocationType::kCommit
                            : xe::memory::AllocationType::kReserve;
      void* result = xe::memory::AllocFixed(
          TranslateRelative(start_page_number << page_size_shift_),
          page_count << page_size_shift_, alloc_type, ToPageAccess(protect));
      if (!result) {
        XELOGE("BaseHeap::Alloc failed to alloc range from host");
        // Restore the free block since we failed.
        InsertFreeBlock(start_page_number, page_count);
        return false;
      }

      if (cvars::scribble_heap && (protect & kMemoryProtectWrite)) {
        RandomizeMemory(result, page_count << page_size_shift_);
      }
    } else if (cvars::scribble_heap && (protect & kMemoryProtectWrite)) {
      RandomizeMemory(TranslateRelative(start_page_number << page_size_shift_),
                      page_count << page_size_shift_);
    }
  }

  // Set page state.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number;
       ++page_number) {
    auto& page_entry = page_table_[page_number];
    page_entry.base_address = start_page_number;
    page_entry.region_page_count = page_count;
    page_entry.allocation_protect = protect;
    page_entry.current_protect = protect;
    page_entry.state = kMemoryAllocationReserve | allocation_type;
    unreserved_page_count_--;
  }

  *out_address = heap_base_ + (start_page_number << page_size_shift_);
  return true;
}

bool BaseHeap::AllocSystemHeap(uint32_t size, uint32_t alignment,
                               uint32_t allocation_type, uint32_t protect,
                               bool top_down, uint32_t* out_address) {
  *out_address = 0;
  size = xe::round_up(size, page_size_);
  alignment = xe::round_up(alignment, page_size_);

  uint32_t low_address = heap_base_;
  if (heap_type_ == xe::HeapType::kGuestVirtual) {
    // Both virtual heaps are same size, so we can assume that we substract
    // constant value.
    low_address = heap_base_ + heap_size_ - 0x10000000;
  }
  uint32_t high_address = heap_base_ + (heap_size_ - 1);
  return AllocRange(low_address, high_address, size, alignment, allocation_type,
                    protect, top_down, out_address);
}

bool BaseHeap::Decommit(uint32_t address, uint32_t size) {
  uint32_t page_count = get_page_count(size, page_size_);
  uint32_t start_page_number = (address - heap_base_) / page_size_;
  uint32_t end_page_number = start_page_number + page_count - 1;
  start_page_number =
      std::min(uint32_t(page_table_.size()) - 1, start_page_number);
  end_page_number = std::min(uint32_t(page_table_.size()) - 1, end_page_number);

  auto global_lock = global_critical_region_.Acquire();

  // Release from host.
  // TODO(benvanik): find a way to actually decommit memory;
  //     mapped memory cannot be decommitted.
  /*BOOL result =
      VirtualFree(TranslateRelative(start_page_number * page_size_),
                  page_count * page_size_, MEM_DECOMMIT);
  if (!result) {
    PLOGW("BaseHeap::Decommit failed due to host VirtualFree failure");
    return false;
  }*/

  // Perform table change.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number;
       ++page_number) {
    auto& page_entry = page_table_[page_number];
    page_entry.state &= ~kMemoryAllocationCommit;
  }

  return true;
}

bool BaseHeap::Release(uint32_t base_address, uint32_t* out_region_size) {
  auto global_lock = global_critical_region_.Acquire();

  // Given address must be a region base address.
  uint32_t base_page_number = (base_address - heap_base_) / page_size_;
  auto base_page_entry = page_table_[base_page_number];
  if (base_page_entry.base_address != base_page_number) {
    XELOGE("BaseHeap::Release failed because address is not a region start");
    return false;
  }

  if (heap_base_ == 0x00000000 && base_page_number == 0) {
    XELOGE("BaseHeap::Release: Attempt to free 0!");
    return false;
  }

  if (out_region_size) {
    *out_region_size = (base_page_entry.region_page_count * page_size_);
  }

  // Release from host not needed as mapping reserves the range for us.
  // TODO(benvanik): protect with NOACCESS?
  /*BOOL result = VirtualFree(
      TranslateRelative(base_page_number * page_size_), 0, MEM_RELEASE);
  if (!result) {
    PLOGE("BaseHeap::Release failed due to host VirtualFree failure");
    return false;
  }*/
  // Instead, we just protect it, if we can.
  if (page_size_ == xe::memory::page_size() ||
      ((base_page_entry.region_page_count * page_size_) %
               xe::memory::page_size() ==
           0 &&
       ((base_page_number * page_size_) % xe::memory::page_size() == 0))) {
    // TODO(benvanik): figure out why games are using memory after releasing
    // it. It's possible this is some virtual/physical stuff where the GPU
    // still can access it.
    if (cvars::protect_on_release) {
      if (!xe::memory::Protect(TranslateRelative(base_page_number * page_size_),
                               base_page_entry.region_page_count * page_size_,
                               xe::memory::PageAccess::kNoAccess, nullptr)) {
        XELOGW("BaseHeap::Release failed due to host VirtualProtect failure");
      }
    }
  }

  // Perform table change.
  uint32_t end_page_number =
      base_page_number + base_page_entry.region_page_count - 1;
  for (uint32_t page_number = base_page_number; page_number <= end_page_number;
       ++page_number) {
    auto& page_entry = page_table_[page_number];
    page_entry.qword = 0;
    unreserved_page_count_++;
  }

  // Insert freed block into tracker with coalescing.
  InsertFreeBlock(base_page_number, base_page_entry.region_page_count);

  return true;
}

bool BaseHeap::Protect(uint32_t address, uint32_t size, uint32_t protect,
                       uint32_t* old_protect) {
  if (!size) {
    XELOGE("BaseHeap::Protect failed due to zero size");
    return false;
  }

  // From the VirtualProtect MSDN page:
  //
  // "The region of affected pages includes all pages containing one or more
  //  bytes in the range from the lpAddress parameter to (lpAddress+dwSize).
  //  This means that a 2-byte range straddling a page boundary causes the
  //  protection attributes of both pages to be changed."
  //
  // "The access protection value can be set only on committed pages. If the
  //  state of any page in the specified region is not committed, the function
  //  fails and returns without modifying the access protection of any pages in
  //  the specified region."

  uint32_t start_page_number = (address - heap_base_) >> page_size_shift_;
  if (start_page_number >= page_table_.size()) {
    XELOGE("BaseHeap::Protect failed due to out-of-bounds base address {:08X}",
           address);
    return false;
  }
  uint32_t end_page_number =
      uint32_t((uint64_t(address) + size - 1 - heap_base_) >> page_size_shift_);
  if (end_page_number >= page_table_.size()) {
    XELOGE(
        "BaseHeap::Protect failed due to out-of-bounds range ({:08X} bytes "
        "from {:08x})",
        size, address);
    return false;
  }

  auto global_lock = global_critical_region_.Acquire();

  // Ensure all pages are in the same reserved region and all are committed.
  uint32_t first_base_address = UINT_MAX;
  for (uint32_t page_number = start_page_number; page_number <= end_page_number;
       ++page_number) {
    auto page_entry = page_table_[page_number];
    if (first_base_address == UINT_MAX) {
      first_base_address = page_entry.base_address;
    } else if (first_base_address != page_entry.base_address) {
      XELOGE("BaseHeap::Protect failed due to request spanning regions");
      return false;
    }
    if (!(page_entry.state & kMemoryAllocationCommit)) {
      XELOGE("BaseHeap::Protect failed due to uncommitted page");
      return false;
    }
  }
  uint32_t xe_page_size = static_cast<uint32_t>(xe::memory::page_size());

  uint32_t page_size_mask = xe_page_size - 1;

  // Attempt host change (hopefully won't fail).
  // We can only do this if our size matches system page granularity.
  uint32_t page_count = end_page_number - start_page_number + 1;
  bool host_offset_aligned = (host_address_offset_ & page_size_mask) == 0;
  if (page_size_ == xe_page_size ||
      (host_offset_aligned &&
       (((page_count << page_size_shift_) & page_size_mask) == 0) &&
       (((start_page_number << page_size_shift_) & page_size_mask) == 0))) {
    memory::PageAccess old_protect_access;
    if (!xe::memory::Protect(
            TranslateRelative(start_page_number << page_size_shift_),
            page_count << page_size_shift_, ToPageAccess(protect),
            old_protect ? &old_protect_access : nullptr)) {
      XELOGE("BaseHeap::Protect failed due to host VirtualProtect failure");
      return false;
    }

    if (old_protect) {
      *old_protect = FromPageAccess(old_protect_access);
    }
  } else {
    // If the host page size is larger than the guest page size, align
    // protection to host pages and use the most permissive access needed by
    // any guest page in each host page to avoid over-restricting smaller guest
    // pages within a host page.
    if (page_size_ < xe_page_size) {
      uint32_t start_offset =
          host_address_offset_ + (start_page_number << page_size_shift_);
      uint32_t end_offset = host_address_offset_ +
                            ((end_page_number + 1) << page_size_shift_) - 1;

      uint32_t aligned_start_offset = start_offset & ~page_size_mask;
      uint32_t aligned_end_offset = (end_offset | page_size_mask) + 1;

      for (uint32_t host_offset = aligned_start_offset;
           host_offset < aligned_end_offset; host_offset += xe_page_size) {
        uint32_t first_guest_page = 0;
        if (host_offset > host_address_offset_) {
          first_guest_page =
              (host_offset - host_address_offset_) >> page_size_shift_;
        }
        uint32_t host_page_end = host_offset + xe_page_size - 1;
        if (host_page_end < host_address_offset_) {
          continue;
        }
        uint32_t last_guest_page =
            (host_page_end - host_address_offset_) >> page_size_shift_;
        if (last_guest_page >= page_table_.size()) {
          last_guest_page = static_cast<uint32_t>(page_table_.size()) - 1;
        }

        xe::memory::PageAccess host_access = xe::memory::PageAccess::kNoAccess;
        for (uint32_t p = first_guest_page; p <= last_guest_page; ++p) {
          uint32_t page_prot = (p >= start_page_number && p <= end_page_number)
                                   ? protect
                                   : page_table_[p].current_protect;
          xe::memory::PageAccess page_access = ToPageAccess(page_prot);
          if (page_access == xe::memory::PageAccess::kReadWrite) {
            host_access = xe::memory::PageAccess::kReadWrite;
            break;
          }
          if (page_access == xe::memory::PageAccess::kReadOnly &&
              host_access == xe::memory::PageAccess::kNoAccess) {
            host_access = xe::memory::PageAccess::kReadOnly;
          }
        }

        xe::memory::Protect(
            reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(membase_) +
                                    heap_base_ + host_offset),
            xe_page_size, host_access, nullptr);
      }

      if (old_protect) {
        *old_protect = page_table_[start_page_number].current_protect;
      }
    } else {
      XELOGW(
          "BaseHeap::Protect: unaligned to host page size; skipping mprotect");
      if (old_protect) {
        *old_protect = page_table_[start_page_number].current_protect;
      }
      return false;
    }
  }

  // Perform table change.
  for (uint32_t page_number = start_page_number; page_number <= end_page_number;
       ++page_number) {
    auto& page_entry = page_table_[page_number];
    page_entry.current_protect = protect;
  }

  return true;
}

bool BaseHeap::QueryRegionInfo(uint32_t base_address,
                               HeapAllocationInfo* out_info) {
  uint32_t start_page_number = (base_address - heap_base_) >> page_size_shift_;
  if (start_page_number > page_table_.size()) {
    XELOGE("BaseHeap::QueryRegionInfo base page out of range");
    return false;
  }

  auto global_lock = global_critical_region_.Acquire();

  auto start_page_entry = page_table_[start_page_number];
  out_info->base_address = base_address;
  out_info->allocation_base = 0;
  out_info->allocation_protect = 0;
  out_info->region_size = 0;
  out_info->state = 0;
  out_info->protect = 0;
  if (start_page_entry.state) {
    // Committed/reserved region.
    out_info->allocation_base =
        heap_base_ + (start_page_entry.base_address << page_size_shift_);
    out_info->allocation_protect = start_page_entry.allocation_protect;
    out_info->allocation_size = start_page_entry.region_page_count
                                << page_size_shift_;
    out_info->state = start_page_entry.state;
    out_info->protect = start_page_entry.current_protect;

    // Scan forward and report the size of the region matching the initial
    // base address's attributes.
    for (uint32_t page_number = start_page_number;
         page_number <
         start_page_entry.base_address + start_page_entry.region_page_count;
         ++page_number) {
      auto page_entry = page_table_[page_number];
      if (page_entry.base_address != start_page_entry.base_address ||
          page_entry.state != start_page_entry.state ||
          page_entry.current_protect != start_page_entry.current_protect) {
        // Different region or different properties within the region; done.
        break;
      }
      out_info->region_size += page_size_;
    }
  } else {
    // Free region.
    for (uint32_t page_number = start_page_number;
         page_number < page_table_.size(); ++page_number) {
      auto page_entry = page_table_[page_number];
      if (page_entry.state) {
        // First non-free page; done with region.
        break;
      }
      out_info->region_size += page_size_;
    }
  }
  return true;
}

bool BaseHeap::QuerySize(uint32_t address, uint32_t* out_size) {
  uint32_t page_number = (address - heap_base_) >> page_size_shift_;
  if (page_number > page_table_.size()) {
    XELOGE("BaseHeap::QuerySize base page out of range");
    *out_size = 0;
    return false;
  }
  auto global_lock = global_critical_region_.Acquire();
  auto page_entry = page_table_[page_number];
  *out_size = (page_entry.region_page_count << page_size_shift_);
  return true;
}

bool BaseHeap::QueryBaseAndSize(uint32_t* in_out_address, uint32_t* out_size) {
  uint32_t page_number = (*in_out_address - heap_base_) >> page_size_shift_;
  if (page_number > page_table_.size()) {
    XELOGE("BaseHeap::QuerySize base page out of range");
    *out_size = 0;
    return false;
  }
  auto global_lock = global_critical_region_.Acquire();
  auto page_entry = page_table_[page_number];
  *in_out_address = (page_entry.base_address << page_size_shift_);
  *out_size = (page_entry.region_page_count << page_size_shift_);
  return true;
}

bool BaseHeap::QueryProtect(uint32_t address, uint32_t* out_protect) {
  uint32_t page_number = (address - heap_base_) >> page_size_shift_;
  if (page_number > page_table_.size()) {
    XELOGE("BaseHeap::QueryProtect base page out of range");
    *out_protect = 0;
    return false;
  }
  auto global_lock = global_critical_region_.Acquire();
  auto page_entry = page_table_[page_number];
  *out_protect = page_entry.current_protect;
  return true;
}

xe::memory::PageAccess BaseHeap::QueryRangeAccess(uint32_t low_address,
                                                  uint32_t high_address) {
  if (low_address > high_address || low_address < heap_base_ ||
      (high_address - heap_base_) >= heap_size_) {
    return xe::memory::PageAccess::kNoAccess;
  }
  uint32_t low_page_number = (low_address - heap_base_) >> page_size_shift_;
  uint32_t high_page_number = (high_address - heap_base_) >> page_size_shift_;
  bool all_readable = true;
  bool all_writable = true;
  {
    auto global_lock = global_critical_region_.Acquire();
    for (uint32_t i = low_page_number; i <= high_page_number; ++i) {
      uint32_t page_protect = page_table_[i].current_protect;
      if (!(page_protect & kMemoryProtectRead)) {
        all_readable = false;
      }
      // Check if page is writable in any form (Write or WriteCombine)
      if (!(page_protect & kMemoryProtectWrite) &&
          !(page_protect & kMemoryProtectWriteCombine)) {
        all_writable = false;
      }
    }
  }
  if (all_readable && all_writable) {
    return xe::memory::PageAccess::kReadWrite;
  } else if (all_readable) {
    return xe::memory::PageAccess::kReadOnly;
  } else {
    return xe::memory::PageAccess::kNoAccess;
  }
}

VirtualHeap::VirtualHeap() = default;

VirtualHeap::~VirtualHeap() = default;

void VirtualHeap::Initialize(Memory* memory, uint8_t* membase,
                             HeapType heap_type, uint32_t heap_base,
                             uint32_t heap_size, uint32_t page_size) {
  BaseHeap::Initialize(memory, membase, heap_type, heap_base, heap_size,
                       page_size);
}

PhysicalHeap::PhysicalHeap() : parent_heap_(nullptr) {}

PhysicalHeap::~PhysicalHeap() = default;

void PhysicalHeap::Initialize(Memory* memory, uint8_t* membase,
                              HeapType heap_type, uint32_t heap_base,
                              uint32_t heap_size, uint32_t page_size,
                              VirtualHeap* parent_heap) {
  uint32_t host_address_offset;
  if (heap_base >= 0xE0000000 &&
      xe::memory::allocation_granularity() > 0x1000) {
    host_address_offset = 0x1000;
  } else {
    host_address_offset = 0;
  }

  BaseHeap::Initialize(memory, membase, heap_type, heap_base, heap_size,
                       page_size, host_address_offset);
  parent_heap_ = parent_heap;

  // The physical base offset (host_address_offset) must be a multiple of
  // page_size. Otherwise, aligned parent allocations become misaligned after
  // translation back to virtual addresses (parent_address + heap_base_ -
  // GetPhysicalAddress(heap_base_) loses alignment).
  xenia_assert(host_address_offset % page_size == 0);

  system_page_size_ = uint32_t(xe::memory::page_size());
  xenia_assert(xe::is_pow2(system_page_size_));
  system_page_shift_ = xe::log2_floor(system_page_size_);

  system_page_count_ =
      (size_t(heap_size_) + host_address_offset + (system_page_size_ - 1)) /
      system_page_size_;
  system_page_flags_.resize((system_page_count_ + 63) / 64);
}

bool PhysicalHeap::Alloc(uint32_t size, uint32_t alignment,
                         uint32_t allocation_type, uint32_t protect,
                         bool top_down, uint32_t* out_address) {
  *out_address = 0;

  // Default top-down. Since parent heap is bottom-up this prevents
  // collisions.
  //
  // This override is why Halo 4 could not boot. Memory::SystemHeapAlloc asks
  // for bottom-up (it passes top_down=false) but the request was discarded
  // here, so xenia's OWN physical allocations - the XMA context array
  // (320 * 64 = 20480 bytes) and a few single-page SystemHeapAllocs - were
  // placed at the top of guest physical memory, at 0x1FCA7000-0x1FCB0000,
  // immediately below the 3.4 MB "?" reservation.
  //
  // Halo 4 binary-searches for the largest contiguous physical block and then
  // asks for the whole span up to that reservation, [free_start, 0x1FCB0000).
  // Those nine pages of ours sit inside it, so the request is nine pages
  // larger than the free block and fails; after a fixed number of probes the
  // title raises an exception and dies.
  //
  // Honour the caller instead. Guest allocations (MmAllocatePhysicalMemoryEx)
  // still pass top_down=true and are unaffected; only xenia's own system
  // allocations move, to the bottom of the heap where they are out of the
  // guest's way and cannot break up its contiguous space.
  //
  // Toggle: debug.canary.fix_sysheap_bottom_up (default ON; set 0 to restore
  // the unconditional top-down override).
  if (!XE_AE_FIX_ENABLED("debug.canary.fix_sysheap_bottom_up")) {
    top_down = true;
  }

  // Adjust alignment size our page size differs from the parent.
  size = xe::round_up(size, page_size_);
  alignment = xe::round_up(alignment, page_size_);

  auto global_lock = global_critical_region_.Acquire();

  // Allocate from parent heap (gets our physical address in 0-512mb).
  uint32_t parent_heap_start = GetPhysicalAddress(heap_base_);
  uint32_t parent_heap_end = GetPhysicalAddress(heap_base_ + (heap_size_ - 1));
  uint32_t parent_address;
  if (!parent_heap_->AllocRange(parent_heap_start, parent_heap_end, size,
                                alignment, allocation_type, protect, top_down,
                                &parent_address)) {
    XELOGE(
        "PhysicalHeap::Alloc unable to alloc physical memory in parent heap "
        "(requested {} bytes, parent free {}/{} pages)",
        size, parent_heap_->unreserved_page_count(),
        parent_heap_->total_page_count());
    return false;
  }

  // Given the address we've reserved in the parent heap, pin that here.
  // Shouldn't be possible for it to be allocated already.
  const uint32_t address = heap_base_ + parent_address - parent_heap_start;
  if ((address + host_address_offset_) % alignment != 0) {
    XELOGE(
        "PhysicalHeap::Alloc translated address {:08X} misaligned "
        "(alignment {:08X}, physical base offset {:08X})",
        address, alignment, parent_heap_start);
    parent_heap_->Release(parent_address);
    return false;
  }
  if (!BaseHeap::AllocFixed(address, size, alignment, allocation_type,
                            protect)) {
    XELOGE(
        "PhysicalHeap::Alloc unable to pin physical memory in physical heap");
    parent_heap_->Release(parent_address);
    return false;
  }
  *out_address = address;
  return true;
}

bool PhysicalHeap::AllocFixed(uint32_t base_address, uint32_t size,
                              uint32_t alignment, uint32_t allocation_type,
                              uint32_t protect) {
  // Adjust alignment size our page size differs from the parent.
  size = xe::round_up(size, page_size_);
  alignment = xe::round_up(alignment, page_size_);

  auto global_lock = global_critical_region_.Acquire();

  // Allocate from parent heap (gets our physical address in 0-512mb).
  // NOTE: this can potentially overwrite heap contents if there are already
  // committed pages in the requested physical range.
  // TODO(benvanik): flag for ensure-not-committed?
  uint32_t parent_base_address = GetPhysicalAddress(base_address);
  if (!parent_heap_->AllocFixed(parent_base_address, size, alignment,
                                allocation_type, protect)) {
    XELOGE(
        "PhysicalHeap::AllocFixed unable to alloc physical memory in parent "
        "heap");
    return false;
  }

  // Given the address we've reserved in the parent heap, pin that here.
  // Shouldn't be possible for it to be allocated already.
  const uint32_t address =
      heap_base_ + parent_base_address - GetPhysicalAddress(heap_base_);
  if ((address + host_address_offset_) % alignment != 0) {
    XELOGE(
        "PhysicalHeap::AllocFixed translated address {:08X} misaligned "
        "(alignment {:08X}, physical base offset {:08X})",
        address, alignment, GetPhysicalAddress(heap_base_));
    parent_heap_->Release(parent_base_address);
    return false;
  }
  if (!BaseHeap::AllocFixed(address, size, alignment, allocation_type,
                            protect)) {
    XELOGE(
        "PhysicalHeap::AllocFixed unable to pin physical memory in physical "
        "heap");
    parent_heap_->Release(parent_base_address);
    return false;
  }

  return true;
}

bool PhysicalHeap::AllocRange(uint32_t low_address, uint32_t high_address,
                              uint32_t size, uint32_t alignment,
                              uint32_t allocation_type, uint32_t protect,
                              bool top_down, uint32_t* out_address) {
  *out_address = 0;

  // Adjust alignment size our page size differs from the parent.
  size = xe::round_up(size, page_size_);
  alignment = xe::round_up(alignment, page_size_);

  auto global_lock = global_critical_region_.Acquire();

  // Allocate from parent heap (gets our physical address in 0-512mb).
  low_address = std::max(heap_base_, low_address);
  high_address = std::min(heap_base_ + (heap_size_ - 1), high_address);
  uint32_t parent_low_address = GetPhysicalAddress(low_address);
  uint32_t parent_high_address = GetPhysicalAddress(high_address);
  uint32_t parent_address;
  if (!parent_heap_->AllocRange(parent_low_address, parent_high_address, size,
                                alignment, allocation_type, protect, top_down,
                                &parent_address)) {
    XELOGE(
        "PhysicalHeap::AllocRange unable to alloc physical memory in parent "
        "heap (requested {} bytes, parent free {}/{} pages)",
        size, parent_heap_->unreserved_page_count(),
        parent_heap_->total_page_count());
    return false;
  }
  // Given the address we've reserved in the parent heap, pin that here.
  // Shouldn't be possible for it to be allocated already.
  const uint32_t address =
      heap_base_ + parent_address - GetPhysicalAddress(heap_base_);
  if ((address + host_address_offset_) % alignment != 0) {
    XELOGE(
        "PhysicalHeap::AllocRange translated address {:08X} misaligned "
        "(alignment {:08X}, physical base offset {:08X})",
        address, alignment, GetPhysicalAddress(heap_base_));
    parent_heap_->Release(parent_address);
    return false;
  }
  if (!BaseHeap::AllocFixed(address, size, alignment, allocation_type,
                            protect)) {
    XELOGE(
        "PhysicalHeap::AllocRange unable to pin physical memory in physical "
        "heap");
    parent_heap_->Release(parent_address);
    return false;
  }
  *out_address = address;
  return true;
}

bool PhysicalHeap::AllocSystemHeap(uint32_t size, uint32_t alignment,
                                   uint32_t allocation_type, uint32_t protect,
                                   bool top_down, uint32_t* out_address) {
  return Alloc(size, alignment, allocation_type, protect, top_down,
               out_address);
}

bool PhysicalHeap::Decommit(uint32_t address, uint32_t size) {
  auto global_lock = global_critical_region_.Acquire();

  uint32_t parent_address = GetPhysicalAddress(address);
  if (!parent_heap_->Decommit(parent_address, size)) {
    XELOGE("PhysicalHeap::Decommit failed due to parent heap failure");
    return false;
  }

  // Not caring about the contents anymore.
  TriggerCallbacks(std::move(global_lock), address, size, true, true);

  return BaseHeap::Decommit(address, size);
}

bool PhysicalHeap::Release(uint32_t base_address, uint32_t* out_region_size) {
  auto global_lock = global_critical_region_.Acquire();

  uint32_t parent_base_address = GetPhysicalAddress(base_address);
  if (!parent_heap_->Release(parent_base_address, out_region_size)) {
    XELOGE("PhysicalHeap::Release failed due to parent heap failure");
    return false;
  }

  // Must invalidate here because the range being released may be reused in
  // another mapping of physical memory - but callback flags are set in each
  // heap separately (https://github.com/xenia-project/xenia/issues/1559 -
  // dynamic vertices in 4D5307F2 start screen and menu allocated in 0xA0000000
  // at addresses that overlap intro video textures in 0xE0000000, with the
  // state of the allocator as of February 24th, 2020). If memory is invalidated
  // in Alloc instead, Alloc won't be aware of callbacks enabled in other heaps,
  // thus callback handlers will keep considering this range valid forever.
  uint32_t region_size;
  if (QuerySize(base_address, &region_size)) {
    TriggerCallbacks(std::move(global_lock), base_address, region_size, true,
                     true);
  }

  return BaseHeap::Release(base_address, out_region_size);
}

bool PhysicalHeap::Protect(uint32_t address, uint32_t size, uint32_t protect,
                           uint32_t* old_protect) {
  auto global_lock = global_critical_region_.Acquire();

  // Only invalidate if making writable again, for simplicity - not when simply
  // marking some range as immutable, for instance.
  if (protect & kMemoryProtectWrite) {
    TriggerCallbacks(std::move(global_lock), address, size, true, true, false);
  }

  if (!parent_heap_->Protect(GetPhysicalAddress(address), size, protect,
                             old_protect)) {
    XELOGE("PhysicalHeap::Protect failed due to parent heap failure");
    return false;
  }

  return BaseHeap::Protect(address, size, protect);
}

void PhysicalHeap::EnableAccessCallbacks(uint32_t physical_address,
                                         uint32_t length,
                                         bool enable_invalidation_notifications,
                                         bool enable_data_providers) {
  // TODO(Triang3l): Implement data providers.
  assert_false(enable_data_providers);
  if (!enable_invalidation_notifications && !enable_data_providers) {
    return;
  }
  uint32_t physical_address_offset = GetPhysicalAddress(heap_base_);
  if (physical_address < physical_address_offset) {
    if (physical_address_offset - physical_address >= length) {
      return;
    }
    length -= physical_address_offset - physical_address;
    physical_address = physical_address_offset;
  }
  uint32_t heap_relative_address = physical_address - physical_address_offset;
  if (heap_relative_address >= heap_size_) {
    return;
  }
  length = std::min(length, heap_size_ - heap_relative_address);
  if (length == 0) {
    return;
  }

  uint32_t system_page_first =
      (heap_relative_address + host_address_offset()) >> system_page_shift_;
  swcache::PrefetchL1(&system_page_flags_[system_page_first >> 6]);
  uint32_t system_page_last =
      (heap_relative_address + length - 1 + host_address_offset()) >>
      system_page_shift_;
  system_page_last = std::min(system_page_last, system_page_count_ - 1);
  assert_true(system_page_first <= system_page_last);

  // Update callback flags for system pages and make their protection stricter
  // if needed.
  xe::memory::PageAccess protect_access =
      enable_data_providers ? xe::memory::PageAccess::kNoAccess
                            : xe::memory::PageAccess::kReadOnly;

  auto global_lock = global_critical_region_.Acquire();
  if (enable_invalidation_notifications) {
    EnableAccessCallbacksInner<true>(system_page_first, system_page_last,
                                     protect_access);
  } else {
    EnableAccessCallbacksInner<false>(system_page_first, system_page_last,
                                      protect_access);
  }
}

template <bool enable_invalidation_notifications>
XE_NOINLINE void PhysicalHeap::EnableAccessCallbacksInner(
    const uint32_t system_page_first, const uint32_t system_page_last,
    xe::memory::PageAccess protect_access) XE_RESTRICT {
  uint8_t* protect_base = membase_ + heap_base_;
  uint32_t protect_system_page_first = UINT32_MAX;

  SystemPageFlagsBlock* XE_RESTRICT sys_page_flags = system_page_flags_.data();
  PageEntry* XE_RESTRICT page_table_ptr = page_table_.data();

  // chrispy: a lot of time is spent in this loop, and i think some of the work
  // may be avoidable and repetitive profiling shows quite a bit of time spent
  // in this loop, but very little spent actually calling Protect
  uint32_t i = system_page_first;

  uint32_t first_guest_page = SystemPagenumToGuestPagenum(system_page_first);
  uint32_t last_guest_page = SystemPagenumToGuestPagenum(system_page_last);

  uint32_t guest_one = SystemPagenumToGuestPagenum(1);

  uint32_t system_one = GuestPagenumToSystemPagenum(1);
  for (; i <= system_page_last; ++i) {
    // Check if need to enable callbacks for the page and raise its protection.
    //
    // If enabling invalidation notifications:
    // - Page writable and not watched for changes yet - protect and enable
    //   invalidation notifications.
    // - Page seen as writable by the guest, but only needs data providers -
    //   just set the bits to enable invalidation notifications (already has
    //   even stricter protection than needed).
    // - Page not writable as requested by the game - don't do anything (need
    //   real access violations here).
    // If enabling data providers:
    // - Page accessible (either read/write or read-only) and didn't need data
    //   providers initially - protect and enable data providers.
    // - Otherwise - do nothing.
    //
    // It's safe not to await data provider completion here before protecting as
    // this never makes protection lighter, so it can't interfere with page
    // faults that await data providers.
    //
    // Enabling data providers doesn't need to be deferred - providers will be
    // polled for the last time without releasing the lock.
    SystemPageFlagsBlock& page_flags_block = sys_page_flags[i >> 6];

#if XE_ARCH_AMD64 == 1
    // x86 modulus shift
    uint64_t page_flags_bit = uint64_t(1) << i;
#else
    uint64_t page_flags_bit = uint64_t(1) << (i & 63);
#endif

    uint32_t guest_page_number = SystemPagenumToGuestPagenum(i);
    xe::memory::PageAccess current_page_access =
        ToPageAccess(page_table_ptr[guest_page_number].current_protect);
    bool protect_system_page = false;
    // Don't do anything with inaccessible pages - don't protect, don't enable
    // callbacks - because real access violations are needed there. And don't
    // enable invalidation notifications for read-only pages for the same
    // reason.
    if (current_page_access != xe::memory::PageAccess::kNoAccess) {
      // TODO(Triang3l): Enable data providers.
      if constexpr (enable_invalidation_notifications) {
        if (current_page_access != xe::memory::PageAccess::kReadOnly &&
            (page_flags_block.notify_on_invalidation & page_flags_bit) == 0) {
          // TODO(Triang3l): Check if data providers are already enabled.
          // If data providers are already enabled for the page, it has even
          // stricter protection.
          protect_system_page = true;
          page_flags_block.notify_on_invalidation |= page_flags_bit;
        }
      }
    }
    if (protect_system_page) {
      if (protect_system_page_first == UINT32_MAX) {
        protect_system_page_first = i;
      }
    } else {
      if (protect_system_page_first != UINT32_MAX) {
        xe::memory::Protect(
            protect_base + (protect_system_page_first << system_page_shift_),
            (i - protect_system_page_first) << system_page_shift_,
            protect_access);
        protect_system_page_first = UINT32_MAX;
      }
    }
  }

  if (protect_system_page_first != UINT32_MAX) {
    xe::memory::Protect(
        protect_base + (protect_system_page_first << system_page_shift_),
        (system_page_last + 1 - protect_system_page_first)
            << system_page_shift_,
        protect_access);
  }
}
bool PhysicalHeap::TriggerCallbacks(
    global_unique_lock_type global_lock_locked_once, uint32_t virtual_address,
    uint32_t length, bool is_write, bool unwatch_exact_range, bool unprotect) {
  // TODO(Triang3l): Support read watches.
  assert_true(is_write);
  if (!is_write) {
    return false;
  }

  if (virtual_address < heap_base_) {
    if (heap_base_ - virtual_address >= length) {
      return false;
    }
    length -= heap_base_ - virtual_address;
    virtual_address = heap_base_;
  }
  uint32_t heap_relative_address = virtual_address - heap_base_;
  if (heap_relative_address >= heap_size_) {
    return false;
  }
  length = std::min(length, heap_size_ - heap_relative_address);
  if (length == 0) {
    return false;
  }

  uint32_t system_page_first =
      (heap_relative_address + host_address_offset()) >> system_page_shift_;
  uint32_t system_page_last =
      (heap_relative_address + length - 1 + host_address_offset()) >>
      system_page_shift_;
  system_page_last = std::min(system_page_last, system_page_count_ - 1);
  assert_true(system_page_first <= system_page_last);
  uint32_t block_index_first = system_page_first >> 6;
  uint32_t block_index_last = system_page_last >> 6;

  // Check if watching any page, whether need to call the callback at all.
  bool any_watched = false;
  for (uint32_t i = block_index_first; i <= block_index_last; ++i) {
    uint64_t block = system_page_flags_[i].notify_on_invalidation;
    if (i == block_index_first) {
      block &= ~((uint64_t(1) << (system_page_first & 63)) - 1);
    }
    if (i == block_index_last && (system_page_last & 63) != 63) {
      block &= (uint64_t(1) << ((system_page_last & 63) + 1)) - 1;
    }
    if (block) {
      any_watched = true;
      break;
    }
  }
  if (!any_watched) {
    // No watches on this page — another thread already cleared them (race
    // condition between the fault firing and acquiring the lock). Return true
    // so the faulting instruction retries; the page is now unprotected and the
    // access will succeed. This is the signal-safe equivalent of the
    // QueryProtect check in the non-Linux path of
    // MMIOHandler::ExceptionCallback.
    return true;
  }

  // Trigger callbacks.
  if (!unprotect) {
    // If not doing anything with protection, no point in unwatching excess
    // pages.
    unwatch_exact_range = true;
  }
  uint32_t physical_address_offset = GetPhysicalAddress(heap_base_);
  uint32_t physical_address_start =
      xe::sat_sub(system_page_first << system_page_shift_,
                  host_address_offset()) +
      physical_address_offset;
  uint32_t physical_length = std::min(
      xe::sat_sub((system_page_last << system_page_shift_) + system_page_size_,
                  host_address_offset()) +
          physical_address_offset - physical_address_start,
      heap_size_ - (physical_address_start - physical_address_offset));
  uint32_t unwatch_first = 0;
  uint32_t unwatch_last = UINT32_MAX;
  for (auto invalidation_callback :
       memory_->physical_memory_invalidation_callbacks_) {
    std::pair<uint32_t, uint32_t> callback_unwatch_range =
        invalidation_callback->first(invalidation_callback->second,
                                     physical_address_start, physical_length,
                                     unwatch_exact_range);
    if (!unwatch_exact_range) {
      unwatch_first = std::max(unwatch_first, callback_unwatch_range.first);
      unwatch_last = std::min(
          unwatch_last,
          xe::sat_add(
              callback_unwatch_range.first,
              std::max(callback_unwatch_range.second, uint32_t(1)) - 1));
    }
  }
  if (!unwatch_exact_range) {
    // Always unwatch at least the requested pages.
    unwatch_first = std::min(unwatch_first, physical_address_start);
    unwatch_last =
        std::max(unwatch_last, physical_address_start + physical_length - 1);
    // Don't unprotect too much if not caring much about the region (limit to
    // 4 MB - somewhat random, but max 1024 iterations of the page loop).
    constexpr uint32_t kMaxUnwatchExcess = 4 * 1024 * 1024;
    unwatch_first = std::max(unwatch_first,
                             physical_address_start & ~(kMaxUnwatchExcess - 1));
    unwatch_last =
        std::min(unwatch_last, (physical_address_start + physical_length - 1) |
                                   (kMaxUnwatchExcess - 1));
    // Convert to heap-relative addresses.
    unwatch_first = xe::sat_sub(unwatch_first, physical_address_offset);
    unwatch_last = xe::sat_sub(unwatch_last, physical_address_offset);
    // Clamp to the heap upper bound.
    unwatch_first = std::min(unwatch_first, heap_size_ - 1);
    unwatch_last = std::min(unwatch_last, heap_size_ - 1);
    // Convert to system pages and update the range.
    unwatch_first += host_address_offset();
    unwatch_last += host_address_offset();
    assert_true(unwatch_first <= unwatch_last);
    system_page_first = unwatch_first >> system_page_shift_;
    system_page_last = unwatch_last >> system_page_shift_;
    block_index_first = system_page_first >> 6;
    block_index_last = system_page_last >> 6;
  }

  // Unprotect ranges that need unprotection.
  if (unprotect) {
    uint8_t* protect_base = membase_ + heap_base_;
    uint32_t unprotect_system_page_first = UINT32_MAX;
    for (uint32_t i = system_page_first; i <= system_page_last; ++i) {
      // Check if need to allow writing to this page.
      bool unprotect_page = (system_page_flags_[i >> 6].notify_on_invalidation &
                             (uint64_t(1) << (i & 63))) != 0;
      if (unprotect_page) {
        uint32_t guest_page_number =
            xe::sat_sub(i << system_page_shift_, host_address_offset()) >>
            page_size_shift_;
        if (ToPageAccess(page_table_[guest_page_number].current_protect) !=
            xe::memory::PageAccess::kReadWrite) {
          unprotect_page = false;
        }
      }
      if (unprotect_page) {
        if (unprotect_system_page_first == UINT32_MAX) {
          unprotect_system_page_first = i;
        }
      } else {
        if (unprotect_system_page_first != UINT32_MAX) {
          xe::memory::Protect(
              protect_base +
                  (unprotect_system_page_first << system_page_shift_),
              (i - unprotect_system_page_first) << system_page_shift_,
              xe::memory::PageAccess::kReadWrite);
          unprotect_system_page_first = UINT32_MAX;
        }
      }
    }
    if (unprotect_system_page_first != UINT32_MAX) {
      xe::memory::Protect(
          protect_base + (unprotect_system_page_first << system_page_shift_),
          (system_page_last + 1 - unprotect_system_page_first)
              << system_page_shift_,
          xe::memory::PageAccess::kReadWrite);
    }
  }

  // Mark pages as not write-watched.
  for (uint32_t i = block_index_first; i <= block_index_last; ++i) {
    uint64_t mask = 0;
    if (i == block_index_first) {
      mask |= (uint64_t(1) << (system_page_first & 63)) - 1;
    }
    if (i == block_index_last && (system_page_last & 63) != 63) {
      mask |= ~((uint64_t(1) << ((system_page_last & 63) + 1)) - 1);
    }
    system_page_flags_[i].notify_on_invalidation &= mask;
  }

  return true;
}

uint32_t PhysicalHeap::GetPhysicalAddress(uint32_t address) const {
  assert_true(address >= heap_base_);
  address -= heap_base_;
  assert_true(address < heap_size_);
  if (heap_base_ >= 0xE0000000) {
    address += 0x1000;
  }
  return address;
}

}  // namespace xe
