#ifndef AR_SAVE_SLOT_MANAGER_H
#define AR_SAVE_SLOT_MANAGER_H
#include "save_slots.h"
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
bool SaveSlotManager_View(const ArRegionalSession *session,bool draft,ActRaiserRegionalRulesView *out);
bool SaveSlotManager_Edit(ArRegionalSession *draft,const OverlayRegionRow *row,int choice);
bool SaveSlotManager_ReadDraft(const SaveSlots *slots,unsigned slot,ArRegionalSession *out,SaveError *error);

/* Overlay receives copies and host operations, never active SRAM or paths. */
typedef struct SettingsOverlaySaveSlotHooks {
  bool (*scan)(SaveSlotCollection *out);
  bool (*draft)(unsigned slot,ArRegionalSession *out,SaveError *error);
  bool (*edit)(ArRegionalSession *draft,const OverlayRegionRow *row,int choice);
  bool (*view)(const ArRegionalSession *draft,ActRaiserRegionalRulesView *out);
  bool (*start)(unsigned slot,uint64_t fingerprint,const ArRegionalSession *draft,SaveError *error);
} SettingsOverlaySaveSlotHooks;
#endif
