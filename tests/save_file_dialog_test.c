#include "save/save_file_dialog.h"
#include "support/test_assert.h"
#include <SDL3/SDL.h>
#include <string.h>

static SDL_DialogFileCallback s_callback;
static void *s_context;
static const SDL_DialogFileFilter *s_filter;
static const char *s_location;
static bool s_saving;

/* Exercise the real callback/lifetime code without opening a native window. */
void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void *context, SDL_Window *window,
                            const SDL_DialogFileFilter *filters, int count, const char *location,
                            bool many) {
  (void)window;
  assert(count == 1 && !many);
  s_callback = callback;
  s_context = context;
  s_filter = filters;
  s_location = location;
  s_saving = false;
}
void SDL_ShowSaveFileDialog(SDL_DialogFileCallback callback, void *context, SDL_Window *window,
                            const SDL_DialogFileFilter *filters, int count, const char *location) {
  SDL_ShowOpenFileDialog(callback, context, window, filters, count, location, false);
  s_saving = true;
}
static int SDLCALL Worker(void *unused) {
  (void)unused;
  const char *files[] = {"/tmp/新しいゲーム.arsave", NULL};
  s_callback(s_context, files, 0);
  return 0;
}
int main(void) {
  SaveError error = {{0}};
  char path[kHostPathCapacity] = "unchanged";
  assert(SaveFileDialog_Begin(NULL, NULL, "saves", &error));
  assert(!s_saving && !strcmp(s_filter->pattern, "arsave;srm;ini"));
  assert(!strcmp(s_location, "saves"));
  assert(!SaveFileDialog_Begin(NULL, "srm", NULL, &error));
  assert(SaveFileDialog_Poll(path, sizeof(path), &error) == kSaveFileDialog_Pending);
  assert(!strcmp(path, "unchanged"));
  SDL_Thread *thread = SDL_CreateThread(Worker, "file-choice-test", NULL);
  assert(thread);
  SDL_WaitThread(thread, NULL);
  assert(SaveFileDialog_Poll(path, sizeof(path), &error) == kSaveFileDialog_Selected);
  assert(!strcmp(path, "/tmp/新しいゲーム.arsave"));

  assert(SaveFileDialog_Begin(NULL, "srm", "new.srm", &error));
  assert(s_saving && !strcmp(s_filter->pattern, "srm"));
  const char *chosen[] = {"new", NULL};
  s_callback(s_context, chosen, 0);
  assert(SaveFileDialog_Poll(path, sizeof(path), &error) == kSaveFileDialog_Selected);
  assert(!strcmp(path, "new.srm"));
  assert(SaveFileDialog_Begin(NULL, "ini", NULL, &error));
  chosen[0] = "new.INI";
  s_callback(s_context, chosen, 0);
  assert(SaveFileDialog_Poll(path, sizeof(path), &error) == kSaveFileDialog_Selected);
  assert(!strcmp(path, "new.INI"));

  assert(SaveFileDialog_Begin(NULL, NULL, NULL, &error));
  const char *cancelled[] = {NULL};
  s_callback(s_context, cancelled, -1);
  assert(SaveFileDialog_Poll(path, sizeof(path), &error) == kSaveFileDialog_Cancelled);
  assert(SaveFileDialog_Begin(NULL, NULL, NULL, &error));
  const char *empty[] = {"", NULL};
  s_callback(s_context, empty, -1);
  assert(SaveFileDialog_Poll(path, sizeof(path), &error) == kSaveFileDialog_Cancelled);
  assert(SaveFileDialog_Begin(NULL, NULL, NULL, &error));
  SDL_SetError("Native picker unavailable");
  s_callback(s_context, NULL, -1);
  assert(SaveFileDialog_Poll(path, sizeof(path), &error) == kSaveFileDialog_Failed);
  assert(strstr(error.message, "Native picker unavailable"));

  assert(SaveFileDialog_Begin(NULL, NULL, NULL, &error));
  SDL_DialogFileCallback late_callback = s_callback;
  void *late_context = s_context;
  SaveFileDialog_Cancel();
  assert(SaveFileDialog_Begin(NULL, NULL, NULL, &error));
  late_callback(late_context, chosen, 0);
  assert(SaveFileDialog_Poll(path, sizeof(path), &error) == kSaveFileDialog_Pending);
  s_callback(s_context, cancelled, -1);
  SaveFileDialog_Cancel();

  assert(SaveFileDialog_Begin(NULL, "arsave", NULL, &error));
  char too_long[kHostPathCapacity + 1];
  memset(too_long, 'x', sizeof(too_long) - 1);
  too_long[sizeof(too_long) - 1] = 0;
  chosen[0] = too_long;
  s_callback(s_context, chosen, 0);
  assert(SaveFileDialog_Poll(path, sizeof(path), &error) == kSaveFileDialog_Failed);
  assert(!SaveFileDialog_Begin(NULL, "invalid", NULL, &error));
  assert(!SaveFileDialog_Begin(NULL, NULL, too_long, &error));
  return 0;
}
