#include "present/preparation_indicator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser/quintet_lzss.h"
#include "host/host_clock.h"

/* US ROM file offsets. The HUD bar is BG3 font art; the angel is town OBJ
 * art posed by her own animation program (see docs/actraiser-1/rom-map.md). */
enum {
  kFontAssetOffset = 0x0BECFB,   /* LZSS: 256 BG3 2bpp tiles */
  kFontBytes = 0x1000,
  kHudPaletteOffset = 0x010F7C,  /* BG3 palettes 0-7, 4 BGR555 colours each */
  kBarPalette = 2,               /* full HP blue, lost HP red */
  kBarFullTile = 0x1B,           /* two full pills ($02:C386) */
  kBarLostTile = 0x1E,           /* two lost pills */
  kAngelCharacters = 0x068000,   /* raw $0D:8000, first town OBJ page */
  kAngelCharacterTiles = 512,
  kTownPaletteOffset = 0x0E3B93, /* temperate CGRAM; OBJ colours from 128 */
  kAngelFrontProgram = 0x00A596, /* $01:A596: four 4-tick frontal poses */
  kProgramLoop = 0xFE,
  kAngelPartLimit = 8,
  kAngelPoseMs = 67,             /* 4 ticks at 60 Hz */
  kLayoutUnitsPerScale = 150,
};

static uint32_t s_atlas[kPreparationAtlasWidth * kPreparationAtlasHeight];
static bool s_has_art;

static uint32_t Argb(const uint8_t *bgr555) {
  const unsigned value = bgr555[0] | (unsigned)bgr555[1] << 8;
  const uint32_t r = (value & 31) * 255 / 31;
  const uint32_t g = (value >> 5 & 31) * 255 / 31;
  const uint32_t b = (value >> 10 & 31) * 255 / 31;
  return UINT32_C(0xff000000) | r << 16 | g << 8 | b;
}

/* Left half of a 2bpp tile: one 4x8 pill. */
static bool DecodePill(const uint8_t *tile, const uint8_t *palette, int atlas_x) {
  bool inked = false;
  for (int y = 0; y < kPreparationPillHeight; y++)
    for (int x = 0; x < kPreparationPillWidth; x++) {
      const unsigned value = ((tile[y * 2] >> (7 - x)) & 1) |
                             (((tile[y * 2 + 1] >> (7 - x)) & 1) << 1);
      if (!value) continue;
      s_atlas[y * kPreparationAtlasWidth + atlas_x + x] = Argb(palette + value * 2);
      inked = true;
    }
  return inked;
}

static bool DecodeAngelPose(const uint8_t *rom, size_t size, unsigned composition,
                            int atlas_x) {
  if (composition + 1 > size) return false;
  const unsigned parts = rom[composition];
  if (!parts || parts > kAngelPartLimit || composition + 1 + parts * 5 > size) return false;
  bool inked = false;
  for (unsigned part = 0; part < parts; part++) {
    const uint8_t *p = rom + composition + 1 + part * 5;
    const unsigned attribute = p[3] | (unsigned)p[4] << 8;
    /* 8x8 parts with small non-negative offsets, as every frontal pose uses. */
    if (p[0] != 0 || p[1] > kPreparationAngelCell - 8 || p[2] > kPreparationAngelCell - 8)
      return false;
    const uint8_t *tile = rom + kAngelCharacters + (attribute % kAngelCharacterTiles) * 32;
    const uint8_t *palette = rom + kTownPaletteOffset + (128 + ((attribute >> 9) & 7) * 16) * 2;
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++) {
        const int sy = attribute & 0x8000 ? 7 - y : y;
        const int bit = attribute & 0x4000 ? x : 7 - x;
        const unsigned value = ((tile[sy * 2] >> bit) & 1) |
                               (((tile[sy * 2 + 1] >> bit) & 1) << 1) |
                               (((tile[sy * 2 + 16] >> bit) & 1) << 2) |
                               (((tile[sy * 2 + 17] >> bit) & 1) << 3);
        if (!value) continue;
        s_atlas[(p[2] + y) * kPreparationAtlasWidth + atlas_x + p[1] + x] =
            Argb(palette + value * 2);
        inked = true;
      }
  }
  return inked;
}

bool PreparationIndicator_DecodeRom(const uint8_t *rom, size_t size) {
  memset(s_atlas, 0, sizeof(s_atlas));
  s_has_art = false;
  if (!rom || size < (size_t)kTownPaletteOffset + 512 ||
      size < (size_t)kAngelCharacters + kAngelCharacterTiles * 32)
    return false;
  const uint8_t *asset = rom + kFontAssetOffset;
  if ((asset[0] | (size_t)asset[1] << 8) != kFontBytes) return false;
  uint8_t font[kFontBytes];
  QuintetLzssState state;
  if (!QuintetLzss_DecompressAsset(asset, size - kFontAssetOffset, font, kFontBytes, &state))
    return false;
  const uint8_t *bar_palette = rom + kHudPaletteOffset + kBarPalette * 8;
  if (!DecodePill(font + kBarFullTile * 16, bar_palette, 0) ||
      !DecodePill(font + kBarLostTile * 16, bar_palette, kPreparationPillWidth))
    return false;
  /* The animation program: (ticks, composition) entries ending in $FE. */
  unsigned at = kAngelFrontProgram;
  for (int frame = 0; frame < kPreparationAngelFrames; frame++, at += 3) {
    if (at + 3 > size || rom[at] == kProgramLoop || rom[at] == 0) return false;
    const unsigned composition = rom[at + 1] | (unsigned)rom[at + 2] << 8;
    if (composition < 0x8000 ||
        !DecodeAngelPose(rom, size, composition,
                         2 * kPreparationPillWidth + frame * kPreparationAngelCell))
      return false;
  }
  if (rom[at] != kProgramLoop) return false;
  s_has_art = true;
  return true;
}

bool PreparationIndicator_HasArt(void) { return s_has_art; }
const uint32_t *PreparationIndicator_AtlasPixels(void) { return s_atlas; }

int PreparationIndicator_Layout(int width, int height, uint64_t elapsed_ms,
                                PreparationSprite *sprites) {
  if (width <= 0 || height <= 0 || !sprites) return 0;
  int scale = (width < height ? width : height) / kLayoutUnitsPerScale;
  if (scale < 1) scale = 1;
  const float pill_width = (float)(kPreparationPillWidth * scale);
  const float pill_height = (float)(kPreparationPillHeight * scale);
  const float left = (float)((width - kPreparationBarPills * kPreparationPillWidth * scale) / 2);
  const float top = (float)(height * 58 / 100) - pill_height / 2;
  /* Ping-pong sweep, eased at both ends; the red window snaps to pills like
   * the native bar. The angel stays centred above the bar. */
  const float phase = (float)(elapsed_ms % (2 * kPreparationIndicatorSweepMs)) /
                      kPreparationIndicatorSweepMs;
  const float linear = phase < 1 ? phase : 2 - phase;
  const float position = linear * linear * (3 - 2 * linear) *
                         (kPreparationBarPills - kPreparationLostPills);
  const int lost_first = (int)(position + 0.5f);
  int count = 0;
  for (int pill = 0; pill < kPreparationBarPills; pill++) {
    const bool lost = pill >= lost_first && pill < lost_first + kPreparationLostPills;
    sprites[count++] = (PreparationSprite){
        .source = {lost ? (float)kPreparationPillWidth : 0, 0, (float)kPreparationPillWidth,
                   (float)kPreparationPillHeight},
        .destination = {left + pill * pill_width, top, pill_width, pill_height}};
  }
  const int frame = (int)((elapsed_ms / kAngelPoseMs) % kPreparationAngelFrames);
  const float centre = left + kPreparationBarPills * pill_width / 2;
  const float cell = (float)(kPreparationAngelCell * scale);
  sprites[count++] = (PreparationSprite){
      .source = {(float)(2 * kPreparationPillWidth + frame * kPreparationAngelCell), 0,
                 (float)kPreparationAngelCell, (float)kPreparationAngelCell},
      .destination = {centre - 8.0f * scale, top - cell - 2.0f * scale, cell, cell}};
  return count;
}

typedef enum IndicatorMode { kIndicator_Normal, kIndicator_Forced, kIndicator_Off } IndicatorMode;

static struct {
  IndicatorMode mode;
  uint64_t started_ms;
  uint64_t drawn_ms;
  bool drawing;
  ArRenderTexture atlas;
  bool atlas_failed;
} s_indicator;

static struct {
  DevToolsReadbackProvider readback;
  char directory[512];
  unsigned frames;
} s_capture;

void PreparationIndicator_SetCapture(DevToolsReadbackProvider readback,
                                     const char *directory) {
  s_capture.readback = readback;
  s_capture.frames = 0;
  snprintf(s_capture.directory, sizeof s_capture.directory, "%s", directory ? directory : "");
}

static void CaptureFrame(uint64_t elapsed_ms) {
  if (!s_capture.directory[0] || !s_capture.readback.capture_rgb24) return;
  DevToolsRgb24Capture frame = {0};
  if (!s_capture.readback.capture_rgb24(s_capture.readback.context, &frame)) return;
  char path[600];
  snprintf(path, sizeof path, "%s/%04u-%06llu.ppm", s_capture.directory, s_capture.frames++,
           (unsigned long long)elapsed_ms);
  FILE *file = fopen(path, "wb");
  if (file) {
    fprintf(file, "P6\n%d %d\n255\n", frame.width, frame.height);
    for (int y = 0; y < frame.height; y++)
      fwrite(frame.pixels + (size_t)y * frame.pitch_bytes, 3, (size_t)frame.width, file);
    fclose(file);
  }
  if (frame.release) frame.release(frame.owner);
}

static bool EnsureAtlasTexture(ArRenderDevice *device) {
  if (ArRenderTexture_IsValid(s_indicator.atlas)) return true;
  if (s_indicator.atlas_failed || !s_has_art) return false;
  const ArRenderTextureDesc desc = {kPreparationAtlasWidth, kPreparationAtlasHeight,
                                    kArRenderPixelFormat_Argb8888, kArRenderTextureUsage_Static,
                                    kArRenderFilter_Nearest, kArRenderBlendMode_Alpha};
  if (!ArRenderDevice_CreateTexture(device, &desc, &s_indicator.atlas) ||
      !ArRenderDevice_UpdateTexture(device, s_indicator.atlas, NULL, s_atlas,
                                    kPreparationAtlasWidth * 4)) {
    ArRenderDevice_DestroyTexture(device, s_indicator.atlas);
    s_indicator.atlas = ArRenderTexture_Invalid();
    s_indicator.atlas_failed = true;
    return false;
  }
  return true;
}

static void DrawPlainBar(ArRenderDevice *device, int width, int height, uint64_t elapsed_ms) {
  const float unit = (float)(width < height ? width : height);
  const float track_width = unit * 0.6f;
  const float track_height = unit * 0.012f < 3 ? 3 : unit * 0.012f;
  const ArRenderRectF track = {(width - track_width) * 0.5f, (height - track_height) * 0.5f,
                               track_width, track_height};
  const float phase = (float)(elapsed_ms % (2 * kPreparationIndicatorSweepMs)) /
                      kPreparationIndicatorSweepMs;
  const float linear = phase < 1 ? phase : 2 - phase;
  const float eased = linear * linear * (3 - 2 * linear);
  const ArRenderRectF segment = {track.x + eased * track_width * 0.75f, track.y,
                                 track_width * 0.25f, track_height};
  ArRenderDevice_DrawSolidRect(device, &track, (ArRenderColorF){1, 1, 1, 0.15f},
                               kArRenderBlendMode_Alpha);
  ArRenderDevice_DrawSolidRect(device, &segment, (ArRenderColorF){0.95f, 0.78f, 0.35f, 1},
                               kArRenderBlendMode_Alpha);
}

static void Draw(ArRenderDevice *device, uint64_t elapsed_ms) {
  ArRenderTargetState state;
  if (!ArRenderDevice_CaptureTargetState(device, &state)) return;
  int width = 0, height = 0;
  /* Only over the window: a build can run while a module has a target bound. */
  if (!ArRenderTexture_IsValid(state.target) && ArRenderDevice_UseOutputCoordinates(device) &&
      ArRenderDevice_GetOutputSize(device, &width, &height) && width > 0 && height > 0 &&
      ArRenderDevice_Clear(device, (ArRenderColorF){0, 0, 0, 1})) {
    if (EnsureAtlasTexture(device)) {
      PreparationSprite sprites[kPreparationSpriteCapacity];
      const int count = PreparationIndicator_Layout(width, height, elapsed_ms, sprites);
      for (int i = 0; i < count; i++)
        ArRenderDevice_DrawTexture(device, s_indicator.atlas, &sprites[i].source,
                                   &sprites[i].destination);
    } else {
      DrawPlainBar(device, width, height, elapsed_ms);
    }
    CaptureFrame(elapsed_ms);
    ArRenderDevice_Present(device);
  }
  ArRenderDevice_EndTarget(device, &state);
}

void PreparationIndicator_Begin(ArRenderDevice *device) {
  (void)device;
  const char *mode = getenv("AR_PREPARATION_INDICATOR");
  s_indicator.mode = !mode ? kIndicator_Normal
                   : !strcmp(mode, "force") ? kIndicator_Forced
                   : !strcmp(mode, "off") ? kIndicator_Off
                                           : kIndicator_Normal;
  s_indicator.started_ms = HostClock_Milliseconds();
  s_indicator.drawn_ms = 0;
}

void PreparationIndicator_Tick(ArRenderDevice *device) {
  if (s_indicator.mode == kIndicator_Off || s_indicator.drawing || !device) return;
  const uint64_t now = HostClock_Milliseconds();
  if ((s_indicator.mode != kIndicator_Forced &&
       now - s_indicator.started_ms < kPreparationIndicatorDelayMs) ||
      now - s_indicator.drawn_ms < kPreparationIndicatorIntervalMs)
    return;
  s_indicator.drawing = true;
  Draw(device, now - s_indicator.started_ms);
  s_indicator.drawn_ms = now;
  s_indicator.drawing = false;
}

void PreparationIndicator_End(ArRenderDevice *device) {
  if (s_indicator.mode == kIndicator_Forced && device) {
    /* Each present waits for vsync, which paces this inspection hold. */
    const uint64_t hold_until = HostClock_Milliseconds() + kPreparationIndicatorForcedHoldMs;
    for (uint64_t now = HostClock_Milliseconds(); now < hold_until;
         now = HostClock_Milliseconds()) {
      s_indicator.drawn_ms = 0;
      PreparationIndicator_Tick(device);
    }
  }
  if (device) ArRenderDevice_DestroyTexture(device, s_indicator.atlas);
  s_indicator.atlas = ArRenderTexture_Invalid();
  s_indicator.atlas_failed = false;
}
