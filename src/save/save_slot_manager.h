#ifndef AR_SAVE_SLOT_MANAGER_H
#define AR_SAVE_SLOT_MANAGER_H
/* SaveSlotManager: what the settings overlay's save-slot pages need: slot
 * details, new-game drafts and their regional-rule edits, handed over as
 * copies and host operations, never live SRAM or managed storage paths.
 * Phase: host.
 * Tests: tests/save_slots_test.c */
#include "save/save_slots.h"
#include "save/save_editor.h"
#include "save/save_file_dialog.h"
#include "regional/session/regional_session.h"
#include "settings_overlay/regional/regional_menu.h"

typedef struct SaveSlotDetails {
  SaveSlotState state;
  uint64_t fingerprint, saved_at;
  bool approximate_time, legacy, prepared;
  SaveSummary summary;
  ActRaiserRegionalRulesView regions;
  RandomizerConfig randomizer;
  SaveError error;
} SaveSlotDetails;
typedef struct SaveSlotCollection {
  unsigned active;
  bool writable;
  SaveError error;
  SaveSlotDetails slots[kSaveSlotCount];
} SaveSlotCollection;

bool SaveSlotManager_Inspect(const SaveSlots *slots,unsigned slot,SaveSlotDetails *out);
bool SaveSlotManager_Draft(ArRegionalSession *out,unsigned slot,const uint8_t id[16],
    const ArRegionalRules *rules,const RandomizerConfig *randomizer);
bool SaveSlotManager_View(const ArRegionalSession *session, bool draft,
                          ActRaiserRegionalRulesView *out);
bool SaveSlotManager_Edit(ArRegionalSession *draft,const OverlayRegionRow *row,int choice);
bool SaveSlotManager_ReadDraft(const SaveSlots *slots, unsigned slot, ArRegionalSession *out,
                               SaveError *error);

/* The picker returns external paths; managed slot storage stays behind hooks. */
typedef struct SettingsOverlaySaveSlotHooks {
  bool (*scan)(SaveSlotCollection *out);
  bool (*draft)(unsigned slot,ArRegionalSession *out,SaveError *error);
  bool (*edit)(ArRegionalSession *draft,const OverlayRegionRow *row,int choice);
  bool (*view)(const ArRegionalSession *draft,ActRaiserRegionalRulesView *out);
  bool (*start)(unsigned slot,uint64_t fingerprint,const ArRegionalSession *draft,SaveError *error);
  bool (*choose_file)(SettingAction action, SaveError *error);
  SaveFileDialogResult (*poll_file)(char *path, size_t capacity, SaveError *error);
  void (*cancel_file)(void);
  bool (*file_action)(SettingAction action, const char *path, SaveError *error);
  /* Called only after the active slot and its fingerprint were confirmed. */
  bool (*apply_edits)(SettingAction action, SaveError *error);
} SettingsOverlaySaveSlotHooks;
#endif
