// Minimal standalone Vulkan compute probe: dispatches probe.spv against a
// fixed set of test inputs and prints sin/cos/sqrt/inversesqrt/trunc/floor/
// fract results as CSV to stdout. No app, no game, no Xenia - isolates
// exactly what this GPU's GLSL.std.450 intrinsics compute for known inputs,
// so it can be diffed against a Python-computed float32 reference.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fstream>
#include <cmath>

#define CHK(x) do { VkResult r = (x); if (r != VK_SUCCESS) { \
  fprintf(stderr, "FAIL %s: %d\n", #x, r); exit(1); } } while (0)

static std::vector<uint32_t> ReadFile(const char* path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
  size_t size = f.tellg();
  f.seekg(0);
  std::vector<uint32_t> buf(size / 4);
  f.read(reinterpret_cast<char*>(buf.data()), size);
  return buf;
}

int main(int argc, char** argv) {
  const char* spv_path = argc > 1 ? argv[1] : "probe.spv";

  // Test inputs: exact integers, near-integer boundary cases (the exact
  // failure mode hypothesized for Halo 3's trunc-into-exact-equality
  // branch chain), pi-multiples (for sin/cos range reduction), and the
  // REAL constants captured live from shader 9EA48FC2B26C325D's c220-c229.
  std::vector<float> inputs;
  for (int n = -10; n <= 10; ++n) inputs.push_back(float(n));
  float eps[] = {1e-7f, 1e-6f, 1e-5f, 1e-4f, -1e-7f, -1e-6f, -1e-5f, -1e-4f};
  for (int n = -5; n <= 5; ++n)
    for (float e : eps) inputs.push_back(float(n) + e);
  float pi = 3.14159265358979323846f;
  float pis[] = {0, pi / 2, pi, -pi / 2, -pi, 2 * pi, -2 * pi, 3 * pi,
                 10000.0f, 100.0f, 50.0f, 6.28318548f, 3.14159274f,
                 -3.14159274f};
  for (float v : pis) inputs.push_back(v);
  // Live-captured constants (c220..c229, all 4 components), from
  // docs/HALO3_MENU_INVESTIGATION.md / CONSTDUMP log.
  float captured[] = {
    0.142857149f, 0.25f, 7.62939453e-06f, 1.5f,           // c220
    4.f, 0.25f, 0.125f, 8.f,                               // c221
    3.f, 2.f, 1.f, 0.f,                                    // c222
    -24.f, -23.f, -21.f, -1.f,                             // c223
    6.28318548f, 10000.f, 0.5f, -3.14159274f,              // c224
    -7.f, -6.f, -4.f, 3.14159274f,                         // c225
    0.5f, 1.f, 4.f, 6.99999952f,                           // c226
    10.f, 9.f, 8.f, 7.f,                                   // c227
    -3.f, -11.f, -2.f, 0.f,                                // c228
    6.f, 5.f, 4.f, 3.f,                                    // c229
  };
  for (float v : captured) inputs.push_back(v);

  uint32_t count = uint32_t(inputs.size());
  fprintf(stderr, "test input count: %u\n", count);

  // --- Vulkan setup ---
  VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app_info.pApplicationName = "probe";
  app_info.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo inst_ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  inst_ci.pApplicationInfo = &app_info;
  VkInstance instance;
  CHK(vkCreateInstance(&inst_ci, nullptr, &instance));

  uint32_t gpu_count = 0;
  vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
  std::vector<VkPhysicalDevice> gpus(gpu_count);
  vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
  VkPhysicalDevice phys = gpus[0];
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(phys, &props);
  fprintf(stderr, "GPU: %s\n", props.deviceName);

  uint32_t qf_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qf_count);
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, qfs.data());
  uint32_t qf_index = UINT32_MAX;
  for (uint32_t i = 0; i < qf_count; ++i) {
    if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf_index = i; break; }
  }
  if (qf_index == UINT32_MAX) { fprintf(stderr, "no compute queue\n"); return 1; }

  float qprio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = qf_index;
  qci.queueCount = 1;
  qci.pQueuePriorities = &qprio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  VkDevice device;
  CHK(vkCreateDevice(phys, &dci, nullptr, &device));
  VkQueue queue;
  vkGetDeviceQueue(device, qf_index, 0, &queue);

  VkPhysicalDeviceMemoryProperties mem_props;
  vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
  auto FindMemType = [&](uint32_t bits, VkMemoryPropertyFlags want) -> uint32_t {
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) &&
          (mem_props.memoryTypes[i].propertyFlags & want) == want) {
        return i;
      }
    }
    fprintf(stderr, "no matching memory type\n");
    exit(1);
  };

  auto MakeBuffer = [&](VkDeviceSize size, VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CHK(vkCreateBuffer(device, &bci, nullptr, &buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHK(vkAllocateMemory(device, &mai, nullptr, &mem));
    CHK(vkBindBufferMemory(device, buf, mem, 0));
  };

  VkBuffer in_buf, out_buf;
  VkDeviceMemory in_mem, out_mem;
  MakeBuffer(count * sizeof(float), in_buf, in_mem);
  MakeBuffer(count * 4 * sizeof(float), out_buf, out_mem);

  void* mapped;
  CHK(vkMapMemory(device, in_mem, 0, count * sizeof(float), 0, &mapped));
  memcpy(mapped, inputs.data(), count * sizeof(float));
  vkUnmapMemory(device, in_mem);

  auto spv = ReadFile(spv_path);
  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = spv.size() * 4;
  smci.pCode = spv.data();
  VkShaderModule shader;
  CHK(vkCreateShaderModule(device, &smci, nullptr, &shader));

  VkDescriptorSetLayoutBinding bindings[2] = {};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[1] = bindings[0];
  bindings[1].binding = 1;
  VkDescriptorSetLayoutCreateInfo dslci{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dslci.bindingCount = 2;
  dslci.pBindings = bindings;
  VkDescriptorSetLayout dsl;
  CHK(vkCreateDescriptorSetLayout(device, &dslci, nullptr, &dsl));

  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &dsl;
  VkPipelineLayout pl;
  CHK(vkCreatePipelineLayout(device, &plci, nullptr, &pl));

  VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpci.stage.module = shader;
  cpci.stage.pName = "main";
  cpci.layout = pl;
  VkPipeline pipeline;
  CHK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr,
                               &pipeline));

  VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &dps;
  VkDescriptorPool dpool;
  CHK(vkCreateDescriptorPool(device, &dpci, nullptr, &dpool));

  VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = dpool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &dsl;
  VkDescriptorSet dset;
  CHK(vkAllocateDescriptorSets(device, &dsai, &dset));

  VkDescriptorBufferInfo in_info{in_buf, 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo out_info{out_buf, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet writes[2] = {};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = dset;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = &in_info;
  writes[1] = writes[0];
  writes[1].dstBinding = 1;
  writes[1].pBufferInfo = &out_info;
  vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

  VkCommandPoolCreateInfo cpci2{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpci2.queueFamilyIndex = qf_index;
  VkCommandPool cpool;
  CHK(vkCreateCommandPool(device, &cpci2, nullptr, &cpool));
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = cpool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  CHK(vkAllocateCommandBuffers(device, &cbai, &cmd));

  VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CHK(vkBeginCommandBuffer(cmd, &cbbi));
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset,
                          0, nullptr);
  vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
  CHK(vkEndCommandBuffer(cmd));

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  CHK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
  CHK(vkQueueWaitIdle(queue));

  void* out_mapped;
  CHK(vkMapMemory(device, out_mem, 0, count * 4 * sizeof(float), 0, &out_mapped));
  const float* out = static_cast<const float*>(out_mapped);
  printf("input,exp2,log2,nclamp_floor,convertftos\n");
  for (uint32_t i = 0; i < count; ++i) {
    printf("%.9g,%.9g,%.9g,%.9g,%.9g\n", inputs[i], out[i * 4 + 0],
           out[i * 4 + 1], out[i * 4 + 2], out[i * 4 + 3]);
  }
  vkUnmapMemory(device, out_mem);

  return 0;
}
