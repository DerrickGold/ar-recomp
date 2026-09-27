#include "save/save_file_dialog.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

typedef struct SaveFileRequest {
  SDL_AtomicInt references, ready;
  SaveFileDialogResult result;
  SaveError error;
  char path[kHostPathCapacity], location[kHostPathCapacity], extension[8];
  SDL_DialogFileFilter filter;
} SaveFileRequest;

/* Only the main thread accesses this pointer. The callback owns a separate
 * reference, so closing/restarting the menu cannot leave dangling userdata. */
static SaveFileRequest *s_request;

static void Release(SaveFileRequest *request) {
  if (SDL_AtomicDecRef(&request->references)) SDL_free(request);
}
static bool Fail(SaveError *error, const char *message) {
  if (error) snprintf(error->message, sizeof(error->message), "%s", message);
  return false;
}
static void SDLCALL Selected(void *context, const char *const *files, int filter) {
  (void)filter;
  SaveFileRequest *request = context;
  request->result = kSaveFileDialog_Cancelled;
  if (!files) {
    request->result = kSaveFileDialog_Failed;
    Fail(&request->error, SDL_GetError());
  } else if (files[0] && *files[0]) {
    const char *suffix = request->extension;
    const size_t length = strlen(files[0]), suffix_length = strlen(suffix);
    if (!suffix_length || (length > suffix_length && files[0][length - suffix_length - 1] == '.' &&
                           !SDL_strcasecmp(files[0] + length - suffix_length, suffix)))
      suffix = "";
    int size = snprintf(request->path, sizeof(request->path), "%s%s%s", files[0],
                        *suffix ? "." : "", suffix);
    request->result = kSaveFileDialog_Selected;
    if (size < 0 || (size_t)size >= sizeof(request->path)) {
      request->path[0] = 0;
      request->result = kSaveFileDialog_Failed;
      Fail(&request->error, "The selected save path is too long.");
    }
  }
  SDL_SetAtomicInt(&request->ready, 1);
  Release(request);
}

bool SaveFileDialog_Begin(SDL_Window *window, const char *extension, const char *location,
                          SaveError *error) {
  if (s_request) return Fail(error, "A save file picker is already open.");
  if (extension && strcmp(extension, "arsave") && strcmp(extension, "srm") &&
      strcmp(extension, "ini"))
    return Fail(error, "Unsupported save export format.");
  if (location && strlen(location) >= kHostPathCapacity)
    return Fail(error, "The save file location is too long.");
  SaveFileRequest *request = SDL_calloc(1, sizeof(*request));
  if (!request) return Fail(error, "Cannot open the save file picker.");
  SDL_SetAtomicInt(&request->references, 2);
  if (location) snprintf(request->location, sizeof(request->location), "%s", location);
  if (extension) snprintf(request->extension, sizeof(request->extension), "%s", extension);
  request->filter =
      (SDL_DialogFileFilter){extension ? extension : "ActRaiser (.arsave, .srm, .ini)",
                             extension ? request->extension : "arsave;srm;ini"};
  /* Use request-owned storage even when the native implementation completes later. */
  if (extension) request->filter.name = request->extension;
  s_request = request;
  if (extension)
    SDL_ShowSaveFileDialog(Selected, request, window, &request->filter, 1, request->location);
  else
    SDL_ShowOpenFileDialog(Selected, request, window, &request->filter, 1, request->location,
                           false);
  return true;
}

SaveFileDialogResult SaveFileDialog_Poll(char *path, size_t capacity, SaveError *error) {
  if (!s_request) return kSaveFileDialog_Cancelled;
  if (!SDL_GetAtomicInt(&s_request->ready)) return kSaveFileDialog_Pending;
  SaveFileRequest *request = s_request;
  s_request = NULL;
  SaveFileDialogResult result = request->result;
  if (error) *error = request->error;
  if (result == kSaveFileDialog_Selected) {
    if (!path || capacity <= strlen(request->path)) {
      Fail(error, "The selected save path is too long.");
      result = kSaveFileDialog_Failed;
    } else {
      snprintf(path, capacity, "%s", request->path);
    }
  }
  Release(request);
  return result;
}

void SaveFileDialog_Cancel(void) {
  if (s_request) Release(s_request);
  s_request = NULL;
}
