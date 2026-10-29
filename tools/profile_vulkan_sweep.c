/* Diagnostic proxy for SDL_VULKAN_LIBRARY, one device/owner thread only.
 * Build/usage and measurement limits: tools/action_editor/README.md.
 * Never linked into the game. Linux graphics/compute queue, 64-bit timestamps.
 * GPU query reads happen only after the harness's EXISTING device-idle wait.
 * Top/bottom timestamps include overlap/dependencies: not additive pass costs.
 * No game build or driver settings are modified. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <stdbool.h>
#include "action_editor/benchmark_probe.h"
enum { MAX_COMMANDS = 256, MAX_EVENTS = 512, MAX_SHADERS = 1024, MAX_RECORDS = 262144 };
typedef struct {
  const char *kind;
  uint64_t shader;
  uint32_t work;
} Event;
typedef struct {
  VkCommandBuffer cmd;
  VkQueryPool pool;
  unsigned count;
  int frame;
  bool submitted;
  uint64_t graphics, compute;
  Event events[MAX_EVENTS];
} Command;
typedef struct {
  uint64_t handle, hash;
} Mapping;
typedef struct {
  int frame;
  unsigned cmd, event;
  const char *kind;
  uint64_t shader, start, end;
  uint32_t work;
  uint64_t deviation;
} Record;
static Command commands[MAX_COMMANDS];
static unsigned command_count;
static Mapping shaders[MAX_SHADERS], pipelines[MAX_SHADERS];
static unsigned shader_count, pipeline_count;
static Record *records;
static unsigned record_count;
static int frame = kArRoomBenchmarkProbeClosed;
static VkDevice device;
static VkInstance instance;
static float period;
static uint32_t valid_bits;
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
#define PROC(name) static PFN_##name real_##name
PROC(vkEndCommandBuffer);
PROC(vkQueueSubmit);
PROC(vkGetCalibratedTimestampsEXT);
PROC(vkCreateDevice);
PROC(vkDestroyDevice);
PROC(vkBeginCommandBuffer);
PROC(vkCmdBindPipeline);
PROC(vkCreateShaderModule);
PROC(vkCreateGraphicsPipelines);
PROC(vkCreateComputePipelines);
PROC(vkCmdDraw);
PROC(vkCmdDrawIndexed);
PROC(vkCmdDispatch);
PROC(vkDeviceWaitIdle);
PROC(vkCmdCopyBuffer);
PROC(vkCmdCopyBufferToImage);
PROC(vkCmdBlitImage);
PROC(vkCreateQueryPool);
PROC(vkDestroyQueryPool);
PROC(vkCmdResetQueryPool);
PROC(vkCmdWriteTimestamp);
PROC(vkGetQueryPoolResults);
static uint64_t Now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
void ArVkProfileFrame(int value) {
  frame = value;
  /* The harness closes the interval after its final idle/collection, before
   * any capture readback. Discard only unsubmitted, pre-acquired buffers. */
  if (value == kArRoomBenchmarkProbeClosed)
    for (unsigned i = 0; i < command_count; ++i) {
      if (commands[i].submitted) {
        fprintf(stderr, "[vk-probe] close before collection\n");
        abort();
      }
      commands[i].count = 0;
    }
}
static void Fail(const char *message) {
  fprintf(stderr, "[vk-probe] %s\n", message);
  abort();
}
static uint64_t Hash(const void *data, size_t size) {
  const uint8_t *p = data;
  uint64_t h = UINT64_C(14695981039346656037);
  while (size--) {
    h ^= *p++;
    h *= UINT64_C(1099511628211);
  }
  return h;
}
static void Put(Mapping *m, unsigned *count, uint64_t handle, uint64_t hash) {
  for (unsigned i = 0; i < *count; ++i)
    if (m[i].handle == handle) {
      m[i].hash = hash;
      return;
    }
  if (*count == MAX_SHADERS) Fail("mapping capacity");
  m[(*count)++] = (Mapping){handle, hash};
}
static uint64_t Get(Mapping *m, unsigned count, uint64_t handle) {
  for (unsigned i = 0; i < count; ++i)
    if (m[i].handle == handle) return m[i].hash;
  return 0;
}
static Command *Find(VkCommandBuffer cmd) {
  for (unsigned i = 0; i < command_count; ++i)
    if (commands[i].cmd == cmd) return &commands[i];
  return NULL;
}
static Command *Start(VkCommandBuffer cmd, const char *kind, uint64_t shader, uint32_t work) {
  Command *c = Find(cmd);
  if (!c || frame == kArRoomBenchmarkProbeClosed) return NULL;
  if (c->count >= MAX_EVENTS) Fail("command event capacity");
  c->events[c->count] = (Event){kind, shader, work};
  real_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, c->pool, c->count * 2);
  return c;
}
static void End(Command *c) {
  if (c) {
    real_vkCmdWriteTimestamp(c->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->pool,
                             c->count * 2 + 1);
    ++c->count;
  }
}
static VkResult Begin(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *info) {
  VkResult r = real_vkBeginCommandBuffer(cmd, info);
  if (r != VK_SUCCESS) return r;
  if (frame == kArRoomBenchmarkProbeClosed) return r;
  Command *c = Find(cmd);
  if (!c) {
    if (command_count == MAX_COMMANDS) Fail("command capacity");
    c = &commands[command_count++];
    *c = (Command){.cmd = cmd};
    const VkQueryPoolCreateInfo q = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                     .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                     .queryCount = MAX_EVENTS * 2};
    if (real_vkCreateQueryPool(device, &q, NULL, &c->pool) != VK_SUCCESS)
      Fail("query creation failed");
  }
  if (c->count && c->submitted) Fail("command reused before harness idle collection");
  c->count = 0;
  c->frame = frame;
  c->graphics = c->compute = 0;
  real_vkCmdResetQueryPool(cmd, c->pool, 0, MAX_EVENTS * 2);
  c->events[0] = (Event){"command_span", 0, 0};
  c->count = 1;
  real_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, c->pool, 0);
  return r;
}
static VkResult EndCommand(VkCommandBuffer cmd) {
  Command *c = Find(cmd);
  if (c && c->count)
    real_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->pool, 1);
  return real_vkEndCommandBuffer(cmd);
}
static VkResult QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *info, VkFence fence) {
  const uint64_t start = Now();
  VkResult r = real_vkQueueSubmit(q, n, info, fence);
  const uint64_t end = Now();
  if (frame == kArRoomBenchmarkProbeClosed) return r;
  if (r == VK_SUCCESS)
    for (unsigned i = 0; i < n; ++i)
      for (unsigned j = 0; j < info[i].commandBufferCount; ++j) {
        Command *c = Find(info[i].pCommandBuffers[j]);
        if (c) {
          c->submitted = true;
          c->frame = frame; /* SDL may acquire this buffer in the preceding frame. */
        }
      }
  if (frame >= 0) {
    if (record_count >= MAX_RECORDS) Fail("record capacity");
    records[record_count++] = (Record){frame, 0, 0, "queue_submit", 0, start, end, n, 0};
  }
  return r;
}
static void Bind(VkCommandBuffer cmd, VkPipelineBindPoint bind, VkPipeline pipeline) {
  Command *c = Find(cmd);
  if (c) {
    uint64_t hash = Get(pipelines, pipeline_count, (uintptr_t)pipeline);
    if (bind == VK_PIPELINE_BIND_POINT_GRAPHICS)
      c->graphics = hash;
    else
      c->compute = hash;
  }
  real_vkCmdBindPipeline(cmd, bind, pipeline);
}
static void Draw(VkCommandBuffer cmd, uint32_t n, uint32_t instances, uint32_t first,
                 uint32_t firstInstance) {
  Command *c = Find(cmd);
  c = Start(cmd, "draw", c ? c->graphics : 0, n * instances);
  real_vkCmdDraw(cmd, n, instances, first, firstInstance);
  End(c);
}
static void DrawIndexed(VkCommandBuffer cmd, uint32_t n, uint32_t instances, uint32_t first,
                        int32_t vertex, uint32_t firstInstance) {
  Command *c = Find(cmd);
  c = Start(cmd, "indexed", c ? c->graphics : 0, n * instances);
  real_vkCmdDrawIndexed(cmd, n, instances, first, vertex, firstInstance);
  End(c);
}
static void Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) {
  Command *c = Find(cmd);
  c = Start(cmd, "compute", c ? c->compute : 0, x * y * z);
  real_vkCmdDispatch(cmd, x, y, z);
  End(c);
}
static void CopyBuffer(VkCommandBuffer cmd, VkBuffer a, VkBuffer b, uint32_t n,
                       const VkBufferCopy *regions) {
  Command *c = Start(cmd, "copy_buffer", 0, n);
  real_vkCmdCopyBuffer(cmd, a, b, n, regions);
  End(c);
}
static void CopyImage(VkCommandBuffer cmd, VkBuffer a, VkImage b, VkImageLayout layout, uint32_t n,
                      const VkBufferImageCopy *regions) {
  Command *c = Start(cmd, "upload_image", 0, n);
  real_vkCmdCopyBufferToImage(cmd, a, b, layout, n, regions);
  End(c);
}
static void Blit(VkCommandBuffer cmd, VkImage a, VkImageLayout al, VkImage b, VkImageLayout bl,
                 uint32_t n, const VkImageBlit *regions, VkFilter filter) {
  Command *c = Start(cmd, "blit", 0, n);
  real_vkCmdBlitImage(cmd, a, al, b, bl, n, regions, filter);
  End(c);
}
static VkResult Shader(VkDevice d, const VkShaderModuleCreateInfo *info,
                       const VkAllocationCallbacks *a, VkShaderModule *out) {
  VkResult r = real_vkCreateShaderModule(d, info, a, out);
  if (r == VK_SUCCESS)
    Put(shaders, &shader_count, (uintptr_t)*out, Hash(info->pCode, info->codeSize));
  return r;
}
static VkResult Graphics(VkDevice d, VkPipelineCache cache, uint32_t n,
                         const VkGraphicsPipelineCreateInfo *info, const VkAllocationCallbacks *a,
                         VkPipeline *out) {
  VkResult r = real_vkCreateGraphicsPipelines(d, cache, n, info, a, out);
  if (r == VK_SUCCESS)
    for (unsigned i = 0; i < n; ++i)
      for (unsigned j = 0; j < info[i].stageCount; ++j)
        if (info[i].pStages[j].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
          Put(pipelines, &pipeline_count, (uintptr_t)out[i],
              Get(shaders, shader_count, (uintptr_t)info[i].pStages[j].module));
  return r;
}
static VkResult Compute(VkDevice d, VkPipelineCache cache, uint32_t n,
                        const VkComputePipelineCreateInfo *info, const VkAllocationCallbacks *a,
                        VkPipeline *out) {
  VkResult r = real_vkCreateComputePipelines(d, cache, n, info, a, out);
  if (r == VK_SUCCESS)
    for (unsigned i = 0; i < n; ++i)
      Put(pipelines, &pipeline_count, (uintptr_t)out[i],
          Get(shaders, shader_count, (uintptr_t)info[i].stage.module));
  return r;
}
static VkResult Idle(VkDevice d) {
  uint64_t idle_start = Now();
  VkResult result = real_vkDeviceWaitIdle(d);
  uint64_t idle_end = Now();
  if (result != VK_SUCCESS) return result;
  if (frame == kArRoomBenchmarkProbeClosed) return result;
  if (frame >= 0) {
    if (record_count >= MAX_RECORDS) Fail("record capacity");
    records[record_count++] = (Record){frame, 0, 0, "device_idle", 0, idle_start, idle_end, 0, 0};
  }
  const VkCalibratedTimestampInfoEXT clocks[2] = {
      {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
       .timeDomain = VK_TIME_DOMAIN_DEVICE_EXT},
      {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
       .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT}};
  uint64_t calibration[2], deviation;
  if (real_vkGetCalibratedTimestampsEXT(d, 2, clocks, calibration, &deviation) != VK_SUCCESS)
    Fail("calibration failed");
  const double offset = (double)calibration[1] - (double)calibration[0] * period;
  for (unsigned j = 0; j < command_count; ++j) {
    Command *c = &commands[j];
    if (!c->count || !c->submitted) continue;
    uint64_t values[MAX_EVENTS * 4];
    VkResult r =
        real_vkGetQueryPoolResults(d, c->pool, 0, c->count * 2, sizeof(values), values, 16,
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r != VK_SUCCESS) Fail("query unavailable after existing idle wait");
    for (unsigned i = 0; i < c->count; ++i) {
      if (!values[i * 4 + 1] || !values[i * 4 + 3]) Fail("missing timestamp");
      if (c->frame >= 0) {
        if (record_count >= MAX_RECORDS) Fail("record capacity");
        records[record_count++] = (Record){c->frame,
                                           j,
                                           i,
                                           c->events[i].kind,
                                           c->events[i].shader,
                                           (uint64_t)(values[i * 4] * (double)period + offset),
                                           (uint64_t)(values[i * 4 + 2] * (double)period + offset),
                                           c->events[i].work,
                                           deviation};
      }
    }
    c->count = 0;
    c->submitted = false;
  }
  return result;
}
static void Destroy(VkDevice d, const VkAllocationCallbacks *a) {
  if (d == device) {
    if (real_vkDeviceWaitIdle(d) != VK_SUCCESS) Fail("final idle failed");
    const char *path = getenv("AR_VK_PROFILE");
    FILE *f = path ? fopen(path, "w") : NULL;
    if (!f) Fail("missing output");
    fprintf(f, "frame,command,event,kind,shader,start_ns,end_ns,work,deviation_ns\n");
    for (unsigned i = 0; i < record_count; ++i) {
      Record *r = &records[i];
      fprintf(f, "%d,%u,%u,%s,%016" PRIx64 ",%" PRIu64 ",%" PRIu64 ",%u,%" PRIu64 "\n", r->frame,
              r->cmd, r->event, r->kind, r->shader, r->start, r->end, r->work, r->deviation);
    }
    const bool written = !ferror(f);
    if (fclose(f) != 0 || !written) Fail("could not write output trace");
    fprintf(stderr, "[vk-probe] events=%u commands=%u period_ns=%g bits=%u\n", record_count,
            command_count, period, valid_bits);
    for (unsigned i = 0; i < command_count; ++i)
      real_vkDestroyQueryPool(d, commands[i].pool, NULL);
    free(records);
    records = NULL;
    record_count = command_count = shader_count = pipeline_count = 0;
    frame = kArRoomBenchmarkProbeClosed;
    device = VK_NULL_HANDLE;
  }
  real_vkDestroyDevice(d, a);
}
static VkResult Create(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                       const VkAllocationCallbacks *a, VkDevice *out) {
  if (device) Fail("multiple live devices unsupported");
  VkDeviceCreateInfo extended = *info;
  const char *extensions[128];
  unsigned count = info->enabledExtensionCount;
  bool found = false;
  if (count >= 128) Fail("extension capacity");
  for (unsigned i = 0; i < count; ++i) {
    extensions[i] = info->ppEnabledExtensionNames[i];
    found |= !strcmp(extensions[i], "VK_EXT_calibrated_timestamps");
  }
  if (!found) extensions[count++] = "VK_EXT_calibrated_timestamps";
  extended.ppEnabledExtensionNames = extensions;
  extended.enabledExtensionCount = count;
  VkResult r = real_vkCreateDevice(physical, &extended, a, out);
  if (r != VK_SUCCESS) return r;
  device = *out;
  gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
#define LOAD(n)                                                                                    \
  real_##n = (PFN_##n)gdpa(device, #n);                                                            \
  if (!real_##n) Fail(#n)
  LOAD(vkEndCommandBuffer);
  LOAD(vkQueueSubmit);
  LOAD(vkGetCalibratedTimestampsEXT);
  LOAD(vkDestroyDevice);
  LOAD(vkBeginCommandBuffer);
  LOAD(vkCmdBindPipeline);
  LOAD(vkCreateShaderModule);
  LOAD(vkCreateGraphicsPipelines);
  LOAD(vkCreateComputePipelines);
  LOAD(vkCmdDraw);
  LOAD(vkCmdDrawIndexed);
  LOAD(vkCmdDispatch);
  LOAD(vkDeviceWaitIdle);
  LOAD(vkCmdCopyBuffer);
  LOAD(vkCmdCopyBufferToImage);
  LOAD(vkCmdBlitImage);
  LOAD(vkCreateQueryPool);
  LOAD(vkDestroyQueryPool);
  LOAD(vkCmdResetQueryPool);
  LOAD(vkCmdWriteTimestamp);
  LOAD(vkGetQueryPoolResults);
#undef LOAD
  VkPhysicalDeviceProperties props;
  ((PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties"))(physical,
                                                                                       &props);
  period = props.limits.timestampPeriod;
  uint32_t n = 0;
  PFN_vkGetPhysicalDeviceQueueFamilyProperties family =
      (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(
          instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  family(physical, &n, NULL);
  VkQueueFamilyProperties *families = calloc(n, sizeof(*families));
  if (!families) Fail("queue-family allocation");
  family(physical, &n, families);
  if (!info->queueCreateInfoCount || info->pQueueCreateInfos[0].queueFamilyIndex >= n)
    Fail("missing timestamp queue family");
  valid_bits = families[info->pQueueCreateInfos[0].queueFamilyIndex].timestampValidBits;
  free(families);
  if (valid_bits != 64 || !props.limits.timestampComputeAndGraphics)
    Fail("probe expects 64-bit graphics/compute timestamps");
  records = calloc(MAX_RECORDS, sizeof(*records));
  if (!records) Fail("allocation");
  return r;
}
static PFN_vkVoidFunction Hook(const char *name) {
#define H(n, f)                                                                                    \
  if (!strcmp(name, #n)) return (PFN_vkVoidFunction)f
  H(vkEndCommandBuffer, EndCommand);
  H(vkQueueSubmit, QueueSubmit);
  H(vkCreateDevice, Create);
  H(vkDestroyDevice, Destroy);
  H(vkBeginCommandBuffer, Begin);
  H(vkCmdBindPipeline, Bind);
  H(vkCreateShaderModule, Shader);
  H(vkCreateGraphicsPipelines, Graphics);
  H(vkCreateComputePipelines, Compute);
  H(vkCmdDraw, Draw);
  H(vkCmdDrawIndexed, DrawIndexed);
  H(vkCmdDispatch, Dispatch);
  H(vkDeviceWaitIdle, Idle);
  H(vkCmdCopyBuffer, CopyBuffer);
  H(vkCmdCopyBufferToImage, CopyImage);
  H(vkCmdBlitImage, Blit);
#undef H
  return NULL;
}
static PFN_vkVoidFunction DeviceProc(VkDevice d, const char *name) {
  PFN_vkVoidFunction f = Hook(name);
  return f ? f : gdpa(d, name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance inst, const char *name) {
  if (!gipa) {
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) Fail(dlerror());
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) Fail("Vulkan loader entry point unavailable");
  }
  if (inst) {
    instance = inst;
    if (!real_vkCreateDevice)
      real_vkCreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
  }
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)DeviceProc;
  PFN_vkVoidFunction f = Hook(name);
  return f ? f : gipa(inst, name);
}
