#include "host/touch_controls.h"

#include <math.h>
#include <string.h>

#include "app/input_map.h"
#include "host/host_video.h"
#include "render/render_device.h"
#include "settings_overlay/settings_overlay_render.h"

enum {
  kTouchMaxFingers = 10,
  kTouchCircleSegments = 28,
  /* Host-only button beyond the twelve pad actions. */
  kTouchMenu = kInputAction_PadCount,
  kTouchButtonCount,
  /* Guide delays release in SDL; use an ordinary button for immediate taps. */
  kTouchMenuButton = SDL_GAMEPAD_BUTTON_MISC1,
};

typedef enum { kFinger_Free = 0, kFinger_Pad, kFinger_Button } FingerRole;

typedef struct Finger {
  SDL_FingerID id;
  FingerRole role;
  int button;       /* kFinger_Button: InputAction or kTouchMenu */
  uint32_t pad;     /* kFinger_Pad: held direction bits */
} Finger;

typedef struct Layout {
  float unit;
  float pad_x, pad_y, pad_radius;
  float face_x, face_y, face_offset, face_radius;
  struct { float x, y, w, h; } rect[kTouchButtonCount]; /* shoulders/pills */
} Layout;

static Finger s_fingers[kTouchMaxFingers];
static uint32_t s_held;        /* bit per kTouchButtonCount entry */
static bool s_touch_seen;
static bool s_external_gamepad;
static SDL_Gamepad *s_gamepad;
static SDL_JoystickID s_gamepad_id;
static ArRenderExtentI s_output;

static bool ControlsEnabled(void) {
#ifdef __ANDROID__
  const bool platform_touch = true;
#else
  const bool platform_touch = false;
#endif
  return (platform_touch || s_touch_seen) && !s_external_gamepad;
}

static void DisconnectGamepad(void) {
  memset(s_fingers, 0, sizeof s_fingers);
  s_held = 0;
  if (s_gamepad) SDL_CloseGamepad(s_gamepad);
  if (s_gamepad_id) SDL_DetachVirtualJoystick(s_gamepad_id);
  s_gamepad = NULL;
  s_gamepad_id = 0;
}

static void RefreshGamepad(void) {
  if (!(SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD)) return;
  int count = 0;
  SDL_JoystickID *ids = SDL_GetGamepads(&count);
  s_external_gamepad = false;
  for (int i = 0; i < count; ++i)
    if (ids[i] != s_gamepad_id) s_external_gamepad = true;
  SDL_free(ids);
  if (!ControlsEnabled()) {
    DisconnectGamepad();
    return;
  }
  if (s_gamepad) return;

  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
  desc.name = "Touch controls";
  s_gamepad_id = SDL_AttachVirtualJoystick(&desc);
  /* SNES face labels follow Nintendo's B/A/Y/X order. Declare that layout
   * so menu and gameplay prompts match the letters drawn on the screen. */
  char *mapping = s_gamepad_id ? SDL_GetGamepadMappingForID(s_gamepad_id) : NULL;
  char *labeled_mapping = NULL;
  /* Open before the first finger press: InputMap receives the queued ADDED
   * event later, but SDL needs an open gamepad to produce button events. */
  if (mapping && SDL_asprintf(&labeled_mapping, "%stype:%s,", mapping,
          SDL_GetGamepadStringForType(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO)) >= 0 &&
      SDL_SetGamepadMapping(s_gamepad_id, labeled_mapping))
    s_gamepad = SDL_OpenGamepad(s_gamepad_id);
  SDL_free(labeled_mapping);
  SDL_free(mapping);
  if (!s_gamepad) {
    SDL_Log("Could not create touch controller: %s", SDL_GetError());
    DisconnectGamepad();
  }
}

void TouchControls_Init(void) {
  TouchControls_Shutdown();
  RefreshGamepad();
}

void TouchControls_Shutdown(void) {
  DisconnectGamepad();
  s_touch_seen = s_external_gamepad = false;
  s_output = (ArRenderExtentI){0};
}

bool TouchControls_IsMenuEvent(const SDL_Event *event) {
  return event && s_gamepad_id &&
      (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
       event->type == SDL_EVENT_GAMEPAD_BUTTON_UP) &&
      event->gbutton.which == s_gamepad_id &&
      event->gbutton.button == kTouchMenuButton;
}

static void ComputeLayout(float w, float h, Layout *l) {
  memset(l, 0, sizeof *l);
  const float u = w < h ? w : h;
  const float margin = 0.05f * u;
  l->unit = u;
  l->pad_radius = 0.20f * u;
  l->pad_x = margin + l->pad_radius + 0.04f * u;
  l->pad_y = h - margin - l->pad_radius - 0.02f * u;
  l->face_radius = 0.080f * u;
  l->face_offset = 0.135f * u;
  l->face_x = w - margin - l->face_offset - l->face_radius - 0.02f * u;
  l->face_y = h - margin - l->face_offset - l->face_radius;
  const float sw = 0.24f * u, sh = 0.09f * u;
  l->rect[kInputAction_L].x = margin;
  l->rect[kInputAction_R].x = w - margin - sw;
  l->rect[kInputAction_L].y = l->rect[kInputAction_R].y = margin;
  l->rect[kInputAction_L].w = l->rect[kInputAction_R].w = sw;
  l->rect[kInputAction_L].h = l->rect[kInputAction_R].h = sh;
  const float pw = 0.15f * u, ph = 0.065f * u, py = h - margin - ph;
  l->rect[kInputAction_Select].x = w * 0.5f - pw - 0.03f * u;
  l->rect[kInputAction_Start].x = w * 0.5f + 0.03f * u;
  l->rect[kInputAction_Select].y = l->rect[kInputAction_Start].y = py;
  l->rect[kInputAction_Select].w = l->rect[kInputAction_Start].w = pw;
  l->rect[kInputAction_Select].h = l->rect[kInputAction_Start].h = ph;
  l->rect[kTouchMenu].x = w * 0.5f - pw * 0.5f;
  l->rect[kTouchMenu].y = margin * 0.5f;
  l->rect[kTouchMenu].w = pw;
  l->rect[kTouchMenu].h = ph;
}

/* Face-button diamond in SNES order: X top, A right, B bottom, Y left. */
static void FaceCenter(const Layout *l, int action, float *x, float *y) {
  *x = l->face_x;
  *y = l->face_y;
  switch (action) {
  case kInputAction_X: *y -= l->face_offset; break;
  case kInputAction_A: *x += l->face_offset; break;
  case kInputAction_B: *y += l->face_offset; break;
  case kInputAction_Y: *x -= l->face_offset; break;
  default: break;
  }
}

static const int kFaceButtons[] = {
  kInputAction_X, kInputAction_A, kInputAction_B, kInputAction_Y,
};
static const int kRectButtons[] = {
  kInputAction_L, kInputAction_R, kInputAction_Select, kInputAction_Start, kTouchMenu,
};

static int HitButton(const Layout *l, float x, float y, bool sliding) {
  /* Generous hit circles: thumbs land off-centre, and a slide keeps rolling
   * between neighbouring face buttons. */
  const float reach = l->face_radius * (sliding ? 1.25f : 1.45f);
  int best = -1;
  float best_distance = reach * reach;
  for (size_t i = 0; i < SDL_arraysize(kFaceButtons); i++) {
    float cx, cy;
    FaceCenter(l, kFaceButtons[i], &cx, &cy);
    const float d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
    if (d < best_distance) { best_distance = d; best = kFaceButtons[i]; }
  }
  if (best >= 0) return best;
  const float slack = 0.02f * l->unit;
  for (size_t i = 0; i < SDL_arraysize(kRectButtons); i++) {
    const int b = kRectButtons[i];
    if (x >= l->rect[b].x - slack && x <= l->rect[b].x + l->rect[b].w + slack &&
        y >= l->rect[b].y - slack && y <= l->rect[b].y + l->rect[b].h + slack)
      return b;
  }
  return -1;
}

static bool InPadZone(const Layout *l, float x, float y) {
  const float dx = x - l->pad_x, dy = y - l->pad_y;
  return dx * dx + dy * dy <= (l->pad_radius * 1.6f) * (l->pad_radius * 1.6f);
}

static uint32_t PadBits(const Layout *l, float x, float y) {
  const float dx = x - l->pad_x, dy = y - l->pad_y;
  const float dead = l->pad_radius * 0.22f;
  if (dx * dx + dy * dy < dead * dead) return 0;
  /* Eight 45-degree sectors; diagonals hold two directions. */
  const float angle = atan2f(dy, dx);           /* screen y grows downward */
  int sector = (int)lroundf(angle / (float)(M_PI / 4.0)) & 7;
  static const uint32_t kSectorBits[8] = {
    1u << kInputAction_Right,
    (1u << kInputAction_Right) | (1u << kInputAction_Down),
    1u << kInputAction_Down,
    (1u << kInputAction_Down) | (1u << kInputAction_Left),
    1u << kInputAction_Left,
    (1u << kInputAction_Left) | (1u << kInputAction_Up),
    1u << kInputAction_Up,
    (1u << kInputAction_Up) | (1u << kInputAction_Right),
  };
  return kSectorBits[sector];
}

static void PublishHeld(void) {
  uint32_t held = 0;
  for (int i = 0; i < kTouchMaxFingers; i++) {
    if (s_fingers[i].role == kFinger_Pad) held |= s_fingers[i].pad;
    else if (s_fingers[i].role == kFinger_Button) held |= 1u << s_fingers[i].button;
  }
  const uint32_t changed = held ^ s_held;
  if (changed && s_gamepad) {
    SDL_Joystick *joystick = SDL_GetGamepadJoystick(s_gamepad);
    SDL_LockJoysticks();
    for (int b = 0; b < kTouchButtonCount; b++) {
      if (!(changed & (1u << b))) continue;
      /* Physical identity must stay fixed when a mapping changes, including
       * between a finger's press and release during binding capture. */
      const int button = b == kTouchMenu ? kTouchMenuButton :
          INPUT_BIND_CODE(InputMap_DefaultBinding((InputAction)b, kInputClass_Gamepad));
      SDL_SetJoystickVirtualButton(joystick, button, (held >> b) & 1u);
    }
    /* Flush each transition: DOWN and UP can arrive in the same event pump,
     * and a later poll would otherwise collapse a short tap to no input. */
    SDL_UpdateJoysticks();
    SDL_UnlockJoysticks();
  }
  s_held = held;
}

static Finger *FindFinger(SDL_FingerID id, bool allocate) {
  Finger *free_slot = NULL;
  for (int i = 0; i < kTouchMaxFingers; i++) {
    if (s_fingers[i].role != kFinger_Free && s_fingers[i].id == id) return &s_fingers[i];
    if (!free_slot && s_fingers[i].role == kFinger_Free) free_slot = &s_fingers[i];
  }
  return allocate ? free_slot : NULL;
}

static void CurrentOutput(float *w, float *h) {
  int pw = s_output.width, ph = s_output.height;
  if ((pw <= 0 || ph <= 0) && g_window) SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
  *w = (float)(pw > 0 ? pw : 1);
  *h = (float)(ph > 0 ? ph : 1);
}

bool TouchControls_HandleEvent(const SDL_Event *event) {
  if (!event) return false;
  if (event->type == SDL_EVENT_GAMEPAD_ADDED || event->type == SDL_EVENT_GAMEPAD_REMOVED) {
    RefreshGamepad();
    return false;
  }
  if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST ||
      event->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
    memset(s_fingers, 0, sizeof s_fingers);
    PublishHeld();
    return false;
  }
  if (event->type != SDL_EVENT_FINGER_DOWN && event->type != SDL_EVENT_FINGER_MOTION &&
      event->type != SDL_EVENT_FINGER_UP && event->type != SDL_EVENT_FINGER_CANCELED)
    return false;
  if (SDL_GetTouchDeviceType(event->tfinger.touchID) != SDL_TOUCH_DEVICE_DIRECT)
    return true;   /* trackpads are not screens */
  if (!s_touch_seen) {
    s_touch_seen = true;
    RefreshGamepad();
  }
  if (!ControlsEnabled() || !s_gamepad) return true;
  float w, h;
  CurrentOutput(&w, &h);
  Layout layout;
  ComputeLayout(w, h, &layout);
  const float x = event->tfinger.x * w, y = event->tfinger.y * h;

  if (event->type == SDL_EVENT_FINGER_DOWN) {
    Finger *finger = FindFinger(event->tfinger.fingerID, true);
    if (!finger) return true;
    finger->id = event->tfinger.fingerID;
    const int button = HitButton(&layout, x, y, false);
    if (button >= 0) {
      finger->role = kFinger_Button;
      finger->button = button;
    } else if (InPadZone(&layout, x, y)) {
      finger->role = kFinger_Pad;
      finger->pad = PadBits(&layout, x, y);
    }
  } else {
    Finger *finger = FindFinger(event->tfinger.fingerID, false);
    if (!finger) return true;
    if (event->type == SDL_EVENT_FINGER_MOTION) {
      if (finger->role == kFinger_Pad) {
        finger->pad = PadBits(&layout, x, y);
      } else if (finger->role == kFinger_Button && finger->button < kInputAction_PadCount &&
                 finger->button != kInputAction_Select && finger->button != kInputAction_Start) {
        const int button = HitButton(&layout, x, y, true);
        if (button >= 0 && button < kInputAction_PadCount) finger->button = button;
      }
    } else {
      finger->role = kFinger_Free;
    }
  }
  PublishHeld();
  return true;
}

static void DrawCircle(float cx, float cy, float radius, ArRenderColorF color) {
  ArRenderVertex2D vertices[kTouchCircleSegments + 1];
  int32_t indices[kTouchCircleSegments * 3];
  vertices[0] = (ArRenderVertex2D){{cx, cy}, color, {0, 0}};
  for (int i = 0; i < kTouchCircleSegments; i++) {
    const float a = (float)(2.0 * M_PI) * (float)i / kTouchCircleSegments;
    vertices[i + 1] = (ArRenderVertex2D){{cx + cosf(a) * radius, cy + sinf(a) * radius},
                                         color, {0, 0}};
    indices[i * 3] = 0;
    indices[i * 3 + 1] = i + 1;
    indices[i * 3 + 2] = (i + 1) % kTouchCircleSegments + 1;
  }
  /* Untextured geometry otherwise inherits the backend's global blend
   * (opaque), so the translucent vertex colour would be ignored. */
  const ArRenderDrawState blend = {.flags = kArRenderDrawState_Blend,
                                   .blend = kArRenderBlendMode_Alpha};
  ArRenderDevice_DrawGeometryWithState(&g_render_device, ArRenderTexture_Invalid(), vertices,
                                       kTouchCircleSegments + 1, indices,
                                       kTouchCircleSegments * 3, &blend);
}

static void DrawLabel(float cx, float cy, int scale, uint8_t alpha, const char *text) {
  const int width = SettingsOverlay_GameTextWidth(text, scale);
  SettingsOverlay_DrawGameText((int)(cx - width * 0.5f), (int)(cy - 4.0f * scale), scale,
                               alpha, text);
}

void TouchControls_Draw(ArRenderExtentI output_size) {
  s_output = output_size;
  if (!ControlsEnabled() || output_size.width <= 0 || output_size.height <= 0) return;
  Layout l;
  ComputeLayout((float)output_size.width, (float)output_size.height, &l);
  const ArRenderColorF idle = {1.f, 1.f, 1.f, .16f};
  const ArRenderColorF lit = {1.f, 1.f, 1.f, .42f};
  const ArRenderColorF base = {0.f, 0.f, 0.f, .22f};
  int scale = (int)(l.unit / 260.0f);
  if (scale < 1) scale = 1;

  /* D-pad: a base disc plus four arms that light with their direction. */
  DrawCircle(l.pad_x, l.pad_y, l.pad_radius, base);
  const float arm = l.pad_radius * 0.36f, reach = l.pad_radius * 0.62f;
  const struct { int action; float dx, dy; } arms[] = {
    {kInputAction_Up, 0, -1}, {kInputAction_Down, 0, 1},
    {kInputAction_Left, -1, 0}, {kInputAction_Right, 1, 0},
  };
  for (size_t i = 0; i < SDL_arraysize(arms); i++) {
    const bool on = (s_held >> arms[i].action) & 1u;
    DrawCircle(l.pad_x + arms[i].dx * reach, l.pad_y + arms[i].dy * reach, arm,
               on ? lit : idle);
  }

  static const char *const kFaceLabels[] = {
    [kInputAction_X] = "X", [kInputAction_A] = "A",
    [kInputAction_B] = "B", [kInputAction_Y] = "Y",
  };
  for (size_t i = 0; i < SDL_arraysize(kFaceButtons); i++) {
    const int b = kFaceButtons[i];
    float cx, cy;
    FaceCenter(&l, b, &cx, &cy);
    const bool on = (s_held >> b) & 1u;
    DrawCircle(cx, cy, l.face_radius, on ? lit : idle);
    DrawLabel(cx, cy, scale * 2, on ? 255 : 170, kFaceLabels[b]);
  }

  static const char *const kRectLabels[kTouchButtonCount] = {
    [kInputAction_L] = "L", [kInputAction_R] = "R", [kInputAction_Select] = "SELECT",
    [kInputAction_Start] = "START", [kTouchMenu] = "MENU",
  };
  for (size_t i = 0; i < SDL_arraysize(kRectButtons); i++) {
    const int b = kRectButtons[i];
    const bool on = (s_held >> b) & 1u;
    const ArRenderRectF rect = {l.rect[b].x, l.rect[b].y, l.rect[b].w, l.rect[b].h};
    ArRenderDevice_DrawSolidRect(&g_render_device, &rect, on ? lit : idle,
                                 kArRenderBlendMode_Alpha);
    DrawLabel(rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f, scale, on ? 255 : 170,
              kRectLabels[b]);
  }
}
