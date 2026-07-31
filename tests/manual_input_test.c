/* The manual reader's control scheme and its decode budget.
 *
 * The reader itself owns textures, a JPEG decoder and a window, none of which
 * can be asserted here. What CAN be asserted is what its inputs mean and how it
 * spends its frame -- so both live in manual_input.c, and this drives them
 * directly rather than through a renderer that would have to exist first.
 *
 * The load-bearing case is TestOneDecodePerFrame: a 739x1080 baseline JPEG costs
 * ~4.7 ms and the budget is refresh/2 -- 5.6 ms on a Steam Deck -- so decoding
 * every page a turn reveals in one frame drops that frame, on exactly the frame
 * the animation starts.
 */

#include "manual_input.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
      g_failures++;                                                        \
    }                                                                      \
  } while (0)

/* ── Keyboard ─────────────────────────────────────────────────────────────── */

/* THE ZOOM RULE, which is the only thing here with any subtlety.
 *
 * At fit there is no overhang, so a pan is a silent no-op and an arrow key that
 * panned would look broken. Zoomed in, an arrow that paged instead of panning
 * would make the reader unusable at exactly the magnification someone needed it
 * at. So the same key means different things, and both meanings are asserted. */
static void TestArrowsPageAtFitAndPanWhenZoomed(void) {
  CHECK(ManualInput_KeyIntent(SDLK_RIGHT, false) == kManualIntent_PageForward);
  CHECK(ManualInput_KeyIntent(SDLK_LEFT, false) == kManualIntent_PageBack);
  CHECK(ManualInput_KeyIntent(SDLK_RIGHT, true) == kManualIntent_PanRight);
  CHECK(ManualInput_KeyIntent(SDLK_LEFT, true) == kManualIntent_PanLeft);

  /* Vertical arrows pan only. At fit they do NOTHING rather than paging, which
   * would put two gestures for the same action on different axes. */
  CHECK(ManualInput_KeyIntent(SDLK_UP, false) == kManualIntent_None);
  CHECK(ManualInput_KeyIntent(SDLK_DOWN, false) == kManualIntent_None);
  CHECK(ManualInput_KeyIntent(SDLK_UP, true) == kManualIntent_PanUp);
  CHECK(ManualInput_KeyIntent(SDLK_DOWN, true) == kManualIntent_PanDown);
}

/* A way to turn pages that does NOT change meaning with the zoom, or a reader
 * zoomed into a map has no way forward but to zoom out first. */
static void TestSomeKeysAlwaysPage(void) {
  const SDL_Keycode forward[] = { SDLK_PAGEDOWN, SDLK_SPACE };
  const SDL_Keycode back[] = { SDLK_PAGEUP, SDLK_BACKSPACE };
  for (int zoomed = 0; zoomed <= 1; zoomed++) {
    for (size_t i = 0; i < sizeof forward / sizeof forward[0]; i++)
      CHECK(ManualInput_KeyIntent(forward[i], zoomed != 0) ==
            kManualIntent_PageForward);
    for (size_t i = 0; i < sizeof back / sizeof back[0]; i++)
      CHECK(ManualInput_KeyIntent(back[i], zoomed != 0) ==
            kManualIntent_PageBack);
  }
}

static void TestZoomAndJumpKeys(void) {
  for (int zoomed = 0; zoomed <= 1; zoomed++) {
    const bool z = zoomed != 0;
    CHECK(ManualInput_KeyIntent(SDLK_EQUALS, z) == kManualIntent_ZoomIn);
    CHECK(ManualInput_KeyIntent(SDLK_KP_PLUS, z) == kManualIntent_ZoomIn);
    CHECK(ManualInput_KeyIntent(SDLK_MINUS, z) == kManualIntent_ZoomOut);
    CHECK(ManualInput_KeyIntent(SDLK_KP_MINUS, z) == kManualIntent_ZoomOut);
    CHECK(ManualInput_KeyIntent(SDLK_0, z) == kManualIntent_ZoomReset);
    CHECK(ManualInput_KeyIntent(SDLK_HOME, z) == kManualIntent_First);
    CHECK(ManualInput_KeyIntent(SDLK_END, z) == kManualIntent_Last);
  }
}

/* Escape is NOT mapped here, and that is deliberate rather than an omission:
 * the reader handles it before consulting this table, because leaving must keep
 * working even if the mapping is wrong. Asserting it stays unmapped is what
 * stops someone "completing" the table and moving that guarantee. */
static void TestUnmappedKeysMeanNothing(void) {
  const SDL_Keycode unmapped[] = {
    SDLK_ESCAPE, SDLK_F1, SDLK_A, SDLK_TAB, SDLK_RETURN, SDLK_LSHIFT,
  };
  for (size_t i = 0; i < sizeof unmapped / sizeof unmapped[0]; i++) {
    CHECK(ManualInput_KeyIntent(unmapped[i], false) == kManualIntent_None);
    CHECK(ManualInput_KeyIntent(unmapped[i], true) == kManualIntent_None);
  }
}

/* ── Gamepad ──────────────────────────────────────────────────────────────── */

static void TestPadMirrorsTheKeyboardsLogic(void) {
  /* The shoulders are the pad's PageUp/PageDown: they page at ANY zoom, which
   * is what leaves the d-pad free to follow the same rule the arrows do. */
  for (int zoomed = 0; zoomed <= 1; zoomed++) {
    const bool z = zoomed != 0;
    CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, z) ==
          kManualIntent_PageForward);
    CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, z) ==
          kManualIntent_PageBack);
  }

  CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false) ==
        kManualIntent_PageForward);
  CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true) ==
        kManualIntent_PanRight);
  CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_DPAD_LEFT, false) ==
        kManualIntent_PageBack);
  CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_DPAD_LEFT, true) ==
        kManualIntent_PanLeft);
  CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_DPAD_UP, false) ==
        kManualIntent_None);
  CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_DPAD_UP, true) ==
        kManualIntent_PanUp);

  /* The d-pad and the arrows must agree, or the reader has two control schemes
   * and the player has to learn which device they are holding. */
  CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false) ==
        ManualInput_KeyIntent(SDLK_RIGHT, false));
  CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true) ==
        ManualInput_KeyIntent(SDLK_RIGHT, true));
  CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_DPAD_UP, true) ==
        ManualInput_KeyIntent(SDLK_UP, true));

  /* East is "back" everywhere else in this menu, and the manual does not get to
   * disagree; a reader you cannot leave with the usual button is a trap. */
  CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_EAST, false) ==
        kManualIntent_Close);
  CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_BACK, false) ==
        kManualIntent_Close);
  CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_SOUTH, false) ==
        kManualIntent_ZoomIn);
  CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_INVALID, false) ==
        kManualIntent_None);
}

/* ── The hint line ────────────────────────────────────────────────────────── */

/* A HINT LINE THAT NAMES THE WRONG DEVICE IS WORSE THAN NO HINT LINE. On a
 * handheld the pad is the only input there is, and "ESC BACK" is then
 * instructions for hardware nobody is holding.
 *
 * These tie the LABEL to the MAPPING: every control the text names has to be one
 * ManualInput_*Intent actually implements, and the close button it names has to
 * be the one that actually closes. That is what makes this more than a spell
 * check -- change the mapping without the caption and it goes red. */
static bool Mentions(const char *text, const char *needle) {
  return strstr(text, needle) != NULL;
}

static void TestTheHintNamesTheDeviceInHand(void) {
  for (int zoomed = 0; zoomed <= 1; zoomed++) {
    const bool z = zoomed != 0;
    const char *kbd = ManualInput_HintText(kManualHintDevice_Keyboard, z);
    const char *pad = ManualInput_HintText(kManualHintDevice_Gamepad, z);
    CHECK(kbd && kbd[0]);
    CHECK(pad && pad[0]);
    CHECK(strcmp(kbd, pad) != 0);

    /* Neither line may name the other device's controls. This is the actual
     * defect being guarded: the reader shipped with the keyboard line shown to
     * everyone, pad included. */
    CHECK(!Mentions(pad, "ESC"));
    CHECK(!Mentions(pad, "CLICK"));
    CHECK(!Mentions(pad, "ARROWS"));
    CHECK(!Mentions(kbd, "D-PAD"));
    CHECK(!Mentions(kbd, "L/R"));

    /* Each names the button that really closes, per the mapping above. */
    CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_EAST, z) ==
          kManualIntent_Close);
    CHECK(Mentions(pad, "B BACK"));
    CHECK(Mentions(kbd, "ESC BACK"));

    /* And each names a way to PAGE, which is the one thing a reader must be
     * able to do. The shoulders page at any zoom; the arrows only when not. */
    CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, z) ==
          kManualIntent_PageForward);
    CHECK(Mentions(pad, "PAGE"));
    CHECK(Mentions(kbd, "PAGE"));
  }

  /* Zoom changes the CONTROLS, so it has to change the words. When zoomed the
   * arrows and the d-pad pan, and the line must say so rather than still
   * claiming they page. */
  const char *kbd_fit = ManualInput_HintText(kManualHintDevice_Keyboard, false);
  const char *kbd_zoom = ManualInput_HintText(kManualHintDevice_Keyboard, true);
  CHECK(strcmp(kbd_fit, kbd_zoom) != 0);
  CHECK(ManualInput_KeyIntent(SDLK_RIGHT, true) == kManualIntent_PanRight);
  CHECK(Mentions(kbd_zoom, "ARROWS PAN"));
  CHECK(ManualInput_KeyIntent(SDLK_RIGHT, false) == kManualIntent_PageForward);
  CHECK(Mentions(kbd_fit, "ARROWS PAGE"));
  /* Zoomed, the keyboard line must still offer a way to turn -- the arrows are
   * busy panning, so PageUp/PageDown is the only route and has to be named. */
  CHECK(ManualInput_KeyIntent(SDLK_PAGEDOWN, true) == kManualIntent_PageForward);
  CHECK(Mentions(kbd_zoom, "PGUP/PGDN"));

  const char *pad_fit = ManualInput_HintText(kManualHintDevice_Gamepad, false);
  const char *pad_zoom = ManualInput_HintText(kManualHintDevice_Gamepad, true);
  CHECK(strcmp(pad_fit, pad_zoom) != 0);
  CHECK(ManualInput_PadIntent(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true) ==
        kManualIntent_PanRight);
  CHECK(Mentions(pad_zoom, "D-PAD PANS"));

  /* The ROM's dialog font has no lower case, and the overlay draws '?' for any
   * glyph it lacks -- so a lower-case caption renders as a row of question
   * marks. Cheap to assert, and invisible until someone looks at a screen. */
  for (int device = 0; device <= 1; device++) {
    for (int zoomed = 0; zoomed <= 1; zoomed++) {
      const char *text =
          ManualInput_HintText((ManualHintDevice)device, zoomed != 0);
      for (int i = 0; text[i]; i++)
        CHECK(!(text[i] >= 'a' && text[i] <= 'z'));
    }
  }
}

/* ── The decode budget ────────────────────────────────────────────────────── */

typedef struct CacheFixture {
  int resident[8];
  int count;
  int queries;
} CacheFixture;

static bool FixtureCached(int page, void *user) {
  CacheFixture *fixture = (CacheFixture *)user;
  fixture->queries++;
  for (int i = 0; i < fixture->count; i++)
    if (fixture->resident[i] == page) return true;
  return false;
}

/* ONE PAGE PER FRAME, IN PRIORITY ORDER.
 *
 * The caller lists the leaf first because it is the page in motion and the one
 * whose absence shows. A budget that returned the first UNCACHED page in index
 * order, or that decoded all three, would both pass a test that only checked
 * "something got decoded". */
static void TestOneDecodePerFrame(void) {
  CacheFixture fixture;
  memset(&fixture, 0, sizeof fixture);

  /* A turn's frame: leaf first, then the revealed side, then the settled side. */
  const int wanted[] = { 7, 8, 6 };

  /* Nothing resident: the LEAF is chosen, not the lowest-numbered page. */
  CHECK(ManualInput_NextDecode(wanted, 3, FixtureCached, &fixture) == 7);

  /* With the leaf resident, the next frame moves to the revealed page -- and
   * still returns exactly one. */
  fixture.resident[fixture.count++] = 7;
  CHECK(ManualInput_NextDecode(wanted, 3, FixtureCached, &fixture) == 8);

  fixture.resident[fixture.count++] = 8;
  CHECK(ManualInput_NextDecode(wanted, 3, FixtureCached, &fixture) == 6);

  /* All resident: nothing to do, and the frame costs no decode at all. This is
   * the steady state -- a reader sitting on a page must not be re-decoding it. */
  fixture.resident[fixture.count++] = 6;
  CHECK(ManualInput_NextDecode(wanted, 3, FixtureCached, &fixture) == -1);
}

/* A cover has no facing page, so the frame carries a -1. Treating it as a page
 * index would decode page -1 or, worse, index the array with it. */
static void TestEmptySidesAreSkipped(void) {
  CacheFixture fixture;
  memset(&fixture, 0, sizeof fixture);
  const int cover[] = { -1, 0, -1 };
  CHECK(ManualInput_NextDecode(cover, 3, FixtureCached, &fixture) == 0);

  const int nothing[] = { -1, -1, -1 };
  CHECK(ManualInput_NextDecode(nothing, 3, FixtureCached, &fixture) == -1);
  /* And an empty side is never even asked about. */
  const int before = fixture.queries;
  CHECK(ManualInput_NextDecode(nothing, 3, FixtureCached, &fixture) == -1);
  CHECK(fixture.queries == before);
}

/* A PAGE THAT CANNOT BE DECODED MUST COUNT AS RESIDENT.
 *
 * Otherwise it is uncached forever, wins the priority order on every single
 * frame, and the reader spends its entire decode budget retrying a page that
 * will never succeed -- so the pages that WOULD have decoded never do, and the
 * reader shows a blank spread indefinitely rather than a single missing page.
 * The reader's own probe folds its failure list in for exactly this reason. */
static void TestAPermanentlyFailedPageDoesNotStarveTheOthers(void) {
  CacheFixture fixture;
  memset(&fixture, 0, sizeof fixture);
  /* Page 7 is the failed one, reported resident the way the reader reports it. */
  fixture.resident[fixture.count++] = 7;
  const int wanted[] = { 7, 8, 6 };
  CHECK(ManualInput_NextDecode(wanted, 3, FixtureCached, &fixture) == 8);
}

static void TestDegenerateArguments(void) {
  CacheFixture fixture;
  memset(&fixture, 0, sizeof fixture);
  const int wanted[] = { 1, 2 };
  CHECK(ManualInput_NextDecode(NULL, 2, FixtureCached, &fixture) == -1);
  CHECK(ManualInput_NextDecode(wanted, 2, NULL, &fixture) == -1);
  CHECK(ManualInput_NextDecode(wanted, 0, FixtureCached, &fixture) == -1);
  CHECK(ManualInput_NextDecode(wanted, -1, FixtureCached, &fixture) == -1);
}

int main(void) {
  TestArrowsPageAtFitAndPanWhenZoomed();
  TestSomeKeysAlwaysPage();
  TestZoomAndJumpKeys();
  TestUnmappedKeysMeanNothing();
  TestPadMirrorsTheKeyboardsLogic();
  TestTheHintNamesTheDeviceInHand();

  TestOneDecodePerFrame();
  TestEmptySidesAreSkipped();
  TestAPermanentlyFailedPageDoesNotStarveTheOthers();
  TestDegenerateArguments();

  if (g_failures) {
    printf("manual_input_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("manual_input_test: all checks passed\n");
  return 0;
}
