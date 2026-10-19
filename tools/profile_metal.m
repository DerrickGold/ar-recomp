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
static unsigned s_capacity = kCapacity;
static atomic_uint s_session_flags;
static dispatch_source_t s_session_timer;
typedef void (*Commit)(id, SEL);
typedef id<MTLRenderCommandEncoder> (*Render)(id, SEL, MTLRenderPassDescriptor *);
typedef void (*Present)(id, SEL, id<MTLDrawable>);
typedef struct { Class cls; Commit original; Render render; Present present; } CommandHook;
static CommandHook s_hooks[kClasses];
static unsigned s_hook_count;
static id<CAMetalDrawable> (*s_next)(id, SEL);
static void (*s_textures)(id, SEL, const id<MTLTexture> *, NSRange);

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
  id<MTLRenderCommandEncoder> encoder = FindHook(command)->render(command, selector, pass);
  id<MTLTexture> target = pass.colorAttachments[0].texture;
  Record("target", Identity(command), SDL_GetTicksNS(), Identity(encoder), Identity(target),
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
  if (s_resources) {
    hook->render = (Render)InstallLocal(cls, @selector(renderCommandEncoderWithDescriptor:), (IMP)TimedRender);
    hook->present = (Present)InstallLocal(cls, @selector(presentDrawable:), (IMP)TimedPresent);
    if (!hook->render || !hook->present) abort();
  }
  fprintf(stderr, "[metal-timing] command class %s\n", class_getName(cls));
  return true;
}

static id<CAMetalDrawable> TimedNext(id layer, SEL selector) {
  uint64_t start = SDL_GetTicksNS();
  id<CAMetalDrawable> drawable = s_next(layer, selector);
  uint64_t end = SDL_GetTicksNS();
  unsigned sequence = atomic_fetch_add_explicit(&s_drawables, 1, memory_order_relaxed) + 1;
  Record("acquire", sequence, start, end, drawable != nil, ThreadId());
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
  s_capacity = s_resources ? kResourceCapacity : kCapacity;
  s_events = calloc(s_capacity, sizeof(*s_events));
  if (!s_events) return;
  for (unsigned i = 0; i < s_capacity; ++i) atomic_init(&s_events[i].ready, false);
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
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
