/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_command_processor.h"

#include <cstdint>
#include <cstring>

#include <unordered_set>

#include "xenia/base/ae_fix_toggle.h"  // TESTRIG(regression-bisect)
#include "xenia/base/ae_fps.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/testrig_debug_server.h"  // TESTRIG(gpu)
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/packet_disassembler.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/vulkan/vulkan_pipeline_cache.h"
#include "xenia/gpu/vulkan/vulkan_render_target_cache.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/ui/vulkan/vulkan_presenter.h"
#include "xenia/ui/vulkan/vulkan_util.h"

DECLARE_bool(clear_memory_page_state);

DECLARE_bool(vulkan_user_clip_planes);

namespace xe {
namespace gpu {
namespace vulkan {

// Generated with `xb buildshaders`.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_pwl_fxaa_luma_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_pwl_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_table_fxaa_luma_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_table_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/fullscreen_cw_vs.h"
}  // namespace shaders

constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeUniformBuffer = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        SpirvShaderTranslator::kConstantBufferCount*
            kLinkedTypeDescriptorPoolSetCount};

constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeStorageBuffer = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kLinkedTypeDescriptorPoolSetCount};

// 2x descriptors for texture images because of unsigned and signed bindings.
constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeTextures[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         2 * kLinkedTypeDescriptorPoolSetCount},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kLinkedTypeDescriptorPoolSetCount},
};

VulkanCommandProcessor::VulkanCommandProcessor(
    VulkanGraphicsSystem* graphics_system, kernel::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state),
      deferred_command_buffer_(*this),
      transient_descriptor_allocator_uniform_buffer_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeUniformBuffer, 1,
          kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_storage_buffer_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeStorageBuffer, 1,
          kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_textures_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          kDescriptorPoolSizeTextures,
          uint32_t(xe::countof(kDescriptorPoolSizeTextures)),
          kLinkedTypeDescriptorPoolSetCount) {}

VulkanCommandProcessor::~VulkanCommandProcessor() = default;

void VulkanCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  cache_clear_requested_ = true;
}

void VulkanCommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) {
  if (pipeline_cache_) {
    pipeline_cache_->InitializePipelineCache(cache_root, title_id);
  }
}

void VulkanCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr,
                                                      uint32_t length) {
  shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  primitive_processor_->MemoryInvalidationCallback(base_ptr, length, true);
}

void VulkanCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {}

std::string VulkanCommandProcessor::GetWindowTitleText() const {
  std::ostringstream title;
  title << "Vulkan";
  if (render_target_cache_) {
    switch (render_target_cache_->GetPath()) {
      case RenderTargetCache::Path::kHostRenderTargets:
        title << " - FBO";
        break;
      case RenderTargetCache::Path::kPixelShaderInterlock:
        title << " - FSI";
        break;
      default:
        break;
    }
    uint32_t draw_resolution_scale_x =
        texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
    uint32_t draw_resolution_scale_y =
        texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
    if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
      title << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
    }
  }
  title << " - HEAVILY INCOMPLETE, early development";
  return title.str();
}

bool VulkanCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    XELOGE("Failed to initialize base command processor context");
    return false;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();

  // The unconditional inclusion of the vertex shader stage also covers the case
  // of manual index / factor buffer fetch (the system constants and the shared
  // memory are needed for that) in the tessellation vertex shader when
  // fullDrawIndexUint32 is not supported.
  guest_shader_pipeline_stages_ = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  guest_shader_vertex_stages_ = VK_SHADER_STAGE_VERTEX_BIT;
  if (device_properties.tessellationShader) {
    guest_shader_pipeline_stages_ |=
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    guest_shader_vertex_stages_ |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  }
  // Decide whether memory export from vertex shaders must be emulated with a
  // compute dispatch. Required when vertex-stage stores are entirely
  // unavailable, and also forced on tiled/binning GPUs (Adreno) which advertise
  // vertexPipelineStoresAndAtomics but do not reliably run vertex-stage stores
  // (the vertex shader executes in a position-only binning pass, so its stores
  // are stripped/duplicated - e.g. Halo 3's memexport-generated menu geometry
  // renders as garbage). Compute stores are reliable on all GPUs, so this is
  // correct everywhere.
  // Qualcomm's Vulkan vendor ID (Adreno).
  constexpr uint32_t kVendorIdQualcomm = 0x5143;
  // TESTRIG(memexport): OPTION 2 (compute-memexport dispatch, see
  // GetOrCreateMemExportComputePipeline / kMemExportCompute below) was tested
  // head-to-head against OPTION 1 (rasterizer-discard vertex stores, see
  // vulkan_pipeline_cache.cc) on the Halo 3 menu terrain buffer: 5-sample
  // cold-boot mean 6.32% (4.82-7.14%) for compute vs. OPTION 1's established
  // ~6.5-7.2% baseline - statistically indistinguishable, same noise band.
  // Re-test 2026-07-24: this was disabled based on a "trunc()-precision-
  // divergence" theory (see project_xenia_ae_renderdoc_findings.md, July 12)
  // that has since been DISPROVEN - an isolated GLSL-intrinsic probe (see
  // tools/adreno_probe/) proved trunc/floor/fract/sqrt/rsqrt/exp2/log2 are all
  // exact on Adreno; only sin/cos was broken, and that's already fixed
  // (Cody-Waite, a0b2f29e). Re-enabling to test with the corrected excuse
  // removed: this dispatches exactly one compute invocation per guest vertex
  // index (no bounds check, no tile-binning pass, no post-transform cache -
  // see the CmdVkDispatch call below), so if it STILL underfills the
  // memexport buffer as badly as the vertex-store path, that pins the bug
  // inside the shader's own r0.y slot-selection math/constants, not on
  // Adreno's store-landing reliability.
  // TESTRIG(gpu): compute-memexport is now a RUNTIME toggle driven from the
  // Debug menu (Settings > Debug > "Compute memexport"), so it can be flipped
  // without a rebuild:
  //     adb shell setprop debug.canary.memexport_compute 1   (on)
  //     adb shell setprop debug.canary.memexport_compute 0   (off, default)
  // Read once here because the flag also decides pipeline/descriptor setup, so a
  // game restart is required for a change to take effect.
  //
  // DEFAULT IS OFF. Enabling it did NOT fix the Halo 3 skinned-geometry collapse
  // (proven 2026-07-25: buffer fill rose 60x with no visual change) and it is the
  // prime suspect for an NFS Carbon regression seen 2026-07-26 (freeze at the
  // first load screen - GPU ring buffer empty, read_ptr==write_ptr, CP spinning
  // ~938k iterations while the guest stopped submitting).
  memexport_use_compute_ =
      xe::testrig::internal::PropertyBool("debug.canary.memexport_compute", false);
  XELOGI("memexport_use_compute = {}", memexport_use_compute_);
  (void)kVendorIdQualcomm;
  if (memexport_use_compute_) {
    // For memory export from vertex shaders converted to compute shaders - the
    // shared memory and constant descriptor sets and the shared memory barriers
    // must include the compute stage.
    guest_shader_pipeline_stages_ |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    guest_shader_vertex_stages_ |= VK_SHADER_STAGE_COMPUTE_BIT;
  }

  // TESTRIG(gpu): expose live GPU command-processor state - see
  // docs/TEST_HARNESS.md.
  xe::testrig::Expose(
      xe::testrig::kPortGpu, "gpu", [this, device_properties]() {
        return fmt::format(
            "device: {}\n"
            "vendorID: 0x{:04X}\n"
            "memexport_use_compute: {}\n"
            "total_draws: {}\n"
            "total_memexport_draws: {}\n"
            "total_memexport_compute_dispatches: {}\n"
            "total_memexport_compute_pipeline_failures: {}",
            device_properties.deviceName, device_properties.vendorID,
            memexport_use_compute_, testrig_total_draws_,
            testrig_total_memexport_draws_,
            testrig_total_memexport_compute_dispatches_,
            testrig_total_memexport_compute_pipeline_failures_);
      });

  // 16384 is bigger than any single uniform buffer that Xenia needs, but is the
  // minimum maxUniformBufferRange, thus the safe minimum amount.
  uniform_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      xe::align(std::max(ui::GraphicsUploadBufferPool::kDefaultPageSize,
                         size_t(16384)),
                size_t(device_properties.minUniformBufferOffsetAlignment)));

  // Descriptor set layouts that don't depend on the setup of other subsystems.
  VkShaderStageFlags guest_shader_stages =
      guest_shader_vertex_stages_ | VK_SHADER_STAGE_FRAGMENT_BIT;
  // Empty.
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = 0;
  descriptor_set_layout_create_info.pBindings = nullptr;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_empty_) != VK_SUCCESS) {
    XELOGE("Failed to create an empty Vulkan descriptor set layout");
    return false;
  }
  // Guest draw constants.
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    VkDescriptorSetLayoutBinding& constants_binding =
        descriptor_set_layout_bindings_constants[i];
    constants_binding.binding = i;
    constants_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constants_binding.descriptorCount = 1;
    constants_binding.pImmutableSamplers = nullptr;
  }
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferSystem]
          .stageFlags =
      guest_shader_stages |
      (device_properties.tessellationShader
           ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT
           : 0) |
      (device_properties.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferFloatVertex]
          .stageFlags = guest_shader_vertex_stages_;
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferFloatPixel]
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferBoolLoop]
          .stageFlags = guest_shader_stages;
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferFetch]
          .stageFlags = guest_shader_stages;
  descriptor_set_layout_create_info.bindingCount =
      uint32_t(xe::countof(descriptor_set_layout_bindings_constants));
  descriptor_set_layout_create_info.pBindings =
      descriptor_set_layout_bindings_constants;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_constants_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for guest draw "
        "constant buffers");
    return false;
  }
  // Transient: uniform buffer for compute shaders.
  VkDescriptorSetLayoutBinding descriptor_set_layout_binding_transient;
  descriptor_set_layout_binding_transient.binding = 0;
  descriptor_set_layout_binding_transient.descriptorType =
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptor_set_layout_binding_transient.descriptorCount = 1;
  descriptor_set_layout_binding_transient.stageFlags =
      VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_binding_transient.pImmutableSamplers = nullptr;
  descriptor_set_layout_create_info.bindingCount = 1;
  descriptor_set_layout_create_info.pBindings =
      &descriptor_set_layout_binding_transient;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kUniformBufferCompute)]) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for a uniform buffer "
        "bound to the compute shader");
    return false;
  }
  // Transient: storage buffer for compute shaders.
  descriptor_set_layout_binding_transient.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_binding_transient.stageFlags =
      VK_SHADER_STAGE_COMPUTE_BIT;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageBufferCompute)]) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for a storage buffer "
        "bound to the compute shader");
    return false;
  }

  shared_memory_ = std::make_unique<VulkanSharedMemory>(
      *this, *memory_, trace_writer_, guest_shader_pipeline_stages_);
  if (!shared_memory_->Initialize()) {
    XELOGE("Failed to initialize shared memory");
    return false;
  }

  primitive_processor_ = std::make_unique<VulkanPrimitiveProcessor>(
      *register_file_, *memory_, trace_writer_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    XELOGE("Failed to initialize the geometric primitive processor");
    return false;
  }

  uint32_t shared_memory_binding_count_log2 =
      SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
          device_properties.maxStorageBufferRange);
  uint32_t shared_memory_binding_count = UINT32_C(1)
                                         << shared_memory_binding_count_log2;

  // Requires the transient descriptor set layouts.
  // Get draw resolution scale using the same method as D3D12
  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x,
                                             draw_resolution_scale_y);
  render_target_cache_ = std::make_unique<VulkanRenderTargetCache>(
      *register_file_, *memory_, trace_writer_, draw_resolution_scale_x,
      draw_resolution_scale_y, *this);
  if (!render_target_cache_->Initialize(shared_memory_binding_count)) {
    XELOGE("Failed to initialize the render target cache");
    return false;
  }

  // Shared memory and EDRAM descriptor set layout.
  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;
  VkDescriptorSetLayoutBinding
      shared_memory_and_edram_descriptor_set_layout_bindings[2];
  shared_memory_and_edram_descriptor_set_layout_bindings[0].binding = 0;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].descriptorCount =
      shared_memory_binding_count;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].stageFlags =
      guest_shader_stages;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].pImmutableSamplers =
      nullptr;
  VkDescriptorSetLayoutCreateInfo
      shared_memory_and_edram_descriptor_set_layout_create_info;
  shared_memory_and_edram_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  shared_memory_and_edram_descriptor_set_layout_create_info.pNext = nullptr;
  shared_memory_and_edram_descriptor_set_layout_create_info.flags = 0;
  shared_memory_and_edram_descriptor_set_layout_create_info.pBindings =
      shared_memory_and_edram_descriptor_set_layout_bindings;
  if (edram_fragment_shader_interlock) {
    // EDRAM.
    shared_memory_and_edram_descriptor_set_layout_bindings[1].binding = 1;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].descriptorCount =
        1;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;
    shared_memory_and_edram_descriptor_set_layout_bindings[1]
        .pImmutableSamplers = nullptr;
    shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount = 2;
  } else {
    shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount = 1;
  }
  if (dfn.vkCreateDescriptorSetLayout(
          device, &shared_memory_and_edram_descriptor_set_layout_create_info,
          nullptr,
          &descriptor_set_layout_shared_memory_and_edram_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for the shared memory "
        "and the EDRAM");
    return false;
  }

  pipeline_cache_ = std::make_unique<VulkanPipelineCache>(
      *this, *register_file_, *render_target_cache_,
      guest_shader_vertex_stages_);
  if (!pipeline_cache_->Initialize()) {
    XELOGE("Failed to initialize the graphics pipeline cache");
    return false;
  }

  // Requires the transient descriptor set layouts.
  // Use the same draw resolution scale as render target cache
  texture_cache_ = VulkanTextureCache::Create(
      *register_file_, *shared_memory_, draw_resolution_scale_x,
      draw_resolution_scale_y, *this, guest_shader_pipeline_stages_);
  if (!texture_cache_) {
    XELOGE("Failed to initialize the texture cache");
    return false;
  }

  // Shared memory and EDRAM common bindings.
  VkDescriptorPoolSize descriptor_pool_sizes[1];
  descriptor_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_pool_sizes[0].descriptorCount =
      shared_memory_binding_count + uint32_t(edram_fragment_shader_interlock);
  VkDescriptorPoolCreateInfo descriptor_pool_create_info;
  descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  descriptor_pool_create_info.pNext = nullptr;
  descriptor_pool_create_info.flags = 0;
  descriptor_pool_create_info.maxSets = 1;
  descriptor_pool_create_info.poolSizeCount = 1;
  descriptor_pool_create_info.pPoolSizes = descriptor_pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &descriptor_pool_create_info, nullptr,
                                 &shared_memory_and_edram_descriptor_pool_) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create the Vulkan descriptor pool for shared memory and "
        "EDRAM");
    return false;
  }
  VkDescriptorSetAllocateInfo descriptor_set_allocate_info;
  descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  descriptor_set_allocate_info.pNext = nullptr;
  descriptor_set_allocate_info.descriptorPool =
      shared_memory_and_edram_descriptor_pool_;
  descriptor_set_allocate_info.descriptorSetCount = 1;
  descriptor_set_allocate_info.pSetLayouts =
      &descriptor_set_layout_shared_memory_and_edram_;
  if (dfn.vkAllocateDescriptorSets(device, &descriptor_set_allocate_info,
                                   &shared_memory_and_edram_descriptor_set_) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to allocate the Vulkan descriptor set for shared memory and "
        "EDRAM");
    return false;
  }
  VkDescriptorBufferInfo
      shared_memory_descriptor_buffers_info[SharedMemory::kBufferSize /
                                            (128 << 20)];
  uint32_t shared_memory_binding_range =
      SharedMemory::kBufferSize >> shared_memory_binding_count_log2;
  for (uint32_t i = 0; i < shared_memory_binding_count; ++i) {
    VkDescriptorBufferInfo& shared_memory_descriptor_buffer_info =
        shared_memory_descriptor_buffers_info[i];
    shared_memory_descriptor_buffer_info.buffer = shared_memory_->buffer();
    shared_memory_descriptor_buffer_info.offset =
        shared_memory_binding_range * i;
    shared_memory_descriptor_buffer_info.range = shared_memory_binding_range;
  }
  VkWriteDescriptorSet write_descriptor_sets[2];
  VkWriteDescriptorSet& write_descriptor_set_shared_memory =
      write_descriptor_sets[0];
  write_descriptor_set_shared_memory.sType =
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write_descriptor_set_shared_memory.pNext = nullptr;
  write_descriptor_set_shared_memory.dstSet =
      shared_memory_and_edram_descriptor_set_;
  write_descriptor_set_shared_memory.dstBinding = 0;
  write_descriptor_set_shared_memory.dstArrayElement = 0;
  write_descriptor_set_shared_memory.descriptorCount =
      shared_memory_binding_count;
  write_descriptor_set_shared_memory.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write_descriptor_set_shared_memory.pImageInfo = nullptr;
  write_descriptor_set_shared_memory.pBufferInfo =
      shared_memory_descriptor_buffers_info;
  write_descriptor_set_shared_memory.pTexelBufferView = nullptr;
  VkDescriptorBufferInfo edram_descriptor_buffer_info;
  if (edram_fragment_shader_interlock) {
    edram_descriptor_buffer_info.buffer = render_target_cache_->edram_buffer();
    edram_descriptor_buffer_info.offset = 0;
    edram_descriptor_buffer_info.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet& write_descriptor_set_edram = write_descriptor_sets[1];
    write_descriptor_set_edram.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_descriptor_set_edram.pNext = nullptr;
    write_descriptor_set_edram.dstSet = shared_memory_and_edram_descriptor_set_;
    write_descriptor_set_edram.dstBinding = 1;
    write_descriptor_set_edram.dstArrayElement = 0;
    write_descriptor_set_edram.descriptorCount = 1;
    write_descriptor_set_edram.descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write_descriptor_set_edram.pImageInfo = nullptr;
    write_descriptor_set_edram.pBufferInfo = &edram_descriptor_buffer_info;
    write_descriptor_set_edram.pTexelBufferView = nullptr;
  }
  dfn.vkUpdateDescriptorSets(device,
                             1 + uint32_t(edram_fragment_shader_interlock),
                             write_descriptor_sets, 0, nullptr);

  // Swap objects.

  // Gamma ramp, either device-local and host-visible at once, or separate
  // device-local texel buffer and host-visible upload buffer.
  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
  // Try to create a device-local host-visible buffer first, to skip copying.
  constexpr uint32_t kGammaRampSize256EntryTable = sizeof(uint32_t) * 256;
  constexpr uint32_t kGammaRampSizePWL = sizeof(uint16_t) * 2 * 3 * 128;
  constexpr uint32_t kGammaRampSize =
      kGammaRampSize256EntryTable + kGammaRampSizePWL;
  VkBufferCreateInfo gamma_ramp_host_visible_buffer_create_info;
  gamma_ramp_host_visible_buffer_create_info.sType =
      VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  gamma_ramp_host_visible_buffer_create_info.pNext = nullptr;
  gamma_ramp_host_visible_buffer_create_info.flags = 0;
  gamma_ramp_host_visible_buffer_create_info.size =
      kGammaRampSize * kMaxFramesInFlight;
  gamma_ramp_host_visible_buffer_create_info.usage =
      VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
  gamma_ramp_host_visible_buffer_create_info.sharingMode =
      VK_SHARING_MODE_EXCLUSIVE;
  gamma_ramp_host_visible_buffer_create_info.queueFamilyIndexCount = 0;
  gamma_ramp_host_visible_buffer_create_info.pQueueFamilyIndices = nullptr;
  if (dfn.vkCreateBuffer(device, &gamma_ramp_host_visible_buffer_create_info,
                         nullptr, &gamma_ramp_buffer_) == VK_SUCCESS) {
    bool use_gamma_ramp_host_visible_buffer = false;
    VkMemoryRequirements gamma_ramp_host_visible_buffer_memory_requirements;
    dfn.vkGetBufferMemoryRequirements(
        device, gamma_ramp_buffer_,
        &gamma_ramp_host_visible_buffer_memory_requirements);
    uint32_t gamma_ramp_host_visible_buffer_memory_types =
        gamma_ramp_host_visible_buffer_memory_requirements.memoryTypeBits &
        (vulkan_device->memory_types().device_local &
         vulkan_device->memory_types().host_visible);
    VkMemoryAllocateInfo gamma_ramp_host_visible_buffer_memory_allocate_info;
    // Prefer a host-uncached (because it's write-only) memory type, but try a
    // host-cached host-visible device-local one as well.
    if (xe::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types &
                ~vulkan_device->memory_types().host_cached,
            &(gamma_ramp_host_visible_buffer_memory_allocate_info
                  .memoryTypeIndex)) ||
        xe::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types,
            &(gamma_ramp_host_visible_buffer_memory_allocate_info
                  .memoryTypeIndex))) {
      VkMemoryAllocateInfo*
          gamma_ramp_host_visible_buffer_memory_allocate_info_last =
              &gamma_ramp_host_visible_buffer_memory_allocate_info;
      gamma_ramp_host_visible_buffer_memory_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      gamma_ramp_host_visible_buffer_memory_allocate_info.pNext = nullptr;
      gamma_ramp_host_visible_buffer_memory_allocate_info.allocationSize =
          gamma_ramp_host_visible_buffer_memory_requirements.size;
      VkMemoryDedicatedAllocateInfo
          gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info;
      if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
        gamma_ramp_host_visible_buffer_memory_allocate_info_last->pNext =
            &gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info;
        gamma_ramp_host_visible_buffer_memory_allocate_info_last =
            reinterpret_cast<VkMemoryAllocateInfo*>(
                &gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info);
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.sType =
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.pNext =
            nullptr;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.image =
            VK_NULL_HANDLE;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.buffer =
            gamma_ramp_buffer_;
      }
      if (dfn.vkAllocateMemory(
              device, &gamma_ramp_host_visible_buffer_memory_allocate_info,
              nullptr, &gamma_ramp_buffer_memory_) == VK_SUCCESS) {
        if (dfn.vkBindBufferMemory(device, gamma_ramp_buffer_,
                                   gamma_ramp_buffer_memory_,
                                   0) == VK_SUCCESS) {
          if (dfn.vkMapMemory(device, gamma_ramp_buffer_memory_, 0,
                              VK_WHOLE_SIZE, 0,
                              &gamma_ramp_upload_mapping_) == VK_SUCCESS) {
            use_gamma_ramp_host_visible_buffer = true;
            gamma_ramp_upload_memory_size_ =
                gamma_ramp_host_visible_buffer_memory_allocate_info
                    .allocationSize;
            gamma_ramp_upload_memory_type_ =
                gamma_ramp_host_visible_buffer_memory_allocate_info
                    .memoryTypeIndex;
          }
        }
        if (!use_gamma_ramp_host_visible_buffer) {
          dfn.vkFreeMemory(device, gamma_ramp_buffer_memory_, nullptr);
          gamma_ramp_buffer_memory_ = VK_NULL_HANDLE;
        }
      }
    }
    if (!use_gamma_ramp_host_visible_buffer) {
      dfn.vkDestroyBuffer(device, gamma_ramp_buffer_, nullptr);
      gamma_ramp_buffer_ = VK_NULL_HANDLE;
    }
  }
  if (gamma_ramp_buffer_ == VK_NULL_HANDLE) {
    // Create separate buffers for the shader and uploading.
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal, gamma_ramp_buffer_,
            gamma_ramp_buffer_memory_)) {
      XELOGE("Failed to create the gamma ramp buffer");
      return false;
    }
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize * kMaxFramesInFlight,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            ui::vulkan::util::MemoryPurpose::kUpload, gamma_ramp_upload_buffer_,
            gamma_ramp_upload_buffer_memory_, &gamma_ramp_upload_memory_type_,
            &gamma_ramp_upload_memory_size_)) {
      XELOGE("Failed to create the gamma ramp upload buffer");
      return false;
    }
    if (dfn.vkMapMemory(device, gamma_ramp_upload_buffer_memory_, 0,
                        VK_WHOLE_SIZE, 0,
                        &gamma_ramp_upload_mapping_) != VK_SUCCESS) {
      XELOGE("Failed to map the gamma ramp upload buffer");
      return false;
    }
  }

  // Gamma ramp buffer views.
  uint32_t gamma_ramp_frame_count =
      gamma_ramp_upload_buffer_ == VK_NULL_HANDLE ? kMaxFramesInFlight : 1;
  VkBufferViewCreateInfo gamma_ramp_buffer_view_create_info;
  gamma_ramp_buffer_view_create_info.sType =
      VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
  gamma_ramp_buffer_view_create_info.pNext = nullptr;
  gamma_ramp_buffer_view_create_info.flags = 0;
  gamma_ramp_buffer_view_create_info.buffer = gamma_ramp_buffer_;
  // 256-entry table.
  gamma_ramp_buffer_view_create_info.format =
      VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSize256EntryTable;
  for (uint32_t i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset = kGammaRampSize * i;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info,
                               nullptr, &gamma_ramp_buffer_views_[i * 2]) !=
        VK_SUCCESS) {
      XELOGE("Failed to create a 256-entry table gamma ramp buffer view");
      return false;
    }
  }
  // Piecewise linear.
  gamma_ramp_buffer_view_create_info.format = VK_FORMAT_R16G16_UINT;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSizePWL;
  for (uint32_t i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset =
        kGammaRampSize * i + kGammaRampSize256EntryTable;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info,
                               nullptr, &gamma_ramp_buffer_views_[i * 2 + 1]) !=
        VK_SUCCESS) {
      XELOGE("Failed to create a PWL gamma ramp buffer view");
      return false;
    }
  }

  // Swap descriptor set layouts.
  VkDescriptorSetLayoutBinding swap_descriptor_set_layout_binding;
  swap_descriptor_set_layout_binding.binding = 0;
  swap_descriptor_set_layout_binding.descriptorCount = 1;
  swap_descriptor_set_layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  swap_descriptor_set_layout_binding.pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo swap_descriptor_set_layout_create_info;
  swap_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  swap_descriptor_set_layout_create_info.pNext = nullptr;
  swap_descriptor_set_layout_create_info.flags = 0;
  swap_descriptor_set_layout_create_info.bindingCount = 1;
  swap_descriptor_set_layout_create_info.pBindings =
      &swap_descriptor_set_layout_binding;
  swap_descriptor_set_layout_binding.descriptorType =
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &swap_descriptor_set_layout_create_info, nullptr,
          &swap_descriptor_set_layout_sampled_image_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create the presentation sampled image descriptor set "
        "layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType =
      VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &swap_descriptor_set_layout_create_info, nullptr,
          &swap_descriptor_set_layout_uniform_texel_buffer_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create the presentation uniform texel buffer descriptor set "
        "layout");
    return false;
  }

  // Swap descriptor pool.
  std::array<VkDescriptorPoolSize, 2> swap_descriptor_pool_sizes;
  VkDescriptorPoolCreateInfo swap_descriptor_pool_create_info;
  swap_descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  swap_descriptor_pool_create_info.pNext = nullptr;
  swap_descriptor_pool_create_info.flags = 0;
  swap_descriptor_pool_create_info.maxSets = 0;
  swap_descriptor_pool_create_info.poolSizeCount = 0;
  swap_descriptor_pool_create_info.pPoolSizes =
      swap_descriptor_pool_sizes.data();
  // TODO(Triang3l): FXAA combined image and sampler sources.
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_sampled_image =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info
                                       .poolSizeCount++];
    swap_descriptor_pool_size_sampled_image.type =
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    // Source images.
    swap_descriptor_pool_size_sampled_image.descriptorCount =
        kMaxFramesInFlight;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight;
  }
  // 256-entry table and PWL gamma ramps. If the gamma ramp buffer is
  // host-visible, for multiple frames.
  uint32_t gamma_ramp_buffer_view_count = 2 * gamma_ramp_frame_count;
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_uniform_texel_buffer =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info
                                       .poolSizeCount++];
    swap_descriptor_pool_size_uniform_texel_buffer.type =
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    swap_descriptor_pool_size_uniform_texel_buffer.descriptorCount =
        gamma_ramp_buffer_view_count;
    swap_descriptor_pool_create_info.maxSets += gamma_ramp_buffer_view_count;
  }
  if (dfn.vkCreateDescriptorPool(device, &swap_descriptor_pool_create_info,
                                 nullptr,
                                 &swap_descriptor_pool_) != VK_SUCCESS) {
    XELOGE("Failed to create the presentation descriptor pool");
    return false;
  }

  // Swap descriptor set allocation.
  VkDescriptorSetAllocateInfo swap_descriptor_set_allocate_info;
  swap_descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  swap_descriptor_set_allocate_info.pNext = nullptr;
  swap_descriptor_set_allocate_info.descriptorPool = swap_descriptor_pool_;
  swap_descriptor_set_allocate_info.descriptorSetCount = 1;
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_uniform_texel_buffer_;
  for (uint32_t i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_gamma_ramp_[i]) !=
        VK_SUCCESS) {
      XELOGE("Failed to allocate the gamma ramp descriptor sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_sampled_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_source_[i]) !=
        VK_SUCCESS) {
      XELOGE(
          "Failed to allocate the presentation source image descriptor sets");
      return false;
    }
  }

  // Gamma ramp descriptor sets.
  VkWriteDescriptorSet gamma_ramp_write_descriptor_set;
  gamma_ramp_write_descriptor_set.sType =
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  gamma_ramp_write_descriptor_set.pNext = nullptr;
  gamma_ramp_write_descriptor_set.dstBinding = 0;
  gamma_ramp_write_descriptor_set.dstArrayElement = 0;
  gamma_ramp_write_descriptor_set.descriptorCount = 1;
  gamma_ramp_write_descriptor_set.descriptorType =
      VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  gamma_ramp_write_descriptor_set.pImageInfo = nullptr;
  gamma_ramp_write_descriptor_set.pBufferInfo = nullptr;
  for (uint32_t i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    gamma_ramp_write_descriptor_set.dstSet = swap_descriptors_gamma_ramp_[i];
    gamma_ramp_write_descriptor_set.pTexelBufferView =
        &gamma_ramp_buffer_views_[i];
    dfn.vkUpdateDescriptorSets(device, 1, &gamma_ramp_write_descriptor_set, 0,
                               nullptr);
  }

  // Gamma ramp application pipeline layout.
  std::array<VkDescriptorSetLayout, kSwapApplyGammaDescriptorSetCount>
      swap_apply_gamma_descriptor_set_layouts{};
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetRamp] =
      swap_descriptor_set_layout_uniform_texel_buffer_;
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetSource] =
      swap_descriptor_set_layout_sampled_image_;
  VkPipelineLayoutCreateInfo swap_apply_gamma_pipeline_layout_create_info;
  swap_apply_gamma_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  swap_apply_gamma_pipeline_layout_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_layout_create_info.flags = 0;
  swap_apply_gamma_pipeline_layout_create_info.setLayoutCount =
      uint32_t(swap_apply_gamma_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_apply_gamma_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 0;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges = nullptr;
  if (dfn.vkCreatePipelineLayout(
          device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
          &swap_apply_gamma_pipeline_layout_) != VK_SUCCESS) {
    XELOGE("Failed to create the gamma ramp application pipeline layout");
    return false;
  }

  // Gamma application render pass. Doesn't make assumptions about outer usage
  // (explicit barriers must be used instead) for simplicity of use in different
  // scenarios with different pipelines.
  VkAttachmentDescription swap_apply_gamma_render_pass_attachment;
  swap_apply_gamma_render_pass_attachment.flags = 0;
  swap_apply_gamma_render_pass_attachment.format =
      ui::vulkan::VulkanPresenter::kGuestOutputFormat;
  swap_apply_gamma_render_pass_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  swap_apply_gamma_render_pass_attachment.loadOp =
      VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  swap_apply_gamma_render_pass_attachment.storeOp =
      VK_ATTACHMENT_STORE_OP_STORE;
  swap_apply_gamma_render_pass_attachment.stencilLoadOp =
      VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  swap_apply_gamma_render_pass_attachment.stencilStoreOp =
      VK_ATTACHMENT_STORE_OP_DONT_CARE;
  swap_apply_gamma_render_pass_attachment.initialLayout =
      VK_IMAGE_LAYOUT_UNDEFINED;
  swap_apply_gamma_render_pass_attachment.finalLayout =
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference swap_apply_gamma_render_pass_color_attachment;
  swap_apply_gamma_render_pass_color_attachment.attachment = 0;
  swap_apply_gamma_render_pass_color_attachment.layout =
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkSubpassDescription swap_apply_gamma_render_pass_subpass = {};
  swap_apply_gamma_render_pass_subpass.pipelineBindPoint =
      VK_PIPELINE_BIND_POINT_GRAPHICS;
  swap_apply_gamma_render_pass_subpass.colorAttachmentCount = 1;
  swap_apply_gamma_render_pass_subpass.pColorAttachments =
      &swap_apply_gamma_render_pass_color_attachment;
  VkSubpassDependency swap_apply_gamma_render_pass_dependencies[2];
  for (uint32_t i = 0; i < 2; ++i) {
    VkSubpassDependency& swap_apply_gamma_render_pass_dependency =
        swap_apply_gamma_render_pass_dependencies[i];
    swap_apply_gamma_render_pass_dependency.srcSubpass =
        i ? 0 : VK_SUBPASS_EXTERNAL;
    swap_apply_gamma_render_pass_dependency.dstSubpass =
        i ? VK_SUBPASS_EXTERNAL : 0;
    swap_apply_gamma_render_pass_dependency.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    swap_apply_gamma_render_pass_dependency.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    swap_apply_gamma_render_pass_dependency.srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    swap_apply_gamma_render_pass_dependency.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    swap_apply_gamma_render_pass_dependency.dependencyFlags =
        VK_DEPENDENCY_BY_REGION_BIT;
  }
  VkRenderPassCreateInfo swap_apply_gamma_render_pass_create_info;
  swap_apply_gamma_render_pass_create_info.sType =
      VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  swap_apply_gamma_render_pass_create_info.pNext = nullptr;
  swap_apply_gamma_render_pass_create_info.flags = 0;
  swap_apply_gamma_render_pass_create_info.attachmentCount = 1;
  swap_apply_gamma_render_pass_create_info.pAttachments =
      &swap_apply_gamma_render_pass_attachment;
  swap_apply_gamma_render_pass_create_info.subpassCount = 1;
  swap_apply_gamma_render_pass_create_info.pSubpasses =
      &swap_apply_gamma_render_pass_subpass;
  swap_apply_gamma_render_pass_create_info.dependencyCount =
      uint32_t(xe::countof(swap_apply_gamma_render_pass_dependencies));
  swap_apply_gamma_render_pass_create_info.pDependencies =
      swap_apply_gamma_render_pass_dependencies;
  if (dfn.vkCreateRenderPass(device, &swap_apply_gamma_render_pass_create_info,
                             nullptr,
                             &swap_apply_gamma_render_pass_) != VK_SUCCESS) {
    XELOGE("Failed to create the gamma ramp application render pass");
    return false;
  }

  // Gamma ramp application pipeline.
  // Using a graphics pipeline, not a compute one, because storage image support
  // is optional for VK_FORMAT_A2B10G10R10_UNORM_PACK32.

  enum SwapApplyGammaPixelShader {
    kSwapApplyGammaPixelShader256EntryTable,
    kSwapApplyGammaPixelShaderPWL,

    kSwapApplyGammaPixelShaderCount,
  };
  std::array<VkShaderModule, kSwapApplyGammaPixelShaderCount>
      swap_apply_gamma_pixel_shaders{};
  bool swap_apply_gamma_pixel_shaders_created =
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTable] =
           ui::vulkan::util::CreateShaderModule(
               vulkan_device, shaders::apply_gamma_table_ps,
               sizeof(shaders::apply_gamma_table_ps))) != VK_NULL_HANDLE &&
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWL] =
           ui::vulkan::util::CreateShaderModule(
               vulkan_device, shaders::apply_gamma_pwl_ps,
               sizeof(shaders::apply_gamma_pwl_ps))) != VK_NULL_HANDLE;
  if (!swap_apply_gamma_pixel_shaders_created) {
    XELOGE("Failed to create the gamma ramp application pixel shader modules");
    for (VkShaderModule swap_apply_gamma_pixel_shader :
         swap_apply_gamma_pixel_shaders) {
      if (swap_apply_gamma_pixel_shader != VK_NULL_HANDLE) {
        dfn.vkDestroyShaderModule(device, swap_apply_gamma_pixel_shader,
                                  nullptr);
      }
    }
    return false;
  }

  VkPipelineShaderStageCreateInfo swap_apply_gamma_pipeline_stages[2];
  swap_apply_gamma_pipeline_stages[0].sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  swap_apply_gamma_pipeline_stages[0].pNext = nullptr;
  swap_apply_gamma_pipeline_stages[0].flags = 0;
  swap_apply_gamma_pipeline_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  swap_apply_gamma_pipeline_stages[0].module =
      ui::vulkan::util::CreateShaderModule(vulkan_device,
                                           shaders::fullscreen_cw_vs,
                                           sizeof(shaders::fullscreen_cw_vs));
  if (swap_apply_gamma_pipeline_stages[0].module == VK_NULL_HANDLE) {
    XELOGE("Failed to create the gamma ramp application vertex shader module");
    for (VkShaderModule swap_apply_gamma_pixel_shader :
         swap_apply_gamma_pixel_shaders) {
      assert_true(swap_apply_gamma_pixel_shader != VK_NULL_HANDLE);
      dfn.vkDestroyShaderModule(device, swap_apply_gamma_pixel_shader, nullptr);
    }
  }
  swap_apply_gamma_pipeline_stages[0].pName = "main";
  swap_apply_gamma_pipeline_stages[0].pSpecializationInfo = nullptr;
  swap_apply_gamma_pipeline_stages[1].sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  swap_apply_gamma_pipeline_stages[1].pNext = nullptr;
  swap_apply_gamma_pipeline_stages[1].flags = 0;
  swap_apply_gamma_pipeline_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  // The fragment shader module will be specified later.
  swap_apply_gamma_pipeline_stages[1].pName = "main";
  swap_apply_gamma_pipeline_stages[1].pSpecializationInfo = nullptr;

  VkPipelineVertexInputStateCreateInfo
      swap_apply_gamma_pipeline_vertex_input_state = {};
  swap_apply_gamma_pipeline_vertex_input_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo
      swap_apply_gamma_pipeline_input_assembly_state;
  swap_apply_gamma_pipeline_input_assembly_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_input_assembly_state.pNext = nullptr;
  swap_apply_gamma_pipeline_input_assembly_state.flags = 0;
  swap_apply_gamma_pipeline_input_assembly_state.topology =
      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  swap_apply_gamma_pipeline_input_assembly_state.primitiveRestartEnable =
      VK_FALSE;

  VkPipelineViewportStateCreateInfo swap_apply_gamma_pipeline_viewport_state;
  swap_apply_gamma_pipeline_viewport_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_viewport_state.pNext = nullptr;
  swap_apply_gamma_pipeline_viewport_state.flags = 0;
  swap_apply_gamma_pipeline_viewport_state.viewportCount = 1;
  swap_apply_gamma_pipeline_viewport_state.pViewports = nullptr;
  swap_apply_gamma_pipeline_viewport_state.scissorCount = 1;
  swap_apply_gamma_pipeline_viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo
      swap_apply_gamma_pipeline_rasterization_state = {};
  swap_apply_gamma_pipeline_rasterization_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_rasterization_state.polygonMode =
      VK_POLYGON_MODE_FILL;
  swap_apply_gamma_pipeline_rasterization_state.cullMode = VK_CULL_MODE_NONE;
  swap_apply_gamma_pipeline_rasterization_state.frontFace =
      VK_FRONT_FACE_CLOCKWISE;
  swap_apply_gamma_pipeline_rasterization_state.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo
      swap_apply_gamma_pipeline_multisample_state = {};
  swap_apply_gamma_pipeline_multisample_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_multisample_state.rasterizationSamples =
      VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState
      swap_apply_gamma_pipeline_color_blend_attachment_state = {};
  swap_apply_gamma_pipeline_color_blend_attachment_state.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo
      swap_apply_gamma_pipeline_color_blend_state = {};
  swap_apply_gamma_pipeline_color_blend_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_color_blend_state.attachmentCount = 1;
  swap_apply_gamma_pipeline_color_blend_state.pAttachments =
      &swap_apply_gamma_pipeline_color_blend_attachment_state;

  static constexpr VkDynamicState kSwapApplyGammaPipelineDynamicStates[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };
  VkPipelineDynamicStateCreateInfo swap_apply_gamma_pipeline_dynamic_state;
  swap_apply_gamma_pipeline_dynamic_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_dynamic_state.pNext = nullptr;
  swap_apply_gamma_pipeline_dynamic_state.flags = 0;
  swap_apply_gamma_pipeline_dynamic_state.dynamicStateCount =
      uint32_t(xe::countof(kSwapApplyGammaPipelineDynamicStates));
  swap_apply_gamma_pipeline_dynamic_state.pDynamicStates =
      kSwapApplyGammaPipelineDynamicStates;

  VkGraphicsPipelineCreateInfo swap_apply_gamma_pipeline_create_info;
  swap_apply_gamma_pipeline_create_info.sType =
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  swap_apply_gamma_pipeline_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_create_info.flags = 0;
  swap_apply_gamma_pipeline_create_info.stageCount =
      uint32_t(xe::countof(swap_apply_gamma_pipeline_stages));
  swap_apply_gamma_pipeline_create_info.pStages =
      swap_apply_gamma_pipeline_stages;
  swap_apply_gamma_pipeline_create_info.pVertexInputState =
      &swap_apply_gamma_pipeline_vertex_input_state;
  swap_apply_gamma_pipeline_create_info.pInputAssemblyState =
      &swap_apply_gamma_pipeline_input_assembly_state;
  swap_apply_gamma_pipeline_create_info.pTessellationState = nullptr;
  swap_apply_gamma_pipeline_create_info.pViewportState =
      &swap_apply_gamma_pipeline_viewport_state;
  swap_apply_gamma_pipeline_create_info.pRasterizationState =
      &swap_apply_gamma_pipeline_rasterization_state;
  swap_apply_gamma_pipeline_create_info.pMultisampleState =
      &swap_apply_gamma_pipeline_multisample_state;
  swap_apply_gamma_pipeline_create_info.pDepthStencilState = nullptr;
  swap_apply_gamma_pipeline_create_info.pColorBlendState =
      &swap_apply_gamma_pipeline_color_blend_state;
  swap_apply_gamma_pipeline_create_info.pDynamicState =
      &swap_apply_gamma_pipeline_dynamic_state;
  swap_apply_gamma_pipeline_create_info.layout =
      swap_apply_gamma_pipeline_layout_;
  swap_apply_gamma_pipeline_create_info.renderPass =
      swap_apply_gamma_render_pass_;
  swap_apply_gamma_pipeline_create_info.subpass = 0;
  swap_apply_gamma_pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  swap_apply_gamma_pipeline_create_info.basePipelineIndex = -1;
  swap_apply_gamma_pipeline_stages[1].module =
      swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTable];
  VkResult swap_apply_gamma_pipeline_256_entry_table_create_result =
      dfn.vkCreateGraphicsPipelines(
          device, VK_NULL_HANDLE, 1, &swap_apply_gamma_pipeline_create_info,
          nullptr, &swap_apply_gamma_256_entry_table_pipeline_);
  swap_apply_gamma_pipeline_stages[1].module =
      swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWL];
  VkResult swap_apply_gamma_pipeline_pwl_create_result =
      dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                    &swap_apply_gamma_pipeline_create_info,
                                    nullptr, &swap_apply_gamma_pwl_pipeline_);
  dfn.vkDestroyShaderModule(device, swap_apply_gamma_pipeline_stages[0].module,
                            nullptr);
  for (VkShaderModule swap_apply_gamma_pixel_shader :
       swap_apply_gamma_pixel_shaders) {
    assert_true(swap_apply_gamma_pixel_shader != VK_NULL_HANDLE);
    dfn.vkDestroyShaderModule(device, swap_apply_gamma_pixel_shader, nullptr);
  }
  if (swap_apply_gamma_pipeline_256_entry_table_create_result != VK_SUCCESS ||
      swap_apply_gamma_pipeline_pwl_create_result != VK_SUCCESS) {
    XELOGE("Failed to create the gamma ramp application pipelines");
    return false;
  }

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));

  return true;
}

void VulkanCommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  DestroyScratchBuffer();

  for (SwapFramebuffer& swap_framebuffer : swap_framebuffers_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                           swap_framebuffer.framebuffer);
  }

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_pwl_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device,
      swap_apply_gamma_256_entry_table_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device,
                                         swap_apply_gamma_render_pass_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_apply_gamma_pipeline_layout_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         swap_descriptor_pool_);

  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      swap_descriptor_set_layout_uniform_texel_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      swap_descriptor_set_layout_sampled_image_);
  for (VkBufferView& gamma_ramp_buffer_view : gamma_ramp_buffer_views_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBufferView, device,
                                           gamma_ramp_buffer_view);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         gamma_ramp_upload_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         gamma_ramp_upload_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         gamma_ramp_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         gamma_ramp_buffer_memory_);

  // Clean up all readback buffers.
  for (auto& pair : readback_buffers_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           pair.second.buffers[0]);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           pair.second.memories[0]);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           pair.second.buffers[1]);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           pair.second.memories[1]);
  }
  readback_buffers_.clear();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         memexport_readback_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         memexport_readback_buffer_memory_);
  memexport_readback_buffer_size_ = 0;

  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorPool, device,
      shared_memory_and_edram_descriptor_pool_);

  texture_cache_.reset();

  pipeline_cache_.reset();

  render_target_cache_.reset();

  primitive_processor_.reset();

  shared_memory_.reset();

  ClearTransientDescriptorPools();

  for (const auto& pipeline_layout_pair : pipeline_layouts_) {
    dfn.vkDestroyPipelineLayout(
        device, pipeline_layout_pair.second.GetPipelineLayout(), nullptr);
  }
  pipeline_layouts_.clear();
  for (const auto& descriptor_set_layout_pair :
       descriptor_set_layouts_textures_) {
    dfn.vkDestroyDescriptorSetLayout(device, descriptor_set_layout_pair.second,
                                     nullptr);
  }
  descriptor_set_layouts_textures_.clear();

  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      descriptor_set_layout_shared_memory_and_edram_);
  for (VkDescriptorSetLayout& descriptor_set_layout_single_transient :
       descriptor_set_layouts_single_transient_) {
    ui::vulkan::util::DestroyAndNullHandle(
        dfn.vkDestroyDescriptorSetLayout, device,
        descriptor_set_layout_single_transient);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device,
                                         descriptor_set_layout_constants_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device, descriptor_set_layout_empty_);

  uniform_buffer_pool_.reset();

  sparse_bind_wait_stage_mask_ = 0;
  sparse_buffer_binds_.clear();
  sparse_memory_binds_.clear();

  deferred_command_buffer_.Reset();
  for (const auto& command_buffer_pair : command_buffers_submitted_) {
    dfn.vkDestroyCommandPool(device, command_buffer_pair.second.pool, nullptr);
  }
  command_buffers_submitted_.clear();
  for (const CommandBuffer& command_buffer : command_buffers_writable_) {
    dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
  }
  command_buffers_writable_.clear();

  for (const auto& destroy_pair : destroy_framebuffers_) {
    dfn.vkDestroyFramebuffer(device, destroy_pair.second, nullptr);
  }
  destroy_framebuffers_.clear();
  for (const auto& destroy_pair : destroy_buffers_) {
    dfn.vkDestroyBuffer(device, destroy_pair.second, nullptr);
  }
  destroy_buffers_.clear();
  for (const auto& destroy_pair : destroy_memory_) {
    dfn.vkFreeMemory(device, destroy_pair.second, nullptr);
  }
  destroy_memory_.clear();

  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));
  frame_completed_ = 0;
  frame_current_ = 1;
  frame_open_ = false;

  for (const auto& semaphore : submissions_in_flight_semaphores_) {
    dfn.vkDestroySemaphore(device, semaphore.second, nullptr);
  }
  submissions_in_flight_semaphores_.clear();
  for (VkFence& fence : submissions_in_flight_fences_) {
    dfn.vkDestroyFence(device, fence, nullptr);
  }
  submissions_in_flight_fences_.clear();
  current_submission_wait_stage_masks_.clear();
  for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
    dfn.vkDestroySemaphore(device, semaphore, nullptr);
  }
  current_submission_wait_semaphores_.clear();
  submission_completed_ = 0;
  submission_open_ = false;

  for (VkSemaphore semaphore : semaphores_free_) {
    dfn.vkDestroySemaphore(device, semaphore, nullptr);
  }
  semaphores_free_.clear();
  for (VkFence fence : fences_free_) {
    dfn.vkDestroyFence(device, fence, nullptr);
  }
  fences_free_.clear();

  device_lost_ = false;

  CommandProcessor::ShutdownContext();
}

void VulkanCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (frame_open_) {
      uint32_t float_constant_index =
          (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (float_constant_index >= 256) {
        float_constant_index -= 256;
        if (current_float_constant_map_pixel_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          current_constant_buffers_up_to_date_ &= ~(
              UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      } else {
        if (current_float_constant_map_vertex_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          current_constant_buffers_up_to_date_ &= ~(
              UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
        }
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantWritten(
          (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
    }
  }
}
void VulkanCommandProcessor::WriteRegistersFromMem(uint32_t start_index,
                                                   uint32_t* base,
                                                   uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = xe::load_and_swap<uint32_t>(base + i);
    VulkanCommandProcessor::WriteRegister(start_index + i, data);
  }
}
void VulkanCommandProcessor::SparseBindBuffer(
    VkBuffer buffer, uint32_t bind_count, const VkSparseMemoryBind* binds,
    VkPipelineStageFlags wait_stage_mask) {
  if (!bind_count) {
    return;
  }
  SparseBufferBind& buffer_bind = sparse_buffer_binds_.emplace_back();
  buffer_bind.buffer = buffer;
  buffer_bind.bind_offset = sparse_memory_binds_.size();
  buffer_bind.bind_count = bind_count;
  sparse_memory_binds_.reserve(sparse_memory_binds_.size() + bind_count);
  sparse_memory_binds_.insert(sparse_memory_binds_.end(), binds,
                              binds + bind_count);
  sparse_bind_wait_stage_mask_ |= wait_stage_mask;
}

void VulkanCommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
}

void VulkanCommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
}

void VulkanCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                       uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  SCOPE_profile_cpu_f("gpu");

  // Frame boundary. The counter owns all of its own state and toggle
  // (debug.canary.fps); see xenia/base/ae_fps.h.
  xe::ae::FpsCounter::OnFrame();

  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    return;
  }

  if (gpu_trace_enabled()) {
    GpuTrace("SWAP", fmt::format("frontbuffer_ptr=0x{:08X} {}x{}",
                                 frontbuffer_ptr, frontbuffer_width,
                                 frontbuffer_height));
  }
  // TESTRIG(halo3): the decoupled image capture is read at the END of IssueSwap
  // (after EndSubmission), so the deferred copy has actually been submitted and
  // completed - reading here (submission still open) would no-op.

  // TESTRIG(halo3-gbuf): the magenta test proved the deferred composite reaches
  // the screen, so the vista is lost in its G-buffer INPUT. Dump the guest-RAM
  // content of the resolved G-buffer targets at swap time (after this frame's
  // scene resolves). Xenia resolves EDRAM -> shared memory (guest RAM), then
  // loads textures from there. If these are non-empty (many distinct values),
  // the scene DID resolve to memory and the break is texture load/invalidation
  // after resolve; if flat/near-empty, the EDRAM->shared-memory resolve itself
  // isn't landing on Adreno. One-shot.
  {
    // TESTRIG(probe): scans 256KB of guest RAM per address. Bounded to 4 runs,
    // but that is still a multi-megabyte scan during startup.
    static int gbuf_logged = 0;
    if (XE_AE_DIAG_ENABLED("debug.canary.probe_gbuf") && gbuf_logged < 4) {
      const uint32_t kGbufAddrs[] = {0x044B0000u, 0x04780000u, 0x043FC000u,
                                     0x04E20000u};
      bool any_populated = false;
      for (uint32_t addr : kGbufAddrs) {
        const uint32_t* p =
            reinterpret_cast<const uint32_t*>(memory_->TranslatePhysical(addr));
        uint32_t nonzero = 0, distinct_est = 0;
        uint32_t prev = 0xDEADBEEFu;
        uint32_t sample_dwords = 65536;  // 256KB window
        for (uint32_t k = 0; k < sample_dwords; ++k) {
          if (p[k] != 0) ++nonzero;
          if (p[k] != prev) {
            ++distinct_est;
            prev = p[k];
          }
        }
        if (nonzero > 1000) any_populated = true;
        XELOGI(
            "GBUF addr=0x{:08X} nonzero={}/{} run_changes={} s0=0x{:08X} "
            "s1=0x{:08X} s2=0x{:08X}",
            addr, nonzero, sample_dwords, distinct_est, p[0], p[1], p[2]);
      }
      // Only "count" this as a real capture once the menu has populated the
      // G-buffer, so we don't burn all logs on the initial empty frames.
      if (any_populated) {
        ++gbuf_logged;
      }
    }
  }

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    return;
  }

  // Obtaining the actual front buffer size to pass to RefreshGuestOutput,
  // resolution-scaled if it's a resolve destination, or not otherwise.
  uint32_t frontbuffer_width_scaled, frontbuffer_height_scaled;
  xenos::TextureFormat frontbuffer_format;
  VkImageView swap_texture_view = texture_cache_->RequestSwapTexture(
      frontbuffer_width_scaled, frontbuffer_height_scaled, frontbuffer_format);
  if (swap_texture_view == VK_NULL_HANDLE) {
    return;
  }

  auto aspect = graphics_system_->GetScaledAspectRatio();

  presenter->RefreshGuestOutput(
      frontbuffer_width_scaled, frontbuffer_height_scaled, aspect.first,
      aspect.second,
      [this, frontbuffer_width_scaled, frontbuffer_height_scaled,
       frontbuffer_format, swap_texture_view](
          ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        // In case the swap command is the only one in the frame.
        if (!BeginSubmission(true)) {
          return false;
        }

        auto& vulkan_context = static_cast<
            ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(
            context);
        uint64_t guest_output_image_version = vulkan_context.image_version();

        const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
        const ui::vulkan::VulkanDevice::Functions& dfn =
            vulkan_device->functions();
        const VkDevice device = vulkan_device->device();

        uint32_t swap_frame_index =
            uint32_t(frame_current_ % kMaxFramesInFlight);

        // This is according to D3D::InitializePresentationParameters from a
        // game executable, which initializes the 256-entry table gamma ramp for
        // 8_8_8_8 output and the PWL gamma ramp for 2_10_10_10.
        // TODO(Triang3l): Choose between the table and PWL based on
        // DC_LUTA_CONTROL, support both for all formats (and also different
        // increments for PWL).
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format ==
                xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;

        // TODO(Triang3l): FXAA can result in more than 8 bits of precision.
        context.SetIs8bpc(!use_pwl_gamma_ramp);

        // Update the gamma ramp if it's out of date.
        uint32_t& gamma_ramp_frame_index_ref =
            use_pwl_gamma_ramp ? gamma_ramp_pwl_current_frame_
                               : gamma_ramp_256_entry_table_current_frame_;
        if (gamma_ramp_frame_index_ref == UINT32_MAX) {
          constexpr uint32_t kGammaRampSize256EntryTable =
              sizeof(uint32_t) * 256;
          constexpr uint32_t kGammaRampSizePWL = sizeof(uint16_t) * 2 * 3 * 128;
          constexpr uint32_t kGammaRampSize =
              kGammaRampSize256EntryTable + kGammaRampSizePWL;
          uint32_t gamma_ramp_offset_in_frame =
              use_pwl_gamma_ramp ? kGammaRampSize256EntryTable : 0;
          uint32_t gamma_ramp_upload_offset =
              kGammaRampSize * swap_frame_index + gamma_ramp_offset_in_frame;
          uint32_t gamma_ramp_size = use_pwl_gamma_ramp
                                         ? kGammaRampSizePWL
                                         : kGammaRampSize256EntryTable;
          void* gamma_ramp_frame_upload =
              reinterpret_cast<uint8_t*>(gamma_ramp_upload_mapping_) +
              gamma_ramp_upload_offset;
          if (std::endian::native != std::endian::little &&
              use_pwl_gamma_ramp) {
            // R16G16 is first R16, where the shader expects the base, and
            // second G16, where the delta should be, but gamma_ramp_pwl_rgb()
            // is an array of 32-bit DC_LUT_PWL_DATA registers - swap 16 bits in
            // each 32.
            auto gamma_ramp_pwl_upload =
                reinterpret_cast<reg::DC_LUT_PWL_DATA*>(
                    gamma_ramp_frame_upload);
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_entry =
                  gamma_ramp_pwl_upload[i];
              reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_entry.base = gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_entry.delta = gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(
                gamma_ramp_frame_upload,
                use_pwl_gamma_ramp
                    ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                    : static_cast<const void*>(gamma_ramp_256_entry_table()),
                gamma_ramp_size);
          }
          bool gamma_ramp_has_upload_buffer =
              gamma_ramp_upload_buffer_memory_ != VK_NULL_HANDLE;
          ui::vulkan::util::FlushMappedMemoryRange(
              vulkan_device,
              gamma_ramp_has_upload_buffer ? gamma_ramp_upload_buffer_memory_
                                           : gamma_ramp_buffer_memory_,
              gamma_ramp_upload_memory_type_, gamma_ramp_upload_offset,
              gamma_ramp_upload_memory_size_, gamma_ramp_size);
          if (gamma_ramp_has_upload_buffer) {
            // Copy from the host-visible buffer to the device-local one.
            PushBufferMemoryBarrier(
                gamma_ramp_buffer_, gamma_ramp_offset_in_frame, gamma_ramp_size,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED, false);
            SubmitBarriers(true);
            VkBufferCopy gamma_ramp_buffer_copy;
            gamma_ramp_buffer_copy.srcOffset = gamma_ramp_upload_offset;
            gamma_ramp_buffer_copy.dstOffset = gamma_ramp_offset_in_frame;
            gamma_ramp_buffer_copy.size = gamma_ramp_size;
            deferred_command_buffer_.CmdVkCopyBuffer(gamma_ramp_upload_buffer_,
                                                     gamma_ramp_buffer_, 1,
                                                     &gamma_ramp_buffer_copy);
            PushBufferMemoryBarrier(
                gamma_ramp_buffer_, gamma_ramp_offset_in_frame, gamma_ramp_size,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
          }
          // The device-local, but not host-visible, gamma ramp buffer doesn't
          // have per-frame sets of gamma ramps.
          gamma_ramp_frame_index_ref =
              gamma_ramp_has_upload_buffer ? 0 : swap_frame_index;
        }

        // Make sure a framebuffer is available for the current guest output
        // image version.
        size_t swap_framebuffer_index = SIZE_MAX;
        size_t swap_framebuffer_new_index = SIZE_MAX;
        // Try to find the existing framebuffer for the current guest output
        // image version, or an unused (without an existing framebuffer, or with
        // one, but that has never actually been used dynamically) slot.
        for (size_t i = 0; i < swap_framebuffers_.size(); ++i) {
          const SwapFramebuffer& existing_swap_framebuffer =
              swap_framebuffers_[i];
          if (existing_swap_framebuffer.framebuffer != VK_NULL_HANDLE &&
              existing_swap_framebuffer.version == guest_output_image_version) {
            swap_framebuffer_index = i;
            break;
          }
          if (existing_swap_framebuffer.framebuffer == VK_NULL_HANDLE ||
              !existing_swap_framebuffer.last_submission) {
            swap_framebuffer_new_index = i;
          }
        }
        if (swap_framebuffer_index == SIZE_MAX) {
          if (swap_framebuffer_new_index == SIZE_MAX) {
            // Replace the earliest used framebuffer.
            swap_framebuffer_new_index = 0;
            for (size_t i = 1; i < swap_framebuffers_.size(); ++i) {
              if (swap_framebuffers_[i].last_submission <
                  swap_framebuffers_[swap_framebuffer_new_index]
                      .last_submission) {
                swap_framebuffer_new_index = i;
              }
            }
          }
          swap_framebuffer_index = swap_framebuffer_new_index;
          SwapFramebuffer& new_swap_framebuffer =
              swap_framebuffers_[swap_framebuffer_new_index];
          if (new_swap_framebuffer.framebuffer != VK_NULL_HANDLE) {
            if (submission_completed_ >= new_swap_framebuffer.last_submission) {
              dfn.vkDestroyFramebuffer(device, new_swap_framebuffer.framebuffer,
                                       nullptr);
            } else {
              destroy_framebuffers_.emplace_back(
                  new_swap_framebuffer.last_submission,
                  new_swap_framebuffer.framebuffer);
            }
            new_swap_framebuffer.framebuffer = VK_NULL_HANDLE;
          }
          VkImageView guest_output_image_view = vulkan_context.image_view();
          VkFramebufferCreateInfo swap_framebuffer_create_info;
          swap_framebuffer_create_info.sType =
              VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
          swap_framebuffer_create_info.pNext = nullptr;
          swap_framebuffer_create_info.flags = 0;
          swap_framebuffer_create_info.renderPass =
              swap_apply_gamma_render_pass_;
          swap_framebuffer_create_info.attachmentCount = 1;
          swap_framebuffer_create_info.pAttachments = &guest_output_image_view;
          swap_framebuffer_create_info.width = frontbuffer_width_scaled;
          swap_framebuffer_create_info.height = frontbuffer_height_scaled;
          swap_framebuffer_create_info.layers = 1;
          if (dfn.vkCreateFramebuffer(
                  device, &swap_framebuffer_create_info, nullptr,
                  &new_swap_framebuffer.framebuffer) != VK_SUCCESS) {
            XELOGE("Failed to create the Vulkan framebuffer for presentation");
            return false;
          }
          new_swap_framebuffer.version = guest_output_image_version;
          // The actual submission index will be set if the framebuffer is
          // actually used, not dropped due to some error.
          new_swap_framebuffer.last_submission = 0;
        }

        if (vulkan_context.image_ever_written_previously()) {
          // Insert a barrier after the last presenter's usage of the guest
          // output image. Will be overwriting all the contents, so oldLayout
          // layout is UNDEFINED. The render pass will do the layout transition,
          // but newLayout must not be UNDEFINED.
          PushImageMemoryBarrier(
              vulkan_context.image(),
              ui::vulkan::util::InitializeSubresourceRange(),
              ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }

        // End the current render pass before inserting barriers and starting a
        // new one, and insert the barrier.
        SubmitBarriers(true);

        SwapFramebuffer& swap_framebuffer =
            swap_framebuffers_[swap_framebuffer_index];
        swap_framebuffer.last_submission = GetCurrentSubmission();

        VkRenderPassBeginInfo render_pass_begin_info;
        render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_begin_info.pNext = nullptr;
        render_pass_begin_info.renderPass = swap_apply_gamma_render_pass_;
        render_pass_begin_info.framebuffer = swap_framebuffer.framebuffer;
        render_pass_begin_info.renderArea.offset.x = 0;
        render_pass_begin_info.renderArea.offset.y = 0;
        render_pass_begin_info.renderArea.extent.width =
            frontbuffer_width_scaled;
        render_pass_begin_info.renderArea.extent.height =
            frontbuffer_height_scaled;
        render_pass_begin_info.clearValueCount = 0;
        render_pass_begin_info.pClearValues = nullptr;
        deferred_command_buffer_.CmdVkBeginRenderPass(
            &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
        // TESTRIG(regression-bisect): 74a4ebfe. IssueSwap begins this pass
        // manually, bypassing the tracked BeginRenderPass helper, so without
        // these two lines the tracking goes stale. An earlier revert of this
        // change rendered the menu fully black, so it is load-bearing - but it
        // is still an AE-only change, so debug.canary.fix_swap_renderpass=0
        // takes it back out for bisection.
        if (XE_AE_FIX_ENABLED("debug.canary.fix_swap_renderpass")) {
          current_render_pass_ = swap_apply_gamma_render_pass_;
          // Not a render target cache framebuffer.
          current_framebuffer_ = nullptr;
        }

        VkViewport viewport;
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = float(frontbuffer_width_scaled);
        viewport.height = float(frontbuffer_height_scaled);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        SetViewport(viewport);
        VkRect2D scissor;
        scissor.offset.x = 0;
        scissor.offset.y = 0;
        scissor.extent.width = frontbuffer_width_scaled;
        scissor.extent.height = frontbuffer_height_scaled;
        SetScissor(scissor);

        BindExternalGraphicsPipeline(
            use_pwl_gamma_ramp ? swap_apply_gamma_pwl_pipeline_
                               : swap_apply_gamma_256_entry_table_pipeline_);

        VkDescriptorSet swap_descriptor_source =
            swap_descriptors_source_[swap_frame_index];
        VkDescriptorImageInfo swap_descriptor_source_image_info;
        swap_descriptor_source_image_info.sampler = VK_NULL_HANDLE;
        swap_descriptor_source_image_info.imageView = swap_texture_view;
        swap_descriptor_source_image_info.imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet swap_descriptor_source_write;
        swap_descriptor_source_write.sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        swap_descriptor_source_write.pNext = nullptr;
        swap_descriptor_source_write.dstSet = swap_descriptor_source;
        swap_descriptor_source_write.dstBinding = 0;
        swap_descriptor_source_write.dstArrayElement = 0;
        swap_descriptor_source_write.descriptorCount = 1;
        swap_descriptor_source_write.descriptorType =
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        swap_descriptor_source_write.pImageInfo =
            &swap_descriptor_source_image_info;
        swap_descriptor_source_write.pBufferInfo = nullptr;
        swap_descriptor_source_write.pTexelBufferView = nullptr;
        dfn.vkUpdateDescriptorSets(device, 1, &swap_descriptor_source_write, 0,
                                   nullptr);

        std::array<VkDescriptorSet, kSwapApplyGammaDescriptorSetCount>
            swap_descriptor_sets{};
        swap_descriptor_sets[kSwapApplyGammaDescriptorSetRamp] =
            swap_descriptors_gamma_ramp_[2 * gamma_ramp_frame_index_ref +
                                         uint32_t(use_pwl_gamma_ramp)];
        swap_descriptor_sets[kSwapApplyGammaDescriptorSetSource] =
            swap_descriptor_source;
        // TODO(Triang3l): Red / blue swap without imageViewFormatSwizzle.
        deferred_command_buffer_.CmdVkBindDescriptorSets(
            VK_PIPELINE_BIND_POINT_GRAPHICS, swap_apply_gamma_pipeline_layout_,
            0, uint32_t(swap_descriptor_sets.size()),
            swap_descriptor_sets.data(), 0, nullptr);

        deferred_command_buffer_.CmdVkDraw(3, 1, 0, 0);

        deferred_command_buffer_.CmdVkEndRenderPass();
        if (XE_AE_FIX_ENABLED("debug.canary.fix_swap_renderpass")) {
          current_render_pass_ = VK_NULL_HANDLE;
        }

        // Insert the release barrier.
        PushImageMemoryBarrier(
            vulkan_context.image(),
            ui::vulkan::util::InitializeSubresourceRange(),
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout);

        // Need to submit all the commands before giving the image back to the
        // presenter so it can submit its own commands for displaying it to the
        // queue, and also need to submit the release barrier.
        EndSubmission(true);
        return true;
      });

  // End the frame even if did not present for any reason (the image refresher
  // was not called), to prevent leaking per-frame resources.
  EndSubmission(true);

  // TESTRIG(halo3): read the resolved-companion capture recorded during this
  // frame's dump (now submitted + complete).
  TestrigReadCapturedImage("VISTA_POSTXFER_NOSRS");
  TestrigReadCapturedSharedMemory("SHM_44B0");
  TestrigReadCapturedEdram("EDRAM_T608");
}

bool VulkanCommandProcessor::PushBufferMemoryBarrier(
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
    uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask &&
      src_access_mask == dst_access_mask &&
      src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Separate different barriers for overlapping buffer ranges into different
  // pipeline barrier commands.
  for (const VkBufferMemoryBarrier& other_buffer_memory_barrier :
       pending_barriers_buffer_memory_barriers_) {
    if (other_buffer_memory_barrier.buffer != buffer ||
        (size != VK_WHOLE_SIZE &&
         offset + size <= other_buffer_memory_barrier.offset) ||
        (other_buffer_memory_barrier.size != VK_WHOLE_SIZE &&
         other_buffer_memory_barrier.offset +
                 other_buffer_memory_barrier.size <=
             offset)) {
      continue;
    }
    if (other_buffer_memory_barrier.offset == offset &&
        other_buffer_memory_barrier.size == size &&
        other_buffer_memory_barrier.srcAccessMask == src_access_mask &&
        other_buffer_memory_barrier.dstAccessMask == dst_access_mask &&
        other_buffer_memory_barrier.srcQueueFamilyIndex ==
            src_queue_family_index &&
        other_buffer_memory_barrier.dstQueueFamilyIndex ==
            dst_queue_family_index) {
      // The barrier is already pending.
      current_pending_barrier_.src_stage_mask |= src_stage_mask;
      current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
      return true;
    }
    SplitPendingBarrier();
    break;
  }

  current_pending_barrier_.src_stage_mask |= src_stage_mask;
  current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
  VkBufferMemoryBarrier& buffer_memory_barrier =
      pending_barriers_buffer_memory_barriers_.emplace_back();
  buffer_memory_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  buffer_memory_barrier.pNext = nullptr;
  buffer_memory_barrier.srcAccessMask = src_access_mask;
  buffer_memory_barrier.dstAccessMask = dst_access_mask;
  buffer_memory_barrier.srcQueueFamilyIndex = src_queue_family_index;
  buffer_memory_barrier.dstQueueFamilyIndex = dst_queue_family_index;
  buffer_memory_barrier.buffer = buffer;
  buffer_memory_barrier.offset = offset;
  buffer_memory_barrier.size = size;
  return true;
}

bool VulkanCommandProcessor::PushImageMemoryBarrier(
    VkImage image, const VkImageSubresourceRange& subresource_range,
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
    VkImageLayout old_layout, VkImageLayout new_layout,
    uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask &&
      src_access_mask == dst_access_mask && old_layout == new_layout &&
      src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Separate different barriers for overlapping image subresource ranges into
  // different pipeline barrier commands.
  for (const VkImageMemoryBarrier& other_image_memory_barrier :
       pending_barriers_image_memory_barriers_) {
    if (other_image_memory_barrier.image != image ||
        !(other_image_memory_barrier.subresourceRange.aspectMask &
          subresource_range.aspectMask) ||
        (subresource_range.levelCount != VK_REMAINING_MIP_LEVELS &&
         subresource_range.baseMipLevel + subresource_range.levelCount <=
             other_image_memory_barrier.subresourceRange.baseMipLevel) ||
        (other_image_memory_barrier.subresourceRange.levelCount !=
             VK_REMAINING_MIP_LEVELS &&
         other_image_memory_barrier.subresourceRange.baseMipLevel +
                 other_image_memory_barrier.subresourceRange.levelCount <=
             subresource_range.baseMipLevel) ||
        (subresource_range.layerCount != VK_REMAINING_ARRAY_LAYERS &&
         subresource_range.baseArrayLayer + subresource_range.layerCount <=
             other_image_memory_barrier.subresourceRange.baseArrayLayer) ||
        (other_image_memory_barrier.subresourceRange.layerCount !=
             VK_REMAINING_ARRAY_LAYERS &&
         other_image_memory_barrier.subresourceRange.baseArrayLayer +
                 other_image_memory_barrier.subresourceRange.layerCount <=
             subresource_range.baseArrayLayer)) {
      continue;
    }
    if (other_image_memory_barrier.subresourceRange.aspectMask ==
            subresource_range.aspectMask &&
        other_image_memory_barrier.subresourceRange.baseMipLevel ==
            subresource_range.baseMipLevel &&
        other_image_memory_barrier.subresourceRange.levelCount ==
            subresource_range.levelCount &&
        other_image_memory_barrier.subresourceRange.baseArrayLayer ==
            subresource_range.baseArrayLayer &&
        other_image_memory_barrier.subresourceRange.layerCount ==
            subresource_range.layerCount &&
        other_image_memory_barrier.srcAccessMask == src_access_mask &&
        other_image_memory_barrier.dstAccessMask == dst_access_mask &&
        other_image_memory_barrier.oldLayout == old_layout &&
        other_image_memory_barrier.newLayout == new_layout &&
        other_image_memory_barrier.srcQueueFamilyIndex ==
            src_queue_family_index &&
        other_image_memory_barrier.dstQueueFamilyIndex ==
            dst_queue_family_index) {
      // The barrier is already pending.
      current_pending_barrier_.src_stage_mask |= src_stage_mask;
      current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
      return true;
    }
    SplitPendingBarrier();
    break;
  }

  current_pending_barrier_.src_stage_mask |= src_stage_mask;
  current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
  VkImageMemoryBarrier& image_memory_barrier =
      pending_barriers_image_memory_barriers_.emplace_back();
  image_memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  image_memory_barrier.pNext = nullptr;
  image_memory_barrier.srcAccessMask = src_access_mask;
  image_memory_barrier.dstAccessMask = dst_access_mask;
  image_memory_barrier.oldLayout = old_layout;
  image_memory_barrier.newLayout = new_layout;
  image_memory_barrier.srcQueueFamilyIndex = src_queue_family_index;
  image_memory_barrier.dstQueueFamilyIndex = dst_queue_family_index;
  image_memory_barrier.image = image;
  image_memory_barrier.subresourceRange = subresource_range;
  return true;
}

bool VulkanCommandProcessor::SubmitBarriers(bool force_end_render_pass) {
  assert_true(submission_open_);
  SplitPendingBarrier();
  if (pending_barriers_.empty()) {
    if (force_end_render_pass) {
      EndRenderPass();
    }
    return false;
  }
  EndRenderPass();
  for (auto it = pending_barriers_.cbegin(); it != pending_barriers_.cend();
       ++it) {
    auto it_next = std::next(it);
    bool is_last = it_next == pending_barriers_.cend();
    // .data() + offset, not &[offset], for buffer and image barriers, because
    // if there are no buffer or image memory barriers in the last pipeline
    // barriers, the offsets may be equal to the sizes of the vectors.
    deferred_command_buffer_.CmdVkPipelineBarrier(
        it->src_stage_mask ? it->src_stage_mask
                           : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        it->dst_stage_mask ? it->dst_stage_mask
                           : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr,
        uint32_t((is_last ? pending_barriers_buffer_memory_barriers_.size()
                          : it_next->buffer_memory_barriers_offset) -
                 it->buffer_memory_barriers_offset),
        pending_barriers_buffer_memory_barriers_.data() +
            it->buffer_memory_barriers_offset,
        uint32_t((is_last ? pending_barriers_image_memory_barriers_.size()
                          : it_next->image_memory_barriers_offset) -
                 it->image_memory_barriers_offset),
        pending_barriers_image_memory_barriers_.data() +
            it->image_memory_barriers_offset);
  }
  pending_barriers_.clear();
  pending_barriers_buffer_memory_barriers_.clear();
  pending_barriers_image_memory_barriers_.clear();
  current_pending_barrier_.buffer_memory_barriers_offset = 0;
  current_pending_barrier_.image_memory_barriers_offset = 0;
  return true;
}

void VulkanCommandProcessor::SubmitBarriersAndEnterRenderTargetCacheRenderPass(
    VkRenderPass render_pass,
    const VulkanRenderTargetCache::Framebuffer* framebuffer) {
  SubmitBarriers(false);
  if (current_render_pass_ == render_pass &&
      current_framebuffer_ == framebuffer) {
    return;
  }
  if (current_render_pass_ != VK_NULL_HANDLE) {
    deferred_command_buffer_.CmdVkEndRenderPass();
  }
  current_render_pass_ = render_pass;
  current_framebuffer_ = framebuffer;
  VkRenderPassBeginInfo render_pass_begin_info;
  render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  render_pass_begin_info.pNext = nullptr;
  render_pass_begin_info.renderPass = render_pass;
  render_pass_begin_info.framebuffer = framebuffer->framebuffer;
  render_pass_begin_info.renderArea.offset.x = 0;
  render_pass_begin_info.renderArea.offset.y = 0;
  // TODO(Triang3l): Actual dirty width / height in the deferred command
  // buffer.
  render_pass_begin_info.renderArea.extent = framebuffer->host_extent;
  render_pass_begin_info.clearValueCount = 0;
  render_pass_begin_info.pClearValues = nullptr;
  deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info,
                                                VK_SUBPASS_CONTENTS_INLINE);
}

void VulkanCommandProcessor::EndRenderPass() {
  assert_true(submission_open_);
  if (current_render_pass_ == VK_NULL_HANDLE) {
    return;
  }
  deferred_command_buffer_.CmdVkEndRenderPass();
  current_render_pass_ = VK_NULL_HANDLE;
  current_framebuffer_ = nullptr;
}

VkDescriptorSet VulkanCommandProcessor::AllocateSingleTransientDescriptor(
    SingleTransientDescriptorLayout transient_descriptor_layout) {
  assert_true(frame_open_);
  VkDescriptorSet descriptor_set;
  std::vector<VkDescriptorSet>& transient_descriptors_free =
      single_transient_descriptors_free_[size_t(transient_descriptor_layout)];
  if (!transient_descriptors_free.empty()) {
    descriptor_set = transient_descriptors_free.back();
    transient_descriptors_free.pop_back();
  } else {
    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    bool is_storage_buffer =
        transient_descriptor_layout ==
        SingleTransientDescriptorLayout::kStorageBufferCompute;
    ui::vulkan::LinkedTypeDescriptorSetAllocator&
        transient_descriptor_allocator =
            is_storage_buffer ? transient_descriptor_allocator_storage_buffer_
                              : transient_descriptor_allocator_uniform_buffer_;
    VkDescriptorPoolSize descriptor_count;
    descriptor_count.type = is_storage_buffer
                                ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor_count.descriptorCount = 1;
    descriptor_set = transient_descriptor_allocator.Allocate(
        GetSingleTransientDescriptorLayout(transient_descriptor_layout),
        &descriptor_count, 1);
    if (descriptor_set == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
  }
  UsedSingleTransientDescriptor used_descriptor;
  used_descriptor.frame = frame_current_;
  used_descriptor.layout = transient_descriptor_layout;
  used_descriptor.set = descriptor_set;
  single_transient_descriptors_used_.emplace_back(used_descriptor);
  return descriptor_set;
}

VkDescriptorSetLayout VulkanCommandProcessor::GetTextureDescriptorSetLayout(
    bool is_vertex, size_t texture_count, size_t sampler_count) {
  size_t binding_count = texture_count + sampler_count;
  if (!binding_count) {
    return descriptor_set_layout_empty_;
  }

  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = uint32_t(texture_count);
  texture_descriptor_set_layout_key.sampler_count = uint32_t(sampler_count);
  texture_descriptor_set_layout_key.is_vertex = uint32_t(is_vertex);
  auto it_existing =
      descriptor_set_layouts_textures_.find(texture_descriptor_set_layout_key);
  if (it_existing != descriptor_set_layouts_textures_.end()) {
    return it_existing->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  descriptor_set_layout_bindings_.clear();
  descriptor_set_layout_bindings_.reserve(binding_count);
  VkShaderStageFlags stage_flags =
      is_vertex ? guest_shader_vertex_stages_ : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (size_t i = 0; i < texture_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = uint32_t(i);
    descriptor_set_layout_binding.descriptorType =
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  for (size_t i = 0; i < sampler_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = uint32_t(texture_count + i);
    descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = uint32_t(binding_count);
  descriptor_set_layout_create_info.pBindings =
      descriptor_set_layout_bindings_.data();
  VkDescriptorSetLayout texture_descriptor_set_layout;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &texture_descriptor_set_layout) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  descriptor_set_layouts_textures_.emplace(texture_descriptor_set_layout_key,
                                           texture_descriptor_set_layout);
  return texture_descriptor_set_layout;
}

const VulkanPipelineCache::PipelineLayoutProvider*
VulkanCommandProcessor::GetPipelineLayout(size_t texture_count_pixel,
                                          size_t sampler_count_pixel,
                                          size_t texture_count_vertex,
                                          size_t sampler_count_vertex) {
  PipelineLayoutKey pipeline_layout_key;
  pipeline_layout_key.texture_count_pixel = uint16_t(texture_count_pixel);
  pipeline_layout_key.sampler_count_pixel = uint16_t(sampler_count_pixel);
  pipeline_layout_key.texture_count_vertex = uint16_t(texture_count_vertex);
  pipeline_layout_key.sampler_count_vertex = uint16_t(sampler_count_vertex);
  {
    auto it = pipeline_layouts_.find(pipeline_layout_key);
    if (it != pipeline_layouts_.end()) {
      return &it->second;
    }
  }

  VkDescriptorSetLayout descriptor_set_layout_textures_vertex =
      GetTextureDescriptorSetLayout(true, texture_count_vertex,
                                    sampler_count_vertex);
  if (descriptor_set_layout_textures_vertex == VK_NULL_HANDLE) {
    XELOGE(
        "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
        "and {} samplers for guest vertex shaders",
        texture_count_vertex, sampler_count_vertex);
    return nullptr;
  }
  VkDescriptorSetLayout descriptor_set_layout_textures_pixel =
      GetTextureDescriptorSetLayout(false, texture_count_pixel,
                                    sampler_count_pixel);
  if (descriptor_set_layout_textures_pixel == VK_NULL_HANDLE) {
    XELOGE(
        "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
        "and {} samplers for guest pixel shaders",
        texture_count_pixel, sampler_count_pixel);
    return nullptr;
  }

  VkDescriptorSetLayout
      descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetCount];
  // Immutable layouts.
  descriptor_set_layouts
      [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
          descriptor_set_layout_shared_memory_and_edram_;
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetConstants] =
      descriptor_set_layout_constants_;
  // Mutable layouts.
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
      descriptor_set_layout_textures_vertex;
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
      descriptor_set_layout_textures_pixel;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Available to pipelines that use the generic tessellation passthrough
  // vertex shader / tessellation-control shader (see TessellationPushConstants
  // above) - unreferenced, and therefore inert, for all other (non-
  // tessellated, or tessellated-with-a-real-guest-domain-shader) pipelines
  // sharing this same layout.
  VkPushConstantRange tessellation_push_constant_range;
  tessellation_push_constant_range.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
  tessellation_push_constant_range.offset = 0;
  tessellation_push_constant_range.size =
      sizeof(TessellationPushConstants);

  VkPipelineLayoutCreateInfo pipeline_layout_create_info;
  pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.pNext = nullptr;
  pipeline_layout_create_info.flags = 0;
  pipeline_layout_create_info.setLayoutCount =
      uint32_t(xe::countof(descriptor_set_layouts));
  pipeline_layout_create_info.pSetLayouts = descriptor_set_layouts;
  pipeline_layout_create_info.pushConstantRangeCount = 1;
  pipeline_layout_create_info.pPushConstantRanges =
      &tessellation_push_constant_range;
  VkPipelineLayout pipeline_layout;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr,
                                 &pipeline_layout) != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan pipeline layout for guest drawing with {} "
        "pixel shader and {} vertex shader textures",
        texture_count_pixel, texture_count_vertex);
    return nullptr;
  }
  auto emplaced_pair = pipeline_layouts_.emplace(
      std::piecewise_construct, std::forward_as_tuple(pipeline_layout_key),
      std::forward_as_tuple(pipeline_layout,
                            descriptor_set_layout_textures_vertex,
                            descriptor_set_layout_textures_pixel));
  // unordered_map insertion doesn't invalidate element references.
  return &emplaced_pair.first->second;
}

VulkanCommandProcessor::ScratchBufferAcquisition
VulkanCommandProcessor::AcquireScratchGpuBuffer(
    VkDeviceSize size, VkPipelineStageFlags initial_stage_mask,
    VkAccessFlags initial_access_mask) {
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || !size) {
    return ScratchBufferAcquisition();
  }

  uint64_t submission_current = GetCurrentSubmission();

  if (scratch_buffer_ != VK_NULL_HANDLE && size <= scratch_buffer_size_) {
    // Already used previously - transition.
    PushBufferMemoryBarrier(scratch_buffer_, 0, VK_WHOLE_SIZE,
                            scratch_buffer_last_stage_mask_, initial_stage_mask,
                            scratch_buffer_last_access_mask_,
                            initial_access_mask);
    scratch_buffer_last_stage_mask_ = initial_stage_mask;
    scratch_buffer_last_access_mask_ = initial_access_mask;
    scratch_buffer_last_usage_submission_ = submission_current;
    scratch_buffer_used_ = true;
    return ScratchBufferAcquisition(*this, scratch_buffer_, initial_stage_mask,
                                    initial_access_mask);
  }

  size = xe::align(size, kScratchBufferSizeIncrement);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();

  VkDeviceMemory new_scratch_buffer_memory;
  VkBuffer new_scratch_buffer;
  // VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT for
  // texture loading.
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, size,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, new_scratch_buffer,
          new_scratch_buffer_memory)) {
    XELOGE(
        "VulkanCommandProcessor: Failed to create a {} MB scratch GPU buffer",
        size >> 20);
    return ScratchBufferAcquisition();
  }

  if (submission_completed_ >= scratch_buffer_last_usage_submission_) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    if (scratch_buffer_ != VK_NULL_HANDLE) {
      dfn.vkDestroyBuffer(device, scratch_buffer_, nullptr);
    }
    if (scratch_buffer_memory_ != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, scratch_buffer_memory_, nullptr);
    }
  } else {
    if (scratch_buffer_ != VK_NULL_HANDLE) {
      destroy_buffers_.emplace_back(scratch_buffer_last_usage_submission_,
                                    scratch_buffer_);
    }
    if (scratch_buffer_memory_ != VK_NULL_HANDLE) {
      destroy_memory_.emplace_back(scratch_buffer_last_usage_submission_,
                                   scratch_buffer_memory_);
    }
  }

  scratch_buffer_memory_ = new_scratch_buffer_memory;
  scratch_buffer_ = new_scratch_buffer;
  scratch_buffer_size_ = size;
  // Not used yet, no need for a barrier.
  scratch_buffer_last_stage_mask_ = initial_access_mask;
  scratch_buffer_last_access_mask_ = initial_stage_mask;
  scratch_buffer_last_usage_submission_ = submission_current;
  scratch_buffer_used_ = true;
  return ScratchBufferAcquisition(*this, new_scratch_buffer, initial_stage_mask,
                                  initial_access_mask);
}

void VulkanCommandProcessor::BindExternalGraphicsPipeline(
    VkPipeline pipeline, bool keep_dynamic_depth_bias,
    bool keep_dynamic_blend_constants, bool keep_dynamic_stencil_mask_ref) {
  if (!keep_dynamic_depth_bias) {
    dynamic_depth_bias_update_needed_ = true;
  }
  if (!keep_dynamic_blend_constants) {
    dynamic_blend_constants_update_needed_ = true;
  }
  if (!keep_dynamic_stencil_mask_ref) {
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
  }
  if (current_external_graphics_pipeline_ == pipeline) {
    return;
  }
  deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                             pipeline);
  current_external_graphics_pipeline_ = pipeline;
  current_guest_graphics_pipeline_ = VK_NULL_HANDLE;
  current_guest_graphics_pipeline_layout_ = VK_NULL_HANDLE;
}

void VulkanCommandProcessor::BindExternalComputePipeline(VkPipeline pipeline) {
  if (current_external_compute_pipeline_ == pipeline) {
    return;
  }
  deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE,
                                             pipeline);
  current_external_compute_pipeline_ = pipeline;
}

void VulkanCommandProcessor::SetViewport(const VkViewport& viewport) {
  if (!dynamic_viewport_update_needed_) {
    dynamic_viewport_update_needed_ |= dynamic_viewport_.x != viewport.x;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.y != viewport.y;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.width != viewport.width;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.height != viewport.height;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.minDepth != viewport.minDepth;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.maxDepth != viewport.maxDepth;
  }
  if (dynamic_viewport_update_needed_) {
    dynamic_viewport_ = viewport;
    deferred_command_buffer_.CmdVkSetViewport(0, 1, &dynamic_viewport_);
    dynamic_viewport_update_needed_ = false;
  }
}

void VulkanCommandProcessor::SetScissor(const VkRect2D& scissor) {
  if (!dynamic_scissor_update_needed_) {
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.offset.x != scissor.offset.x;
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.offset.y != scissor.offset.y;
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.extent.width != scissor.extent.width;
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.extent.height != scissor.extent.height;
  }
  if (dynamic_scissor_update_needed_) {
    dynamic_scissor_ = scissor;
    deferred_command_buffer_.CmdVkSetScissor(0, 1, &dynamic_scissor_);
    dynamic_scissor_update_needed_ = false;
  }
}

Shader* VulkanCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                           uint32_t guest_address,
                                           const uint32_t* host_address,
                                           uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool VulkanCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type,
                                       uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    return IssueCopy();
  }

  const ui::vulkan::VulkanDevice::Properties& device_properties =
      GetVulkanDevice()->properties();

  memexport_ranges_.clear();

  // Vertex shader analysis.
  auto vertex_shader = static_cast<VulkanShader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    return false;
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  // If the shader uses memory export, collect the exported ranges. When vertex
  // stores are reliable, the memexport happens during the graphics draw's
  // vertex shader; when memexport_use_compute_ is set (Adreno / no vertex
  // stores), a compute dispatch emulating the vertex shader does the export
  // before the draw (see below).
  if (vertex_shader->memexport_eM_written() != 0 &&
      (device_properties.vertexPipelineStoresAndAtomics ||
       memexport_use_compute_)) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  VulkanShader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // See xenos::EdramMode for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<VulkanShader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader,
                                                             regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (memexport_ranges_.empty()) {
      // This draw has no effect.
      return true;
    }
  }
  if (pixel_shader && pixel_shader->memexport_eM_written() != 0 &&
      device_properties.fragmentStoresAndAtomics) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }

  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(
                          regs.Get<reg::SQ_PROGRAM_CNTL>(),
                          regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos))
                   : 0;

  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  SpirvShaderTranslator::Modification vertex_shader_modification;
  SpirvShaderTranslator::Modification pixel_shader_modification;
  VulkanShader::VulkanTranslation* vertex_shader_translation;
  VulkanShader::VulkanTranslation* pixel_shader_translation;

  // Two iterations because a submission (even the current one - in which case
  // it needs to be ended, and a new one must be started) may need to be awaited
  // in case of a sampler count overflow, and if that happens, all subsystem
  // updates done previously must be performed again because the updates done
  // before the awaiting may be referencing objects destroyed by
  // CompletedSubmissionUpdated.
  for (uint32_t i = 0; i < 2; ++i) {
    if (!BeginSubmission(true)) {
      return false;
    }

    // Process primitives.
    if (!primitive_processor_->Process(primitive_processing_result)) {
      return false;
    }
    if (!primitive_processing_result.host_draw_vertex_count) {
      // Nothing to draw.
      return true;
    }
    // TODO(Triang3l): Geometry-type-specific vertex shader, vertex shader as
    // compute.
    // Tessellated draws (domain shader modes - CP-indexed or patch-indexed,
    // triangle/quad/line) only have a SPIR-V translation path for the
    // adaptive triangle patch case (kTriangleDomainPatchIndexed + kAdaptive -
    // the one actually used by Halo 3 / NFS Carbon's water/terrain
    // rendering; see VulkanPipelineCache::
    // EnsureTessellationShadersAdaptiveTriangleCreated and
    // SpirvShaderTranslator::StartVertexOrTessEvalShaderBeforeMain). Other
    // domain types/modes still cleanly no-op (matching the "this draw has no
    // effect" convention used elsewhere in this function) rather than fail
    // repeatedly, which was indistinguishable from a hang to the user even
    // though the command processor was correctly moving on.
    bool is_adaptive_triangle_tessellation =
        primitive_processing_result.host_vertex_shader_type ==
            Shader::HostVertexShaderType::kTriangleDomainPatchIndexed &&
        regs.Get<reg::VGT_HOS_CNTL>().tess_mode ==
            xenos::TessellationMode::kAdaptive;
    if (primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kVertex &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        !is_adaptive_triangle_tessellation) {
      return true;
    }

    // Shader modifications.
    vertex_shader_modification =
        pipeline_cache_->GetCurrentVertexShaderModification(
            *vertex_shader, primitive_processing_result.host_vertex_shader_type,
            interpolator_mask, ps_param_gen_pos != UINT32_MAX);
    pixel_shader_modification =
        pixel_shader ? pipeline_cache_->GetCurrentPixelShaderModification(
                           *pixel_shader, interpolator_mask, ps_param_gen_pos)
                     : SpirvShaderTranslator::Modification(0);

    // Translate the shaders now to obtain the sampler bindings.
    vertex_shader_translation = static_cast<VulkanShader::VulkanTranslation*>(
        vertex_shader->GetOrCreateTranslation(
            vertex_shader_modification.value));
    pixel_shader_translation =
        pixel_shader ? static_cast<VulkanShader::VulkanTranslation*>(
                           pixel_shader->GetOrCreateTranslation(
                               pixel_shader_modification.value))
                     : nullptr;
    if (!pipeline_cache_->EnsureShadersTranslated(vertex_shader_translation,
                                                  pixel_shader_translation)) {
      return false;
    }

    // Obtain the samplers. Note that the bindings don't depend on the shader
    // modification, so if on the second iteration of this loop it becomes
    // different for some reason (like a race condition with the guest in index
    // buffer processing in the primitive processor resulting in different host
    // vertex shader types), the bindings will stay the same.
    // TODO(Triang3l): Sampler caching and reuse for adjacent draws within one
    // submission.
    uint32_t samplers_overflowed_count = 0;
    for (uint32_t j = 0; j < 2; ++j) {
      std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>&
          shader_samplers =
              j ? current_samplers_pixel_ : current_samplers_vertex_;
      if (!i) {
        shader_samplers.clear();
      }
      const VulkanShader* shader = j ? pixel_shader : vertex_shader;
      if (!shader) {
        continue;
      }
      const std::vector<VulkanShader::SamplerBinding>& shader_sampler_bindings =
          shader->GetSamplerBindingsAfterTranslation();
      if (!i) {
        shader_samplers.reserve(shader_sampler_bindings.size());
        for (const VulkanShader::SamplerBinding& shader_sampler_binding :
             shader_sampler_bindings) {
          shader_samplers.emplace_back(
              texture_cache_->GetSamplerParameters(shader_sampler_binding),
              VK_NULL_HANDLE);
        }
      }
      for (std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
               shader_sampler_pair : shader_samplers) {
        // UseSampler calls are needed even on the second iteration in case the
        // submission was broken (and thus the last usage submission indices for
        // the used samplers need to be updated) due to an overflow within one
        // submission. Though sampler overflow is a very rare situation overall.
        bool sampler_overflowed;
        VkSampler shader_sampler = texture_cache_->UseSampler(
            shader_sampler_pair.first, sampler_overflowed);
        shader_sampler_pair.second = shader_sampler;
        if (shader_sampler == VK_NULL_HANDLE) {
          if (!sampler_overflowed || i) {
            // If !sampler_overflowed, just failed to create a sampler for some
            // reason.
            // If i == 1, an overflow has happened twice, can't recover from it
            // anymore (would enter an infinite loop otherwise if the number of
            // attempts was not limited to 2). Possibly too many unique samplers
            // in one draw, or failed to await submission completion.
            return false;
          }
          ++samplers_overflowed_count;
        }
      }
    }
    if (!samplers_overflowed_count) {
      break;
    }
    assert_zero(i);
    // Free space for as many samplers as how many haven't been allocated
    // successfully - obtain the submission index that needs to be awaited to
    // reuse `samplers_overflowed_count` slots. This must be done after all the
    // UseSampler calls, not inside the loop calling UseSampler, because earlier
    // UseSampler calls may "mark for deletion" some samplers that later
    // UseSampler calls in the loop may actually demand.
    uint64_t sampler_overflow_await_submission =
        texture_cache_->GetSubmissionToAwaitOnSamplerOverflow(
            samplers_overflowed_count);
    assert_true(sampler_overflow_await_submission <= GetCurrentSubmission());
    CheckSubmissionFenceAndDeviceLoss(sampler_overflow_await_submission);
  }

  // Set up the render targets - this may perform dispatches and draws.
  reg::RB_DEPTHCONTROL normalized_depth_control =
      draw_util::GetNormalizedDepthControl(regs);
  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(
                         regs, pixel_shader->writes_color_targets())
                   : 0;
  if (!render_target_cache_->Update(is_rasterization_done,
                                    normalized_depth_control,
                                    normalized_color_mask, *vertex_shader)) {
    return false;
  }

  // TESTRIG(halo3-transfer): the ownership transfers (the copy-forward
  // "repaint") for this draw just ran inside Update(); the vista's own geometry
  // hasn't drawn yet. Capture the vista's 4xMSAA G-buffer RT (tile 1216) here,
  // AFTER the repaint but BEFORE geometry, to see whether the repaint carried
  // the previous frame's content faithfully or corrupted it. Read-only /
  // non-destructive (restores RT state). See docs/HALO3_FINDINGS_CHECKLIST.md.
  //
  // Gated: this sits in the per-draw path, so it must cost nothing when the
  // Halo 3 investigation is not running. It was previously called
  // unconditionally on every draw - cheap (it early-outs on the RT key) but not
  // free, and it broke the rule that diagnostics stay off unless in use. Kept
  // rather than deleted because that investigation is paused, not finished.
  if (XE_AE_DIAG_ENABLED("debug.canary.halo3_vista_probe")) {
    render_target_cache_->TestrigCaptureVistaRtPostTransfer();
  }

  // Create the pipeline (for this, need the render pass from the render target
  // cache), translating the shaders - doing this now to obtain the used
  // textures.
  VkPipeline pipeline;
  const VulkanPipelineCache::PipelineLayoutProvider* pipeline_layout_provider;
  if (!pipeline_cache_->ConfigurePipeline(
          vertex_shader_translation, pixel_shader_translation,
          primitive_processing_result, normalized_depth_control,
          normalized_color_mask,
          render_target_cache_->last_update_render_pass_key(), pipeline,
          pipeline_layout_provider)) {
    return false;
  }

  // Update the textures before most other work in the submission because
  // samplers depend on this (and in case of sampler overflow in a submission,
  // submissions must be split) - may perform dispatches and copying.
  uint32_t used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation() |
      (pixel_shader != nullptr
           ? pixel_shader->GetUsedTextureMaskAfterTranslation()
           : 0);
  // TESTRIG(halo3-geo-corruption): shader context for whichever
  // INVALID_TEXFETCH_SLOT log line(s) immediately follow from the
  // RequestTextures call below - correlate by adjacency, since
  // TextureCache::RequestTextures doesn't have the shader hashes.
  {
    static std::atomic<bool> testrig_gpu_enabled_tex{true};
    static std::atomic<int64_t> testrig_gpu_next_check_ms_tex{0};
    if (xe::testrig::HotPathEnabledCached("gpu", testrig_gpu_enabled_tex,
                                           testrig_gpu_next_check_ms_tex)) {
      XELOGI("TEXREQUEST vsh={:016X} psh={:016X} mask=0x{:08X}",
             vertex_shader->ucode_data_hash(),
             pixel_shader ? pixel_shader->ucode_data_hash() : 0,
             used_texture_mask);
    }
  }
  texture_cache_->RequestTextures(used_texture_mask);

  // Update the graphics pipeline, and if the new graphics pipeline has a
  // different layout, invalidate incompatible descriptor sets before updating
  // current_guest_graphics_pipeline_layout_.
  if (current_guest_graphics_pipeline_ != pipeline) {
    deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                               pipeline);
    current_guest_graphics_pipeline_ = pipeline;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
  }
  auto pipeline_layout =
      static_cast<const PipelineLayout*>(pipeline_layout_provider);
  if (current_guest_graphics_pipeline_layout_ != pipeline_layout) {
    if (current_guest_graphics_pipeline_layout_) {
      // Keep descriptor set layouts for which the new pipeline layout is
      // compatible with the previous one (pipeline layouts are compatible for
      // set N if set layouts 0 through N are compatible).
      uint32_t descriptor_sets_kept =
          uint32_t(SpirvShaderTranslator::kDescriptorSetCount);
      if (current_guest_graphics_pipeline_layout_
              ->descriptor_set_layout_textures_vertex_ref() !=
          pipeline_layout->descriptor_set_layout_textures_vertex_ref()) {
        descriptor_sets_kept = std::min(
            descriptor_sets_kept,
            uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesVertex));
      }
      if (current_guest_graphics_pipeline_layout_
              ->descriptor_set_layout_textures_pixel_ref() !=
          pipeline_layout->descriptor_set_layout_textures_pixel_ref()) {
        descriptor_sets_kept = std::min(
            descriptor_sets_kept,
            uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesPixel));
      }
    } else {
      // No or unknown pipeline layout previously bound - all bindings are in an
      // indeterminate state.
      current_graphics_descriptor_sets_bound_up_to_date_ = 0;
    }
    current_guest_graphics_pipeline_layout_ = pipeline_layout;
  }

  if (primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kTriangleDomainPatchIndexed &&
      regs.Get<reg::VGT_HOS_CNTL>().tess_mode ==
          xenos::TessellationMode::kAdaptive) {
    // Data for the generic tessellation passthrough vertex shader and
    // tessellation-control shader - see VulkanCommandProcessor::
    // TessellationPushConstants and VulkanPipelineCache::
    // EnsureTessellationShadersAdaptiveTriangleCreated. Same register
    // sources as the D3D12 backend's equivalent system constants
    // (d3d12_command_processor.cc's UpdateSystemConstantValues).
    TessellationPushConstants tessellation_push_constants;
    tessellation_push_constants.vertex_index_endian =
        uint32_t(primitive_processing_result.host_shader_index_endian);
    tessellation_push_constants.vertex_index_offset =
        uint32_t(regs.Get<reg::VGT_INDX_OFFSET>().indx_offset);
    tessellation_push_constants.vertex_index_min =
        regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
    tessellation_push_constants.vertex_index_max =
        regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
    // Tessellation factors are biased by 1.0 relative to the raw guest
    // values (matching the D3D12 backend and the images referenced in
    // adaptive_triangle.hs.hlsl).
    tessellation_push_constants.tessellation_factor_min =
        regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
    tessellation_push_constants.tessellation_factor_max =
        regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
    deferred_command_buffer_.CmdVkPushConstants(
        pipeline_layout->GetPipelineLayout(),
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        0, sizeof(tessellation_push_constants), &tessellation_push_constants);
  }

  bool host_render_targets_used = render_target_cache_->GetPath() ==
                                  RenderTargetCache::Path::kHostRenderTargets;

  // Get dynamic rasterizer state.
  draw_util::ViewportInfo viewport_info;

  // Just handling maxViewportDimensions is enough - viewportBoundsRange[1] must
  // be at least 2 * max(maxViewportDimensions[0...1]) - 1, and
  // maxViewportDimensions must be greater than or equal to the size of the
  // largest possible framebuffer attachment (if the viewport has positive
  // offset and is between maxViewportDimensions and viewportBoundsRange[1],
  // GetHostViewportInfo will adjust ndc_scale/ndc_offset to clamp it, and the
  // clamped range will be outside the largest possible framebuffer anyway.
  // FIXME(Triang3l): Possibly handle maxViewportDimensions and
  // viewportBoundsRange separately because when using fragment shader
  // interlocks, framebuffers are not used, while the range may be wider than
  // dimensions? Though viewport bigger than 4096 - the smallest possible
  // maximum dimension (which is below the 8192 texture size limit on the Xbox
  // 360) - and with offset, is probably a situation that never happens in real
  // life. Or even disregard the viewport bounds range in the fragment shader
  // interlocks case completely - apply the viewport and the scissor offset
  // directly to pixel address and to things like ps_param_gen.
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();
  draw_util::GetViewportInfoArgs gviargs{};
  gviargs.Setup(draw_resolution_scale_x, draw_resolution_scale_y,
                texture_cache_->draw_resolution_scale_x_divisor(),
                texture_cache_->draw_resolution_scale_y_divisor(), false,
                device_properties.maxViewportDimensions[0],
                device_properties.maxViewportDimensions[1], true,
                normalized_depth_control, false, host_render_targets_used,
                pixel_shader && pixel_shader->writes_depth());
  gviargs.SetupRegisterValues(regs);

  draw_util::GetHostViewportInfo(&gviargs, viewport_info);

  // TESTRIG(gpu): correlate the NDC-Y regime with the DRAW that produced it.
  //
  // debug.canary.ndcy already proves two regimes exist (vport_y_scale_ena=1 ->
  // ndc_scale[1] negative, flip applied; =0 -> positive, no flip). That alone
  // does not say which regime the Halo 3 VISTA draw is in. Tagging each
  // distinct (shader, ndc_scale[1] sign) pair answers it directly.
  //
  // Deduplicated per shader hash + sign, so this is a handful of lines.
  if (XE_AE_DIAG_ENABLED("debug.canary.ndcy_draw")) {
    static std::atomic<uint64_t> ndcy_draw_keys[64];
    uint64_t vs_hash = vertex_shader ? vertex_shader->ucode_data_hash() : 0;
    bool y_flipped = viewport_info.ndc_scale[1] < 0.0f;
    uint64_t key = (vs_hash << 1) | uint64_t(y_flipped);
    if (!key) key = 1;
    bool seen = false;
    for (auto& slot : ndcy_draw_keys) {
      uint64_t v = slot.load(std::memory_order_relaxed);
      if (v == key) { seen = true; break; }
      if (!v && slot.compare_exchange_strong(v, key)) break;
    }
    if (!seen) {
      XELOGI("NDCYDRAW vs={:016X} ndc_scale_y={} flipped={} extent_y={}",
             vs_hash, viewport_info.ndc_scale[1], y_flipped ? 1 : 0,
             viewport_info.xy_extent[1]);
    }
  }

  // DIAG(gpu/guest-constants): dump the guest-computed vertex shader constants,
  // once per distinct vertex shader.
  //
  // Rationale (docs/HALO3_VISTA_46_VS_64.md s20): the GPU renders what it is
  // given. The NDC transform, translated SPIR-V, viewport maths, resolve, dump
  // and transfer paths have now all been measured or diffed IDENTICAL to
  // XenDroid, which renders the vista correctly on this same device. What has
  // never been compared is the DATA - the view/projection matrix the guest
  // computes on the PowerPC side and uploads as float constants. A sign error
  // in the guest's own matrix inverts the scene while leaving every GPU-side
  // check identical, and it would not touch the 2D UI, which is pre-transformed
  // and never passes through a guest matrix.
  //
  // Keyed on the vertex shader hash so the two builds' logs join on shader
  // identity, exactly like NDCYDRAW. c0-c7 covers the usual view-projection
  // matrix slots; raw bits as well as decimal, because a sign flip or a
  // denormal is clearer in hex.
  //
  // Property name and output format must stay IDENTICAL in XDtester or the logs
  // will not diff.
  if (XE_AE_DIAG_ENABLED("debug.canary.vsconst")) {
    static std::atomic<uint64_t> vsconst_keys[64];
    uint64_t vs_hash =
        vertex_shader ? vertex_shader->ucode_data_hash() : uint64_t(0);
    uint64_t key = vs_hash ? vs_hash : 1;
    bool seen = false;
    for (auto& slot : vsconst_keys) {
      uint64_t v = slot.load(std::memory_order_relaxed);
      if (v == key) {
        seen = true;
        break;
      }
      if (!v && slot.compare_exchange_strong(v, key)) break;
    }
    if (!seen) {
      const uint32_t* creg = register_file_->values;
      xe::StringBuffer vb;
      vb.AppendFormat("VSCONST vs={:016X} ey={}", vs_hash,
                      viewport_info.xy_extent[1]);
      for (uint32_t c = 0; c <= 7; ++c) {
        const uint32_t* cu =
            &creg[XE_GPU_REG_SHADER_CONSTANT_000_X + (c << 2)];
        const float* cf = reinterpret_cast<const float*>(cu);
        vb.AppendFormat(
            " c{}=({:.6g},{:.6g},{:.6g},{:.6g})[{:08X},{:08X},{:08X},{:08X}]",
            c, cf[0], cf[1], cf[2], cf[3], cu[0], cu[1], cu[2], cu[3]);
      }
      XELOGI("{}", vb.buffer());
    }
  }

  // Update dynamic graphics pipeline state.
  UpdateDynamicState(viewport_info, primitive_polygonal,
                     normalized_depth_control, draw_resolution_scale_x,
                     draw_resolution_scale_y);

  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  // Whether to load the guest 32-bit (usually big-endian) vertex index
  // indirectly in the vertex shader if full 32-bit indices are not supported by
  // the host.
  bool shader_32bit_index_dma =
      !device_properties.fullDrawIndexUint32 &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32 &&
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kVertex;

  // Update system constants before uploading them.
  UpdateSystemConstantValues(primitive_polygonal, primitive_processing_result,
                             shader_32bit_index_dma, viewport_info,
                             used_texture_mask, normalized_depth_control,
                             normalized_color_mask);

  // Update uniform buffers and descriptor sets after binding the pipeline with
  // the new layout.
  if (!UpdateBindings(vertex_shader, pixel_shader)) {
    return false;
  }

  // TESTRIG(gpu): single cached toggle check reused for every counter
  // increment/diagnostic log below (see testrig_debug_server.h
  // HotPathEnabledCached) - this is the actual per-draw overhead the toggle
  // exists to remove; when off it's one atomic load, when on it costs one
  // property lookup every ~250ms. Declared here (rather than right before its
  // original use further down) so it's also in scope for the vertex-fetch
  // diagnostics in the residency loop immediately below.
  static std::atomic<bool> testrig_gpu_enabled{true};
  static std::atomic<int64_t> testrig_gpu_next_check_ms{0};
  bool testrig_gpu_hot = xe::testrig::HotPathEnabledCached(
      "gpu", testrig_gpu_enabled, testrig_gpu_next_check_ms);

  // Ensure vertex buffers are resident.
  // TODO(Triang3l): Cache residency for ranges in a way similar to how texture
  // validity is tracked.
  uint64_t vertex_buffers_resident[2] = {};
  for (const Shader::VertexBinding& vertex_binding :
       vertex_shader->vertex_bindings()) {
    uint32_t vfetch_index = vertex_binding.fetch_constant;
    if (vertex_buffers_resident[vfetch_index >> 6] &
        (uint64_t(1) << (vfetch_index & 63))) {
      continue;
    }
    xenos::xe_gpu_vertex_fetch_t vfetch_constant =
        regs.GetVertexFetch(vfetch_index);
    switch (vfetch_constant.type) {
      case xenos::FetchConstantType::kVertex:
        break;
      case xenos::FetchConstantType::kInvalidVertex:
        if (cvars::gpu_allow_invalid_fetch_constants) {
          // TESTRIG(halo3-geo-corruption): this case is normally completely
          // silent (gpu_allow_invalid_fetch_constants just lets the draw
          // through with whatever garbage is at this fetch-constant slot) -
          // log it so we can see which shader/vfetch slot hits an invalid
          // fetch constant, for the jungle-scene character/attachment
          // geometry corruption (see project memory: an earlier session
          // found 817 invalid fetch-constant addresses in a burst in this
          // same area, sharing the same second dword, never followed up).
          if (testrig_gpu_hot) {  // TESTRIG(gpu)
            XELOGI(
                "INVALID_FETCH_ALLOWED sh={:016X} vf={} dw0=0x{:08X} "
                "dw1=0x{:08X}",
                vertex_shader->ucode_data_hash(), vfetch_index,
                vfetch_constant.dword_0, vfetch_constant.dword_1);
          }
          break;
        }
        XELOGW(
            "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type! "
            "This "
            "is incorrect behavior, but you can try bypassing this by "
            "launching Xenia with --gpu_allow_invalid_fetch_constants=true.",
            vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
        return false;
      default:
        XELOGW(
            "Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
            vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
        return false;
    }
    if (!shared_memory_->RequestRange(vfetch_constant.address << 2,
                                      vfetch_constant.size << 2)) {
      XELOGE(
          "Failed to request vertex buffer at 0x{:08X} (size {}) in the shared "
          "memory",
          vfetch_constant.address << 2, vfetch_constant.size << 2);
      return false;
    }
    // TESTRIG(halo3-nondeterminism)/DEBUG(halo3-vtx): per-vfetch diagnostics
    // from the (resolved, see docs/HALO3_FINDINGS_CHECKLIST.md) memexport/
    // non-determinism investigation. Was unconditional - MEMSRC and VTXDIST
    // below do full per-draw buffer scans (not just logging), which is real
    // hot-path cost regardless of whether the log line ends up firing. Gated
    // behind testrig_gpu_hot rather than deleted, per this harness's
    // convention of keeping test code permanently but toggle-gated.
    if (testrig_gpu_hot) {  // TESTRIG(gpu)
      uint32_t vf_addr_check = vfetch_constant.address << 2;
      if (vf_addr_check >= 0x05700000u && vf_addr_check < 0x05800000u) {
        XELOGI(
            "ANYFETCH_IN_RANGE sh={:016X} vtxcount={} vf={} addr=0x{:08X} "
            "size={} eM=0x{:X}",
            vertex_shader->ucode_data_hash(),
            primitive_processing_result.host_draw_vertex_count, vfetch_index,
            vf_addr_check, vfetch_constant.size << 2,
            uint32_t(vertex_shader->memexport_eM_written()));
      }
      if (vertex_shader->memexport_eM_written() != 0) {
        uint32_t saddr = vfetch_constant.address << 2;
        uint32_t ssize = vfetch_constant.size << 2;
        const uint32_t* sd = reinterpret_cast<const uint32_t*>(
            memory_->TranslatePhysical(saddr));
        uint32_t snz = 0;
        uint32_t sdw = ssize / 4;
        for (uint32_t k = 0; k < sdw; ++k) {
          if (sd[k] != 0) ++snz;
        }
        if (sdw >= 4)
        XELOGI(
            "MEMSRC sh={:016X} vf={} addr=0x{:08X} size={} nonzero={}/{}",
            vertex_shader->ucode_data_hash(), vfetch_index, saddr, ssize, snz,
            sdw);
      }
      uint32_t vaddr = vfetch_constant.address << 2;
      uint32_t vsize = vfetch_constant.size << 2;
      if (vsize >= 4096) {
        const uint32_t* vd =
            reinterpret_cast<const uint32_t*>(memory_->TranslatePhysical(vaddr));
        uint32_t dwords = vsize / 4;
        // Scan the WHOLE buffer for the compute-memexport marker (0xCAFEF00D)
        // and for non-zero data, to tell whether the exported data lands where
        // this draw fetches (marker) and whether the buffer is populated.
        uint32_t marker_post = 0;   // 0xCAFEF00D, after eA validation
        uint32_t marker_pre = 0;    // 0xCAFE0001, before eA validation
        uint32_t nonzero_count = 0;
        uint32_t v0 = vd[0];
        bool uniform = true;
        // Distribution: first/last nonzero, and how many nonzero fall in each
        // tenth of the buffer. Clustered at the start => a count/culling limit;
        // scattered throughout => slot scatter (precision collisions).
        uint32_t first_nz = 0xFFFFFFFFu;
        uint32_t last_nz = 0;
        uint32_t hist[10] = {0};
        for (uint32_t k = 0; k < dwords; ++k) {
          uint32_t d = vd[k];
          if (d == 0xCAFEF00Du) ++marker_post;
          if (d == 0xCAFE0001u) ++marker_pre;
          if (d != 0) {
            ++nonzero_count;
            if (first_nz == 0xFFFFFFFFu) first_nz = k;
            last_nz = k;
            ++hist[(uint64_t(k) * 10) / dwords];
          }
          if (d != v0) uniform = false;
        }
        if (marker_post || marker_pre || uniform || vsize == 573440) {
          XELOGI(
              "VTXDIST idx={} addr=0x{:08X} nonzero={}/{} firstnz={} lastnz={} "
              "consumersh={:016X} consumervtx={} "
              "hist= {} {} {} {} {} {} {} {} {} {}",
              vfetch_index, vaddr, nonzero_count, dwords, first_nz, last_nz,
              vertex_shader->ucode_data_hash(),
              primitive_processing_result.host_draw_vertex_count, hist[0],
              hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7],
              hist[8], hist[9]);
        }
        // TESTRIG(halo3-recordfields): FORMAT-CORRECT value comparison.
        //
        // Why this exists: the fill metric is a proven red herring (RADV renders
        // correctly while filling LESS of this buffer than Adreno; same slots,
        // same consumers, same draw counts), so the only surviving explanation
        // is that the exported VALUES differ. The earlier VALSHAPE probe tried
        // that and had to be RETRACTED, because it read every dword as float32 -
        // but this is an 80-byte INTERLEAVED record (Stride=20 dwords) holding
        // SIX different formats, so 16 of its 20 dwords are packed half/short/
        // 2_10_10_10/8_8_8_8 data whose bit patterns merely LOOK like NaN and
        // denormals when reinterpreted as float. That artifact is what produced
        // the bogus "NaN smoking gun".
        //
        // Record layout, decoded from the producer's own vfetch instructions
        // (shader 9EA48FC2B26C325D, instr 90-100):
        //   dw 0-3   FMT_32_32_32_32_FLOAT   <-- the ONLY genuine float32 field
        //   dw 4-5   FMT_16_16_16_16_FLOAT       (half4)
        //   dw 6-7   FMT_16_16_16_16
        //   dw 8-9   FMT_16_16_16_16
        //   dw 10-11 FMT_16_16_16_16
        //   dw 12-13 FMT_16_16_16_16_FLOAT
        //   dw 14    FMT_16_16_FLOAT
        //   dw 15    FMT_16_16
        //   dw 16    FMT_2_10_10_10
        //   dw 17-18 FMT_8_8_8_8
        // So read ONLY dwords 0-3 per record, as float4.
        //
        // The discriminator is SPREAD, not magnitude: if skinned positions
        // collapse into the "tight degenerate cluster" that the p0=TRUE decode
        // predicts, the standard deviation across records is tiny; a correct
        // buffer spans a broad world-space range. Scene/animation state differs
        // between platforms so raw values are not comparable - the SHAPE is.
        // Identical code runs in the RADV oracle tree for direct comparison.
        if (vsize == 573440) {
          const uint32_t kStrideDw = 20;
          const uint32_t recs = dwords / kStrideDw;
          uint32_t rec_zero = 0, rec_nonfinite = 0, rec_valid = 0;
          double sx = 0, sy = 0, sz = 0, sxx = 0, syy = 0, szz = 0;
          float mnx = 3.4e38f, mxx = -3.4e38f;
          float mny = 3.4e38f, mxy = -3.4e38f;
          float mnz = 3.4e38f, mxz = -3.4e38f;
          for (uint32_t r = 0; r < recs; ++r) {
            const float* f =
                reinterpret_cast<const float*>(&vd[r * kStrideDw]);
            if (!std::isfinite(f[0]) || !std::isfinite(f[1]) ||
                !std::isfinite(f[2])) {
              ++rec_nonfinite;
              continue;
            }
            if (f[0] == 0.0f && f[1] == 0.0f && f[2] == 0.0f) {
              ++rec_zero;
              continue;
            }
            ++rec_valid;
            sx += f[0]; sy += f[1]; sz += f[2];
            sxx += double(f[0]) * f[0];
            syy += double(f[1]) * f[1];
            szz += double(f[2]) * f[2];
            if (f[0] < mnx) mnx = f[0];
            if (f[0] > mxx) mxx = f[0];
            if (f[1] < mny) mny = f[1];
            if (f[1] > mxy) mxy = f[1];
            if (f[2] < mnz) mnz = f[2];
            if (f[2] > mxz) mxz = f[2];
          }
          double n = rec_valid ? double(rec_valid) : 1.0;
          double vx = sxx / n - (sx / n) * (sx / n);
          double vy = syy / n - (sy / n) * (sy / n);
          double vz = szz / n - (sz / n) * (sz / n);
          XELOGI(
              "RECFIELD0 recs={} valid={} zero={} nonfinite={} "
              "mean=({:.4g},{:.4g},{:.4g}) sd=({:.4g},{:.4g},{:.4g}) "
              "xrange=[{:.4g},{:.4g}] yrange=[{:.4g},{:.4g}] "
              "zrange=[{:.4g},{:.4g}]",
              recs, rec_valid, rec_zero, rec_nonfinite, sx / n, sy / n, sz / n,
              vx > 0 ? std::sqrt(vx) : 0.0, vy > 0 ? std::sqrt(vy) : 0.0,
              vz > 0 ? std::sqrt(vz) : 0.0, mnx, mxx, mny, mxy, mnz, mxz);
        }
        // TESTRIG(halo3-recordfields2): REFINEMENT of RECFIELD0 after a correct
        // critique: "more valid records doesn't necessarily mean BETTER valid
        // records."
        //
        // RECFIELD0's "valid" only meant not-all-zero and finite, so a record
        // holding 1.5e38 counted as valid - and BOTH platforms sit at 1e37-1e38,
        // which are not plausible world-space positions (real ones are ~1e0-1e4).
        // So that comparison compared garbage to garbage. Worse, mean/sd are
        // dominated by those outliers, so two totally different datasets would
        // still look "statistically identical". And Adreno having MORE valid
        // records may itself be the defect - extra bogus records rendering as
        // the degenerate cluster - not evidence of health.
        //
        // So classify by MAGNITUDE and by SLOT instead of averaging:
        //   plaus  : all |xyz| < 1e5  -> a believable world-space position
        //   small  : all |xyz| < 1e-3 -> collapsed toward the origin (the "ball")
        //   huge   : any |xyz| >= 1e20 -> exploded / garbage bit patterns
        //   mid    : everything else
        // plus a 10-bucket histogram of WHERE the plausible records sit in the
        // buffer, so we can tell whether both platforms populate the SAME slots.
        // Identical code runs in the other tree for direct comparison.
        if (vsize == 573440) {
          const uint32_t kStrideDw = 20;
          const uint32_t recs = dwords / kStrideDw;
          uint32_t n_plaus = 0, n_small = 0, n_huge = 0, n_mid = 0;
          uint32_t n_zero2 = 0, n_nf2 = 0;
          uint32_t plaus_hist[10] = {0};
          float pmin = 3.4e38f, pmax = -3.4e38f;
          double plaus_absmax = 0.0;
          for (uint32_t r = 0; r < recs; ++r) {
            const float* f =
                reinterpret_cast<const float*>(&vd[r * kStrideDw]);
            if (!std::isfinite(f[0]) || !std::isfinite(f[1]) ||
                !std::isfinite(f[2])) {
              ++n_nf2;
              continue;
            }
            if (f[0] == 0.0f && f[1] == 0.0f && f[2] == 0.0f) {
              ++n_zero2;
              continue;
            }
            float ax = std::fabs(f[0]), ay = std::fabs(f[1]),
                  az = std::fabs(f[2]);
            float amax = ax > ay ? (ax > az ? ax : az) : (ay > az ? ay : az);
            if (amax >= 1.0e20f) {
              ++n_huge;
            } else if (amax < 1.0e-3f) {
              ++n_small;
            } else if (amax < 1.0e5f) {
              ++n_plaus;
              ++plaus_hist[(uint64_t(r) * 10) / recs];
              if (f[0] < pmin) pmin = f[0];
              if (f[0] > pmax) pmax = f[0];
              if (amax > plaus_absmax) plaus_absmax = amax;
            } else {
              ++n_mid;
            }
          }
          XELOGI(
              "RECFIELD2 recs={} zero={} nonfinite={} plausible={} small={} "
              "mid={} huge={} plaus_xrange=[{:.4g},{:.4g}] plaus_absmax={:.4g} "
              "plaushist= {} {} {} {} {} {} {} {} {} {}",
              recs, n_zero2, n_nf2, n_plaus, n_small, n_mid, n_huge,
              n_plaus ? pmin : 0.0f, n_plaus ? pmax : 0.0f, plaus_absmax,
              plaus_hist[0], plaus_hist[1], plaus_hist[2], plaus_hist[3],
              plaus_hist[4], plaus_hist[5], plaus_hist[6], plaus_hist[7],
              plaus_hist[8], plaus_hist[9]);
        }
        // VALSHAPE probe (2026-07-24): the RADV oracle proved the FILL metric is
        // a red herring (RADV renders correctly while filling LESS of this buffer
        // than Adreno). Same slots, same consumers, same draw counts -> the only
        // surviving explanation is that the exported VALUES differ. A raw byte
        // diff across platforms is meaningless (different scene/animation state),
        // so compare the statistical SHAPE of the float data instead, which is
        // scene-independent enough to be conclusive: if the skinned positions are
        // collapsing to a point, the magnitudes cluster tightly / go non-finite,
        // whereas a correct buffer spans a broad, sane world-space range.
        // Identical code runs in the RADV oracle tree for direct comparison.
        if (vsize == 573440) {
          const float* vf = reinterpret_cast<const float*>(vd);
          uint32_t n_nonfinite = 0, n_zero = 0, n_denorm = 0;
          // Magnitude buckets for finite non-zero values.
          uint32_t mag[6] = {0};  // <1e-3, <1, <1e2, <1e4, <1e6, >=1e6
          float fmin = 3.4e38f, fmax = -3.4e38f;
          double abs_sum = 0.0;
          for (uint32_t k = 0; k < dwords; ++k) {
            float f = vf[k];
            if (!std::isfinite(f)) {
              ++n_nonfinite;
              continue;
            }
            if (f == 0.0f) {
              ++n_zero;
              continue;
            }
            float a = std::fabs(f);
            if (a < 1.0e-30f) ++n_denorm;
            abs_sum += a;
            if (f < fmin) fmin = f;
            if (f > fmax) fmax = f;
            uint32_t b = a < 1e-3f ? 0 : a < 1e0f ? 1 : a < 1e2f ? 2
                         : a < 1e4f ? 3 : a < 1e6f ? 4 : 5;
            ++mag[b];
          }
          uint32_t n_finite_nz = dwords - n_nonfinite - n_zero;
          XELOGI(
              "VALSHAPE addr=0x{:08X} consumersh={:016X} nonfinite={} zero={} "
              "denorm={} finitenz={} min={:.4g} max={:.4g} meanabs={:.4g} "
              "mag[<1e-3,<1,<1e2,<1e4,<1e6,>=1e6]= {} {} {} {} {} {}",
              vaddr, vertex_shader->ucode_data_hash(), n_nonfinite, n_zero,
              n_denorm, n_finite_nz, fmin, fmax,
              n_finite_nz ? (abs_sum / double(n_finite_nz)) : 0.0, mag[0],
              mag[1], mag[2], mag[3], mag[4], mag[5]);
          // ===== SENTINEL VERIFIER (2026-07-25) =====
          // Pairs with kMemExportSentinelTest in
          // spirv_shader_translator_memexport.cc, which makes the producer write
          // the guest VERTEX INDEX into component .x of each exported element.
          // The consumer reads an 80-byte interleaved record (Stride=20 dwords,
          // verified from both shaders' ucode), and the 16-byte position stream
          // lands at record offset 0 - so record N starts at byte N*80 and its
          // first dword must decode to float(N). Memexport stores big-endian
          // (guest order, confirmed by RECDUMP), hence the byteswap.
          // This is the first SCENE-INDEPENDENT correctness check in this
          // investigation: no cross-platform comparison, no "does it look
          // plausible" judgement. match=all => producer writes the right value to
          // the right slot (bug is downstream). mismatches => producer caught.
          {
            const uint32_t kRecordStride = 80;  // bytes
            uint32_t records = vsize / kRecordStride;
            uint32_t checked = 0, match = 0, zero = 0, mismatch = 0;
            uint32_t first_bad_rec = 0xFFFFFFFFu;
            float first_bad_val = 0.0f;
            for (uint32_t rec = 0; rec < records; ++rec) {
              uint32_t raw = vd[(rec * kRecordStride) / 4];
              if (raw == 0) {
                ++zero;
                continue;
              }
              // Guest data is big-endian; byteswap before reading as float.
              uint32_t swapped = __builtin_bswap32(raw);
              float f;
              std::memcpy(&f, &swapped, sizeof(f));
              ++checked;
              if (std::isfinite(f) && f == float(rec)) {
                ++match;
              } else {
                ++mismatch;
                if (first_bad_rec == 0xFFFFFFFFu) {
                  first_bad_rec = rec;
                  first_bad_val = f;
                }
              }
            }
            XELOGI(
                "SENTINEL addr=0x{:08X} sh={:016X} records={} nonzero={} "
                "MATCH={} MISMATCH={} zero={} first_bad_rec={} "
                "first_bad_val={:.6g}",
                vaddr, vertex_shader->ucode_data_hash(), records, checked, match,
                mismatch, zero, first_bad_rec, first_bad_val);
          }
          // RECDUMP (2026-07-25): targeted follow-up. Aggregate statistics could
          // NOT distinguish the platforms (RADV, which renders correctly, has the
          // same 1e38 range and MORE NaN than Adreno), and the buffer holds six
          // different formats so any single float interpretation is unreliable.
          // So dump the RAW BITS of the first records starting at the first
          // non-zero dword - hex is format-agnostic, so comparing the structure
          // (plausible float exponents vs junk, repeated/degenerate patterns)
          // works across platforms even though absolute values differ with scene
          // state. Also print the float reading for convenience.
          if (first_nz != 0xFFFFFFFFu && first_nz + 12 <= dwords) {
            const uint32_t* p = vd + first_nz;
            const float* pf = reinterpret_cast<const float*>(p);
            XELOGI(
                "RECDUMP addr=0x{:08X} at_dword={} hex= {:08X} {:08X} {:08X} "
                "{:08X} {:08X} {:08X} {:08X} {:08X} | flt= {:.5g} {:.5g} "
                "{:.5g} {:.5g} {:.5g} {:.5g} {:.5g} {:.5g}",
                vaddr, first_nz, p[0], p[1], p[2], p[3], p[4], p[5], p[6],
                p[7], pf[0], pf[1], pf[2], pf[3], pf[4], pf[5], pf[6], pf[7]);
          }
        }
      }
    }
    vertex_buffers_resident[vfetch_index >> 6] |= uint64_t(1)
                                                  << (vfetch_index & 63);
  }

  // Synchronize the memory pages backing memory scatter export streams, and
  // calculate the range that includes the streams for the buffer barrier.
  uint32_t memexport_extent_start = UINT32_MAX, memexport_extent_end = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    // Ported from XenDroid: record the guest pages this memexport draw wrote,
    // so a later fence/coherency request can drain only when the range it cares
    // about actually holds export output.
    MarkMemexportPagesWritten(memexport_range.base_address_dwords << 2,
                              memexport_range.size_bytes);
    uint32_t memexport_range_base_bytes = memexport_range.base_address_dwords
                                          << 2;
    // DEBUG(halo3-vtx): log memexport target addresses. If 0x0574CA80 (the
    // all-zeros vertex buffer the menu vista fetches) appears here, the menu
    // geometry is memexport-generated -> memexport writeback is the bug.
    if (testrig_gpu_hot) {  // TESTRIG(gpu)
      XELOGI("MEMEXPORT_TARGET sh={:016X} vtx={} addr=0x{:08X} size={}",
             vertex_shader->ucode_data_hash(),
             primitive_processing_result.host_draw_vertex_count,
             memexport_range_base_bytes, memexport_range.size_bytes);
    }
    if (gpu_trace_enabled()) {
      GpuTrace("MEMEXPORT",
               fmt::format("sh={:016X} addr=0x{:08X} size={}",
                           vertex_shader->ucode_data_hash(),
                           memexport_range_base_bytes,
                           memexport_range.size_bytes));
    }
    // TESTRIG(halo3-nondeterminism): dump the constants feeding the
    // trunc+exact-equality branch chain (c220-c229) plus all 32 loop
    // constants, for the Halo 3 menu terrain shader specifically, once per
    // draw. Comparing this across cold boots tells us whether the CPU-side
    // simulation state feeding this shader differs boot-to-boot (a game/
    // timing-seeded explanation for the observed non-determinism) versus
    // being identical every boot (which would point at a GPU-side race
    // instead, since the inputs would be provably the same).
    if (testrig_gpu_hot &&  // TESTRIG(gpu)
        vertex_shader->ucode_data_hash() == 0x9EA48FC2B26C325Dull) {
      // TESTRIG(halo3-nondeterminism): one-shot snapshot of the target
      // buffer's content BEFORE this (the first tracked) draw touches it -
      // reflects whatever earlier GPU activity this session already left
      // there (via SharedMemory's page-validity tracking, which skips
      // re-uploading/re-zeroing from guest RAM once a page is marked
      // gpu_written). If this is genuinely non-zero on some boots, that's
      // direct evidence of boot-timing-variable "warm-up" seed state, a
      // hypothesis distinct from (and not yet ruled out by) the CPU-constant
      // and GPU-invocation-ordering checks already done.
      static bool logged_prefill_snapshot = false;
      if (!logged_prefill_snapshot) {
        logged_prefill_snapshot = true;
        const uint32_t* prefill = reinterpret_cast<const uint32_t*>(
            memory_->TranslatePhysical(memexport_range_base_bytes));
        uint32_t prefill_nonzero = 0;
        uint32_t prefill_dwords =
            std::min<uint32_t>(memexport_range.size_bytes / 4, 143360);
        uint32_t prefill_first_nz = 0xFFFFFFFFu;
        for (uint32_t k = 0; k < prefill_dwords; ++k) {
          if (prefill[k] != 0) {
            ++prefill_nonzero;
            if (prefill_first_nz == 0xFFFFFFFFu) prefill_first_nz = k;
          }
        }
        XELOGI(
            "PREFILL_SNAPSHOT addr=0x{:08X} nonzero={}/{} firstnz={} "
            "sample0=0x{:08X} sample1=0x{:08X} sample2=0x{:08X}",
            memexport_range_base_bytes, prefill_nonzero, prefill_dwords,
            prefill_first_nz, prefill[0], prefill[1], prefill[2]);
      }
      const uint32_t* regs = register_file_->values;
      xe::StringBuffer cbuf;
      cbuf.Append("CONSTDUMP c220-229:");
      for (uint32_t c = 220; c <= 229; ++c) {
        const float* cf = reinterpret_cast<const float*>(
            &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (c << 2)]);
        cbuf.AppendFormat(" c{}=({:.9g},{:.9g},{:.9g},{:.9g})", c, cf[0],
                          cf[1], cf[2], cf[3]);
      }
      XELOGI("{}", cbuf.buffer());
        // BONEC probe (2026-08-06): dump the constants the producer shader
        // actually receives, as RAW BITS. Underfill, denormal flushing and
        // fill rate have all been refuted by measurement; the one surviving
        // signal is that exported VALUES differ (meanabs 5.07x apart, disjoint
        // across 799 samples). But meanabs ~1e37 is absurd for vertex data, so
        // that metric reads packed data as float32 and cannot be interpreted
        // physically. These constants need no interpretation.
        //
        // c78.x is the slot divisor (consumer: slot = floor(vtxIndex/c78.x));
        // c144+ are the bone matrices. Both are GUEST-CPU-COMPUTED, so if they
        // differ between x64 and a64 the cause is the JIT, not the GPU.
        // Identical bits kill the CPU-side theory outright.
        // Hex, not %g - formatting hides the low-bit differences we are hunting.
        {
          xe::StringBuffer bbuf;
          bbuf.AppendFormat("BONEC bw={} sh={:016X} c78=",
                            g_bone_write_count.load(),
                            vertex_shader->ucode_data_hash());
          for (uint32_t i = 0; i < 4; ++i) {
            bbuf.AppendFormat("{:08X} ",
                regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (78 << 2) + i]);
          }
          bbuf.Append("| c144-151=");
          for (uint32_t c = 144; c <= 151; ++c) {
            for (uint32_t i = 0; i < 4; ++i) {
              bbuf.AppendFormat("{:08X} ",
                  regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (c << 2) + i]);
            }
          }
          XELOGI("{}", bbuf.buffer());
        }
      xe::StringBuffer lbuf;
      lbuf.Append("CONSTDUMP loop:");
      for (uint32_t l = 0; l < 32; ++l) {
        lbuf.AppendFormat(" l{}=0x{:08X}",
                          l, regs[XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + l]);
      }
      XELOGI("{}", lbuf.buffer());
    }
    if (!shared_memory_->RequestRange(memexport_range_base_bytes,
                                      memexport_range.size_bytes)) {
      XELOGE(
          "Failed to request memexport stream at 0x{:08X} (size {}) in the "
          "shared memory",
          memexport_range_base_bytes, memexport_range.size_bytes);
      return false;
    }
    memexport_extent_start =
        std::min(memexport_extent_start, memexport_range_base_bytes);
    memexport_extent_end =
        std::max(memexport_extent_end,
                 memexport_range_base_bytes + memexport_range.size_bytes);
  }

  // Insert the shared memory barrier if needed.
  // TODO(Triang3l): Find some PM4 command that can be used for indication of
  // when memexports should be awaited instead of inserting the barrier in Use
  // every time if memory export was done in the previous draw?
  if (memexport_extent_start < memexport_extent_end) {
    shared_memory_->Use(
        VulkanSharedMemory::Usage::kGuestDrawReadWrite,
        std::make_pair(memexport_extent_start,
                       memexport_extent_end - memexport_extent_start));
  } else {
    shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  }

  // EXPERIMENT 2026-07-24: multiplier on the memexport compute dispatch size.
  // 1 = stock behaviour (one invocation per guest vertex).
  // RESULT: tested at 2 - menu fill moved only 10,750 -> 11,760 (+9%), nowhere
  // near the ~2x that would be expected if records were going unwritten for
  // lack of invocations. So the dispatch count is NOT the bottleneck: the
  // producer already runs enough threads, and the slots the consumer reads but
  // never receives data are not being targeted by ANY invocation. Reverted to
  // 1; kept as a documented knob so this isn't re-tested from scratch.
  constexpr uint32_t kMemExportDispatchMultiplier = 1;

  // TESTRIG(gpu): MEMEXPORT PATH-SPLIT PROBE (2026-07-24).
  // The Halo 3 skinning buffer fills to a hard wall at 58.5% (VTXDIST hist
  // bins 6-9 exactly zero) and the shortfall equals whole MISSING producer
  // draws, not missing threads within a draw. The prime suspect is the
  // !IsHostVertexShaderTypeDomain() condition immediately below: any memexport
  // draw that arrives via the tessellation/domain-shader path is silently
  // skipped and writes NOTHING. Halo 3 uses tessellation heavily, so if a large
  // share of memexport draws (weighted by vertex count = records they would
  // have written) are domain-type, that quantitatively explains the missing
  // ~42% and the fix is to handle memexport on the tessellated path instead of
  // skipping it. Counts are cumulative; read the LAST line logged.
  if (testrig_gpu_hot && !memexport_ranges_.empty() &&
      vertex_shader->memexport_eM_written() != 0) {
    static std::atomic<uint64_t> plain_draws{0}, plain_verts{0};
    static std::atomic<uint64_t> domain_draws{0}, domain_verts{0};
    const bool is_domain = Shader::IsHostVertexShaderTypeDomain(
        primitive_processing_result.host_vertex_shader_type);
    const uint32_t verts = primitive_processing_result.host_draw_vertex_count;
    uint64_t pd, pv, dd, dv;
    if (is_domain) {
      dd = domain_draws.fetch_add(1) + 1;
      dv = domain_verts.fetch_add(verts) + verts;
      pd = plain_draws.load();
      pv = plain_verts.load();
    } else {
      pd = plain_draws.fetch_add(1) + 1;
      pv = plain_verts.fetch_add(verts) + verts;
      dd = domain_draws.load();
      dv = domain_verts.load();
    }
    // Log every 256th memexport draw so the running totals are visible without
    // flooding the log (this path is very hot - ~1100+ draws per capture).
    if (((pd + dd) & 0xFF) == 0) {
      const uint64_t total_verts = pv + dv;
      XELOGI(
          "MEMEXPORT_PATHSPLIT plain_draws={} plain_verts={} "
          "domain_draws={} domain_verts={} domain_vert_pct={} "
          "(domain draws are SKIPPED -> write nothing)",
          pd, pv, dd, dv,
          total_verts ? (dv * 100 / total_verts) : 0);
    }
  }

  // Emulate vertex-shader memory export with a compute dispatch on GPUs where
  // vertex-stage stores are unreliable (see memexport_use_compute_). The vertex
  // shader is translated as a compute shader (kMemExportCompute) that writes the
  // exported data to shared memory before the consuming draw fetches it - this
  // is what makes e.g. Halo 3's memexport-generated menu geometry render. Only
  // the plain (non-tessellated) vertex path is handled; the graphics draw still
  // runs afterwards (its own vertex-stage stores simply do nothing on Adreno).
  if (memexport_use_compute_ && !memexport_ranges_.empty() &&
      vertex_shader->memexport_eM_written() != 0 &&
      !Shader::IsHostVertexShaderTypeDomain(
          primitive_processing_result.host_vertex_shader_type)) {
    SpirvShaderTranslator::Modification memexport_compute_modification =
        pipeline_cache_->GetCurrentVertexShaderModification(
            *vertex_shader, Shader::HostVertexShaderType::kMemExportCompute,
            interpolator_mask, ps_param_gen_pos != UINT32_MAX);
    auto memexport_compute_translation =
        static_cast<VulkanShader::VulkanTranslation*>(
            vertex_shader->GetOrCreateTranslation(
                memexport_compute_modification.value));
    VkPipeline memexport_compute_pipeline = VK_NULL_HANDLE;
    if (pipeline_cache_->EnsureShadersTranslated(memexport_compute_translation,
                                                 nullptr)) {
      memexport_compute_pipeline =
          pipeline_cache_->GetOrCreateMemExportComputePipeline(
              memexport_compute_translation, pipeline_layout_provider);
    }
    // DEBUG(halo3-vtx): confirm the compute-memexport dispatch actually runs,
    // and that the shared-memory (set 0) and constants (set 1) descriptor sets
    // it will bind are non-null (a null constants set => the shader reads zero
    // constants => invalid eA => no export).
    if (testrig_gpu_hot) {  // TESTRIG(gpu)
      XELOGI(
          "MEMEXPORT_COMPUTE hosttype={} vtxcount={} pipeline={} eM=0x{:X} "
          "ds0={} ds1={}",
          uint32_t(primitive_processing_result.host_vertex_shader_type),
          primitive_processing_result.host_draw_vertex_count,
          memexport_compute_pipeline != VK_NULL_HANDLE ? 1 : 0,
          uint32_t(vertex_shader->memexport_eM_written()),
          current_graphics_descriptor_sets_
                      [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] !=
                  VK_NULL_HANDLE
              ? 1
              : 0,
          current_graphics_descriptor_sets_
                      [SpirvShaderTranslator::kDescriptorSetConstants] !=
                  VK_NULL_HANDLE
              ? 1
              : 0);
    }
    if (memexport_compute_pipeline == VK_NULL_HANDLE) {
      if (testrig_gpu_hot) {  // TESTRIG(gpu)
        ++testrig_total_memexport_compute_pipeline_failures_;
      }
    } else {
      if (testrig_gpu_hot) {  // TESTRIG(gpu)
        ++testrig_total_memexport_compute_dispatches_;
      }
      // The dispatch cannot run inside a render pass - end it and flush the
      // pre-dispatch shared memory barrier queued by the Use() call above.
      SubmitBarriers(true);
      BindExternalComputePipeline(memexport_compute_pipeline);
      // The compute shader reuses the guest graphics descriptor sets (the
      // pipeline layout is the same): shared memory (written by the export),
      // the constants, and the vertex textures if the shader samples any.
      uint32_t memexport_descriptor_set_count =
          SpirvShaderTranslator::kDescriptorSetConstants + 1;
      if (!vertex_shader->GetTextureBindingsAfterTranslation().empty() ||
          !vertex_shader->GetSamplerBindingsAfterTranslation().empty()) {
        memexport_descriptor_set_count =
            SpirvShaderTranslator::kDescriptorSetTexturesVertex + 1;
      }
      deferred_command_buffer_.CmdVkBindDescriptorSets(
          VK_PIPELINE_BIND_POINT_COMPUTE,
          pipeline_layout_provider->GetPipelineLayout(), 0,
          memexport_descriptor_set_count, current_graphics_descriptor_sets_, 0,
          nullptr);
      // One invocation per guest vertex (the compute local size is 1, so no
      // bounds check is needed - the invocation ID is the vertex index).
      // EXPERIMENT 2026-07-24 (kMemExportDispatchMultiplier): the memexport
      // buffer only fills ~47% on Adreno even via compute, and a half-filled
      // buffer renders skinned characters as a "ball" (written slots land
      // correctly, unwritten slots stay zero so those vertices collapse to the
      // model origin). Testing whether the producer simply needs MORE
      // invocations than host_draw_vertex_count to write every record the
      // consumer reads. If the fill % scales with this multiplier, the dispatch
      // count is the bottleneck and the real fix is to derive the count from
      // the memexport stream's index_count rather than the draw's vertex count.
      deferred_command_buffer_.CmdVkDispatch(
          primitive_processing_result.host_draw_vertex_count *
              kMemExportDispatchMultiplier,
          1, 1);
      // Make the exported data visible to the consuming draw's vertex fetch /
      // index read (compute SHADER_WRITE -> vertex INDEX/SHADER_READ). Re-Use
      // the same range so the shared memory barrier system commits the write.
      if (memexport_extent_start < memexport_extent_end) {
        shared_memory_->Use(
            VulkanSharedMemory::Usage::kGuestDrawReadWrite,
            std::make_pair(memexport_extent_start,
                           memexport_extent_end - memexport_extent_start));
      }
    }
  }

  // After all commands that may dispatch, copy or insert barriers, submit the
  // barriers (may end the render pass), and (re)enter the render pass before
  // drawing.
  SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      render_target_cache_->last_update_render_pass(),
      render_target_cache_->last_update_framebuffer());

  // DEBUG(halo3-vtx): log memexport draw vertex count (covers both paths).
  // Was unconditional (only the counter below was gated) - the Halo 3 vista
  // investigation these three logs were added for is resolved (see
  // docs/HALO3_FINDINGS_CHECKLIST.md), so they're no longer needed on every
  // draw; gating them behind testrig_gpu_hot like their neighbors below
  // rather than deleting them, per this harness's own convention of keeping
  // test code permanently but toggle-gated.
  if (testrig_gpu_hot && !memexport_ranges_.empty()) {  // TESTRIG(gpu)
    XELOGI("MEMEXPORT_DRAW vtxcount={} prim={} idxtype={} eM=0x{:X}",
           primitive_processing_result.host_draw_vertex_count,
           uint32_t(primitive_processing_result.host_primitive_type),
           uint32_t(primitive_processing_result.index_buffer_type),
           uint32_t(vertex_shader->memexport_eM_written()));
    ++testrig_total_memexport_draws_;
  }
  // TESTRIG(halo3-nondeterminism): log any LARGE draw (likely the actual
  // terrain-rasterizing consumer, not a small memexport producer) so it can
  // be identified without relying on buffer-size heuristics that turned out
  // to also match the producer's own self-referential source read.
  if (testrig_gpu_hot &&  // TESTRIG(gpu)
      primitive_processing_result.host_draw_vertex_count >= 1000) {
    XELOGI(
        "BIGDRAW sh={:016X} vtxcount={} prim={} idxtype={} eM=0x{:X} "
        "rasterdiscard={}",
        vertex_shader->ucode_data_hash(),
        primitive_processing_result.host_draw_vertex_count,
        uint32_t(primitive_processing_result.host_primitive_type),
        uint32_t(primitive_processing_result.index_buffer_type),
        uint32_t(vertex_shader->memexport_eM_written()),
        vertex_shader->memexport_eM_written() != 0 ? 1 : 0);
  }
  // TESTRIG(halo3-menu-vs-3d): log EVERY draw (vertex + pixel shader hash,
  // vertex count) to find the 2D UI's rendering path - is it a separate,
  // simple, non-memexport path (as expected), and does it touch the same
  // guest memory region as the 3D vista at all?
  if (testrig_gpu_hot) {  // TESTRIG(gpu)
    XELOGI("ANYDRAW vsh={:016X} psh={:016X} vtxcount={} eM=0x{:X}",
           vertex_shader->ucode_data_hash(),
           pixel_shader ? pixel_shader->ucode_data_hash() : 0,
           primitive_processing_result.host_draw_vertex_count,
           uint32_t(vertex_shader->memexport_eM_written()));
  }
  if (gpu_trace_enabled()) {
    auto ct_color_info = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[0]);
    GpuTrace("DRAW",
             fmt::format("vsh={:016X} psh={:016X} vtx={} eM=0x{:X} cbase0={}",
                         vertex_shader->ucode_data_hash(),
                         pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                         primitive_processing_result.host_draw_vertex_count,
                         uint32_t(vertex_shader->memexport_eM_written()),
                         uint32_t(ct_color_info.color_base)));
  }
  // TESTRIG(halo3-rtmap): map which EDRAM render target each distinct draw
  // targets. The force-vis probe proved the terrain consumer's pixels never
  // reach the visible framebuffer, so the bug is render-target composition:
  // the vista is drawn to an offscreen color RT, resolved to a texture, and a
  // fullscreen pass paints it. Logging color/depth EDRAM base per shader (once
  // each) shows who-draws-where - if the terrain (488D9488) and the final
  // presented content sit at different color_base tiles, that confirms the
  // offscreen-RT structure and pins the resolve/composite as the break.
  {
    static std::unordered_set<uint64_t> logged_rtmap;
    uint64_t vh = vertex_shader->ucode_data_hash();
    if (logged_rtmap.insert(vh).second) {
      auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
      auto modecontrol = regs.Get<reg::RB_MODECONTROL>();
      auto color_mask = regs.Get<reg::RB_COLOR_MASK>();
      auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
      uint32_t cb[4];
      for (uint32_t i = 0; i < 4; ++i) {
        cb[i] = regs.Get<reg::RB_COLOR_INFO>(
                        reg::RB_COLOR_INFO::rt_register_indices[i])
                    .color_base;
      }
      XELOGI(
          "RTMAP vsh={:016X} psh={:016X} edram_mode={} colormask=0x{:X} "
          "pitch={} msaa={} cbase=[{},{},{},{}] dbase={}",
          vh, pixel_shader ? pixel_shader->ucode_data_hash() : 0,
          uint32_t(modecontrol.edram_mode), color_mask.value,
          surface_info.surface_pitch, uint32_t(surface_info.msaa_samples),
          cb[0], cb[1], cb[2], cb[3], depth_info.depth_base);
      // Also log the guest byte addresses of every texture this draw samples,
      // so the fullscreen background pass can be correlated against RESOLVE
      // dest addresses: if it samples an address that a resolve wrote, the
      // chain is intact (bug = resolve content); if it samples something no
      // resolve targets, the binding/resolve is the break.
      xe::StringBuffer tb;
      tb.AppendFormat("TEXSRC vsh={:016X} addrs=", vh);
      uint32_t tex_remaining = used_texture_mask;
      uint32_t tex_index;
      bool any_tex = false;
      while (xe::bit_scan_forward(tex_remaining, &tex_index)) {
        tex_remaining &= ~(uint32_t(1) << tex_index);
        xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(tex_index);
        tb.AppendFormat("[fc{}=0x{:08X}]", tex_index, fetch.base_address << 12);
        any_tex = true;
      }
      if (!any_tex) tb.Append("(none)");
      XELOGI("{}", tb.buffer());
    }
  }
  // TESTRIG(halo3-nondeterminism): for the terrain-consuming shader
  // specifically, log the real PA_CL_VTE_CNTL hardware register - this is
  // what actually decides whether kSysFlag_WNotReciprocal is set for THIS
  // draw (i.e. whether the degenerate-W-clip fix's code path even applies
  // here), instead of assuming it from the ucode alone.
  if (testrig_gpu_hot &&  // TESTRIG(gpu) - fires every draw of this shader,
                          // not one-shot like the CONSTDUMP blocks below.
      vertex_shader->ucode_data_hash() == 0x488D9488AB7ED7D8ull) {
    auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
    XELOGI(
        "CONSUMER_VTE vtx_xy_fmt={} vtx_z_fmt={} vtx_w0_fmt={} raw=0x{:08X}",
        pa_cl_vte_cntl.vtx_xy_fmt, pa_cl_vte_cntl.vtx_z_fmt,
        pa_cl_vte_cntl.vtx_w0_fmt, pa_cl_vte_cntl.value);
    // TESTRIG(halo3-consumer-p0): the entire terrain body of this shader is
    // gated on predicate p0, set at ucode instr 49 by
    //   setp_ne_push r9.w, c228.xxxx, r0.zzzz
    // whose interpreter semantics are p0 = (c228.x == 0) && (r0.z != 0), and
    // r0.z is built (instr 46-47) from exact float comparisons on the FETCHED
    // vf1 vertex: r0.z = (r7.x >= c229.w ? 1:0) + (r11.w == 0 ? 1:0). If
    // c228.x is not exactly 0, p0 can NEVER be true and no terrain ever draws
    // regardless of buffer fill - which would make the whole fill-% hunt a red
    // herring. Dump the constants that decide p0 (c78,c228,c229) plus the
    // position-transform constants the p0 body uses (c7,c32-c36,c66-c69,c71,
    // c77,c223-c227), once per session, to check directly. Prior CONSTDUMP
    // covered only the PRODUCER (9EA48FC2); this consumer's constants have
    // never been inspected.
    static bool logged_consumer_consts = false;
    if (!logged_consumer_consts) {
      logged_consumer_consts = true;
      const uint32_t* regvals = register_file_->values;
      const uint32_t kConsts[] = {7,  32, 33,  34,  35,  36,  66,  67,
                                  68, 69, 71,  77,  78,  223, 224, 225,
                                  226, 227, 228, 229};
      xe::StringBuffer ccb;
      ccb.Append("CONSUMER_CONST:");
      for (uint32_t c : kConsts) {
        const float* cf = reinterpret_cast<const float*>(
            &regvals[XE_GPU_REG_SHADER_CONSTANT_000_X + (c << 2)]);
        ccb.AppendFormat(" c{}=({:.9g},{:.9g},{:.9g},{:.9g})", c, cf[0], cf[1],
                         cf[2], cf[3]);
      }
      XELOGI("{}", ccb.buffer());
      // The shader moves several constants into registers via the max(c,c)
      // identity idiom (instr 56: r0=c32; 183: r1=-c5; 184: r0=c6) that feed
      // the position/normal transform. c32 dumped as NaN above - if c5/c6 are
      // also NaN/garbage, and especially if the raw bit pattern VARIES across
      // cold boots, that's a live candidate for both the collapse AND the
      // non-determinism (uninitialized constant memory -> vendor-divergent NaN
      // propagation through max/cndeq). Dump raw hex to see exact bit patterns.
      const uint32_t kRawConsts[] = {5, 6, 32, 58, 60, 74, 230};
      xe::StringBuffer rcb;
      rcb.Append("CONSUMER_CONST_RAW:");
      for (uint32_t c : kRawConsts) {
        const uint32_t* cu = &regvals[XE_GPU_REG_SHADER_CONSTANT_000_X + (c << 2)];
        rcb.AppendFormat(" c{}=(0x{:08X},0x{:08X},0x{:08X},0x{:08X})", c, cu[0],
                         cu[1], cu[2], cu[3]);
      }
      XELOGI("{}", rcb.buffer());
    }
  }
  // TESTRIG(halo3-consumer-matrix): user hypothesis (2026-07-30) - the vista is
  // ONE solid mesh, so a bad transform makes it look cleanly flipped, while a
  // character is MANY parts (arms/legs/head), so if each part gets a bad
  // transform the parts pile onto each other and read as a "ball". This fits the
  // user's own earlier observation that an intact HEAD is visible inside the
  // ball: per-part mis-transform relocates parts while leaving each internally
  // rigid, whereas true vertex collapse toward a point would destroy the head.
  //
  // The consumer applies c33/c34/c35 as a rotation basis and c36 as the
  // translation column (ucode instr 66-75 of 488D9488AB7ED7D8, a mad chain:
  // out = r.x*c33 + r.y*c34 + r.w*c35 + c36). The pre-existing ONE-SHOT
  // CONSUMER_CONST dump above sampled c33..c36 as EXACT IDENTITY with a ZERO
  // translation column - exactly the predicted mechanism. But per-part matrices
  // vary PER DRAW by definition, so a single sample proves nothing.
  //
  // So count DISTINCT matrices across draws instead of dumping each one:
  //   always identity      => parts really do all get the same no-op transform
  //                           (hypothesis supported - a real mechanism at last)
  //   many distinct values => parts DO get real per-part transforms, so the
  //                           collapse is in the data they are applied to, not
  //                           in the transform (hypothesis killed)
  //
  // Rate-limited deliberately: this shader ran 27223 times in one 20s capture,
  // and both the Android harness and the desktop oracle have self-DoSed on
  // unthrottled probes before (1.8M lines / 318MB once).
  if (testrig_gpu_hot &&  // TESTRIG(gpu)
      vertex_shader->ucode_data_hash() == 0x488D9488AB7ED7D8ull) {
    const uint32_t* mtx_regvals = register_file_->values;
    // FNV-1a over the RAW BITS of c33..c36 (16 dwords) so NaN payloads and -0.0
    // are distinguished too, rather than comparing float values.
    uint64_t fp = 14695981039346656037ull;
    for (uint32_t c = 33; c <= 36; ++c) {
      const uint32_t* cu =
          &mtx_regvals[XE_GPU_REG_SHADER_CONSTANT_000_X + (c << 2)];
      for (int i = 0; i < 4; ++i) {
        fp ^= cu[i];
        fp *= 1099511628211ull;
      }
    }
    static uint64_t mtx_seen[32] = {};
    static uint32_t mtx_seen_count = 0;
    static uint64_t mtx_draws = 0;
    static uint64_t mtx_identity_draws = 0;
    static uint32_t mtx_dumped = 0;
    ++mtx_draws;

    // Identity basis with a zero translation column?
    bool is_identity = true;
    for (uint32_t c = 33; c <= 36 && is_identity; ++c) {
      const float* cf = reinterpret_cast<const float*>(
          &mtx_regvals[XE_GPU_REG_SHADER_CONSTANT_000_X + (c << 2)]);
      for (int i = 0; i < 4; ++i) {
        const float expect = (int(c) - 33 == i) ? 1.0f : 0.0f;
        if (cf[i] != expect) {
          is_identity = false;
          break;
        }
      }
    }
    if (is_identity) {
      ++mtx_identity_draws;
    }

    bool is_new = true;
    for (uint32_t i = 0; i < mtx_seen_count; ++i) {
      if (mtx_seen[i] == fp) {
        is_new = false;
        break;
      }
    }
    if (is_new && mtx_seen_count < 32) {
      mtx_seen[mtx_seen_count++] = fp;
      // Dump the first few DISTINCT matrices in full - that is the interesting
      // signal and it is bounded, unlike dumping per draw.
      if (mtx_dumped < 8) {
        ++mtx_dumped;
        xe::StringBuffer mb;
        mb.AppendFormat("CONSUMER_MTX distinct#{} draw={} identity={}",
                        mtx_seen_count, mtx_draws, is_identity ? 1 : 0);
        for (uint32_t c = 33; c <= 36; ++c) {
          const float* cf = reinterpret_cast<const float*>(
              &mtx_regvals[XE_GPU_REG_SHADER_CONSTANT_000_X + (c << 2)]);
          mb.AppendFormat(" c{}=({:.9g},{:.9g},{:.9g},{:.9g})", c, cf[0], cf[1],
                          cf[2], cf[3]);
        }
        XELOGI("{}", mb.buffer());
      }
    }
    // Periodic summary - this line is the actual answer to the question.
    if ((mtx_draws % 2048) == 0) {
      XELOGI(
          "CONSUMER_MTX_SUMMARY draws={} distinct_matrices={}{} "
          "identity_draws={} ({}%)",
          mtx_draws, mtx_seen_count, mtx_seen_count >= 32 ? "+capped" : "",
          mtx_identity_draws,
          mtx_draws ? (mtx_identity_draws * 100 / mtx_draws) : 0);
    }
  }
  // TESTRIG(halo3-blend): user hypothesis - the 3D scene renders correctly
  // but a blend mode makes it read as flat blue against the 2D UI
  // foreground. Log the actual blend state + color mask for the consuming
  // shader's draws to check directly instead of guessing from ucode.
  if (testrig_gpu_hot &&  // TESTRIG(gpu) - fires every draw of these shaders.
      (vertex_shader->ucode_data_hash() == 0x488D9488AB7ED7D8ull ||
       vertex_shader->ucode_data_hash() == 0x3D774C769771A211ull)) {
    auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
    auto rb_color_mask = regs.Get<reg::RB_COLOR_MASK>();
    auto rb_blendcontrol0 = regs.Get<reg::RB_BLENDCONTROL>(
        XE_GPU_REG_RB_BLENDCONTROL0);
    XELOGI(
        "SCENE_BLEND sh={:016X} blend_enable_colorcontrol=0x{:08X} "
        "colormask=0x{:X} src_color={} dst_color={} comb_color={} "
        "src_alpha={} dst_alpha={} comb_alpha={}",
        vertex_shader->ucode_data_hash(), rb_colorcontrol.value,
        rb_color_mask.value, uint32_t(rb_blendcontrol0.color_srcblend),
        uint32_t(rb_blendcontrol0.color_destblend),
        uint32_t(rb_blendcontrol0.color_comb_fcn),
        uint32_t(rb_blendcontrol0.alpha_srcblend),
        uint32_t(rb_blendcontrol0.alpha_destblend),
        uint32_t(rb_blendcontrol0.alpha_comb_fcn));
  }
  if (testrig_gpu_hot) {  // TESTRIG(gpu)
    ++testrig_total_draws_;
  }

  // Draw.
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kNone ||
      shader_32bit_index_dma) {
    deferred_command_buffer_.CmdVkDraw(
        primitive_processing_result.host_draw_vertex_count, 1, 0, 0);
  } else {
    std::pair<VkBuffer, VkDeviceSize> index_buffer;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        index_buffer.first = shared_memory_->buffer();
        index_buffer.second = primitive_processing_result.guest_index_base;
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer = primitive_processor_->GetConvertedIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer = primitive_processor_->GetBuiltinIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        return false;
    }
    deferred_command_buffer_.CmdVkBindIndexBuffer(
        index_buffer.first, index_buffer.second,
        primitive_processing_result.host_index_format ==
                xenos::IndexFormat::kInt16
            ? VK_INDEX_TYPE_UINT16
            : VK_INDEX_TYPE_UINT32);
    deferred_command_buffer_.CmdVkDrawIndexed(
        primitive_processing_result.host_draw_vertex_count, 1, 0, 0, 0);
  }

  // TESTRIG(halo3-loadvsuse): at the composite draw, capture the EXACT image
  // bound to fc0 (the vista G-buffer albedo) in its at-draw layout. Read at
  // swap. If uniform here but the resolve companion is varied, the composite
  // samples unpublished/unordered data (load-vs-use / barrier ordering).
  if (pixel_shader &&
      pixel_shader->ucode_data_hash() == 0x373E65D9ADCF4380ull) {
    // TESTRIG(halo3-composite-trace): re-trace from the composite side. Log
    // EVERY texture the composite actually samples (guest address/format/dims),
    // verified from the fetch constants rather than assumed, so we know exactly
    // which guest regions feed it and can trace which resolve wrote each.
    texture_cache_->TestrigLogCompositeBindings();
    // With the tile-1216 clobber skipped, capture shared memory 0x044B0000 (the
    // resolve-copy output that the composite's albedo texture loads from) to see
    // whether tile 608's varied albedo actually reaches it, or the resolve-copy
    // collapses it too.
    static int shm_cap_n = 0;
    if (shm_cap_n++ < 4) {
      // Same-frame pair: EDRAM tile 608 (the resolve-copy's INPUT, known varied)
      // vs shared memory 0x044B0000 (its OUTPUT). If EDRAM varied + SHM uniform
      // in the same steady-state frame, the 608 EDRAM->SHM resolve-copy is the
      // collapse. 608 * 5120 bytes/tile.
      TestrigCaptureSharedMemoryDeferred(0x044B0000ull, 256u * 1024u);
      TestrigCaptureEdramDeferred(608ull * 5120ull, 256u * 1024u);
    }
  }

  // Invalidate textures in memexported memory and watch for changes.
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,
                                      memexport_range.size_bytes, false);
  }

  // CPU readback for memexport data (if enabled).
  if (GetGPUSetting(GPUSetting::ReadbackMemexport) &&
      !memexport_ranges_.empty()) {
    // Calculate total size of all memexport ranges.
    uint32_t memexport_total_size = 0;
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      memexport_total_size += memexport_range.size_bytes;
    }

    if (memexport_total_size > 0) {
      VkBuffer readback_buffer = RequestReadbackBuffer(memexport_total_size);
      if (readback_buffer != VK_NULL_HANDLE) {
        const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
        const ui::vulkan::VulkanDevice::Functions& dfn =
            vulkan_device->functions();
        const VkDevice device = vulkan_device->device();

        VkBuffer shared_memory_buffer = shared_memory_->buffer();

        // Ensure shared memory is ready for transfer.
        shared_memory_->Use(VulkanSharedMemory::Usage::kRead);

        // Copy each memexport range to the readback buffer.
        uint32_t readback_buffer_offset = 0;
        for (const draw_util::MemExportRange& memexport_range :
             memexport_ranges_) {
          VkBufferCopy copy_region = {};
          copy_region.srcOffset = memexport_range.base_address_dwords << 2;
          copy_region.dstOffset = readback_buffer_offset;
          copy_region.size = memexport_range.size_bytes;

          deferred_command_buffer_.CmdVkCopyBuffer(
              shared_memory_buffer, readback_buffer, 1, &copy_region);

          readback_buffer_offset += memexport_range.size_bytes;
        }

        // Wait for GPU to finish (SYNCHRONIZATION STALL)
        if (AwaitAllQueueOperationsCompletion()) {
          // Map staging buffer and copy to guest memory.
          void* mapped_data;
          if (dfn.vkMapMemory(device, memexport_readback_buffer_memory_, 0,
                              memexport_total_size, 0,
                              &mapped_data) == VK_SUCCESS) {
            if (mapped_data) {
              const uint8_t* readback_bytes =
                  static_cast<const uint8_t*>(mapped_data);
              for (const draw_util::MemExportRange& memexport_range :
                   memexport_ranges_) {
                std::memcpy(memory_->TranslatePhysical(
                                memexport_range.base_address_dwords << 2),
                            readback_bytes, memexport_range.size_bytes);
                readback_bytes += memexport_range.size_bytes;
              }
            } else {
              XELOGE(
                  "VulkanCommandProcessor: Failed to map readback buffer "
                  "(mapped_data is null)");
            }
            dfn.vkUnmapMemory(device, memexport_readback_buffer_memory_);
          } else {
            XELOGE(
                "VulkanCommandProcessor: Failed to map readback buffer memory "
                "for memexport");
          }
        } else {
          XELOGE(
              "VulkanCommandProcessor: Failed to complete queue operations for "
              "memexport readback");
        }
      }
    }
  }

  return true;
}

namespace {
// TESTRIG(halo3): persistent host-visible buffer for decoupled image capture
// (single GPU, diagnostic use only).
VkBuffer g_halo3_cap_buf = VK_NULL_HANDLE;
VkDeviceMemory g_halo3_cap_mem = VK_NULL_HANDLE;
uint32_t g_halo3_cap_w = 0, g_halo3_cap_h = 0;
bool g_halo3_cap_pending = false;
uint64_t g_halo3_cap_submission = 0;
VkImage g_halo3_resolve_img = VK_NULL_HANDLE;
VkDeviceMemory g_halo3_resolve_mem = VK_NULL_HANDLE;
VkFormat g_halo3_resolve_fmt = VK_FORMAT_UNDEFINED;
// TESTRIG(halo3): shared-memory (VkBuffer) region capture, to read what the
// resolve-copy actually wrote to guest shared memory (the load's input).
VkBuffer g_halo3_shm_buf = VK_NULL_HANDLE;
VkDeviceMemory g_halo3_shm_mem = VK_NULL_HANDLE;
uint32_t g_halo3_shm_bytes = 0;
bool g_halo3_shm_pending = false;
uint64_t g_halo3_shm_submission = 0;
// TESTRIG(halo3): EDRAM (VkBuffer) region capture - the DUMP's output / the
// resolve-copy's input. Splits dump vs resolve-copy as the collapse point.
VkBuffer g_halo3_edram_buf = VK_NULL_HANDLE;
VkDeviceMemory g_halo3_edram_mem = VK_NULL_HANDLE;
uint32_t g_halo3_edram_bytes = 0;
bool g_halo3_edram_pending = false;
uint64_t g_halo3_edram_submission = 0;
}  // namespace

void VulkanCommandProcessor::TestrigCaptureEdramDeferred(uint64_t offset,
                                                         uint32_t size) {
  const ui::vulkan::VulkanDevice* const vd = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vd->functions();
  const VkDevice device = vd->device();
  uint32_t bytes = std::min<uint32_t>(size, 256u * 1024u);
  if (g_halo3_edram_buf == VK_NULL_HANDLE) {
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = 256u * 1024u;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dfn.vkCreateBuffer(device, &bi, nullptr, &g_halo3_edram_buf) !=
        VK_SUCCESS) {
      g_halo3_edram_buf = VK_NULL_HANDLE;
      return;
    }
    VkMemoryRequirements mr;
    dfn.vkGetBufferMemoryRequirements(device, g_halo3_edram_buf, &mr);
    uint32_t mti = ui::vulkan::util::ChooseMemoryType(
        vd->memory_types(), mr.memoryTypeBits,
        ui::vulkan::util::MemoryPurpose::kReadback);
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mti;
    if (mti == UINT32_MAX ||
        dfn.vkAllocateMemory(device, &mai, nullptr, &g_halo3_edram_mem) !=
            VK_SUCCESS) {
      dfn.vkDestroyBuffer(device, g_halo3_edram_buf, nullptr);
      g_halo3_edram_buf = VK_NULL_HANDLE;
      return;
    }
    dfn.vkBindBufferMemory(device, g_halo3_edram_buf, g_halo3_edram_mem, 0);
  }
  VkBuffer edram = render_target_cache_->edram_buffer();
  // Make prior compute writes to EDRAM visible to a transfer read.
  PushBufferMemoryBarrier(edram, 0, VK_WHOLE_SIZE,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  SubmitBarriers(true);
  VkBufferCopy region = {};
  region.srcOffset = offset;
  region.dstOffset = 0;
  region.size = bytes;
  deferred_command_buffer_.CmdVkCopyBuffer(edram, g_halo3_edram_buf, 1, &region);
  // Restore EDRAM to its compute-write visibility for subsequent use.
  PushBufferMemoryBarrier(edram, 0, VK_WHOLE_SIZE,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
  g_halo3_edram_bytes = bytes;
  g_halo3_edram_pending = true;
  g_halo3_edram_submission = GetCurrentSubmission();
}

void VulkanCommandProcessor::TestrigReadCapturedEdram(const char* tag) {
  if (!g_halo3_edram_pending || g_halo3_edram_buf == VK_NULL_HANDLE) return;
  g_halo3_edram_pending = false;
  const ui::vulkan::VulkanDevice* const vd = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vd->functions();
  const VkDevice device = vd->device();
  CheckSubmissionFenceAndDeviceLoss(g_halo3_edram_submission);
  if (GetCompletedSubmission() < g_halo3_edram_submission) {
    return;
  }
  void* mapped = nullptr;
  if (dfn.vkMapMemory(device, g_halo3_edram_mem, 0, g_halo3_edram_bytes, 0,
                      &mapped) == VK_SUCCESS &&
      mapped) {
    const uint32_t* p = static_cast<const uint32_t*>(mapped);
    uint32_t n = g_halo3_edram_bytes / 4, nonzero = 0, changes = 0,
             prev = 0xDEADBEEFu;
    for (uint32_t k = 0; k < n; ++k) {
      if (p[k] != 0) ++nonzero;
      if (p[k] != prev) {
        ++changes;
        prev = p[k];
      }
    }
    XELOGI(
        "TESTRIG_EDRAM {} bytes={} nonzero={}/{} distinct_runs={} s0=0x{:08X} "
        "smid=0x{:08X} slast=0x{:08X}",
        tag, g_halo3_edram_bytes, nonzero, n, changes, p[0], p[n / 2],
        p[n - 1]);
    dfn.vkUnmapMemory(device, g_halo3_edram_mem);
  }
}

void VulkanCommandProcessor::TestrigCaptureSharedMemoryDeferred(uint64_t offset,
                                                                uint32_t size) {
  const ui::vulkan::VulkanDevice* const vd = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vd->functions();
  const VkDevice device = vd->device();
  uint32_t bytes = std::min<uint32_t>(size, 256u * 1024u);
  if (g_halo3_shm_buf == VK_NULL_HANDLE) {
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = 256u * 1024u;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dfn.vkCreateBuffer(device, &bi, nullptr, &g_halo3_shm_buf) !=
        VK_SUCCESS) {
      g_halo3_shm_buf = VK_NULL_HANDLE;
      return;
    }
    VkMemoryRequirements mr;
    dfn.vkGetBufferMemoryRequirements(device, g_halo3_shm_buf, &mr);
    uint32_t mti = ui::vulkan::util::ChooseMemoryType(
        vd->memory_types(), mr.memoryTypeBits,
        ui::vulkan::util::MemoryPurpose::kReadback);
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mti;
    if (mti == UINT32_MAX ||
        dfn.vkAllocateMemory(device, &mai, nullptr, &g_halo3_shm_mem) !=
            VK_SUCCESS) {
      dfn.vkDestroyBuffer(device, g_halo3_shm_buf, nullptr);
      g_halo3_shm_buf = VK_NULL_HANDLE;
      return;
    }
    dfn.vkBindBufferMemory(device, g_halo3_shm_buf, g_halo3_shm_mem, 0);
  }
  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  VkBufferCopy region = {};
  region.srcOffset = offset;
  region.dstOffset = 0;
  region.size = bytes;
  deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(),
                                           g_halo3_shm_buf, 1, &region);
  g_halo3_shm_bytes = bytes;
  g_halo3_shm_pending = true;
  g_halo3_shm_submission = GetCurrentSubmission();
}

void VulkanCommandProcessor::TestrigReadCapturedSharedMemory(const char* tag) {
  if (!g_halo3_shm_pending || g_halo3_shm_buf == VK_NULL_HANDLE) return;
  g_halo3_shm_pending = false;
  const ui::vulkan::VulkanDevice* const vd = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vd->functions();
  const VkDevice device = vd->device();
  CheckSubmissionFenceAndDeviceLoss(g_halo3_shm_submission);
  if (GetCompletedSubmission() < g_halo3_shm_submission) {
    return;
  }
  void* mapped = nullptr;
  if (dfn.vkMapMemory(device, g_halo3_shm_mem, 0, g_halo3_shm_bytes, 0,
                      &mapped) == VK_SUCCESS &&
      mapped) {
    const uint32_t* p = static_cast<const uint32_t*>(mapped);
    uint32_t n = g_halo3_shm_bytes / 4, nonzero = 0, changes = 0,
             prev = 0xDEADBEEFu;
    for (uint32_t k = 0; k < n; ++k) {
      if (p[k] != 0) ++nonzero;
      if (p[k] != prev) {
        ++changes;
        prev = p[k];
      }
    }
    XELOGI(
        "TESTRIG_SHM {} bytes={} nonzero={}/{} distinct_runs={} s0=0x{:08X} "
        "smid=0x{:08X} slast=0x{:08X}",
        tag, g_halo3_shm_bytes, nonzero, n, changes, p[0], p[n / 2], p[n - 1]);
    dfn.vkUnmapMemory(device, g_halo3_shm_mem);
  }
}

void VulkanCommandProcessor::TestrigCaptureImageDeferred(
    VkImage image, VkImageLayout current_layout, uint32_t width,
    uint32_t height) {
  uint32_t w = std::min<uint32_t>(width, 256), h = std::min<uint32_t>(height, 256);
  const ui::vulkan::VulkanDevice* const vd = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vd->functions();
  const VkDevice device = vd->device();
  if (g_halo3_cap_buf == VK_NULL_HANDLE) {
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = 256 * 256 * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dfn.vkCreateBuffer(device, &bi, nullptr, &g_halo3_cap_buf) !=
        VK_SUCCESS) {
      g_halo3_cap_buf = VK_NULL_HANDLE;
      XELOGI("TESTRIG_CAPIMG create-buffer FAILED");
      return;
    }
    VkMemoryRequirements mr;
    dfn.vkGetBufferMemoryRequirements(device, g_halo3_cap_buf, &mr);
    uint32_t mti = ui::vulkan::util::ChooseMemoryType(
        vd->memory_types(), mr.memoryTypeBits,
        ui::vulkan::util::MemoryPurpose::kReadback);
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mti;
    if (mti == UINT32_MAX ||
        dfn.vkAllocateMemory(device, &mai, nullptr, &g_halo3_cap_mem) !=
            VK_SUCCESS) {
      dfn.vkDestroyBuffer(device, g_halo3_cap_buf, nullptr);
      g_halo3_cap_buf = VK_NULL_HANDLE;
      XELOGI("TESTRIG_CAPIMG alloc-memory FAILED mti={}", mti);
      return;
    }
    dfn.vkBindBufferMemory(device, g_halo3_cap_buf, g_halo3_cap_mem, 0);
  }
  VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  PushImageMemoryBarrier(image, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         current_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  SubmitBarriers(true);
  // DIAG(gpu/rt-orientation): capture a full-height COLUMN, not a corner crop.
  //
  // The 256x256 crop above only ever sees the top-left corner of a render
  // target that is far larger, so it cannot show a whole-image vertical
  // gradient - the first attempt came back flat with trailing zeros where the
  // crop overran the content. A narrow full-height strip through the middle of
  // the image gives the vertical luminance profile that actually answers
  // whether the render target is mirrored, and 4 x height x 4 bytes fits the
  // existing 256*256*4 buffer for any realistic render target height.
  // Column mode is for luminance profiling; the 2D crop is for LOOKING at the
  // image. Default to the 2D crop now that the raw dump exists - the vista's
  // content sits in the top ~213 rows, so a 256x256 top-left crop contains all
  // of it vertically. debug.canary.rtcap_column re-enables the strip.
  const bool column_mode = XE_AE_DIAG_ENABLED("debug.canary.rtcap_column") &&
                           height > 256u && (4u * height) <= (256u * 256u);
  if (column_mode) {
    w = 4u;
    h = height;
  }
  VkBufferImageCopy region = {};
  region.bufferRowLength = w;
  region.bufferImageHeight = h;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  if (column_mode) {
    region.imageOffset.x = int32_t(width / 2u);
  }
  region.imageExtent.width = w;
  region.imageExtent.height = h;
  region.imageExtent.depth = 1;
  deferred_command_buffer_.CmdVkCopyImageToBuffer(
      image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_halo3_cap_buf, 1, &region);
  PushImageMemoryBarrier(image, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, current_layout);
  g_halo3_cap_w = w;
  g_halo3_cap_h = h;
  g_halo3_cap_pending = true;
  g_halo3_cap_submission = GetCurrentSubmission();
  XELOGI("TESTRIG_CAPIMG recorded pending sub={} {}x{}", g_halo3_cap_submission,
         w, h);
}

void VulkanCommandProcessor::TestrigReadCapturedImage(const char* tag) {
  if (!g_halo3_cap_pending || g_halo3_cap_buf == VK_NULL_HANDLE) return;
  g_halo3_cap_pending = false;
  const ui::vulkan::VulkanDevice* const vd = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vd->functions();
  const VkDevice device = vd->device();
  // Wait specifically for the submission that recorded the copy to complete
  // (AwaitAllQueueOperationsCompletion bails without waiting when in-flight
  // fences remain). Must be called after the copy's submission is closed
  // (i.e. after IssueSwap's EndSubmission). Diagnostic only.
  CheckSubmissionFenceAndDeviceLoss(g_halo3_cap_submission);
  if (GetCompletedSubmission() < g_halo3_cap_submission) {
    XELOGI("TESTRIG_CAPIMG {} not-yet-complete (sub {} > completed {})", tag,
           g_halo3_cap_submission, GetCompletedSubmission());
    return;
  }
  uint32_t bytes = g_halo3_cap_w * g_halo3_cap_h * 4;
  void* mapped = nullptr;
  if (dfn.vkMapMemory(device, g_halo3_cap_mem, 0, bytes, 0, &mapped) ==
          VK_SUCCESS &&
      mapped) {
    const uint32_t* p = static_cast<const uint32_t*>(mapped);
    uint32_t n = bytes / 4, nonzero = 0, changes = 0, prev = 0xDEADBEEFu;
    for (uint32_t k = 0; k < n; ++k) {
      if (p[k] != 0) ++nonzero;
      if (p[k] != prev) {
        ++changes;
        prev = p[k];
      }
    }
    XELOGI(
        "TESTRIG_CAPIMG {} {}x{} nonzero={}/{} distinct_runs={} s0=0x{:08X} "
        "smid=0x{:08X} slast=0x{:08X}",
        tag, g_halo3_cap_w, g_halo3_cap_h, nonzero, n, changes, p[0], p[n / 2],
        p[n - 1]);
    // DIAG(gpu/rt-orientation): vertical luminance profile of the captured
    // render target.
    //
    // docs/HALO3_VISTA_46_VS_64.md section 15 proved the mirroring is already
    // present upstream of the resolve, and section 16 then showed every code
    // path between the host RT and the resolve is identical to XenDroid's. The
    // remaining question is whether the HOST RENDER TARGET ITSELF holds a
    // mirrored image - and that is a measurement, not a diff.
    //
    // The vista is a sky-over-ground scene, so it has a strong monotonic
    // vertical luminance gradient. Reporting mean luminance per row band makes
    // the orientation readable directly:
    //   bright bands FIRST (row 0 side)  -> sky at the top, RT is correct, and
    //                                       the mirror is introduced later;
    //   bright bands LAST                -> the host RT is already mirrored,
    //                                       so the cause is in the guest draws
    //                                       that produced it.
    // Byte order does not matter here: every channel is summed, so the profile
    // is a brightness curve regardless of the RT's component layout.
    if (g_halo3_cap_h >= 8 && g_halo3_cap_w) {
      constexpr uint32_t kBands = 16;
      uint64_t band_sum[kBands] = {};
      uint64_t band_px[kBands] = {};
      for (uint32_t y = 0; y < g_halo3_cap_h; ++y) {
        uint32_t band = y * kBands / g_halo3_cap_h;
        if (band >= kBands) band = kBands - 1;
        for (uint32_t x = 0; x < g_halo3_cap_w; ++x) {
          uint32_t texel = p[y * g_halo3_cap_w + x];
          band_sum[band] += (texel & 0xFFu) + ((texel >> 8) & 0xFFu) +
                            ((texel >> 16) & 0xFFu);
          ++band_px[band];
        }
      }
      std::string profile;
      for (uint32_t b = 0; b < kBands; ++b) {
        profile += fmt::format(
            "{} ", band_px[b] ? band_sum[b] / band_px[b] : uint64_t(0));
      }
      XELOGI("TESTRIG_ROWPROFILE {} h={} bands: {}", tag, g_halo3_cap_h,
             profile);
    }
    // DIAG(gpu/rt-orientation): write the captured render target to a raw file
    // so it can be LOOKED AT instead of inferred from statistics.
    //
    // The luminance-band profile was too coarse to settle orientation (s16.6).
    // The user confirms the menu camera never rolls - sky is always up - so the
    // image being mirrored is established; what is not established is WHERE it
    // becomes mirrored. Seeing the host render target directly answers that
    // outright: correct here means the flip is downstream (dump/resolve/texture
    // load); mirrored here means it is in the draws that produced it.
    {
      FILE* f = fopen("/data/data/org.xeniaae.canary/cache/rtcap.raw", "wb");
      if (f) {
        fwrite(&g_halo3_cap_w, 4, 1, f);
        fwrite(&g_halo3_cap_h, 4, 1, f);
        fwrite(mapped, 1, bytes, f);
        fclose(f);
        XELOGI("TESTRIG_CAPIMG wrote rtcap.raw {}x{}", g_halo3_cap_w,
               g_halo3_cap_h);
      }
    }
    dfn.vkUnmapMemory(device, g_halo3_cap_mem);
  }
}

void VulkanCommandProcessor::TestrigReadbackAndLogImage(
    const char* tag, VkImage image, VkImageLayout current_layout,
    uint32_t width, uint32_t height) {
  uint32_t copy_w = std::min<uint32_t>(width, 256);
  uint32_t copy_h = std::min<uint32_t>(height, 256);
  uint32_t bytes = copy_w * copy_h * 4;
  VkBuffer rbuf = RequestReadbackBuffer(bytes);
  if (rbuf == VK_NULL_HANDLE) return;
  const ui::vulkan::VulkanDevice* const vd = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vd->functions();
  const VkDevice device = vd->device();
  VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  PushImageMemoryBarrier(image, range, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT, current_layout,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  SubmitBarriers(true);
  VkBufferImageCopy region = {};
  region.bufferOffset = 0;
  region.bufferRowLength = copy_w;
  region.bufferImageHeight = copy_h;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageExtent.width = copy_w;
  region.imageExtent.height = copy_h;
  region.imageExtent.depth = 1;
  deferred_command_buffer_.CmdVkCopyImageToBuffer(
      image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &region);
  PushImageMemoryBarrier(image, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, current_layout);
  SubmitBarriers(true);
  if (!AwaitAllQueueOperationsCompletion()) return;
  void* mapped = nullptr;
  if (dfn.vkMapMemory(device, memexport_readback_buffer_memory_, 0, bytes, 0,
                      &mapped) == VK_SUCCESS &&
      mapped) {
    const uint32_t* p = static_cast<const uint32_t*>(mapped);
    uint32_t n = bytes / 4, nonzero = 0, changes = 0, prev = 0xDEADBEEFu;
    for (uint32_t k = 0; k < n; ++k) {
      if (p[k] != 0) ++nonzero;
      if (p[k] != prev) {
        ++changes;
        prev = p[k];
      }
    }
    XELOGI(
        "TESTRIG_IMAGE {} {}x{} nonzero={}/{} changes={} s0=0x{:08X} "
        "s1=0x{:08X} smid=0x{:08X}",
        tag, copy_w, copy_h, nonzero, n, changes, p[0], p[1], p[n / 2]);
    dfn.vkUnmapMemory(device, memexport_readback_buffer_memory_);
  }
}

void VulkanCommandProcessor::TestrigResolveAndReadImage(
    const char* tag, VkImage msaa_src, VkImageLayout src_layout,
    VkFormat format, uint32_t width, uint32_t height) {
  uint32_t w = std::min<uint32_t>(width, 512);
  uint32_t h = std::min<uint32_t>(height, 512);
  const ui::vulkan::VulkanDevice* const vd = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vd->functions();
  const VkDevice device = vd->device();
  if (g_halo3_resolve_img == VK_NULL_HANDLE || g_halo3_resolve_fmt != format) {
    if (g_halo3_resolve_img != VK_NULL_HANDLE) {
      dfn.vkDestroyImage(device, g_halo3_resolve_img, nullptr);
      dfn.vkFreeMemory(device, g_halo3_resolve_mem, nullptr);
      g_halo3_resolve_img = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {512, 512, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(
            vd, ici, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
            g_halo3_resolve_img, g_halo3_resolve_mem)) {
      XELOGI("TESTRIG_RESOLVE {} scratch image create FAILED", tag);
      g_halo3_resolve_img = VK_NULL_HANDLE;
      return;
    }
    g_halo3_resolve_fmt = format;
  }
  uint32_t bytes = w * h * 4;
  VkBuffer rbuf = RequestReadbackBuffer(bytes);
  if (rbuf == VK_NULL_HANDLE) return;
  VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  PushImageMemoryBarrier(
      msaa_src, range, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
          VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_TRANSFER_READ_BIT, src_layout,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  PushImageMemoryBarrier(g_halo3_resolve_img, range,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  SubmitBarriers(true);
  VkImageResolve resolve_region = {};
  resolve_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  resolve_region.srcSubresource.layerCount = 1;
  // Read from the CENTER of the RT (the top-left corner is often uniform sky).
  resolve_region.srcOffset.x = 0;
  resolve_region.srcOffset.y = 0;
  resolve_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  resolve_region.dstSubresource.layerCount = 1;
  resolve_region.extent = {w, h, 1};
  deferred_command_buffer_.CmdVkResolveImage(
      msaa_src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_halo3_resolve_img,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolve_region);
  PushImageMemoryBarrier(
      g_halo3_resolve_img, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  PushImageMemoryBarrier(msaa_src, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_layout);
  SubmitBarriers(true);
  VkBufferImageCopy copy = {};
  copy.bufferRowLength = w;
  copy.bufferImageHeight = h;
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = {w, h, 1};
  deferred_command_buffer_.CmdVkCopyImageToBuffer(
      g_halo3_resolve_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &copy);
  SubmitBarriers(true);
  if (!AwaitAllQueueOperationsCompletion()) return;
  void* mapped = nullptr;
  if (dfn.vkMapMemory(device, memexport_readback_buffer_memory_, 0, bytes, 0,
                      &mapped) == VK_SUCCESS &&
      mapped) {
    const uint32_t* p = static_cast<const uint32_t*>(mapped);
    uint32_t n = bytes / 4, nonzero = 0, changes = 0, prev = 0xDEADBEEFu;
    for (uint32_t k = 0; k < n; ++k) {
      if (p[k] != 0) ++nonzero;
      if (p[k] != prev) {
        ++changes;
        prev = p[k];
      }
    }
    XELOGI(
        "TESTRIG_RESOLVE {} {}x{} nonzero={}/{} changes={} s0=0x{:08X} "
        "smid=0x{:08X} (changes>1 => raw MSAA samples survived storage)",
        tag, w, h, nonzero, n, changes, p[0], p[n / 2]);
    // Dump the raw RGBA8 to a file so it can be viewed on the host.
    FILE* f = std::fopen(
        "/storage/emulated/0/Android/data/org.xeniaae.canary/files/xeniaae/"
        "vista_rt.raw",
        "wb");
    if (f) {
      std::fwrite(p, 1, bytes, f);
      std::fclose(f);
      XELOGI("TESTRIG_RESOLVE {} wrote {}x{} RGBA8 to vista_rt.raw", tag, w, h);
    }
    dfn.vkUnmapMemory(device, memexport_readback_buffer_memory_);
  }
}

bool VulkanCommandProcessor::gpu_trace_enabled() {
  // Cached, cheap gate. Unlike the other testrig subsystems, the gputrace flag
  // defaults OFF (opt-in) - the full pipeline trace is very verbose, so it must
  // be explicitly turned on and shouldn't spam normal runs:
  //   adb shell setprop debug.canary.testrig.gputrace 1
  // Still honors the testrig master switch (master 0 disables everything).
  static std::atomic<bool> enabled{false};
  static std::atomic<int64_t> next_check_ms{0};
  int64_t now_ms = xe::testrig::internal::NowMs();
  if (now_ms >= next_check_ms.load(std::memory_order_relaxed)) {
    bool e = xe::testrig::internal::PropertyEnabled(
                 "debug.canary.testrig.master", true) &&
             xe::testrig::internal::PropertyEnabled(
                 "debug.canary.testrig.gputrace", false);
    enabled.store(e, std::memory_order_relaxed);
    next_check_ms.store(now_ms + xe::testrig::internal::kHotPathCacheMs,
                        std::memory_order_relaxed);
    return e;
  }
  return enabled.load(std::memory_order_relaxed);
}

void VulkanCommandProcessor::GpuTrace(const char* stage,
                                      const std::string& detail) {
  static std::atomic<uint64_t> seq{0};
  XELOGI("GPUTRACE seq={} frame={} {} {}",
         seq.fetch_add(1, std::memory_order_relaxed), frame_current_, stage,
         detail);
}

void VulkanCommandProcessor::TestrigReadbackAndLogBuffer(const char* tag,
                                                         VkBuffer buffer,
                                                         uint64_t offset,
                                                         uint64_t size) {
  uint32_t copy_size = std::min<uint32_t>(262144u, uint32_t(size));
  if (!copy_size) return;
  VkBuffer rbuf = RequestReadbackBuffer(copy_size);
  if (rbuf == VK_NULL_HANDLE) return;
  const ui::vulkan::VulkanDevice* const vd = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vd->functions();
  const VkDevice device = vd->device();
  VkBufferCopy region = {};
  region.srcOffset = offset;
  region.dstOffset = 0;
  region.size = copy_size;
  deferred_command_buffer_.CmdVkCopyBuffer(buffer, rbuf, 1, &region);
  if (!AwaitAllQueueOperationsCompletion()) return;
  void* mapped = nullptr;
  if (dfn.vkMapMemory(device, memexport_readback_buffer_memory_, 0, copy_size, 0,
                      &mapped) == VK_SUCCESS &&
      mapped) {
    const uint32_t* p = static_cast<const uint32_t*>(mapped);
    uint32_t n = copy_size / 4, nonzero = 0, changes = 0, prev = 0xDEADBEEFu;
    for (uint32_t k = 0; k < n; ++k) {
      if (p[k] != 0) ++nonzero;
      if (p[k] != prev) {
        ++changes;
        prev = p[k];
      }
    }
    XELOGI(
        "TESTRIG_READBACK {} nonzero={}/{} changes={} s0=0x{:08X} s1=0x{:08X} "
        "s2=0x{:08X}",
        tag, nonzero, n, changes, p[0], p[1], p[2]);
    dfn.vkUnmapMemory(device, memexport_readback_buffer_memory_);
  }
}

bool VulkanCommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (!BeginSubmission(true)) {
    return false;
  }

  uint32_t written_address, written_length;
  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                     written_address, written_length)) {
    return false;
  }
  // TESTRIG(halo3-rtmap): every EDRAM->guest-RAM resolve, with the guest
  // address range it wrote. Correlate against RTMAP (which EDRAM tile the
  // vista drew to) and against the texture the fullscreen background pass
  // samples: the chain is vista-draw -> resolve(EDRAM->guest addr A) ->
  // fullscreen pass samples texture at addr A. A missing resolve, or a
  // fullscreen pass sampling a different address, is the bug.
    {  // TESTRIG(gpu): was UNGATED - flooded ~650 lines/sec with diagnostics off,
     // collapsing framerate to an apparent freeze (NFS Carbon stuck at the main
     // menu, 2026-07-26). Leftover from the Halo 3 vista investigation.
    static std::atomic<bool> tr_en{true};
    static std::atomic<int64_t> tr_next{0};
    if (xe::testrig::HotPathEnabledCached("gpu", tr_en, tr_next)) {
    XELOGI("RESOLVE dest_addr=0x{:08X} length={}", written_address,
             written_length);
    }
  }

  if (gpu_trace_enabled()) {
    GpuTrace("RESOLVE", fmt::format("dest=0x{:08X} length={}", written_address,
                                    written_length));
  }

  // TESTRIG(halo3-gbufgpu): read the SHARED-MEMORY GPU BUFFER content at the
  // vista's G-buffer resolve dest, bypassing the readback memory-accessible
  // gate below (which skips these GPU-scratch addresses - that's why the
  // earlier guest-RAM dump read all zeros). This is the decisive instrument:
  // if the shared memory holds the vista here (many distinct nonzero dwords),
  // the EDRAM resolve WORKS and the break is the texture LOAD from shared
  // memory; if it's zeros, the break is upstream (host-RT->edram_buffer dump or
  // the resolve compute). Read-only - does NOT write guest RAM. One-shot per
  // address.
  {
    static int gbufgpu_dumps = 0;
    // DISABLED for clean build: this mid-resolve GBUFGPU readback does an
    // AwaitAll (full GPU idle) that perturbs Halo 3's streaming sync and can
    // trigger the streaming-semaphore deadlock. Set the cap to 0 to disable.
    if (gbufgpu_dumps < 0 && written_length >= 4096 &&
        (written_address == 0x044B0000u || written_address == 0x04780000u ||
         written_address == 0x043FC000u)) {
      ++gbufgpu_dumps;
      uint32_t copy_size = std::min<uint32_t>(262144u, written_length);
      VkBuffer rbuf = RequestReadbackBuffer(copy_size);
      if (rbuf != VK_NULL_HANDLE) {
        const ui::vulkan::VulkanDevice* const vd = GetVulkanDevice();
        const ui::vulkan::VulkanDevice::Functions& dfn = vd->functions();
        const VkDevice device = vd->device();
        shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
        VkBufferCopy region = {};
        region.srcOffset = written_address;
        region.dstOffset = 0;
        region.size = copy_size;
        deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(), rbuf,
                                                 1, &region);
        if (AwaitAllQueueOperationsCompletion()) {
          void* mapped = nullptr;
          if (dfn.vkMapMemory(device, memexport_readback_buffer_memory_, 0,
                              copy_size, 0, &mapped) == VK_SUCCESS &&
              mapped) {
            const uint32_t* p = static_cast<const uint32_t*>(mapped);
            uint32_t n = copy_size / 4, nonzero = 0, changes = 0,
                     prev = 0xDEADBEEFu;
            for (uint32_t k = 0; k < n; ++k) {
              if (p[k] != 0) ++nonzero;
              if (p[k] != prev) {
                ++changes;
                prev = p[k];
              }
            }
            XELOGI(
                "GBUFGPU addr=0x{:08X} nonzero={}/{} changes={} s0=0x{:08X} "
                "s1=0x{:08X} s2=0x{:08X}",
                written_address, nonzero, n, changes, p[0], p[1], p[2]);
            dfn.vkUnmapMemory(device, memexport_readback_buffer_memory_);
          }
        }
      }
    }
  }

  // CPU readback resolve path (if not disabled).
  ReadbackResolveMode readback_mode = GetReadbackResolveMode();
  if (readback_mode != ReadbackResolveMode::kDisabled &&
      !texture_cache_->IsDrawResolutionScaled() && written_length > 0) {
    // Early check: if destination memory is not accessible, skip all the
    // expensive GPU readback work.
    VirtualHeap* physical_heap = memory_->GetPhysicalHeap();
    bool memory_accessible = false;
    if (physical_heap) {
      HeapAllocationInfo alloc_info;
      if (physical_heap->QueryRegionInfo(written_address, &alloc_info) &&
          (alloc_info.state & kMemoryAllocationCommit) &&
          (alloc_info.protect & kMemoryProtectWrite)) {
        uint32_t end_address = written_address + written_length;
        uint32_t region_end = alloc_info.base_address + alloc_info.region_size;
        if (end_address <= region_end) {
          memory_accessible = true;
        }
      }
    }

    if (!memory_accessible) {
      // Destination memory not accessible, skip readback entirely
      return true;
    }

    // Ported from XenDroid: UMA direct readback.
    //
    // The shared memory buffer is host-mapped (see the host-visible port in
    // vulkan_shared_memory.cc), so the CPU can read the resolved bytes straight
    // out of it - no device->host staging copy at all, and guest RAM and the
    // GPU never diverge. This is the mechanism XenDroid credits for fixing
    // Halo 3's collapsed skinned geometry.
    //
    // XenDroid's note is worth keeping: gating readback on an *imported* guest
    // RAM buffer "disabled readback outright wherever guest RAM cannot be
    // imported (no VK_EXT_external_memory_host, i.e. every Adreno)" - which is
    // exactly this device, so the host-mapped route is the one that works here.
    //
    // Falls through to the existing staging path when the buffer did not land
    // on a host-visible type, so enabling the mode can never make things worse
    // than kFast.
    if (readback_mode == ReadbackResolveMode::kUma &&
        shared_memory_->IsHostMapped()) {
      // Make the resolve's writes visible to the host, then drain so the CPU
      // does not race the GPU writing the same region. Guest shader stages are
      // included because in-pass resolves write shared memory from the fragment
      // stage, not only from compute/transfer.
      PushBufferMemoryBarrier(
          shared_memory_->buffer(), 0, VK_WHOLE_SIZE,
          guest_shader_pipeline_stages_ |
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
              VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_HOST_BIT,
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_HOST_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
          VK_QUEUE_FAMILY_IGNORED, false);
      SubmitBarriers(true);
      if (!AwaitAllQueueOperationsCompletion()) {
        XELOGE("UMAREAD resolve readback drain failed");
        return true;
      }
      shared_memory_->ReadHostMapped(written_address, written_length,
                                     memory_->TranslatePhysical(written_address));
      {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1) < 8) {
          XELOGI("UMAREAD direct 0x{:08X} len={}", written_address,
                 written_length);
        }
      }
      return true;
    }

    // Create a key for this specific resolve operation
    uint64_t resolve_key =
        MakeReadbackResolveKey(written_address, written_length);
    ReadbackBuffer& rb = readback_buffers_[resolve_key];
    rb.last_used_frame = frame_current_;

    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();

    uint32_t write_index = rb.current_index;
    uint32_t size = AlignReadbackBufferSize(written_length);

    // Allocate/resize write buffer if needed
    if (size > rb.sizes[write_index]) {
      // Create buffer with TRANSFER_DST usage for copying from GPU.
      VkBufferCreateInfo buffer_info = {};
      buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      buffer_info.size = size;
      buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

      VkBuffer new_buffer;
      if (dfn.vkCreateBuffer(device, &buffer_info, nullptr, &new_buffer) !=
          VK_SUCCESS) {
        XELOGE(
            "VulkanCommandProcessor: Failed to create readback buffer of {} MB",
            size >> 20);
        return true;
      }

      // Get memory requirements.
      VkMemoryRequirements memory_requirements;
      dfn.vkGetBufferMemoryRequirements(device, new_buffer,
                                        &memory_requirements);

      // Allocate HOST_VISIBLE | HOST_CACHED | HOST_COHERENT memory for
      // readback.
      const uint32_t memory_type_index = ui::vulkan::util::ChooseMemoryType(
          vulkan_device->memory_types(), memory_requirements.memoryTypeBits,
          ui::vulkan::util::MemoryPurpose::kReadback);

      if (memory_type_index == UINT32_MAX) {
        XELOGE(
            "VulkanCommandProcessor: Failed to find memory type for readback "
            "buffer");
        dfn.vkDestroyBuffer(device, new_buffer, nullptr);
        return true;
      }

      VkMemoryAllocateInfo memory_info = {};
      memory_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      memory_info.allocationSize = memory_requirements.size;
      memory_info.memoryTypeIndex = memory_type_index;

      VkDeviceMemory new_memory;
      if (dfn.vkAllocateMemory(device, &memory_info, nullptr, &new_memory) !=
          VK_SUCCESS) {
        XELOGE(
            "VulkanCommandProcessor: Failed to allocate readback buffer "
            "memory");
        dfn.vkDestroyBuffer(device, new_buffer, nullptr);
        return true;
      }

      // Bind memory to buffer.
      if (dfn.vkBindBufferMemory(device, new_buffer, new_memory, 0) !=
          VK_SUCCESS) {
        XELOGE("VulkanCommandProcessor: Failed to bind readback buffer memory");
        dfn.vkFreeMemory(device, new_memory, nullptr);
        dfn.vkDestroyBuffer(device, new_buffer, nullptr);
        return true;
      }

      // Clean up old buffer if exists
      if (rb.buffers[write_index] != VK_NULL_HANDLE) {
        dfn.vkDestroyBuffer(device, rb.buffers[write_index], nullptr);
      }
      if (rb.memories[write_index] != VK_NULL_HANDLE) {
        dfn.vkFreeMemory(device, rb.memories[write_index], nullptr);
      }

      rb.buffers[write_index] = new_buffer;
      rb.memories[write_index] = new_memory;
      rb.sizes[write_index] = size;
    }

    VkBuffer shared_memory_buffer = shared_memory_->buffer();

    // Ensure shared memory is ready for transfer.
    shared_memory_->Use(VulkanSharedMemory::Usage::kRead);

    // Copy GPU buffer → staging buffer.
    VkBufferCopy copy_region = {};
    copy_region.srcOffset = written_address;
    copy_region.dstOffset = 0;
    copy_region.size = written_length;

    deferred_command_buffer_.CmdVkCopyBuffer(
        shared_memory_buffer, rb.buffers[write_index], 1, &copy_region);

    bool use_delayed_sync = (readback_mode == ReadbackResolveMode::kFast);
    uint32_t read_index = write_index;

    if (use_delayed_sync) {
      // Use previous frame's data (avoid stall)
      read_index = 1 - write_index;
    } else {
      // Wait for GPU to finish (accurate but slow)
      if (!AwaitAllQueueOperationsCompletion()) {
        XELOGE(
            "VulkanCommandProcessor: Failed to complete queue operations for "
            "resolve readback");
        return true;
      }
    }

    // Read from the appropriate buffer
    // If using delayed sync but previous buffer doesn't exist, use current
    // buffer with sync as fallback
    if (use_delayed_sync && (rb.buffers[read_index] == VK_NULL_HANDLE ||
                             written_length > rb.sizes[read_index])) {
      read_index = write_index;
      if (!AwaitAllQueueOperationsCompletion()) {
        XELOGE(
            "VulkanCommandProcessor: Failed to complete queue operations for "
            "resolve readback fallback");
        return true;
      }
    }

    if (rb.buffers[read_index] != VK_NULL_HANDLE &&
        written_length <= rb.sizes[read_index]) {
      void* mapped_data;
      if (dfn.vkMapMemory(device, rb.memories[read_index], 0, written_length, 0,
                          &mapped_data) == VK_SUCCESS) {
        if (mapped_data) {
          // Memory accessibility already checked at the start of this function
          uint8_t* dest_ptr = memory_->TranslatePhysical(written_address);
          memory::vastcpy(dest_ptr, static_cast<uint8_t*>(mapped_data),
                          written_length);
        } else {
          XELOGE(
              "VulkanCommandProcessor: Failed to map readback buffer "
              "(mapped_data is null)");
        }
        dfn.vkUnmapMemory(device, rb.memories[read_index]);
      } else {
        XELOGE(
            "VulkanCommandProcessor: Failed to map readback buffer memory for "
            "resolve");
      }
    }
  }

  return true;
}

VkBuffer VulkanCommandProcessor::RequestReadbackBuffer(uint32_t size) {
  if (size == 0) {
    return VK_NULL_HANDLE;
  }

  size = AlignReadbackBufferSize(size);

  if (size > memexport_readback_buffer_size_) {
    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();

    // Create buffer with TRANSFER_DST usage for copying from GPU.
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer new_buffer;
    if (dfn.vkCreateBuffer(device, &buffer_info, nullptr, &new_buffer) !=
        VK_SUCCESS) {
      XELOGE(
          "VulkanCommandProcessor: Failed to create readback buffer of {} MB",
          size >> 20);
      return VK_NULL_HANDLE;
    }

    // Get memory requirements.
    VkMemoryRequirements memory_requirements;
    dfn.vkGetBufferMemoryRequirements(device, new_buffer, &memory_requirements);

    // Allocate HOST_VISIBLE | HOST_CACHED | HOST_COHERENT memory for readback.
    const uint32_t memory_type_index = ui::vulkan::util::ChooseMemoryType(
        vulkan_device->memory_types(), memory_requirements.memoryTypeBits,
        ui::vulkan::util::MemoryPurpose::kReadback);
    if (memory_type_index == UINT32_MAX) {
      XELOGE(
          "VulkanCommandProcessor: Failed to find suitable memory type for "
          "readback buffer");
      dfn.vkDestroyBuffer(device, new_buffer, nullptr);
      return VK_NULL_HANDLE;
    }

    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = memory_requirements.size;
    alloc_info.memoryTypeIndex = memory_type_index;

    VkDeviceMemory new_memory;
    if (dfn.vkAllocateMemory(device, &alloc_info, nullptr, &new_memory) !=
        VK_SUCCESS) {
      XELOGE(
          "VulkanCommandProcessor: Failed to allocate memory for readback "
          "buffer");
      dfn.vkDestroyBuffer(device, new_buffer, nullptr);
      return VK_NULL_HANDLE;
    }

    // Bind memory to buffer.
    if (dfn.vkBindBufferMemory(device, new_buffer, new_memory, 0) !=
        VK_SUCCESS) {
      XELOGE(
          "VulkanCommandProcessor: Failed to bind memory to readback buffer");
      dfn.vkFreeMemory(device, new_memory, nullptr);
      dfn.vkDestroyBuffer(device, new_buffer, nullptr);
      return VK_NULL_HANDLE;
    }

    // Destroy old buffer if it exists.
    if (memexport_readback_buffer_ != VK_NULL_HANDLE) {
      dfn.vkDestroyBuffer(device, memexport_readback_buffer_, nullptr);
      dfn.vkFreeMemory(device, memexport_readback_buffer_memory_, nullptr);
    }

    memexport_readback_buffer_ = new_buffer;
    memexport_readback_buffer_memory_ = new_memory;
    memexport_readback_buffer_size_ = size;
  }

  return memexport_readback_buffer_;
}

void VulkanCommandProcessor::InitializeTrace() {
  CommandProcessor::InitializeTrace();

  if (!BeginSubmission(true)) {
    return;
  }
  // TODO(Triang3l): Write the EDRAM.
  bool shared_memory_submitted =
      shared_memory_->InitializeTraceSubmitDownloads();
  if (!shared_memory_submitted) {
    return;
  }
  AwaitAllQueueOperationsCompletion();
  if (shared_memory_submitted) {
    shared_memory_->InitializeTraceCompleteDownloads();
  }
}

void VulkanCommandProcessor::CheckSubmissionFenceAndDeviceLoss(
    uint64_t await_submission) {
  // Only report once, no need to retry a wait that won't succeed anyway.
  if (device_lost_) {
    return;
  }

  if (await_submission >= GetCurrentSubmission()) {
    if (submission_open_) {
      EndSubmission(false);
    }
    // A submission won't be ended if it hasn't been started, or if ending
    // has failed - clamp the index.
    await_submission = GetCurrentSubmission() - 1;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  size_t fences_total = submissions_in_flight_fences_.size();
  size_t fences_awaited = 0;
  if (await_submission > submission_completed_) {
    // Await in a blocking way if requested.
    // TODO(Triang3l): Await only one fence. "Fence signal operations that are
    // defined by vkQueueSubmit additionally include in the first
    // synchronization scope all commands that occur earlier in submission
    // order."
    // TESTRIG(frame-budget): this is the CPU BLOCKING on the GPU. Separating it
    // from translation is the whole question: the CP thread measured ~100%
    // "executing", but that figure covers both real PM4->Vulkan work and time
    // parked here. Those need opposite fixes - cheaper draws vs fewer sync
    // points - so attributing it wrongly would send the next days of work in
    // the wrong direction.
    const auto xe_fence_begin = std::chrono::steady_clock::now();
    VkResult wait_result = dfn.vkWaitForFences(
        device, uint32_t(await_submission - submission_completed_),
        submissions_in_flight_fences_.data(), VK_TRUE, UINT64_MAX);
    xe_gpu_wait_ns_.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - xe_fence_begin).count()),
        std::memory_order_relaxed);
    if (wait_result == VK_SUCCESS) {
      fences_awaited += await_submission - submission_completed_;
    } else {
      XELOGE("Failed to await submission completion Vulkan fences");
      if (wait_result == VK_ERROR_DEVICE_LOST) {
        device_lost_ = true;
      }
    }
  }
  // Check how far into the submissions the GPU currently is, in order because
  // submission themselves can be executed out of order, but Xenia serializes
  // that for simplicity.
  while (fences_awaited < fences_total) {
    VkResult fence_status = dfn.vkWaitForFences(
        device, 1, &submissions_in_flight_fences_[fences_awaited], VK_TRUE, 0);
    if (fence_status != VK_SUCCESS) {
      if (fence_status == VK_ERROR_DEVICE_LOST) {
        device_lost_ = true;
      }
      break;
    }
    ++fences_awaited;
  }
  if (device_lost_) {
    graphics_system_->OnHostGpuLossFromAnyThread(true);
    return;
  }
  if (!fences_awaited) {
    // Not updated - no need to reclaim or download things.
    return;
  }
  // Reclaim fences.
  fences_free_.reserve(fences_free_.size() + fences_awaited);
  auto submissions_in_flight_fences_awaited_end =
      submissions_in_flight_fences_.cbegin();
  std::advance(submissions_in_flight_fences_awaited_end, fences_awaited);
  fences_free_.insert(fences_free_.cend(),
                      submissions_in_flight_fences_.cbegin(),
                      submissions_in_flight_fences_awaited_end);
  submissions_in_flight_fences_.erase(submissions_in_flight_fences_.cbegin(),
                                      submissions_in_flight_fences_awaited_end);
  submission_completed_ += fences_awaited;

  // Reclaim semaphores.
  while (!submissions_in_flight_semaphores_.empty()) {
    const auto& semaphore_submission =
        submissions_in_flight_semaphores_.front();
    if (semaphore_submission.first > submission_completed_) {
      break;
    }
    semaphores_free_.push_back(semaphore_submission.second);
    submissions_in_flight_semaphores_.pop_front();
  }

  // Reclaim command pools.
  while (!command_buffers_submitted_.empty()) {
    const auto& command_buffer_pair = command_buffers_submitted_.front();
    if (command_buffer_pair.first > submission_completed_) {
      break;
    }
    command_buffers_writable_.push_back(command_buffer_pair.second);
    command_buffers_submitted_.pop_front();
  }

  shared_memory_->CompletedSubmissionUpdated();

  primitive_processor_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();

  texture_cache_->CompletedSubmissionUpdated(submission_completed_);

  // Destroy objects scheduled for destruction.
  while (!destroy_framebuffers_.empty()) {
    const auto& destroy_pair = destroy_framebuffers_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkDestroyFramebuffer(device, destroy_pair.second, nullptr);
    destroy_framebuffers_.pop_front();
  }
  while (!destroy_buffers_.empty()) {
    const auto& destroy_pair = destroy_buffers_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkDestroyBuffer(device, destroy_pair.second, nullptr);
    destroy_buffers_.pop_front();
  }
  while (!destroy_memory_.empty()) {
    const auto& destroy_pair = destroy_memory_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkFreeMemory(device, destroy_pair.second, nullptr);
    destroy_memory_.pop_front();
  }
}

bool VulkanCommandProcessor::BeginSubmission(bool is_guest_command) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (device_lost_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame. Also check whether the device
  // is still available, and whether the await was successful.
  uint64_t await_submission =
      is_opening_frame
          ? closed_frame_submissions_[frame_current_ % kMaxFramesInFlight]
          : 0;
  CheckSubmissionFenceAndDeviceLoss(await_submission);
  if (device_lost_ || submission_completed_ < await_submission) {
    return false;
  }

  if (is_opening_frame) {
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ = std::max(frame_current_, uint64_t(kMaxFramesInFlight)) -
                       kMaxFramesInFlight;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_;
         ++frame) {
      if (closed_frame_submissions_[frame % kMaxFramesInFlight] >
          submission_completed_) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;

    // Start a new deferred command buffer - will submit it to the real one in
    // the end of the submission (when async pipeline object creation requests
    // are fulfilled).
    deferred_command_buffer_.Reset();

    // Reset cached state of the command buffer.
    dynamic_viewport_update_needed_ = true;
    dynamic_scissor_update_needed_ = true;
    dynamic_depth_bias_update_needed_ = true;
    dynamic_blend_constants_update_needed_ = true;
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
    current_render_pass_ = VK_NULL_HANDLE;
    current_framebuffer_ = nullptr;
    current_guest_graphics_pipeline_ = VK_NULL_HANDLE;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
    current_external_compute_pipeline_ = VK_NULL_HANDLE;
    current_guest_graphics_pipeline_layout_ = nullptr;
    current_graphics_descriptor_sets_bound_up_to_date_ = 0;

    primitive_processor_->BeginSubmission();

    texture_cache_->BeginSubmission(GetCurrentSubmission());
  }

  if (is_opening_frame) {
    frame_open_ = true;

    // Swap all readback buffers for delayed sync (one frame behind)
    for (auto& pair : readback_buffers_) {
      pair.second.current_index = 1 - pair.second.current_index;
    }

    // Evict old readback buffers only when map gets too large to prevent
    // unbounded memory growth. Don't do this every frame as it's expensive.
    if (readback_buffers_.size() > kMaxReadbackBuffers) {
      const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
      const ui::vulkan::VulkanDevice::Functions& dfn =
          vulkan_device->functions();
      const VkDevice device = vulkan_device->device();

      for (auto it = readback_buffers_.begin();
           it != readback_buffers_.end();) {
        // Evict if not used recently
        if (frame_current_ > kReadbackBufferEvictionAgeFrames &&
            it->second.last_used_frame <
                frame_current_ - kReadbackBufferEvictionAgeFrames) {
          // Release both buffers and memories
          ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                                 it->second.buffers[0]);
          ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                                 it->second.memories[0]);
          ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                                 it->second.buffers[1]);
          ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                                 it->second.memories[1]);
          it = readback_buffers_.erase(it);
        } else {
          ++it;
        }
      }
    }

    // Reset bindings that depend on transient data.
    std::memset(current_float_constant_map_vertex_, 0,
                sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0,
                sizeof(current_float_constant_map_pixel_));
    std::memset(current_graphics_descriptor_sets_, 0,
                sizeof(current_graphics_descriptor_sets_));
    current_constant_buffers_up_to_date_ = 0;
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
            shared_memory_and_edram_descriptor_set_;
    current_graphics_descriptor_set_values_up_to_date_ =
        UINT32_C(1)
        << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram;

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    // FIXME(Triang3l): This will result in a memory leak if the guest is not
    // presenting.
    uniform_buffer_pool_->Reclaim(frame_completed_);
    while (!single_transient_descriptors_used_.empty()) {
      const UsedSingleTransientDescriptor& used_transient_descriptor =
          single_transient_descriptors_used_.front();
      if (used_transient_descriptor.frame > frame_completed_) {
        break;
      }
      single_transient_descriptors_free_[size_t(
                                             used_transient_descriptor.layout)]
          .push_back(used_transient_descriptor.set);
      single_transient_descriptors_used_.pop_front();
    }
    while (!constants_transient_descriptors_used_.empty()) {
      const std::pair<uint64_t, VkDescriptorSet>& used_transient_descriptor =
          constants_transient_descriptors_used_.front();
      if (used_transient_descriptor.first > frame_completed_) {
        break;
      }
      constants_transient_descriptors_free_.push_back(
          used_transient_descriptor.second);
      constants_transient_descriptors_used_.pop_front();
    }
    while (!texture_transient_descriptor_sets_used_.empty()) {
      const UsedTextureTransientDescriptorSet& used_transient_descriptor_set =
          texture_transient_descriptor_sets_used_.front();
      if (used_transient_descriptor_set.frame > frame_completed_) {
        break;
      }
      auto it = texture_transient_descriptor_sets_free_.find(
          used_transient_descriptor_set.layout);
      if (it == texture_transient_descriptor_sets_free_.end()) {
        it =
            texture_transient_descriptor_sets_free_
                .emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(used_transient_descriptor_set.layout),
                    std::forward_as_tuple())
                .first;
      }
      it->second.push_back(used_transient_descriptor_set.set);
      texture_transient_descriptor_sets_used_.pop_front();
    }

    primitive_processor_->BeginFrame();

    texture_cache_->BeginFrame();
  }

  return true;
}

// TESTRIG(frame-budget): reports how the GPU thread's second was actually spent.
//
// The base CommandProcessor already reports starved-vs-executing. That showed
// ~100% "executing", but executing covers BOTH translating PM4 into Vulkan AND
// sitting blocked on the GPU. This splits that apart:
//   gpu_wait  - CPU parked in vkWaitForFences, i.e. the GPU is the limit
//   submit    - inside vkQueueSubmit (driver-side command processing)
//   remainder - real translation work on the CPU
// Cheaper draws and fewer sync points are opposite fixes, so this decides which.
void VulkanCommandProcessor::XeReportFrameBudget() {
  if (!XE_AE_DIAG_ENABLED("debug.canary.frame_budget")) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - xe_budget_last_ < std::chrono::seconds(1)) {
    return;
  }
  const uint64_t elapsed_ns =
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          now - xe_budget_last_).count());
  xe_budget_last_ = now;
  const uint64_t waited = xe_gpu_wait_ns_.exchange(0, std::memory_order_relaxed);
  const uint64_t submitted = xe_submit_ns_.exchange(0, std::memory_order_relaxed);
  if (elapsed_ns == 0) return;
  XELOGI(
      "TESTRIG(frame-budget): GPU-thread second: blocked-on-GPU {}% ({} ms) | "
      "vkQueueSubmit {}% ({} ms) | translating+other {}%",
      (waited * 100) / elapsed_ns, waited / 1000000,
      (submitted * 100) / elapsed_ns, submitted / 1000000,
      100 - ((waited + submitted) * 100) / elapsed_ns);
}

bool VulkanCommandProcessor::EndSubmission(bool is_swap) {
  XeReportFrameBudget();
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Make sure everything needed for submitting exist.
  if (submission_open_) {
    if (fences_free_.empty()) {
      VkFenceCreateInfo fence_create_info;
      fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      fence_create_info.pNext = nullptr;
      fence_create_info.flags = 0;
      VkFence fence;
      if (dfn.vkCreateFence(device, &fence_create_info, nullptr, &fence) !=
          VK_SUCCESS) {
        XELOGE("Failed to create a Vulkan fence");
        // Try to submit later. Completely dropping the submission is not
        // permitted because resources would be left in an undefined state.
        return false;
      }
      fences_free_.push_back(fence);
    }
    if (!sparse_memory_binds_.empty() && semaphores_free_.empty()) {
      VkSemaphoreCreateInfo semaphore_create_info;
      semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      semaphore_create_info.pNext = nullptr;
      semaphore_create_info.flags = 0;
      VkSemaphore semaphore;
      if (dfn.vkCreateSemaphore(device, &semaphore_create_info, nullptr,
                                &semaphore) != VK_SUCCESS) {
        XELOGE("Failed to create a Vulkan semaphore");
        return false;
      }
      semaphores_free_.push_back(semaphore);
    }
    if (command_buffers_writable_.empty()) {
      CommandBuffer command_buffer;
      VkCommandPoolCreateInfo command_pool_create_info;
      command_pool_create_info.sType =
          VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      command_pool_create_info.pNext = nullptr;
      command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      command_pool_create_info.queueFamilyIndex =
          vulkan_device->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr,
                                  &command_buffer.pool) != VK_SUCCESS) {
        XELOGE("Failed to create a Vulkan command pool");
        return false;
      }
      VkCommandBufferAllocateInfo command_buffer_allocate_info;
      command_buffer_allocate_info.sType =
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      command_buffer_allocate_info.pNext = nullptr;
      command_buffer_allocate_info.commandPool = command_buffer.pool;
      command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      command_buffer_allocate_info.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info,
                                       &command_buffer.buffer) != VK_SUCCESS) {
        XELOGE("Failed to allocate a Vulkan command buffer");
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
        return false;
      }
      command_buffers_writable_.push_back(command_buffer);
    }
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    primitive_processor_->EndFrame();
  }

  if (submission_open_) {
    assert_false(scratch_buffer_used_);

    EndRenderPass();

    render_target_cache_->EndSubmission();

    primitive_processor_->EndSubmission();

    shared_memory_->EndSubmission();

    uniform_buffer_pool_->FlushWrites();

    // Submit sparse binds earlier, before executing the deferred command
    // buffer, to reduce latency.
    if (!sparse_memory_binds_.empty()) {
      sparse_buffer_bind_infos_temp_.clear();
      sparse_buffer_bind_infos_temp_.reserve(sparse_buffer_binds_.size());
      for (const SparseBufferBind& sparse_buffer_bind : sparse_buffer_binds_) {
        VkSparseBufferMemoryBindInfo& sparse_buffer_bind_info =
            sparse_buffer_bind_infos_temp_.emplace_back();
        sparse_buffer_bind_info.buffer = sparse_buffer_bind.buffer;
        sparse_buffer_bind_info.bindCount = sparse_buffer_bind.bind_count;
        sparse_buffer_bind_info.pBinds =
            sparse_memory_binds_.data() + sparse_buffer_bind.bind_offset;
      }
      assert_false(semaphores_free_.empty());
      VkSemaphore bind_sparse_semaphore = semaphores_free_.back();
      VkBindSparseInfo bind_sparse_info;
      bind_sparse_info.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
      bind_sparse_info.pNext = nullptr;
      bind_sparse_info.waitSemaphoreCount = 0;
      bind_sparse_info.pWaitSemaphores = nullptr;
      bind_sparse_info.bufferBindCount =
          uint32_t(sparse_buffer_bind_infos_temp_.size());
      bind_sparse_info.pBufferBinds =
          !sparse_buffer_bind_infos_temp_.empty()
              ? sparse_buffer_bind_infos_temp_.data()
              : nullptr;
      bind_sparse_info.imageOpaqueBindCount = 0;
      bind_sparse_info.pImageOpaqueBinds = nullptr;
      bind_sparse_info.imageBindCount = 0;
      bind_sparse_info.pImageBinds = 0;
      bind_sparse_info.signalSemaphoreCount = 1;
      bind_sparse_info.pSignalSemaphores = &bind_sparse_semaphore;
      VkResult bind_sparse_result;
      {
        ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
            vulkan_device->AcquireQueue(
                vulkan_device->queue_family_sparse_binding(), 0);
        bind_sparse_result = dfn.vkQueueBindSparse(
            queue_acquisition.queue(), 1, &bind_sparse_info, VK_NULL_HANDLE);
      }
      if (bind_sparse_result != VK_SUCCESS) {
        XELOGE("Failed to submit Vulkan sparse binds");
        return false;
      }
      current_submission_wait_semaphores_.push_back(bind_sparse_semaphore);
      semaphores_free_.pop_back();
      current_submission_wait_stage_masks_.push_back(
          sparse_bind_wait_stage_mask_);
      sparse_bind_wait_stage_mask_ = 0;
      sparse_buffer_binds_.clear();
      sparse_memory_binds_.clear();
    }

    SubmitBarriers(true);

    assert_false(command_buffers_writable_.empty());
    CommandBuffer command_buffer = command_buffers_writable_.back();
    if (dfn.vkResetCommandPool(device, command_buffer.pool, 0) != VK_SUCCESS) {
      XELOGE("Failed to reset a Vulkan command pool");
      return false;
    }
    VkCommandBufferBeginInfo command_buffer_begin_info;
    command_buffer_begin_info.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_buffer_begin_info.pNext = nullptr;
    command_buffer_begin_info.flags =
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    command_buffer_begin_info.pInheritanceInfo = nullptr;
    if (dfn.vkBeginCommandBuffer(command_buffer.buffer,
                                 &command_buffer_begin_info) != VK_SUCCESS) {
      XELOGE("Failed to begin a Vulkan command buffer");
      return false;
    }
    deferred_command_buffer_.Execute(command_buffer.buffer);
    if (dfn.vkEndCommandBuffer(command_buffer.buffer) != VK_SUCCESS) {
      XELOGE("Failed to end a Vulkan command buffer");
      return false;
    }

    VkSubmitInfo submit_info;
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = nullptr;
    if (!current_submission_wait_semaphores_.empty()) {
      submit_info.waitSemaphoreCount =
          uint32_t(current_submission_wait_semaphores_.size());
      submit_info.pWaitSemaphores = current_submission_wait_semaphores_.data();
      submit_info.pWaitDstStageMask =
          current_submission_wait_stage_masks_.data();
    } else {
      submit_info.waitSemaphoreCount = 0;
      submit_info.pWaitSemaphores = nullptr;
      submit_info.pWaitDstStageMask = nullptr;
    }
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer.buffer;
    submit_info.signalSemaphoreCount = 0;
    submit_info.pSignalSemaphores = nullptr;
    assert_false(fences_free_.empty());
    VkFence fence = fences_free_.back();
    if (dfn.vkResetFences(device, 1, &fence) != VK_SUCCESS) {
      XELOGE("Failed to reset a Vulkan submission fence");
      return false;
    }
    VkResult submit_result;
    {
      ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
          vulkan_device->AcquireQueue(
              vulkan_device->queue_family_graphics_compute(), 0);
      submit_result =
          [&]() {
            const auto b = std::chrono::steady_clock::now();
            VkResult r = dfn.vkQueueSubmit(queue_acquisition.queue(), 1,
                                           &submit_info, fence);
            xe_submit_ns_.fetch_add(
                uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - b).count()),
                std::memory_order_relaxed);
            return r;
          }();
    }
    if (submit_result != VK_SUCCESS) {
      XELOGE("Failed to submit a Vulkan command buffer: VkResult={}", int(submit_result));
      if (submit_result == VK_ERROR_DEVICE_LOST && !device_lost_) {
        device_lost_ = true;
        graphics_system_->OnHostGpuLossFromAnyThread(true);
      }
      return false;
    }
    uint64_t submission_current = GetCurrentSubmission();
    current_submission_wait_stage_masks_.clear();
    for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
      submissions_in_flight_semaphores_.emplace_back(submission_current,
                                                     semaphore);
    }
    current_submission_wait_semaphores_.clear();
    command_buffers_submitted_.emplace_back(submission_current, command_buffer);
    command_buffers_writable_.pop_back();
    // Increments the current submission number, going to the next submission.
    submissions_in_flight_fences_.push_back(fence);
    fences_free_.pop_back();

    submission_open_ = false;
  }

  if (is_closing_frame) {
    if (cvars::clear_memory_page_state) {
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }

    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kMaxFramesInFlight] =
        GetCurrentSubmission() - 1;

    if (cache_clear_requested_ && AwaitAllQueueOperationsCompletion()) {
      cache_clear_requested_ = false;

      DestroyScratchBuffer();

      for (SwapFramebuffer& swap_framebuffer : swap_framebuffers_) {
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                               swap_framebuffer.framebuffer);
      }

      assert_true(command_buffers_submitted_.empty());
      for (const CommandBuffer& command_buffer : command_buffers_writable_) {
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
      }
      command_buffers_writable_.clear();

      ClearTransientDescriptorPools();

      uniform_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();

      render_target_cache_->ClearCache();

      // Not clearing the pipeline layouts and the descriptor set layouts as
      // they're referenced by pipelines, which are not destroyed.

      primitive_processor_->ClearCache();

      shared_memory_->ClearCache();
    }
  }

  return true;
}

void VulkanCommandProcessor::ClearTransientDescriptorPools() {
  texture_transient_descriptor_sets_free_.clear();
  texture_transient_descriptor_sets_used_.clear();
  transient_descriptor_allocator_textures_.Reset();

  constants_transient_descriptors_free_.clear();
  constants_transient_descriptors_used_.clear();
  for (std::vector<VkDescriptorSet>& transient_descriptors_free :
       single_transient_descriptors_free_) {
    transient_descriptors_free.clear();
  }
  single_transient_descriptors_used_.clear();
  transient_descriptor_allocator_storage_buffer_.Reset();
  transient_descriptor_allocator_uniform_buffer_.Reset();
}

void VulkanCommandProcessor::SplitPendingBarrier() {
  size_t pending_buffer_memory_barrier_count =
      pending_barriers_buffer_memory_barriers_.size();
  size_t pending_image_memory_barrier_count =
      pending_barriers_image_memory_barriers_.size();
  if (!current_pending_barrier_.src_stage_mask &&
      !current_pending_barrier_.dst_stage_mask &&
      current_pending_barrier_.buffer_memory_barriers_offset >=
          pending_buffer_memory_barrier_count &&
      current_pending_barrier_.image_memory_barriers_offset >=
          pending_image_memory_barrier_count) {
    return;
  }
  pending_barriers_.emplace_back(current_pending_barrier_);
  current_pending_barrier_.src_stage_mask = 0;
  current_pending_barrier_.dst_stage_mask = 0;
  current_pending_barrier_.buffer_memory_barriers_offset =
      pending_buffer_memory_barrier_count;
  current_pending_barrier_.image_memory_barriers_offset =
      pending_image_memory_barrier_count;
}

void VulkanCommandProcessor::DestroyScratchBuffer() {
  assert_false(scratch_buffer_used_);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  scratch_buffer_last_usage_submission_ = 0;
  scratch_buffer_last_access_mask_ = 0;
  scratch_buffer_last_stage_mask_ = 0;
  scratch_buffer_size_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         scratch_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         scratch_buffer_memory_);
}

void VulkanCommandProcessor::UpdateDynamicState(
    const draw_util::ViewportInfo& viewport_info, bool primitive_polygonal,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;

  // Window parameters.
  // http://ftp.tku.edu.tw/NetBSD/NetBSD-current/xsrc/external/mit/xf86-video-ati/dist/src/r600_reg_auto_r6xx.h
  // See r200UpdateWindow:
  // https://github.com/freedreno/mesa/blob/master/src/mesa/drivers/dri/r200/r200_state.c
  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();

  // Viewport.
  VkViewport viewport;
  if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
    viewport.x = float(viewport_info.xy_offset[0]);
    viewport.y = float(viewport_info.xy_offset[1]);
    viewport.width = float(viewport_info.xy_extent[0]);
    viewport.height = float(viewport_info.xy_extent[1]);
  } else {
    // Vulkan viewport width must be greater than 0.0f, but the Xenia  viewport
    // may be empty for various reasons - set the viewport to outside the
    // framebuffer.
    viewport.x = -1.0f;
    viewport.y = -1.0f;
    viewport.width = 1.0f;
    viewport.height = 1.0f;
  }
  viewport.minDepth = viewport_info.z_min;
  viewport.maxDepth = viewport_info.z_max;
  SetViewport(viewport);

  // Scissor.
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  // Scale the scissor to match the render target resolution scale
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;
  VkRect2D scissor_rect;
  scissor_rect.offset.x = int32_t(scissor.offset[0]);
  scissor_rect.offset.y = int32_t(scissor.offset[1]);
  scissor_rect.extent.width = scissor.extent[0];
  scissor_rect.extent.height = scissor.extent[1];
  SetScissor(scissor_rect);

  if (render_target_cache_->GetPath() ==
      RenderTargetCache::Path::kHostRenderTargets) {
    // Depth bias.
    float depth_bias_constant_factor, depth_bias_slope_factor;
    draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal,
                                             depth_bias_slope_factor,
                                             depth_bias_constant_factor);
    depth_bias_constant_factor *=
        regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
                xenos::DepthRenderTargetFormat::kD24S8
            ? draw_util::kD3D10PolygonOffsetFactorUnorm24
            : draw_util::kD3D10PolygonOffsetFactorFloat24;
    // With non-square resolution scaling, make sure the worst-case impact is
    // reverted (slope only along the scaled axis), thus max. More bias is
    // better than less bias, because less bias means Z fighting with the
    // background is more likely.
    depth_bias_slope_factor *=
        xenos::kPolygonOffsetScaleSubpixelUnit *
        float(std::max(render_target_cache_->draw_resolution_scale_x(),
                       render_target_cache_->draw_resolution_scale_y()));
    // std::memcmp instead of != so in case of NaN, every draw won't be
    // invalidating it.
    dynamic_depth_bias_update_needed_ |=
        std::memcmp(&dynamic_depth_bias_constant_factor_,
                    &depth_bias_constant_factor, sizeof(float)) != 0;
    dynamic_depth_bias_update_needed_ |=
        std::memcmp(&dynamic_depth_bias_slope_factor_, &depth_bias_slope_factor,
                    sizeof(float)) != 0;
    if (dynamic_depth_bias_update_needed_) {
      dynamic_depth_bias_constant_factor_ = depth_bias_constant_factor;
      dynamic_depth_bias_slope_factor_ = depth_bias_slope_factor;
      deferred_command_buffer_.CmdVkSetDepthBias(
          dynamic_depth_bias_constant_factor_, 0.0f,
          dynamic_depth_bias_slope_factor_);
      dynamic_depth_bias_update_needed_ = false;
    }

    // Blend constants.
    float blend_constants[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    dynamic_blend_constants_update_needed_ |=
        std::memcmp(dynamic_blend_constants_, blend_constants,
                    sizeof(float) * 4) != 0;
    if (dynamic_blend_constants_update_needed_) {
      std::memcpy(dynamic_blend_constants_, blend_constants, sizeof(float) * 4);
      deferred_command_buffer_.CmdVkSetBlendConstants(dynamic_blend_constants_);
      dynamic_blend_constants_update_needed_ = false;
    }

    // Stencil masks and references.
    // Due to pretty complex conditions involving registers not directly related
    // to stencil (primitive type, culling), changing the values only when
    // stencil is actually needed. However, due to the way dynamic state needs
    // to be set in Vulkan, which doesn't take into account whether the state
    // actually has effect on drawing, and because the masks and the references
    // are always dynamic in Xenia guest pipelines, they must be set in the
    // command buffer before any draw.
    if (normalized_depth_control.stencil_enable) {
      Register stencil_ref_mask_front_reg, stencil_ref_mask_back_reg;
      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        if (GetVulkanDevice()->properties().separateStencilMaskRef) {
          stencil_ref_mask_front_reg = XE_GPU_REG_RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
        } else {
          // Choose the back face values only if drawing only back faces.
          stencil_ref_mask_front_reg =
              regs.Get<reg::PA_SU_SC_MODE_CNTL>().cull_front
                  ? XE_GPU_REG_RB_STENCILREFMASK_BF
                  : XE_GPU_REG_RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = stencil_ref_mask_front_reg;
        }
      } else {
        stencil_ref_mask_front_reg = XE_GPU_REG_RB_STENCILREFMASK;
        stencil_ref_mask_back_reg = XE_GPU_REG_RB_STENCILREFMASK;
      }
      auto stencil_ref_mask_front =
          regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_front_reg);
      auto stencil_ref_mask_back =
          regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_back_reg);
      // Compare mask.
      dynamic_stencil_compare_mask_front_update_needed_ |=
          dynamic_stencil_compare_mask_front_ !=
          stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_front_ = stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_back_update_needed_ |=
          dynamic_stencil_compare_mask_back_ !=
          stencil_ref_mask_back.stencilmask;
      dynamic_stencil_compare_mask_back_ = stencil_ref_mask_back.stencilmask;
      // Write mask.
      dynamic_stencil_write_mask_front_update_needed_ |=
          dynamic_stencil_write_mask_front_ !=
          stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_front_ =
          stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_back_update_needed_ |=
          dynamic_stencil_write_mask_back_ !=
          stencil_ref_mask_back.stencilwritemask;
      dynamic_stencil_write_mask_back_ = stencil_ref_mask_back.stencilwritemask;
      // Reference.
      dynamic_stencil_reference_front_update_needed_ |=
          dynamic_stencil_reference_front_ != stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_front_ = stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_back_update_needed_ |=
          dynamic_stencil_reference_back_ != stencil_ref_mask_back.stencilref;
      dynamic_stencil_reference_back_ = stencil_ref_mask_back.stencilref;
    }
    // Using VK_STENCIL_FACE_FRONT_AND_BACK for higher safety when running on
    // the Vulkan portability subset without separateStencilMaskRef.
    if (dynamic_stencil_compare_mask_front_update_needed_ ||
        dynamic_stencil_compare_mask_back_update_needed_) {
      if (dynamic_stencil_compare_mask_front_ ==
          dynamic_stencil_compare_mask_back_) {
        deferred_command_buffer_.CmdVkSetStencilCompareMask(
            VK_STENCIL_FACE_FRONT_AND_BACK,
            dynamic_stencil_compare_mask_front_);
      } else {
        if (dynamic_stencil_compare_mask_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilCompareMask(
              VK_STENCIL_FACE_FRONT_BIT, dynamic_stencil_compare_mask_front_);
        }
        if (dynamic_stencil_compare_mask_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilCompareMask(
              VK_STENCIL_FACE_BACK_BIT, dynamic_stencil_compare_mask_back_);
        }
      }
      dynamic_stencil_compare_mask_front_update_needed_ = false;
      dynamic_stencil_compare_mask_back_update_needed_ = false;
    }
    if (dynamic_stencil_write_mask_front_update_needed_ ||
        dynamic_stencil_write_mask_back_update_needed_) {
      if (dynamic_stencil_write_mask_front_ ==
          dynamic_stencil_write_mask_back_) {
        deferred_command_buffer_.CmdVkSetStencilWriteMask(
            VK_STENCIL_FACE_FRONT_AND_BACK, dynamic_stencil_write_mask_front_);
      } else {
        if (dynamic_stencil_write_mask_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilWriteMask(
              VK_STENCIL_FACE_FRONT_BIT, dynamic_stencil_write_mask_front_);
        }
        if (dynamic_stencil_write_mask_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilWriteMask(
              VK_STENCIL_FACE_BACK_BIT, dynamic_stencil_write_mask_back_);
        }
      }
      dynamic_stencil_write_mask_front_update_needed_ = false;
      dynamic_stencil_write_mask_back_update_needed_ = false;
    }
    if (dynamic_stencil_reference_front_update_needed_ ||
        dynamic_stencil_reference_back_update_needed_) {
      if (dynamic_stencil_reference_front_ == dynamic_stencil_reference_back_) {
        deferred_command_buffer_.CmdVkSetStencilReference(
            VK_STENCIL_FACE_FRONT_AND_BACK, dynamic_stencil_reference_front_);
      } else {
        if (dynamic_stencil_reference_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilReference(
              VK_STENCIL_FACE_FRONT_BIT, dynamic_stencil_reference_front_);
        }
        if (dynamic_stencil_reference_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilReference(
              VK_STENCIL_FACE_BACK_BIT, dynamic_stencil_reference_back_);
        }
      }
      dynamic_stencil_reference_front_update_needed_ = false;
      dynamic_stencil_reference_back_update_needed_ = false;
    }
  }

  // TODO(Triang3l): VK_EXT_extended_dynamic_state and
  // VK_EXT_extended_dynamic_state2.
}

void VulkanCommandProcessor::UpdateSystemConstantValues(
    bool primitive_polygonal,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool shader_32bit_index_dma, const draw_util::ViewportInfo& viewport_info,
    uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_stencilrefmask = regs.Get<reg::RB_STENCILREFMASK>();
  auto rb_stencilrefmask_bf =
      regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  auto vgt_indx_offset = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);

  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  // Get the color info register values for each render target. Also, for FSI,
  // exclude components that don't exist in the format from the write mask.
  // Don't exclude fully overlapping render targets, however - two render
  // targets with the same base address are used in the lighting pass of
  // 4D5307E6, for example, with the needed one picked with dynamic control
  // flow.
  reg::RB_COLOR_INFO color_infos[xenos::kMaxColorRenderTargets];
  float rt_clamp[4][4];
  // Two UINT32_MAX if no components actually existing in the RT are written.
  uint32_t rt_keep_masks[4][2];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
    color_infos[i] = color_info;
    if (edram_fragment_shader_interlock) {
      RenderTargetCache::GetPSIColorFormatInfo(
          color_info.color_format, (normalized_color_mask >> (i * 4)) & 0b1111,
          rt_clamp[i][0], rt_clamp[i][1], rt_clamp[i][2], rt_clamp[i][3],
          rt_keep_masks[i][0], rt_keep_masks[i][1]);
    }
  }

  // Disable depth and stencil if it aliases a color render target (for
  // instance, during the XBLA logo in 58410954, though depth writing is already
  // disabled there).
  bool depth_stencil_enabled = normalized_depth_control.stencil_enable ||
                               normalized_depth_control.z_enable;
  if (edram_fragment_shader_interlock && depth_stencil_enabled) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (rb_depth_info.depth_base == color_infos[i].color_base &&
          (rt_keep_masks[i][0] != UINT32_MAX ||
           rt_keep_masks[i][1] != UINT32_MAX)) {
        depth_stencil_enabled = false;
        break;
      }
    }
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  // Vertex index shader loading.
  if (shader_32bit_index_dma) {
    flags |= SpirvShaderTranslator::kSysFlag_VertexIndexLoad;
  }
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= SpirvShaderTranslator ::
          kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
  }
  // W0 division control.
  // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
  // 8: VTX_XY_FMT = true: the incoming XY have already been multiplied by 1/W0.
  //               = false: multiply the X, Y coordinates by 1/W0.
  // 9: VTX_Z_FMT = true: the incoming Z has already been multiplied by 1/W0.
  //              = false: multiply the Z coordinate by 1/W0.
  // 10: VTX_W0_FMT = true: the incoming W0 is not 1/W0. Perform the reciprocal
  //                        to get 1/W0.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  }
  // Whether the primitive is polygonal, and gl_FrontFacing matters.
  if (primitive_polygonal) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  // Primitive type.
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }
  // MSAA sample count.
  flags |= uint32_t(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }
  // Alpha test.
  xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function)
           << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma writing.
  // TODO(Triang3l): Gamma as sRGB check.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (color_infos[i].color_format ==
        xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    }
  }
  if (edram_fragment_shader_interlock && depth_stencil_enabled) {
    flags |= SpirvShaderTranslator::kSysFlag_FSIDepthStencil;
    if (normalized_depth_control.z_enable) {
      flags |= uint32_t(normalized_depth_control.zfunc)
               << SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess_Shift;
      if (normalized_depth_control.z_write_enable) {
        flags |= SpirvShaderTranslator::kSysFlag_FSIDepthWrite;
      }
    } else {
      // In case stencil is used without depth testing - always pass, and
      // don't modify the stored depth.
      flags |= SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess |
               SpirvShaderTranslator::kSysFlag_FSIDepthPassIfEqual |
               SpirvShaderTranslator::kSysFlag_FSIDepthPassIfGreater;
    }
    if (normalized_depth_control.stencil_enable) {
      flags |= SpirvShaderTranslator::kSysFlag_FSIStencilTest;
    }
    // Hint - if not applicable to the shader, will not have effect.
    if (alpha_test_function == xenos::CompareFunction::kAlways &&
        !rb_colorcontrol.alpha_to_mask_enable) {
      flags |= SpirvShaderTranslator::kSysFlag_FSIDepthStencilEarlyWrite;
    }
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  // Index buffer address for loading in the shaders.
  if (flags &
      (SpirvShaderTranslator::kSysFlag_VertexIndexLoad |
       SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)) {
    dirty |= system_constants_.vertex_index_load_address !=
             primitive_processing_result.guest_index_base;
    system_constants_.vertex_index_load_address =
        primitive_processing_result.guest_index_base;
  }

  // Index or tessellation edge factor buffer endianness.
  dirty |= system_constants_.vertex_index_endian !=
           primitive_processing_result.host_shader_index_endian;
  system_constants_.vertex_index_endian =
      primitive_processing_result.host_shader_index_endian;

  // Vertex index offset.
  dirty |= system_constants_.vertex_base_index != vgt_indx_offset;
  system_constants_.vertex_base_index = vgt_indx_offset;

  // Conversion to host normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // User clip planes, for vertex and domain shaders. Ported from XenDroid;
  // gated so that with the cvar off nothing is written and the constant buffer
  // contents are unchanged. See docs/HALO3_VISTA_UPSIDE_DOWN.md.
  if (cvars::vulkan_user_clip_planes) {
    auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
    if (!pa_cl_clip_cntl.clip_disable && pa_cl_clip_cntl.ucp_ena) {
      uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
      uint32_t user_clip_plane_index;
      uint32_t written = 0;
      while (xe::bit_scan_forward(user_clip_planes_remaining,
                                  &user_clip_plane_index)) {
        user_clip_planes_remaining =
            xe::clear_lowest_bit(user_clip_planes_remaining);
        if (user_clip_plane_index >= 6) {
          continue;
        }
        // The shader indexes the planes densely (0..count-1) in enable order,
        // so pack them rather than indexing by the register slot.
        float* write_ptr = system_constants_.user_clip_planes[written++];
        const void* user_clip_plane_regs =
            &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
        if (std::memcmp(write_ptr, user_clip_plane_regs, 4 * sizeof(float))) {
          dirty = true;
          std::memcpy(write_ptr, user_clip_plane_regs, 4 * sizeof(float));
        }
      }
    }
  }

  // Point size.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min =
        float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max =
        float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x =
        float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y =
        float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min !=
             point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max !=
             point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] !=
             point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] !=
             point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader.
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / gamma.
  {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_signs_uint =
          system_constants_.texture_swizzled_signs[texture_index >> 2];
      uint32_t texture_signs_shift = 8 * (texture_index & 3);
      uint8_t texture_signs =
          texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
      uint32_t texture_signs_shifted = uint32_t(texture_signs)
                                       << texture_signs_shift;
      uint32_t texture_signs_mask = ((UINT32_C(1) << 8) - 1)
                                    << texture_signs_shift;
      dirty |=
          (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
      texture_signs_uint =
          (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
    }
  }

  // Texture host swizzle in the shader.
  if (!GetVulkanDevice()->properties().imageViewFormatSwizzle) {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_swizzles_uint =
          system_constants_.texture_swizzles[texture_index >> 1];
      uint32_t texture_swizzle_shift = 12 * (texture_index & 1);
      uint32_t texture_swizzle =
          texture_cache_->GetActiveTextureHostSwizzle(texture_index);
      uint32_t texture_swizzle_shifted = uint32_t(texture_swizzle)
                                         << texture_swizzle_shift;
      uint32_t texture_swizzle_mask = ((UINT32_C(1) << 12) - 1)
                                      << texture_swizzle_shift;
      dirty |= (texture_swizzles_uint & texture_swizzle_mask) !=
               texture_swizzle_shifted;
      texture_swizzles_uint = (texture_swizzles_uint & ~texture_swizzle_mask) |
                              texture_swizzle_shifted;
    }
  }

  // Alpha test.
  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;

  uint32_t edram_tile_dwords_scaled =
      xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples *
      (draw_resolution_scale_x * draw_resolution_scale_y);

  // EDRAM pitch for FSI render target writing.
  if (edram_fragment_shader_interlock) {
    // Align, then multiply by 32bpp tile size in dwords.
    uint32_t edram_32bpp_tile_pitch_dwords_scaled =
        ((rb_surface_info.surface_pitch *
          (rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1)) +
         (xenos::kEdramTileWidthSamples - 1)) /
        xenos::kEdramTileWidthSamples * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_32bpp_tile_pitch_dwords_scaled !=
             edram_32bpp_tile_pitch_dwords_scaled;
    system_constants_.edram_32bpp_tile_pitch_dwords_scaled =
        edram_32bpp_tile_pitch_dwords_scaled;
  }

  // Color exponent bias and FSI render target writing.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (render_target_cache_->GetPath() ==
            RenderTargetCache::Path::kHostRenderTargets &&
        (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 &&
             !render_target_cache_->IsFixedRG16TruncatedToMinus1To1() ||
         color_info.color_format ==
                 xenos::ColorRenderTargetFormat::k_16_16_16_16 &&
             !render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1())) {
      // Remap from -32...32 to -1...1 by dividing the output values by 32,
      // losing blending correctness, but getting the full range.
      color_exp_bias -= 5;
    }
    float color_exp_bias_scale;
    *reinterpret_cast<int32_t*>(&color_exp_bias_scale) =
        UINT32_C(0x3F800000) + (color_exp_bias << 23);
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
    if (edram_fragment_shader_interlock) {
      dirty |=
          system_constants_.edram_rt_keep_mask[i][0] != rt_keep_masks[i][0];
      system_constants_.edram_rt_keep_mask[i][0] = rt_keep_masks[i][0];
      dirty |=
          system_constants_.edram_rt_keep_mask[i][1] != rt_keep_masks[i][1];
      system_constants_.edram_rt_keep_mask[i][1] = rt_keep_masks[i][1];
      if (rt_keep_masks[i][0] != UINT32_MAX ||
          rt_keep_masks[i][1] != UINT32_MAX) {
        uint32_t rt_base_dwords_scaled =
            color_info.color_base * edram_tile_dwords_scaled;
        dirty |= system_constants_.edram_rt_base_dwords_scaled[i] !=
                 rt_base_dwords_scaled;
        system_constants_.edram_rt_base_dwords_scaled[i] =
            rt_base_dwords_scaled;
        uint32_t format_flags =
            RenderTargetCache::AddPSIColorFormatFlags(color_info.color_format);
        dirty |= system_constants_.edram_rt_format_flags[i] != format_flags;
        system_constants_.edram_rt_format_flags[i] = format_flags;
        uint32_t blend_factors_ops =
            regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
        dirty |= system_constants_.edram_rt_blend_factors_ops[i] !=
                 blend_factors_ops;
        system_constants_.edram_rt_blend_factors_ops[i] = blend_factors_ops;
        // Can't do float comparisons here because NaNs would result in always
        // setting the dirty flag.
        dirty |= std::memcmp(system_constants_.edram_rt_clamp[i], rt_clamp[i],
                             4 * sizeof(float)) != 0;
        std::memcpy(system_constants_.edram_rt_clamp[i], rt_clamp[i],
                    4 * sizeof(float));
      }
    }
  }

  if (edram_fragment_shader_interlock) {
    uint32_t depth_base_dwords_scaled =
        rb_depth_info.depth_base * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_depth_base_dwords_scaled !=
             depth_base_dwords_scaled;
    system_constants_.edram_depth_base_dwords_scaled = depth_base_dwords_scaled;

    // For non-polygons, front polygon offset is used, and it's enabled if
    // POLY_OFFSET_PARA_ENABLED is set, for polygons, separate front and back
    // are used.
    float poly_offset_front_scale = 0.0f, poly_offset_front_offset = 0.0f;
    float poly_offset_back_scale = 0.0f, poly_offset_back_offset = 0.0f;
    if (primitive_polygonal) {
      if (pa_su_sc_mode_cntl.poly_offset_front_enable) {
        poly_offset_front_scale =
            regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset =
            regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      }
      if (pa_su_sc_mode_cntl.poly_offset_back_enable) {
        poly_offset_back_scale =
            regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
        poly_offset_back_offset =
            regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
      }
    } else {
      if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
        poly_offset_front_scale =
            regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset =
            regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
        poly_offset_back_scale = poly_offset_front_scale;
        poly_offset_back_offset = poly_offset_front_offset;
      }
    }
    // With non-square resolution scaling, make sure the worst-case impact is
    // reverted (slope only along the scaled axis), thus max. More bias is
    // better than less bias, because less bias means Z fighting with the
    // background is more likely.
    float poly_offset_scale_factor =
        xenos::kPolygonOffsetScaleSubpixelUnit *
        std::max(draw_resolution_scale_x, draw_resolution_scale_y);
    poly_offset_front_scale *= poly_offset_scale_factor;
    poly_offset_back_scale *= poly_offset_scale_factor;
    dirty |= system_constants_.edram_poly_offset_front_scale !=
             poly_offset_front_scale;
    system_constants_.edram_poly_offset_front_scale = poly_offset_front_scale;
    dirty |= system_constants_.edram_poly_offset_front_offset !=
             poly_offset_front_offset;
    system_constants_.edram_poly_offset_front_offset = poly_offset_front_offset;
    dirty |= system_constants_.edram_poly_offset_back_scale !=
             poly_offset_back_scale;
    system_constants_.edram_poly_offset_back_scale = poly_offset_back_scale;
    dirty |= system_constants_.edram_poly_offset_back_offset !=
             poly_offset_back_offset;
    system_constants_.edram_poly_offset_back_offset = poly_offset_back_offset;

    if (depth_stencil_enabled && normalized_depth_control.stencil_enable) {
      uint32_t stencil_front_reference_masks =
          rb_stencilrefmask.value & 0xFFFFFF;
      dirty |= system_constants_.edram_stencil_front_reference_masks !=
               stencil_front_reference_masks;
      system_constants_.edram_stencil_front_reference_masks =
          stencil_front_reference_masks;
      uint32_t stencil_func_ops =
          (normalized_depth_control.value >> 8) & ((1 << 12) - 1);
      dirty |=
          system_constants_.edram_stencil_front_func_ops != stencil_func_ops;
      system_constants_.edram_stencil_front_func_ops = stencil_func_ops;

      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        uint32_t stencil_back_reference_masks =
            rb_stencilrefmask_bf.value & 0xFFFFFF;
        dirty |= system_constants_.edram_stencil_back_reference_masks !=
                 stencil_back_reference_masks;
        system_constants_.edram_stencil_back_reference_masks =
            stencil_back_reference_masks;
        uint32_t stencil_func_ops_bf =
            (normalized_depth_control.value >> 20) & ((1 << 12) - 1);
        dirty |= system_constants_.edram_stencil_back_func_ops !=
                 stencil_func_ops_bf;
        system_constants_.edram_stencil_back_func_ops = stencil_func_ops_bf;
      } else {
        dirty |= std::memcmp(system_constants_.edram_stencil_back,
                             system_constants_.edram_stencil_front,
                             2 * sizeof(uint32_t)) != 0;
        std::memcpy(system_constants_.edram_stencil_back,
                    system_constants_.edram_stencil_front,
                    2 * sizeof(uint32_t));
      }
    }

    dirty |= system_constants_.edram_blend_constant[0] !=
             regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    system_constants_.edram_blend_constant[0] =
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    dirty |= system_constants_.edram_blend_constant[1] !=
             regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    system_constants_.edram_blend_constant[1] =
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    dirty |= system_constants_.edram_blend_constant[2] !=
             regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    system_constants_.edram_blend_constant[2] =
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    dirty |= system_constants_.edram_blend_constant[3] !=
             regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
    system_constants_.edram_blend_constant[3] =
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  }

  if (dirty) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
  }
}

bool VulkanCommandProcessor::UpdateBindings(const VulkanShader* vertex_shader,
                                            const VulkanShader* pixel_shader) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Invalidate constant buffers and descriptors for changed data.

  // Float constants.
  // These are the constant base addresses/ranges for shaders.
  // We have these hardcoded right now cause nothing seems to differ on the Xbox
  // 360 (however, OpenGL ES on Adreno 200 on Android has different ranges).
  assert_true(regs[XE_GPU_REG_SQ_VS_CONST] == 0x000FF000 ||
              regs[XE_GPU_REG_SQ_VS_CONST] == 0x00000000);
  assert_true(regs[XE_GPU_REG_SQ_PS_CONST] == 0x000FF100 ||
              regs[XE_GPU_REG_SQ_PS_CONST] == 0x00000000);
  // Check if the float constant layout is still the same and get the counts.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  uint32_t float_constant_count_vertex = float_constant_map_vertex.float_count;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] !=
        float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] =
          float_constant_map_vertex.float_bitmap[i];
      // If no float constants at all, any buffer can be reused for them, so not
      // invalidating.
      if (float_constant_count_vertex) {
        current_constant_buffers_up_to_date_ &=
            ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
      }
    }
  }
  uint32_t float_constant_count_pixel = 0;
  if (pixel_shader != nullptr) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    float_constant_count_pixel = float_constant_map_pixel.float_count;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] !=
          float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] =
            float_constant_map_pixel.float_bitmap[i];
        if (float_constant_count_pixel) {
          current_constant_buffers_up_to_date_ &= ~(
              UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0,
                sizeof(current_float_constant_map_pixel_));
  }

  // Write the new constant buffers.
  constexpr uint32_t kAllConstantBuffersMask =
      (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferCount) - 1;
  assert_zero(current_constant_buffers_up_to_date_ & ~kAllConstantBuffersMask);
  if ((current_constant_buffers_up_to_date_ & kAllConstantBuffersMask) !=
      kAllConstantBuffersMask) {
    current_graphics_descriptor_set_values_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants);
    size_t uniform_buffer_alignment =
        size_t(vulkan_device->properties().minUniformBufferOffsetAlignment);
    // System constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferSystem];
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, sizeof(SpirvShaderTranslator::SystemConstants),
          uniform_buffer_alignment, buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = sizeof(SpirvShaderTranslator::SystemConstants);
      std::memcpy(mapping, &system_constants_,
                  sizeof(SpirvShaderTranslator::SystemConstants));
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem;
    }
    // Vertex shader float constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferFloatVertex];
      // Even if the shader doesn't need any float constants, a valid binding
      // must still be provided (the pipeline layout always has float constants,
      // for both the vertex shader and the pixel shader), so if the first draw
      // in the frame doesn't have float constants at all, still allocate a
      // dummy buffer.
      size_t float_constants_size =
          sizeof(float) * 4 *
          std::max(float_constant_count_vertex, UINT32_C(1));
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, float_constants_size, uniform_buffer_alignment,
          buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(float_constants_size);
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t float_constant_map_entry =
            current_float_constant_map_vertex_[i];
        uint32_t float_constant_index;
        while (xe::bit_scan_forward(float_constant_map_entry,
                                    &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(mapping,
                      &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) +
                            (float_constant_index << 2)],
                      sizeof(float) * 4);
          mapping += sizeof(float) * 4;
        }
      }
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex;
    }
    // Pixel shader float constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferFloatPixel];
      size_t float_constants_size =
          sizeof(float) * 4 * std::max(float_constant_count_pixel, UINT32_C(1));
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, float_constants_size, uniform_buffer_alignment,
          buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(float_constants_size);
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t float_constant_map_entry =
            current_float_constant_map_pixel_[i];
        uint32_t float_constant_index;
        while (xe::bit_scan_forward(float_constant_map_entry,
                                    &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(mapping,
                      &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) +
                            (float_constant_index << 2)],
                      sizeof(float) * 4);
          mapping += sizeof(float) * 4;
        }
      }
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel;
    }
    // Bool and loop constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferBoolLoop];
      constexpr size_t kBoolLoopConstantsSize = sizeof(uint32_t) * (8 + 32);
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, kBoolLoopConstantsSize, uniform_buffer_alignment,
          buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(kBoolLoopConstantsSize);
      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                  kBoolLoopConstantsSize);
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop;
    }
    // Fetch constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferFetch];
      constexpr size_t kFetchConstantsSize = sizeof(uint32_t) * 6 * 32;
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, kFetchConstantsSize, uniform_buffer_alignment,
          buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(kFetchConstantsSize);
      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                  kFetchConstantsSize);
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch;
    }
  }

  // Textures and samplers.
  const std::vector<VulkanShader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  const std::vector<VulkanShader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  uint32_t sampler_count_vertex = uint32_t(samplers_vertex.size());
  uint32_t texture_count_vertex = uint32_t(textures_vertex.size());
  const std::vector<VulkanShader::SamplerBinding>* samplers_pixel;
  const std::vector<VulkanShader::TextureBinding>* textures_pixel;
  uint32_t sampler_count_pixel, texture_count_pixel;
  if (pixel_shader) {
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    sampler_count_pixel = uint32_t(samplers_pixel->size());
    texture_count_pixel = uint32_t(textures_pixel->size());
  } else {
    samplers_pixel = nullptr;
    textures_pixel = nullptr;
    sampler_count_pixel = 0;
    texture_count_pixel = 0;
  }
  // TODO(Triang3l): Reuse texture and sampler bindings if not changed.
  current_graphics_descriptor_set_values_up_to_date_ &=
      ~((UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex) |
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel));

  // Make sure new descriptor sets are bound to the command buffer.

  current_graphics_descriptor_sets_bound_up_to_date_ &=
      current_graphics_descriptor_set_values_up_to_date_;

  // Fill the texture and sampler write image infos.

  bool write_vertex_textures =
      (texture_count_vertex || sampler_count_vertex) &&
      !(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex));
  bool write_pixel_textures =
      (texture_count_pixel || sampler_count_pixel) &&
      !(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel));
  descriptor_write_image_info_.clear();
  descriptor_write_image_info_.reserve(
      (write_vertex_textures ? texture_count_vertex + sampler_count_vertex
                             : 0) +
      (write_pixel_textures ? texture_count_pixel + sampler_count_pixel : 0));
  size_t vertex_texture_image_info_offset = descriptor_write_image_info_.size();
  if (write_vertex_textures && texture_count_vertex) {
    for (const VulkanShader::TextureBinding& texture_binding :
         textures_vertex) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.imageView =
          texture_cache_->GetActiveBindingOrNullImageView(
              texture_binding.fetch_constant, texture_binding.dimension,
              bool(texture_binding.is_signed));
      descriptor_image_info.imageLayout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
  }
  size_t vertex_sampler_image_info_offset = descriptor_write_image_info_.size();
  if (write_vertex_textures && sampler_count_vertex) {
    for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
             sampler_pair : current_samplers_vertex_) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.sampler = sampler_pair.second;
    }
  }
  size_t pixel_texture_image_info_offset = descriptor_write_image_info_.size();
  if (write_pixel_textures && texture_count_pixel) {
    for (const VulkanShader::TextureBinding& texture_binding :
         *textures_pixel) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.imageView =
          texture_cache_->GetActiveBindingOrNullImageView(
              texture_binding.fetch_constant, texture_binding.dimension,
              bool(texture_binding.is_signed));
      descriptor_image_info.imageLayout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
  }
  size_t pixel_sampler_image_info_offset = descriptor_write_image_info_.size();
  if (write_pixel_textures && sampler_count_pixel) {
    for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
             sampler_pair : current_samplers_pixel_) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.sampler = sampler_pair.second;
    }
  }

  // Write the new descriptor sets.

  // Consecutive bindings updated via a single VkWriteDescriptorSet must have
  // identical stage flags, but for the constants they vary. Plus vertex and
  // pixel texture images and samplers.
  std::array<VkWriteDescriptorSet,
             SpirvShaderTranslator::kConstantBufferCount + 2 * 2>
      write_descriptor_sets;
  uint32_t write_descriptor_set_count = 0;
  uint32_t write_descriptor_set_bits = 0;
  assert_not_zero(
      current_graphics_descriptor_set_values_up_to_date_ &
      (UINT32_C(1)
       << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram));
  // Constant buffers.
  if (!(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants))) {
    VkDescriptorSet constants_descriptor_set;
    if (!constants_transient_descriptors_free_.empty()) {
      constants_descriptor_set = constants_transient_descriptors_free_.back();
      constants_transient_descriptors_free_.pop_back();
    } else {
      VkDescriptorPoolSize constants_descriptor_count;
      constants_descriptor_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      constants_descriptor_count.descriptorCount =
          SpirvShaderTranslator::kConstantBufferCount;
      constants_descriptor_set =
          transient_descriptor_allocator_uniform_buffer_.Allocate(
              descriptor_set_layout_constants_, &constants_descriptor_count, 1);
      if (constants_descriptor_set == VK_NULL_HANDLE) {
        return false;
      }
    }
    constants_transient_descriptors_used_.emplace_back(
        frame_current_, constants_descriptor_set);
    // Consecutive bindings updated via a single VkWriteDescriptorSet must have
    // identical stage flags, but for the constants they vary.
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
      VkWriteDescriptorSet& write_constants =
          write_descriptor_sets[write_descriptor_set_count++];
      write_constants.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_constants.pNext = nullptr;
      write_constants.dstSet = constants_descriptor_set;
      write_constants.dstBinding = i;
      write_constants.dstArrayElement = 0;
      write_constants.descriptorCount = 1;
      write_constants.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      write_constants.pImageInfo = nullptr;
      write_constants.pBufferInfo = &current_constant_buffer_infos_[i];
      write_constants.pTexelBufferView = nullptr;
    }
    write_descriptor_set_bits |=
        UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants;
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetConstants] =
            constants_descriptor_set;
  }
  // Vertex shader textures and samplers.
  if (write_vertex_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    uint32_t texture_descriptor_set_write_count = WriteTransientTextureBindings(
        true, texture_count_vertex, sampler_count_vertex,
        current_guest_graphics_pipeline_layout_
            ->descriptor_set_layout_textures_vertex_ref(),
        descriptor_write_image_info_.data() + vertex_texture_image_info_offset,
        descriptor_write_image_info_.data() + vertex_sampler_image_info_offset,
        write_textures);
    if (!texture_descriptor_set_write_count) {
      return false;
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |=
        UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex;
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
            write_textures[0].dstSet;
  }
  // Pixel shader textures and samplers.
  if (write_pixel_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    uint32_t texture_descriptor_set_write_count = WriteTransientTextureBindings(
        false, texture_count_pixel, sampler_count_pixel,
        current_guest_graphics_pipeline_layout_
            ->descriptor_set_layout_textures_pixel_ref(),
        descriptor_write_image_info_.data() + pixel_texture_image_info_offset,
        descriptor_write_image_info_.data() + pixel_sampler_image_info_offset,
        write_textures);
    if (!texture_descriptor_set_write_count) {
      return false;
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |=
        UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel;
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
            write_textures[0].dstSet;
  }
  // Write.
  if (write_descriptor_set_count) {
    dfn.vkUpdateDescriptorSets(device, write_descriptor_set_count,
                               write_descriptor_sets.data(), 0, nullptr);
  }
  // Only make valid if all descriptor sets have been allocated and written
  // successfully.
  current_graphics_descriptor_set_values_up_to_date_ |=
      write_descriptor_set_bits;

  // Bind the new descriptor sets.
  uint32_t descriptor_sets_needed =
      (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetCount) - 1;
  if (!texture_count_vertex && !sampler_count_vertex) {
    descriptor_sets_needed &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
  }
  if (!texture_count_pixel && !sampler_count_pixel) {
    descriptor_sets_needed &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
  }
  uint32_t descriptor_sets_remaining =
      descriptor_sets_needed &
      ~current_graphics_descriptor_sets_bound_up_to_date_;
  uint32_t descriptor_set_index;
  while (
      xe::bit_scan_forward(descriptor_sets_remaining, &descriptor_set_index)) {
    uint32_t descriptor_set_mask_tzcnt =
        xe::tzcnt(~(descriptor_sets_remaining |
                    ((UINT32_C(1) << descriptor_set_index) - 1)));
    // TODO(Triang3l): Bind to compute for memexport emulation without vertex
    // shader memory stores.
    deferred_command_buffer_.CmdVkBindDescriptorSets(
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        current_guest_graphics_pipeline_layout_->GetPipelineLayout(),
        descriptor_set_index, descriptor_set_mask_tzcnt - descriptor_set_index,
        current_graphics_descriptor_sets_ + descriptor_set_index, 0, nullptr);
    if (descriptor_set_mask_tzcnt >= 32) {
      break;
    }
    descriptor_sets_remaining &=
        ~((UINT32_C(1) << descriptor_set_mask_tzcnt) - 1);
  }
  current_graphics_descriptor_sets_bound_up_to_date_ |= descriptor_sets_needed;

  return true;
}

uint8_t* VulkanCommandProcessor::WriteTransientUniformBufferBinding(
    size_t size, SingleTransientDescriptorLayout transient_descriptor_layout,
    VkDescriptorBufferInfo& descriptor_buffer_info_out,
    VkWriteDescriptorSet& write_descriptor_set_out) {
  assert_true(frame_open_);
  VkDescriptorSet descriptor_set =
      AllocateSingleTransientDescriptor(transient_descriptor_layout);
  if (descriptor_set == VK_NULL_HANDLE) {
    return nullptr;
  }
  uint8_t* mapping = uniform_buffer_pool_->Request(
      frame_current_, size,
      size_t(GetVulkanDevice()->properties().minUniformBufferOffsetAlignment),
      descriptor_buffer_info_out.buffer, descriptor_buffer_info_out.offset);
  if (!mapping) {
    return nullptr;
  }
  descriptor_buffer_info_out.range = VkDeviceSize(size);
  write_descriptor_set_out.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write_descriptor_set_out.pNext = nullptr;
  write_descriptor_set_out.dstSet = descriptor_set;
  write_descriptor_set_out.dstBinding = 0;
  write_descriptor_set_out.dstArrayElement = 0;
  write_descriptor_set_out.descriptorCount = 1;
  write_descriptor_set_out.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  write_descriptor_set_out.pImageInfo = nullptr;
  write_descriptor_set_out.pBufferInfo = &descriptor_buffer_info_out;
  write_descriptor_set_out.pTexelBufferView = nullptr;
  return mapping;
}

uint8_t* VulkanCommandProcessor::WriteTransientUniformBufferBinding(
    size_t size, SingleTransientDescriptorLayout transient_descriptor_layout,
    VkDescriptorSet& descriptor_set_out) {
  VkDescriptorBufferInfo write_descriptor_buffer_info;
  VkWriteDescriptorSet write_descriptor_set;
  uint8_t* mapping = WriteTransientUniformBufferBinding(
      size, transient_descriptor_layout, write_descriptor_buffer_info,
      write_descriptor_set);
  if (!mapping) {
    return nullptr;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  dfn.vkUpdateDescriptorSets(device, 1, &write_descriptor_set, 0, nullptr);
  descriptor_set_out = write_descriptor_set.dstSet;
  return mapping;
}

uint32_t VulkanCommandProcessor::WriteTransientTextureBindings(
    bool is_vertex, uint32_t texture_count, uint32_t sampler_count,
    VkDescriptorSetLayout descriptor_set_layout,
    const VkDescriptorImageInfo* texture_image_info,
    const VkDescriptorImageInfo* sampler_image_info,
    VkWriteDescriptorSet* descriptor_set_writes_out) {
  assert_true(frame_open_);
  if (!texture_count && !sampler_count) {
    return 0;
  }
  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = texture_count;
  texture_descriptor_set_layout_key.sampler_count = sampler_count;
  texture_descriptor_set_layout_key.is_vertex = uint32_t(is_vertex);
  VkDescriptorSet texture_descriptor_set;
  auto textures_free_it = texture_transient_descriptor_sets_free_.find(
      texture_descriptor_set_layout_key);
  if (textures_free_it != texture_transient_descriptor_sets_free_.end() &&
      !textures_free_it->second.empty()) {
    texture_descriptor_set = textures_free_it->second.back();
    textures_free_it->second.pop_back();
  } else {
    std::array<VkDescriptorPoolSize, 2> texture_descriptor_counts;
    uint32_t texture_descriptor_counts_count = 0;
    if (texture_count) {
      VkDescriptorPoolSize& texture_descriptor_count =
          texture_descriptor_counts[texture_descriptor_counts_count++];
      texture_descriptor_count.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      texture_descriptor_count.descriptorCount = texture_count;
    }
    if (sampler_count) {
      VkDescriptorPoolSize& texture_descriptor_count =
          texture_descriptor_counts[texture_descriptor_counts_count++];
      texture_descriptor_count.type = VK_DESCRIPTOR_TYPE_SAMPLER;
      texture_descriptor_count.descriptorCount = sampler_count;
    }
    assert_not_zero(texture_descriptor_counts_count);
    texture_descriptor_set = transient_descriptor_allocator_textures_.Allocate(
        descriptor_set_layout, texture_descriptor_counts.data(),
        texture_descriptor_counts_count);
    if (texture_descriptor_set == VK_NULL_HANDLE) {
      return 0;
    }
  }
  UsedTextureTransientDescriptorSet& used_texture_descriptor_set =
      texture_transient_descriptor_sets_used_.emplace_back();
  used_texture_descriptor_set.frame = frame_current_;
  used_texture_descriptor_set.layout = texture_descriptor_set_layout_key;
  used_texture_descriptor_set.set = texture_descriptor_set;
  uint32_t descriptor_set_write_count = 0;
  if (texture_count) {
    VkWriteDescriptorSet& descriptor_set_write =
        descriptor_set_writes_out[descriptor_set_write_count++];
    descriptor_set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write.pNext = nullptr;
    descriptor_set_write.dstSet = texture_descriptor_set;
    descriptor_set_write.dstBinding = 0;
    descriptor_set_write.dstArrayElement = 0;
    descriptor_set_write.descriptorCount = texture_count;
    descriptor_set_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_write.pImageInfo = texture_image_info;
    descriptor_set_write.pBufferInfo = nullptr;
    descriptor_set_write.pTexelBufferView = nullptr;
  }
  if (sampler_count) {
    VkWriteDescriptorSet& descriptor_set_write =
        descriptor_set_writes_out[descriptor_set_write_count++];
    descriptor_set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write.pNext = nullptr;
    descriptor_set_write.dstSet = texture_descriptor_set;
    descriptor_set_write.dstBinding = texture_count;
    descriptor_set_write.dstArrayElement = 0;
    descriptor_set_write.descriptorCount = sampler_count;
    descriptor_set_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_write.pImageInfo = sampler_image_info;
    descriptor_set_write.pBufferInfo = nullptr;
    descriptor_set_write.pTexelBufferView = nullptr;
  }
  assert_not_zero(descriptor_set_write_count);
  return descriptor_set_write_count;
}

#define COMMAND_PROCESSOR VulkanCommandProcessor
#include "../pm4_command_processor_implement.h"
}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
