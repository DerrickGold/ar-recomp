/* Diagnostic-only Metal command/drawable timing. Never linked into the game.
 * Build and usage: docs/gpu-presentation-timing.md. Hooks public protocol
 * methods on the active concrete classes, without changing their ordering.
 * Events stay in a bounded buffer until exit; no GPU waits/readbacks are added.
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>
#include <SDL3/SDL_timer.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/resource.h>

enum { kCapacity = 262144, kResourceCapacity = 1048576, kClasses = 8 };
typedef struct Event {
  atomic_bool ready;
  const char *kind;
  uint64_t id, start, end, value0, value1;
} Event;
static Event *s_events;
static atomic_uint s_count, s_commands, s_drawables;
static atomic_bool s_enabled;
static const char *s_path;
static bool s_display_callbacks = true;
static bool s_resources;
static bool s_scheduling;
static bool s_pipelines;
static bool s_waits;
static bool s_memory;
static unsigned s_capacity = kCapacity;
static atomic_uint s_session_flags;
static dispatch_source_t s_session_timer;
typedef void (*Commit)(id, SEL);
typedef id<MTLRenderCommandEncoder> (*Render)(id, SEL, MTLRenderPassDescriptor *);
typedef void (*Present)(id, SEL, id<MTLDrawable>);
typedef struct {
  Class cls;
  Commit original, wait_complete;
  Render render;
  Present present;
} CommandHook;
static CommandHook s_hooks[kClasses];
static unsigned s_hook_count;
static id<CAMetalDrawable> (*s_next)(id, SEL);
static id<MTLCommandBuffer> (*s_queue_command)(id, SEL);
static void (*s_textures)(id, SEL, const id<MTLTexture> *, NSRange);
typedef void (*DrawablePresent)(id, SEL);
typedef void (*DrawablePresentTime)(id, SEL, CFTimeInterval);
typedef struct {
  Class cls;
  DrawablePresent present;
  DrawablePresentTime at_time, after_duration;
} DrawableHook;
static DrawableHook s_drawable_hooks[kClasses];
static unsigned s_drawable_hook_count;
static pthread_mutex_t s_drawable_hook_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t Identity(id object) { return (uint64_t)(uintptr_t)(__bridge void *)object; }

static CommandHook *FindHook(id object) {
  for (Class cls = object_getClass(object); cls; cls = class_getSuperclass(cls))
    for (unsigned i = 0; i < s_hook_count; ++i)
      if (s_hooks[i].cls == cls) return &s_hooks[i];
  abort();
}

static IMP InstallLocal(Class cls, SEL selector, IMP replacement) {
  Method method = class_getInstanceMethod(cls, selector);
  if (!method) return NULL;
  IMP original = method_getImplementation(method);
  if (!class_addMethod(cls, selector, replacement, method_getTypeEncoding(method)))
    class_replaceMethod(cls, selector, replacement, method_getTypeEncoding(method));
  return original;
}

static uint64_t ThreadId(void) {
  static _Thread_local uint64_t value;
  if (!value) pthread_threadid_np(NULL, &value);
  return value;
}

static uint64_t ThreadCpuTime(void) {
  struct timespec time;
  return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) == 0
      ? (uint64_t)time.tv_sec * 1000000000 + (uint64_t)time.tv_nsec : 0;
}

static void SampleSession(void) {
  @autoreleasepool {
    NSDictionary *session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    unsigned flags = [session[(__bridge NSString *)kCGSessionOnConsoleKey] boolValue] |
        ([session[(__bridge NSString *)kCGSessionLoginDoneKey] boolValue] << 1) |
        ([session[@"CGSSessionScreenIsLocked"] boolValue] << 2);
    atomic_store_explicit(&s_session_flags, flags, memory_order_relaxed);
  }
}

static void Record(const char *kind, uint64_t id, uint64_t start, uint64_t end,
                   uint64_t a, uint64_t b) {
  if (!atomic_load_explicit(&s_enabled, memory_order_relaxed)) return;
  unsigned index = atomic_fetch_add_explicit(&s_count, 1, memory_order_relaxed);
  if (index >= s_capacity) return;
  Event *e = &s_events[index];
  e->kind = kind; e->id = id; e->start = start; e->end = end;
  e->value0 = a; e->value1 = b;
  atomic_store_explicit(&e->ready, true, memory_order_release);
}

/* Observe existing waits/allocations only; never wait on behalf of the probe.
 * CPU time distinguishes work from blocking/descheduling without claiming
 * that a wall-time scope reveals the kernel's blocked stack.
 */
typedef struct { bool available; struct rusage usage; } MemorySample;

static MemorySample SampleMemory(void) {
  MemorySample sample = {0};
  if (s_memory) sample.available = getrusage(RUSAGE_SELF, &sample.usage) == 0;
  return sample;
}

static void RecordScope(const char *kind, id object, uint64_t start, uint64_t cpu_start,
                        const MemorySample *before) {
  uint64_t end = SDL_GetTicksNS(), cpu_end = ThreadCpuTime();
  Record(kind, Identity(object), start, end,
         cpu_start && cpu_end >= cpu_start ? cpu_end - cpu_start : UINT64_MAX, ThreadId());
  if (s_memory && end - start >= 1000000) {
    MemorySample after = SampleMemory();
    /* These counters cover the whole process, not just this calling thread. */
    if (before->available && after.available) {
      Record("scope_faults", Identity(object), start, end,
             after.usage.ru_majflt - before->usage.ru_majflt,
             after.usage.ru_minflt - before->usage.ru_minflt);
      Record("scope_switches", Identity(object), start, end,
             after.usage.ru_nvcsw - before->usage.ru_nvcsw,
             after.usage.ru_nivcsw - before->usage.ru_nivcsw);
    } else Record("scope_memory_unavailable", Identity(object), start, end, 0, 0);
  }
}

static void TimedWaitComplete(id command, SEL selector) {
  MemorySample memory = SampleMemory();
  uint64_t cpu = ThreadCpuTime(), start = SDL_GetTicksNS();
  FindHook(command)->wait_complete(command, selector);
  RecordScope("wait_complete", command, start, cpu, &memory);
}

static id<MTLCommandBuffer> TimedQueueCommand(id queue, SEL selector) {
  MemorySample memory = SampleMemory();
  uint64_t cpu = ThreadCpuTime(), start = SDL_GetTicksNS();
  id<MTLCommandBuffer> command = s_queue_command(queue, selector);
  RecordScope("queue_command", command, start, cpu, &memory);
  return command;
}

/* SDL 3.4's Metal backend uses these four synchronous library/PSO entrypoints.
 * Observe the native device so SDL's own lazily built renderer pipelines are
 * included, not just the game's GpuCompileLog wrappers. Creation is not proof
 * of a driver cache miss; these are API wall times, not compiler busy times.
 * The `new` method family's +1 ownership must also apply to our C functions.
 */
typedef id<MTLLibrary> (*LibrarySource)(id, SEL, NSString *, MTLCompileOptions *, NSError **) NS_RETURNS_RETAINED;
typedef id<MTLLibrary> (*LibraryData)(id, SEL, dispatch_data_t, NSError **) NS_RETURNS_RETAINED;
typedef id<MTLRenderPipelineState> (*RenderPipeline)(id, SEL, MTLRenderPipelineDescriptor *, NSError **) NS_RETURNS_RETAINED;
typedef id<MTLComputePipelineState> (*ComputePipeline)(id, SEL, MTLComputePipelineDescriptor *,
    MTLPipelineOption, MTLAutoreleasedComputePipelineReflection *, NSError **) NS_RETURNS_RETAINED;
typedef void (*BindPipeline)(id, SEL, id);
typedef struct {
  Class device_class, render_class, compute_class;
  LibrarySource source;
  LibraryData data;
  RenderPipeline render;
  ComputePipeline compute;
  BindPipeline bind_render, bind_compute;
} PipelineHooks;
/* Set up once before enabling the observer; no mutation on callback threads.
 * This probe targets the default device, matching the measured SDL backend.
 */
static PipelineHooks s_pipeline_hooks;

static id<MTLLibrary> TimedLibrarySource(id device, SEL selector, NSString *source,
    MTLCompileOptions *options, NSError **error) NS_RETURNS_RETAINED {
  uint64_t start = SDL_GetTicksNS();
  id<MTLLibrary> result = s_pipeline_hooks.source(device, selector, source, options, error);
  Record("library_source", Identity(result), start, SDL_GetTicksNS(), ThreadId(), Identity(device));
  return result;
}

static id<MTLLibrary> TimedLibraryData(id device, SEL selector, dispatch_data_t data,
    NSError **error) NS_RETURNS_RETAINED {
  uint64_t start = SDL_GetTicksNS();
  id<MTLLibrary> result = s_pipeline_hooks.data(device, selector, data, error);
  Record("library_data", Identity(result), start, SDL_GetTicksNS(), ThreadId(), Identity(device));
  return result;
}

static id<MTLRenderPipelineState> TimedRenderPipeline(id device, SEL selector,
    MTLRenderPipelineDescriptor *descriptor, NSError **error) NS_RETURNS_RETAINED {
  uint64_t start = SDL_GetTicksNS();
  id<MTLRenderPipelineState> result = s_pipeline_hooks.render(device, selector, descriptor, error);
  Record("pipeline_render", Identity(result), start, SDL_GetTicksNS(), ThreadId(), Identity(device));
  return result;
}

static id<MTLComputePipelineState> TimedComputePipeline(id device, SEL selector,
    MTLComputePipelineDescriptor *descriptor, MTLPipelineOption options,
    MTLAutoreleasedComputePipelineReflection *reflection, NSError **error) NS_RETURNS_RETAINED {
  uint64_t start = SDL_GetTicksNS();
  id<MTLComputePipelineState> result = s_pipeline_hooks.compute(device, selector, descriptor, options, reflection, error);
  Record("pipeline_compute", Identity(result), start, SDL_GetTicksNS(), ThreadId(), Identity(device));
  return result;
}

static void TimedBindRender(id encoder, SEL selector, id pipeline) {
  uint64_t start = SDL_GetTicksNS();
  s_pipeline_hooks.bind_render(encoder, selector, pipeline);
  Record("bind_render", Identity(pipeline), start, SDL_GetTicksNS(), Identity(encoder), ThreadId());
}

static void TimedBindCompute(id encoder, SEL selector, id pipeline) {
  uint64_t start = SDL_GetTicksNS();
  s_pipeline_hooks.bind_compute(encoder, selector, pipeline);
  Record("bind_compute", Identity(pipeline), start, SDL_GetTicksNS(), Identity(encoder), ThreadId());
}

static bool HookPipelines(id<MTLDevice> device, id<MTLCommandQueue> queue) {
  Class cls = s_pipeline_hooks.device_class = object_getClass(device);
  s_pipeline_hooks.source = (LibrarySource)InstallLocal(cls,
      @selector(newLibraryWithSource:options:error:), (IMP)TimedLibrarySource);
  s_pipeline_hooks.data = (LibraryData)InstallLocal(cls,
      @selector(newLibraryWithData:error:), (IMP)TimedLibraryData);
  s_pipeline_hooks.render = (RenderPipeline)InstallLocal(cls,
      @selector(newRenderPipelineStateWithDescriptor:error:), (IMP)TimedRenderPipeline);
  s_pipeline_hooks.compute = (ComputePipeline)InstallLocal(cls,
      @selector(newComputePipelineStateWithDescriptor:options:reflection:error:), (IMP)TimedComputePipeline);
  MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
      width:8 height:8 mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget;
  MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = [device newTextureWithDescriptor:desc];
  id<MTLCommandBuffer> command = [queue commandBuffer];
  id<MTLRenderCommandEncoder> render = [command renderCommandEncoderWithDescriptor:pass];
  s_pipeline_hooks.render_class = object_getClass(render);
  s_pipeline_hooks.bind_render = (BindPipeline)InstallLocal(s_pipeline_hooks.render_class,
      @selector(setRenderPipelineState:), (IMP)TimedBindRender);
  [render endEncoding];
  id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
  s_pipeline_hooks.compute_class = object_getClass(compute);
  s_pipeline_hooks.bind_compute = (BindPipeline)InstallLocal(s_pipeline_hooks.compute_class,
      @selector(setComputePipelineState:), (IMP)TimedBindCompute);
  [compute endEncoding];
  fprintf(stderr, "[metal-timing] pipeline classes %s / %s / %s\n", class_getName(cls),
          class_getName(s_pipeline_hooks.render_class), class_getName(s_pipeline_hooks.compute_class));
  return s_pipeline_hooks.source && s_pipeline_hooks.data && s_pipeline_hooks.render &&
      s_pipeline_hooks.compute && s_pipeline_hooks.bind_render && s_pipeline_hooks.bind_compute;
}

static void ClockSample(void) {
  uint64_t a = SDL_GetTicksNS();
  if (!a) return; /* SDL_Quit resets its timer epoch before our destructor. */
  uint64_t metal = (uint64_t)(CACurrentMediaTime() * 1e9);
  Record("clock", 0, a, SDL_GetTicksNS(), metal, 0);
}

static void TimedCommit(id<MTLCommandBuffer> command, SEL selector) {
  /* A concrete subclass can inherit the instrumented method. Resolve its
   * nearest registered ancestor instead of requiring an exact class match. */
  Commit original = FindHook(command)->original;
  if (!atomic_load_explicit(&s_enabled, memory_order_relaxed)) {
    original(command, selector); return;
  }
  unsigned sequence = atomic_fetch_add_explicit(&s_commands, 1, memory_order_relaxed) + 1;
  if ((sequence & 1023) == 1) ClockSample();
  if (s_scheduling) [command addScheduledHandler:^(id<MTLCommandBuffer> scheduled) {
    Record("scheduled", sequence, SDL_GetTicksNS(), scheduled.status, ThreadId(), 0);
  }];
  [command addCompletedHandler:^(id<MTLCommandBuffer> done) {
    Record("gpu", sequence, SDL_GetTicksNS(), (uint64_t)done.status,
           (uint64_t)(done.GPUStartTime * 1e9), (uint64_t)(done.GPUEndTime * 1e9));
  }];
  uint64_t start = SDL_GetTicksNS();
  original(command, selector);
  Record("commit", sequence, start, SDL_GetTicksNS(), ThreadId(), Identity(command));
}

/* Optional resource identities disambiguate an SDL texture container from its
 * cycled Metal backing. No objects are retained by these records. */
static id<MTLRenderCommandEncoder> TimedRender(id command, SEL selector, MTLRenderPassDescriptor *pass) {
  MemorySample memory = SampleMemory();
  uint64_t cpu = s_waits ? ThreadCpuTime() : 0, start = s_waits ? SDL_GetTicksNS() : 0;
  id<MTLRenderCommandEncoder> encoder = FindHook(command)->render(command, selector, pass);
  if (s_waits) RecordScope("render_encoder", command, start, cpu, &memory);
  id<MTLTexture> target = pass.colorAttachments[0].texture;
  if (s_resources) Record("target", Identity(command), SDL_GetTicksNS(), Identity(encoder), Identity(target),
         ((uint64_t)target.width << 32) | target.height);
  return encoder;
}

static void TimedTextures(id encoder, SEL selector, const id<MTLTexture> *textures, NSRange range) {
  for (NSUInteger i = 0; i < range.length; ++i)
    if (textures[i]) Record("sample", Identity(encoder), SDL_GetTicksNS(), 0,
                             Identity(textures[i]), range.location + i);
  s_textures(encoder, selector, textures, range);
}

static void TimedPresent(id command, SEL selector, id<CAMetalDrawable> drawable) {
  Record("present", Identity(command), SDL_GetTicksNS(), 0, Identity(drawable.texture), Identity(drawable));
  FindHook(command)->present(command, selector, drawable);
}

static DrawableHook *FindDrawableHook(id object) {
  /* Requests can arrive on Metal callback threads while the owner acquires
   * the first drawable of another concrete class. Publish each hook fully. */
  pthread_mutex_lock(&s_drawable_hook_mutex);
  DrawableHook *found = NULL;
  for (Class cls = object_getClass(object); cls && !found; cls = class_getSuperclass(cls))
    for (unsigned i = 0; i < s_drawable_hook_count; ++i)
      if (s_drawable_hooks[i].cls == cls) { found = &s_drawable_hooks[i]; break; }
  pthread_mutex_unlock(&s_drawable_hook_mutex);
  if (!found) abort();
  return found; /* Published entries are immutable; never hold the lock during present. */
}

static void TimedDrawablePresent(id<CAMetalDrawable> drawable, SEL selector) {
  uint64_t start = SDL_GetTicksNS();
  FindDrawableHook(drawable)->present(drawable, selector);
  Record("drawable_present", drawable.drawableID, start, SDL_GetTicksNS(),
         Identity(drawable), ThreadId());
}

static void TimedDrawablePresentAtTime(id<CAMetalDrawable> drawable, SEL selector, CFTimeInterval time) {
  uint64_t start = SDL_GetTicksNS();
  FindDrawableHook(drawable)->at_time(drawable, selector, time);
  Record("drawable_at_time", drawable.drawableID, start, SDL_GetTicksNS(),
         Identity(drawable), (uint64_t)(time * 1e9));
}

static void TimedDrawablePresentAfterDuration(id<CAMetalDrawable> drawable, SEL selector, CFTimeInterval time) {
  uint64_t start = SDL_GetTicksNS();
  FindDrawableHook(drawable)->after_duration(drawable, selector, time);
  Record("drawable_after_duration", drawable.drawableID, start, SDL_GetTicksNS(),
         Identity(drawable), (uint64_t)(time * 1e9));
}

static void HookDrawable(id<CAMetalDrawable> drawable) {
  pthread_mutex_lock(&s_drawable_hook_mutex);
  Class cls = object_getClass(drawable);
  for (unsigned i = 0; i < s_drawable_hook_count; ++i)
    if (s_drawable_hooks[i].cls == cls) {
      pthread_mutex_unlock(&s_drawable_hook_mutex);
      return;
    }
  Method method = class_getInstanceMethod(cls, @selector(present));
  if (method && method_getImplementation(method) == (IMP)TimedDrawablePresent) {
    pthread_mutex_unlock(&s_drawable_hook_mutex);
    return;
  }
  if (!method || s_drawable_hook_count == kClasses) abort();
  DrawableHook *hook = &s_drawable_hooks[s_drawable_hook_count++];
  hook->cls = cls;
  hook->present = (DrawablePresent)InstallLocal(cls, @selector(present), (IMP)TimedDrawablePresent);
  hook->at_time = (DrawablePresentTime)InstallLocal(cls, @selector(presentAtTime:), (IMP)TimedDrawablePresentAtTime);
  hook->after_duration = (DrawablePresentTime)InstallLocal(cls, @selector(presentAfterMinimumDuration:),
                                                         (IMP)TimedDrawablePresentAfterDuration);
  if (!hook->present || !hook->at_time || !hook->after_duration) abort();
  pthread_mutex_unlock(&s_drawable_hook_mutex);
  fprintf(stderr, "[metal-timing] drawable class %s\n", class_getName(cls));
}

static bool HookCommand(id<MTLCommandBuffer> command) {
  Class cls = object_getClass(command);
  if (!cls) return false;
  for (unsigned i = 0; i < s_hook_count; ++i) if (s_hooks[i].cls == cls) return true;
  if (s_hook_count == kClasses) return false;
  Method method = class_getInstanceMethod(cls, @selector(commit));
  if (!method) return false;
  /* Already intercepted through inheritance: recording this as the original
   * implementation would recurse when the subclass is committed. */
  if (method_getImplementation(method) == (IMP)TimedCommit) return true;
  CommandHook *hook = &s_hooks[s_hook_count++];
  hook->cls = cls;
  hook->original = (Commit)method_getImplementation(method);
  /* Add a local override when the method is inherited; do not replace a
   * shared superclass implementation and accidentally instrument it twice. */
  if (!class_addMethod(cls, @selector(commit), (IMP)TimedCommit, method_getTypeEncoding(method)))
    class_replaceMethod(cls, @selector(commit), (IMP)TimedCommit, method_getTypeEncoding(method));
  if (s_resources || s_waits) {
    hook->render = (Render)InstallLocal(cls, @selector(renderCommandEncoderWithDescriptor:), (IMP)TimedRender);
    if (!hook->render) abort();
  }
  if (s_resources) {
    hook->present = (Present)InstallLocal(cls, @selector(presentDrawable:), (IMP)TimedPresent);
    if (!hook->present) abort();
  }
  if (s_waits) {
    hook->wait_complete = (Commit)InstallLocal(cls, @selector(waitUntilCompleted), (IMP)TimedWaitComplete);
    if (!hook->wait_complete) abort();
  }
  fprintf(stderr, "[metal-timing] command class %s\n", class_getName(cls));
  return true;
}

static id<CAMetalDrawable> TimedNext(id layer, SEL selector) {
  uint64_t cpu_start = s_scheduling ? ThreadCpuTime() : 0;
  uint64_t start = SDL_GetTicksNS();
  id<CAMetalDrawable> drawable = s_next(layer, selector);
  uint64_t end = SDL_GetTicksNS();
  unsigned sequence = atomic_fetch_add_explicit(&s_drawables, 1, memory_order_relaxed) + 1;
  Record("acquire", sequence, start, end, drawable != nil, ThreadId());
  if (s_scheduling) {
    uint64_t cpu_end = ThreadCpuTime();
    if (cpu_start && cpu_end >= cpu_start)
      Record("acquire_cpu", sequence, start, end, cpu_end - cpu_start, 0);
    else
      Record("acquire_cpu_unavailable", sequence, start, end, 0, 0);
    if (drawable) {
      HookDrawable(drawable);
      Record("drawable", sequence, end, drawable.drawableID, Identity(drawable), Identity(layer));
    }
  }
  if (s_resources && drawable)
    Record("surface", sequence, end, Identity(drawable), Identity(drawable.texture), Identity(layer));
  if ((sequence % 60) == 1) {
    /* CGSessionCopyCurrentDictionary can take milliseconds. Poll it off the
     * render thread; only read the cached session state at this boundary. */
    Record("state", sequence, SDL_GetTicksNS(), 0, [NSApp isActive],
           atomic_load_explicit(&s_session_flags, memory_order_relaxed));
    CAMetalLayer *metalLayer = layer;
    Record("layer", sequence, SDL_GetTicksNS(), metalLayer.presentsWithTransaction,
           metalLayer.displaySyncEnabled, metalLayer.maximumDrawableCount);
  }
  if (drawable && s_display_callbacks) [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
    Record("display", sequence, SDL_GetTicksNS(), 0, (uint64_t)(shown.presentedTime * 1e9), 0);
  }];
  return drawable;
}

__attribute__((constructor)) static void Start(void) {
  s_path = getenv("AR_METAL_TIMING_TRACE");
  if (!s_path || !*s_path) return;
  const char *display = getenv("AR_METAL_TIMING_DISPLAY");
  s_display_callbacks = !display || strcmp(display, "0");
  const char *resources = getenv("AR_METAL_TIMING_RESOURCES");
  s_resources = resources && !strcmp(resources, "1");
  const char *scheduling = getenv("AR_METAL_TIMING_SCHEDULING");
  s_scheduling = scheduling && !strcmp(scheduling, "1");
  const char *pipelines = getenv("AR_METAL_TIMING_PIPELINES");
  s_pipelines = pipelines && !strcmp(pipelines, "1");
  const char *waits = getenv("AR_METAL_TIMING_WAITS");
  s_waits = waits && !strcmp(waits, "1");
  const char *memory = getenv("AR_METAL_TIMING_MEMORY");
  s_memory = memory && !strcmp(memory, "1");
  s_waits = s_waits || s_memory;
  s_capacity = (s_resources || s_pipelines || s_waits) ? kResourceCapacity : kCapacity;
  s_events = calloc(s_capacity, sizeof(*s_events));
  if (!s_events) return;
  for (unsigned i = 0; i < s_capacity; ++i) atomic_init(&s_events[i].ready, false);
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (s_pipelines && !HookPipelines(device, queue)) abort();
    if (s_resources) {
      MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
          width:8 height:8 mipmapped:NO];
      desc.usage = MTLTextureUsageRenderTarget;
      id<MTLTexture> target = [device newTextureWithDescriptor:desc];
      MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = target;
      id<MTLCommandBuffer> command = [queue commandBuffer];
      id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
      s_textures = (void (*)(id,SEL,const id<MTLTexture> *,NSRange))InstallLocal(object_getClass(encoder),
          @selector(setFragmentTextures:withRange:), (IMP)TimedTextures);
      [encoder endEncoding];
      if (!s_textures) abort();
    }
    bool ok = HookCommand([queue commandBuffer]) && HookCommand([queue commandBufferWithUnretainedReferences]);
    if (s_waits) {
      s_queue_command = (id<MTLCommandBuffer> (*)(id, SEL))InstallLocal(object_getClass(queue),
          @selector(commandBuffer), (IMP)TimedQueueCommand);
      if (!s_queue_command) abort();
    }
    Method next = class_getInstanceMethod([CAMetalLayer class], @selector(nextDrawable));
    if (!ok || !next) {
      fprintf(stderr, "[metal-timing] instrumentation unavailable\n"); return;
    }
    s_next = (id<CAMetalDrawable> (*)(id,SEL))method_getImplementation(next);
    method_setImplementation(next, (IMP)TimedNext);
    SampleSession();
    s_session_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(s_session_timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                              NSEC_PER_SEC, NSEC_PER_SEC/10);
    dispatch_source_set_event_handler(s_session_timer, ^{ SampleSession(); });
    dispatch_resume(s_session_timer);
    atomic_store(&s_enabled, true);
    if (s_pipelines) Record("pipeline_scope", Identity(device), SDL_GetTicksNS(), 0, device.registryID, 1);
    if (s_waits) Record("wait_scope", Identity(device), SDL_GetTicksNS(), 0, device.registryID, 1);
    if (s_memory) Record("memory_scope", Identity(device), SDL_GetTicksNS(), 0, device.registryID, 1);
    fprintf(stderr, "[metal-timing] enabled; asynchronous command completion and drawable callbacks\n");
  }
}

__attribute__((destructor)) static void Finish(void) {
  if (!atomic_load(&s_enabled)) return;
  atomic_store(&s_enabled, false);
  if (s_session_timer) dispatch_source_cancel(s_session_timer);
  unsigned count = atomic_load(&s_count), written = 0;
  FILE *f = fopen(s_path, "w");
  if (!f) { perror("metal timing trace"); return; }
  fprintf(f, "kind,id,start_ns,end_ns,value0,value1\n");
  for (unsigned i = 0; i < count && i < s_capacity; ++i) {
    Event *e = &s_events[i];
    if (!atomic_load_explicit(&e->ready, memory_order_acquire)) continue;
    fprintf(f, "%s,%llu,%llu,%llu,%llu,%llu\n", e->kind,
            (unsigned long long)e->id, (unsigned long long)e->start,
            (unsigned long long)e->end, (unsigned long long)e->value0,
            (unsigned long long)e->value1);
    ++written;
  }
  fclose(f);
  fprintf(stderr, "[metal-timing] events=%u written=%u overflow=%u commands=%u drawables=%u\n",
          count, written, count > s_capacity ? count-s_capacity : 0,
          atomic_load(&s_commands), atomic_load(&s_drawables));
  /* No wait for late callbacks, and no free while a callback could still be
   * finishing. Process teardown reclaims the bounded diagnostic buffer. */
}
