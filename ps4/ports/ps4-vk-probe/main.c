// Vulkan probe: Mesa's RADV running on the PS4 through GNM (port/mesa/ac_ps4_drm.c).
//
// Step 1: instance, physical device, logical device.
// Step 2: the GPU fills a host-visible buffer (vkCmdFillBuffer), fence wait, CPU check.
// Step 3: a spinning triangle (SPIR-V compiled on the console by ACO) rendered into two linear
//         images that are registered as VideoOut framebuffers and flipped at vsync for 5 s.
// Logs to /data/triaevum/vk-probe.log.

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <math.h>

#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>
#include <vulkan/vulkan_core.h>

#include "shaders/triangle_frag.h"
#include "shaders/triangle_vert.h"

void ac_ps4_trace(const char* fmt, ...);  // Mesa PS4 layer (port/mesa/ac_ps4_drm.c)
int sceSystemServiceLoadExec(const char* path, char* const argv[]);
int sceSysmoduleLoadModuleInternal(uint32_t id);

// RADV's loader entry point (linked statically, no Vulkan loader on the PS4).
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                   const char* name);

static int g_log = -1;

__attribute__((format(printf, 1, 2))) static void Log(const char* fmt, ...) {
  char line[512];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  if (n <= 0)
    return;
  if (n > (int)sizeof(line) - 1)
    n = sizeof(line) - 1;
  if (g_log >= 0) {
    write(g_log, line, (size_t)n);
    fsync(g_log);
  }
  sceKernelDebugOutText(0, line);
}

// Mesa's own diagnostics go to stderr: keep them in the log too.
static void RedirectStderr(void) {
  if (freopen("/data/triaevum/vk-probe-stderr.log", "w", stderr))
    setvbuf(stderr, NULL, _IONBF, 0);
}

#define LOAD(instance, name) PFN_##name name = (PFN_##name)vk_icdGetInstanceProcAddr(instance, #name)
#define CHECK(call)                                    \
  do {                                                 \
    VkResult r_ = (call);                              \
    Log("%s = %d\n", #call, (int)r_);                  \
    if (r_ != VK_SUCCESS)                              \
      goto done;                                       \
  } while (0)

static void Exit(void) {
  fflush(stderr);
  if (g_log >= 0)
    close(g_log);
  sceSystemServiceLoadExec("exit", NULL);
  _exit(0);
}

#define T_CHECK(call)                                  \
  do {                                                 \
    VkResult r_ = (call);                              \
    if (r_ != VK_SUCCESS) {                            \
      Log("%s = %d\n", #call, (int)r_);                \
      return -1;                                       \
    }                                                  \
  } while (0)

// Step 3: spinning triangle on the TV. Two linear R8G8B8A8 images are the VideoOut framebuffers
// (GPU address = CPU address on the PS4, so vkMapMemory gives the address to register).
static int Triangle(VkPhysicalDevice gpu, VkDevice device, VkQueue queue, uint32_t queue_family,
                    PFN_vkGetDeviceProcAddr gdpa, PFN_vkGetPhysicalDeviceMemoryProperties gpmp,
                    PFN_vkGetPhysicalDeviceFormatProperties gpfp) {
#define DL(name) PFN_##name name = (PFN_##name)gdpa(device, #name)
  DL(vkCreateImage);
  DL(vkGetImageMemoryRequirements);
  DL(vkAllocateMemory);
  DL(vkBindImageMemory);
  DL(vkMapMemory);
  DL(vkGetImageSubresourceLayout);
  DL(vkCreateImageView);
  DL(vkCreateRenderPass);
  DL(vkCreateFramebuffer);
  DL(vkCreateShaderModule);
  DL(vkCreatePipelineLayout);
  DL(vkCreateGraphicsPipelines);
  DL(vkCreateCommandPool);
  DL(vkAllocateCommandBuffers);
  DL(vkBeginCommandBuffer);
  DL(vkEndCommandBuffer);
  DL(vkResetCommandBuffer);
  DL(vkCmdBeginRenderPass);
  DL(vkCmdEndRenderPass);
  DL(vkCmdBindPipeline);
  DL(vkCmdPushConstants);
  DL(vkCmdSetViewport);
  DL(vkCmdSetScissor);
  DL(vkCmdDraw);
  DL(vkCreateFence);
  DL(vkResetFences);
  DL(vkWaitForFences);
  DL(vkQueueSubmit);
#undef DL
  enum { W = 1920, H = 1080 };
  const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

  VkFormatProperties fp;
  gpfp(gpu, format, &fp);
  Log("step 3: R8G8B8A8 linear features %#x\n", fp.linearTilingFeatures);
  if (!(fp.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
    return -1;

  // Video out.
  Log("sceUserServiceInitialize = %#x\n", sceUserServiceInitialize(NULL));
  const int video = sceVideoOutOpen(0xFF, 0, 0, NULL);
  Log("sceVideoOutOpen = %#x\n", video);
  if (video < 0)
    return -1;

  VkPhysicalDeviceMemoryProperties mem;
  gpmp(gpu, &mem);
  VkImage images[2];
  VkImageView views[2];
  VkFramebuffer framebuffers[2];
  void* addresses[2];
  uint32_t* pixels[2];
  uint32_t pitch = W;

  VkRenderPass pass;
  {
    VkAttachmentDescription att = {.format = format,
                                   .samples = VK_SAMPLE_COUNT_1_BIT,
                                   .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                   .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                   .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                   .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                   .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &ref};
    VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 1,
                                   .pAttachments = &att,
                                   .subpassCount = 1,
                                   .pSubpasses = &sub};
    T_CHECK(vkCreateRenderPass(device, &info, NULL, &pass));
  }

  for (int i = 0; i < 2; i++) {
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {W, H, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_LINEAR,
                              .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    T_CHECK(vkCreateImage(device, &info, NULL, &images[i]));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, images[i], &req);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t t = 0; t < mem.memoryTypeCount && type == UINT32_MAX; t++)
      if ((req.memoryTypeBits & (1u << t)) && (mem.memoryTypes[t].propertyFlags & want) == want)
        type = t;
    // VideoOut wants 2 MiB aligned framebuffers: over-allocate and bind at an aligned offset.
    const VkDeviceSize slack = 2 << 20;
    VkMemoryAllocateInfo ainfo = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = req.size + slack,
                                  .memoryTypeIndex = type};
    VkDeviceMemory memory;
    T_CHECK(vkAllocateMemory(device, &ainfo, NULL, &memory));
    uint8_t* base = NULL;
    T_CHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void**)&base));
    VkDeviceSize offset = ((uintptr_t)base + slack - 1) / slack * slack - (uintptr_t)base;
    offset = (offset + req.alignment - 1) / req.alignment * req.alignment;
    T_CHECK(vkBindImageMemory(device, images[i], memory, offset));
    VkImageSubresource sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device, images[i], &sr, &layout);
    addresses[i] = base + offset + layout.offset;
    pixels[i] = (uint32_t*)addresses[i];
    pitch = (uint32_t)(layout.rowPitch / 4);
    Log("framebuffer %d: memory type %u, size %llu, align %llu, at %p, row pitch %llu\n", i, type,
        (unsigned long long)req.size, (unsigned long long)req.alignment, addresses[i],
        (unsigned long long)layout.rowPitch);

    VkImageViewCreateInfo vinfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                   .image = images[i],
                                   .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                   .format = format,
                                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    T_CHECK(vkCreateImageView(device, &vinfo, NULL, &views[i]));
    VkFramebufferCreateInfo finfo = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                     .renderPass = pass,
                                     .attachmentCount = 1,
                                     .pAttachments = &views[i],
                                     .width = W,
                                     .height = H,
                                     .layers = 1};
    T_CHECK(vkCreateFramebuffer(device, &finfo, NULL, &framebuffers[i]));
  }

  OrbisVideoOutBufferAttribute attribute;
  sceVideoOutSetBufferAttribute(&attribute, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
                                ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0, W, H, pitch);
  const int registered = sceVideoOutRegisterBuffers(video, 0, addresses, 2, &attribute);
  Log("sceVideoOutRegisterBuffers = %#x\n", registered);
  if (registered < 0)
    return -1;
  OrbisKernelEqueue flip_queue;
  sceKernelCreateEqueue(&flip_queue, "flip");
  sceVideoOutAddFlipEvent(flip_queue, video, NULL);

  // Pipeline.
  VkShaderModule vs, fs;
  {
    VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = sizeof(triangle_vert),
                                     .pCode = triangle_vert};
    T_CHECK(vkCreateShaderModule(device, &info, NULL, &vs));
    info.codeSize = sizeof(triangle_frag);
    info.pCode = triangle_frag;
    T_CHECK(vkCreateShaderModule(device, &info, NULL, &fs));
  }
  VkPipelineLayout pipeline_layout;
  {
    VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
    VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &range};
    T_CHECK(vkCreatePipelineLayout(device, &info, NULL, &pipeline_layout));
  }
  VkPipeline pipeline;
  {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs,
         .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                         .stageCount = 2,
                                         .pStages = stages,
                                         .pVertexInputState = &vi,
                                         .pInputAssemblyState = &ia,
                                         .pViewportState = &vp,
                                         .pRasterizationState = &rs,
                                         .pMultisampleState = &ms,
                                         .pColorBlendState = &cb,
                                         .pDynamicState = &ds,
                                         .layout = pipeline_layout,
                                         .renderPass = pass};
    const uint64_t t0 = sceKernelGetProcessTime();
    T_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline));
    Log("pipeline compiled in %llu us\n", (unsigned long long)(sceKernelGetProcessTime() - t0));
  }

  VkCommandPool pool;
  VkCommandPoolCreateInfo pinfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = queue_family};
  T_CHECK(vkCreateCommandPool(device, &pinfo, NULL, &pool));
  VkCommandBuffer cmd;
  VkCommandBufferAllocateInfo cinfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
  T_CHECK(vkAllocateCommandBuffers(device, &cinfo, &cmd));
  VkFence fence;
  VkFenceCreateInfo finfo = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  T_CHECK(vkCreateFence(device, &finfo, NULL, &fence));

  const int frames = 300;
  uint64_t gpu_us = 0;
  for (int frame = 0; frame < frames; frame++) {
    const int b = frame & 1;
    const uint64_t t0 = sceKernelGetProcessTime();
    T_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    T_CHECK(vkBeginCommandBuffer(cmd, &begin));
    const float pulse = 0.5f + 0.5f * sinf(frame * 0.05f);
    VkClearValue clear = {.color = {{0.05f, 0.05f, 0.1f + 0.2f * pulse, 1.0f}}};
    VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = pass,
                                .framebuffer = framebuffers[b],
                                .renderArea = {{0, 0}, {W, H}},
                                .clearValueCount = 1,
                                .pClearValues = &clear};
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport viewport = {0, 0, W, H, 0, 1};
    VkRect2D scissor = {{0, 0}, {W, H}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const float push[2] = {frame * 0.03f, (float)H / W};
    vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    T_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &cmd};
    T_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    T_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 1000000000ull));
    T_CHECK(vkResetFences(device, 1, &fence));
    gpu_us += sceKernelGetProcessTime() - t0;

    const int flipped = sceVideoOutSubmitFlip(video, b, ORBIS_VIDEO_OUT_FLIP_VSYNC, frame);
    OrbisKernelEvent ev;
    int count = 0;
    sceKernelWaitEqueue(flip_queue, &ev, 1, &count, NULL);
    if (frame == 0 || frame % 60 == 59)
      Log("frame %d: flip %#x, centre %#x, corner %#x, avg render+wait %llu us\n", frame, flipped,
          pixels[b][(H / 2) * pitch + W / 2], pixels[b][0],
          (unsigned long long)(gpu_us / (frame + 1)));
  }
  Log("step 3 done\n");
  return 0;
}

int main(void) {
  sceKernelMkdir("/data/triaevum", 0777);  // best effort: logs (and later content) live here
  g_log = open("/data/triaevum/vk-probe.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
  RedirectStderr();
  setenv("RADV_DEBUG", "startup", 1);
  setenv("MESA_LOG_FILE", "/data/triaevum/mesa.log", 1);
  setenv("MESA_DEBUG", "1", 1);
  fprintf(stderr, "stderr works\n");
  ac_ps4_trace("probe: trace test\n");
  {
    // Are the data relocations applied? vk_physical_device_trampolines is a table of function
    // pointers filled by R_X86_64_RELATIVE relocations; slot 28 (offset 0xe0) =
    // vk_tramp_GetPhysicalDeviceProperties2, which RADV's WSI found NULL.
    extern void* vk_physical_device_trampolines[];
    int filled = 0;
    for (int i = 0; i < 72; i++)
      filled += vk_physical_device_trampolines[i] != NULL;
    Log("trampolines at %p: %d of 72 slots set; [0] %p [1] %p [28] %p [29] %p\n",
        (void*)vk_physical_device_trampolines, filled, vk_physical_device_trampolines[0],
        vk_physical_device_trampolines[1], vk_physical_device_trampolines[28],
        vk_physical_device_trampolines[29]);
    const uint64_t* page = (const uint64_t*)((uintptr_t)vk_physical_device_trampolines & ~0x3fffull);
    for (int i = 0; i < 64; i += 4)
      Log("  data+%#05x: %016llx %016llx %016llx %016llx\n", i * 8,
          (unsigned long long)page[i], (unsigned long long)page[i + 1],
          (unsigned long long)page[i + 2], (unsigned long long)page[i + 3]);
  }
  Log("Vulkan probe start\n");
  // System modules used by the runtime (see probe-gnm): load before any call into them.
  sceSysmoduleLoadModuleInternal(0x80000010);  // SystemService
  sceSysmoduleLoadModuleInternal(0x80000011);  // UserService
  sceSysmoduleLoadModuleInternal(0x80000022);  // VideoOut

  LOAD(NULL, vkCreateInstance);
  LOAD(NULL, vkEnumerateInstanceVersion);
  if (!vkCreateInstance) {
    Log("no vkCreateInstance\n");
    Exit();
  }
  uint32_t api = 0;
  if (vkEnumerateInstanceVersion)
    vkEnumerateInstanceVersion(&api);
  Log("instance API %u.%u.%u\n", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
      VK_API_VERSION_PATCH(api));

  VkInstance instance = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "TriAevum PS4 Vulkan probe",
                             .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&info, NULL, &instance));
  }
  {
    // RADV's WSI got NULL for vkGetPhysicalDeviceProperties2 from this lookup (02.08/02.09).
    extern PFN_vkVoidFunction vk_instance_get_proc_addr_unchecked(const void* instance,
                                                                  const char* name);
    extern PFN_vkVoidFunction vk_physical_device_dispatch_table_get(const void* table,
                                                                    const char* name);
    extern void* vk_physical_device_trampolines[];
    static const char* const names[] = {
        "vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR",
        "vkGetPhysicalDeviceMemoryProperties", "vkGetPhysicalDeviceQueueFamilyProperties",
        "vkGetPhysicalDeviceExternalSemaphoreProperties", "vkGetPhysicalDeviceProperties",
        "vkGetPhysicalDeviceFeatures2"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
      Log("lookup %s: unchecked %p, trampolines %p\n", names[i],
          (void*)vk_instance_get_proc_addr_unchecked(instance, names[i]),
          (void*)vk_physical_device_dispatch_table_get(vk_physical_device_trampolines, names[i]));
  }
  LOAD(instance, vkEnumeratePhysicalDevices);
  LOAD(instance, vkGetPhysicalDeviceProperties);
  LOAD(instance, vkGetPhysicalDeviceQueueFamilyProperties);
  LOAD(instance, vkGetPhysicalDeviceMemoryProperties);
  LOAD(instance, vkCreateDevice);
  LOAD(instance, vkGetDeviceProcAddr);

  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  {
    uint32_t count = 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, &gpu));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    Log("GPU: %s, API %u.%u.%u, driver %#x, device id %#x\n", props.deviceName,
        VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
        VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, props.deviceID);
  }

  uint32_t queue_family = 0;
  {
    VkQueueFamilyProperties families[8];
    uint32_t count = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
    for (uint32_t i = 0; i < count; i++)
      Log("queue family %u: flags %#x, %u queues\n", i, families[i].queueFlags,
          families[i].queueCount);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = queue_family,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &queue};
    CHECK(vkCreateDevice(gpu, &info, NULL, &device));
  }

#define DLOAD(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
  DLOAD(vkGetDeviceQueue);
  DLOAD(vkCreateBuffer);
  DLOAD(vkGetBufferMemoryRequirements);
  DLOAD(vkAllocateMemory);
  DLOAD(vkBindBufferMemory);
  DLOAD(vkMapMemory);
  DLOAD(vkCreateCommandPool);
  DLOAD(vkAllocateCommandBuffers);
  DLOAD(vkBeginCommandBuffer);
  DLOAD(vkCmdFillBuffer);
  DLOAD(vkEndCommandBuffer);
  DLOAD(vkCreateFence);
  DLOAD(vkQueueSubmit);
  DLOAD(vkWaitForFences);

  // Step 2: GPU fill of a host-visible, host-coherent buffer.
  {
    const VkDeviceSize size = 1 << 20;
    VkQueue queue;
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    VkBuffer buffer;
    VkBufferCreateInfo binfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                .size = size,
                                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateBuffer(device, &binfo, NULL, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
      Log("memory type %u: flags %#x heap %u\n", i, mem.memoryTypes[i].propertyFlags,
          mem.memoryTypes[i].heapIndex);
      const VkMemoryPropertyFlags want =
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      if (type == UINT32_MAX && (req.memoryTypeBits & (1u << i)) &&
          (mem.memoryTypes[i].propertyFlags & want) == want)
        type = i;
    }
    Log("using memory type %u\n", type);
    VkDeviceMemory memory;
    VkMemoryAllocateInfo ainfo = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = req.size,
                                  .memoryTypeIndex = type};
    CHECK(vkAllocateMemory(device, &ainfo, NULL, &memory));
    CHECK(vkBindBufferMemory(device, buffer, memory, 0));
    uint32_t* data = NULL;
    CHECK(vkMapMemory(device, memory, 0, size, 0, (void**)&data));
    memset(data, 0, size);
    Log("buffer mapped at %p\n", (void*)data);

    VkCommandPool pool;
    VkCommandPoolCreateInfo pinfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                     .queueFamilyIndex = queue_family};
    CHECK(vkCreateCommandPool(device, &pinfo, NULL, &pool));
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cinfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = pool,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(device, &cinfo, &cmd));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &begin));
    vkCmdFillBuffer(cmd, buffer, 0, size, 0x12345678);
    CHECK(vkEndCommandBuffer(cmd));

    VkFence fence;
    VkFenceCreateInfo finfo = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device, &finfo, NULL, &fence));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &cmd};
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
    Log("step 2: first %#x, last %#x (expect 0x12345678)\n", data[0], data[size / 4 - 1]);
  }

  // Step 3: triangle on the TV.
  {
    VkQueue queue;
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    LOAD(instance, vkGetPhysicalDeviceFormatProperties);
    Log("step 3 = %d\n", Triangle(gpu, device, queue, queue_family, vkGetDeviceProcAddr,
                                  vkGetPhysicalDeviceMemoryProperties,
                                  vkGetPhysicalDeviceFormatProperties));
  }

done:
  Log("Vulkan probe done\n");
  Exit();
  return 0;
}
