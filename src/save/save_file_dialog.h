#ifndef AR_SAVE_FILE_DIALOG_H
#define AR_SAVE_FILE_DIALOG_H
/* Native save-file selection, with callback lifetime isolated from the game
 * session. The save-slot host supplies the window; the menu polls the result. */

#include "save/save_system.h"

struct SDL_Window;
typedef enum SaveFileDialogResult {
  kSaveFileDialog_Pending,
  kSaveFileDialog_Cancelled,
  kSaveFileDialog_Selected,
  kSaveFileDialog_Failed,
} SaveFileDialogResult;

/* Main-thread owner of one asynchronous native picker. A NULL extension opens
 * an import picker; otherwise choose an arsave/srm/ini export destination.
 * Callbacks only copy results; they never touch game, save or menu state. */
bool SaveFileDialog_Begin(struct SDL_Window *window, const char *extension, const char *location,
                          SaveError *error);
SaveFileDialogResult SaveFileDialog_Poll(char *path, size_t capacity, SaveError *error);
/* Releases the menu's interest; a late native callback safely discards its result. */
void SaveFileDialog_Cancel(void);

#endif
