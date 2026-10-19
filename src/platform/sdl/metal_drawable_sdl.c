#include "platform/sdl/metal_drawable_sdl.h"

#if defined(SDL_PLATFORM_MACOS)
#include <dlfcn.h>
#include <stdint.h>

/* A small C bridge to public Cocoa/QuartzCore methods. Hermetic game builds
 * have C headers and libSystem, but do not require an Objective-C compiler or
 * the AppKit SDK. SDL has already loaded Cocoa and its Objective-C runtime.
 * Resolve that runtime once; never patch methods or create a second view.
 * Every message below has an explicit ABI (NSInteger/NSUInteger are pointer
 * sized on both supported Mac architectures; no structure-return messages). */
typedef void *Object;
typedef void *Selector;
typedef Object (*ObjectMessage)(Object, Selector);
typedef Object (*TagMessage)(Object, Selector, intptr_t);
typedef bool (*ClassMessage)(Object, Selector, Object);
typedef uintptr_t (*CountMessage)(Object, Selector);
typedef void (*SetCountMessage)(Object, Selector, uintptr_t);

static struct {
  bool attempted, ready;
  void *send;
  Object metal_class;
  Selector content_view, tagged_view, layer, is_kind, count, set_count;
} s_api;

static bool ResolveApi(void) {
  if (s_api.attempted) return s_api.ready;
  s_api.attempted = true;
  Object (*get_class)(const char *) =
      (Object (*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
  Selector (*selector)(const char *) =
      (Selector (*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
  s_api.send = dlsym(RTLD_DEFAULT, "objc_msgSend");
  if (!get_class || !selector || !s_api.send) return false;
  s_api.metal_class = get_class("CAMetalLayer");
  s_api.content_view = selector("contentView");
  s_api.tagged_view = selector("viewWithTag:");
  s_api.layer = selector("layer");
  s_api.is_kind = selector("isKindOfClass:");
  s_api.count = selector("maximumDrawableCount");
  s_api.set_count = selector("setMaximumDrawableCount:");
  s_api.ready = s_api.metal_class != NULL;
  return s_api.ready;
}

unsigned ArSdlMetalDrawable_Configure(SDL_Window *window, unsigned count) {
  if (!window || (count != 0 && count != 2 && count != 3) ||
      !SDL_IsMainThread() || !ResolveApi()) return 0;
  const SDL_PropertiesID props = SDL_GetWindowProperties(window);
  Object native = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
  const Sint64 tag = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_COCOA_METAL_VIEW_TAG_NUMBER, 0);
  if (!native || !tag) return 0;
  Object view = ((ObjectMessage)s_api.send)(native, s_api.content_view);
  view = ((TagMessage)s_api.send)(view, s_api.tagged_view, (intptr_t)tag);
  Object layer = ((ObjectMessage)s_api.send)(view, s_api.layer);
  if (!layer || !((ClassMessage)s_api.send)(layer, s_api.is_kind, s_api.metal_class)) return 0;
  unsigned actual = (unsigned)((CountMessage)s_api.send)(layer, s_api.count);
  if (count && actual != count) {
    ((SetCountMessage)s_api.send)(layer, s_api.set_count, count);
    actual = (unsigned)((CountMessage)s_api.send)(layer, s_api.count);
  }
  return actual;
}
#else
unsigned ArSdlMetalDrawable_Configure(SDL_Window *window, unsigned count) {
  (void)window;
  (void)count;
  return 0;
}
#endif
