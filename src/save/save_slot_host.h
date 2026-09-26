#ifndef AR_SAVE_SLOT_HOST_H
#define AR_SAVE_SLOT_HOST_H
/* SaveSlotHost: this session's save slots. At boot it opens the slots and
 * attaches the battery save to the destination slot (headless, replay and
 * explicit-path runs bypass the slots and use fixed save paths), initializes
 * that slot's regional campaign, and installs the hooks that route storage
 * commits, regional settings and the overlay's save pages here. At exit it
 * flushes and closes the slots. Phase: host (main thread). */

#include <stdbool.h>

/* Opens the slots unless this run bypasses them, then attaches and loads the
 * battery save and applies armed save-editor boot edits. Dies rather than let
 * a fresh start overwrite recoverable progress. */
void SaveSlotHost_AttachBatterySave(bool headless);
/* Initializes the destination slot's regional campaign and stages a new game
 * prepared there. */
void SaveSlotHost_InitializeRegionalCampaign(void);
/* Confirms the slot switch, then installs the storage hooks, the regional
 * settings writer and the settings overlay's save-slot hooks. */
void SaveSlotHost_InstallHooks(void);
/* Coalesced after completed game ticks: persist game-originated battery
 * changes, report failures once, and stop after five seconds of failed writes.
 * Recording/replay protection is enforced here, including at shutdown. */
void SaveSlotHost_AfterTicks(void);
/* Flush only game-originated changes before tearing down the runner. Session
 * editor changes remain transient. False reports a failed battery write. */
bool SaveSlotHost_FlushBatterySave(void);
/* Flushes and closes the slots; false when the flush failed. */
bool SaveSlotHost_Close(void);

#endif  /* AR_SAVE_SLOT_HOST_H */
