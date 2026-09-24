#ifndef ACTRAISER_REGIONAL_MEDIA_H
#define ACTRAISER_REGIONAL_MEDIA_H
#include "regional/media/regional_media.h"
#include "regional/media/regional_actor_art.h"

/* Application boot/shutdown boundary. Views must already be validated by the
 * media parser; their bytes remain immutable until ClearDonors after gameplay.
 * No paths, host renderer handles or CPU objects cross this boundary. */
bool ActRaiserRegionalMedia_AddDonor(const ArRegionalMediaView *view);
void ActRaiserRegionalMedia_ClearDonors(void);
/* Parsed once on donor registration; immutable until ClearDonors. */
const ArRegionalActorArtView *ActRaiserRegionalMedia_ActorArt(void);
uint8_t ActRaiserRegionalMedia_AvailableArtwork(void);
ArRegionalMediaBytes ActRaiserRegionalMedia_DeathHeimCharacters(bool enabled,uint16_t scene);
/* PAL donors share these exact graphics; English, German, then French is a
 * stable preference order, independent of the selected language pack. */
ArRegionalMediaBytes ActRaiserRegionalMedia_ActionHealth(bool enabled);
/* Returns the complete 256-byte room-entry window. Dirty updates use its
 * first128 bytes. spell0 selects the fifth window; spell1..4 are actual spells. */
ArRegionalMediaBytes ActRaiserRegionalMedia_ActionHud(bool enabled,unsigned spell);
typedef struct ActRaiserTitleArt {
  ArRegionalMediaBytes characters,map,palette;
} ActRaiserTitleArt;
/* All three views are present or all absent. Copyright and font are not part
 * of the donor; callers retain the native title's animation and text paths. */
ActRaiserTitleArt ActRaiserRegionalMedia_Title(bool enabled);
/* rule0/1 identify song-table entries9/12, not their one-based Music Mode IDs. */
ArRegionalMediaBytes ActRaiserRegionalMedia_Sequence(unsigned rule,bool enabled);
uint8_t ActRaiserRegionalMedia_AvailableSequences(void);
typedef enum ActRaiserTownArtBank {
  kActRaiserTownArt_Early,
  kActRaiserTownArt_Late,
  kActRaiserTownArt_Objects,
} ActRaiserTownArtBank;
enum { kActRaiserTownArtMaximumSpans = 4 };
typedef struct ActRaiserTownArtSpan {
  uint16_t offset; /* Byte offset in the selected 16 KiB character bank. */
  ArRegionalMediaBytes bytes;
} ActRaiserTownArtSpan;
/* Bounded, ordered sparse replacements. No changes to bank progression,
 * composition metadata, palettes or other regional/unused characters. Views
 * have the same immutable boot lifetime as the registered donor. */
unsigned ActRaiserRegionalMedia_TownSpans(uint8_t mask,ActRaiserTownArtBank bank,
    ActRaiserTownArtSpan out[kActRaiserTownArtMaximumSpans]);
#endif
