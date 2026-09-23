#include "regional/regional_campaign.h"
#include "host/campaign_identity.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#define REMOVE_DIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MAKE_DIR(p) mkdir(p, 0700)
#define REMOVE_DIR(p) rmdir(p)
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Identity(void *context, uint8_t id[16]) {
  memset(id, 0, 16);
  id[0] = ++*(uint8_t *)context;
  return true;
}
static void Remove(const char *path) {
  char sidecar[256];
  remove(path);
  snprintf(sidecar, sizeof(sidecar), "%s.archeckpoint", path);
  remove(sidecar);
  snprintf(sidecar, sizeof(sidecar), "%s.tmp", path);
  REMOVE_DIR(sidecar); remove(sidecar);
}
static void Run(SaveBackend backend) {
  const char *native = "regional-campaign-test.srm", *ini = "regional-campaign-test.ini";
  const char *path = backend == kSaveBackend_Ini ? ini : native;
  const char *donor = "regional-campaign-import.srm";
  SaveFileFormat format = backend == kSaveBackend_Ini ? kSaveFileFormat_Ini : kSaveFileFormat_NativeSrm;
  Remove(native); Remove(ini); Remove(donor);
  uint8_t image[kActRaiserSramSize] = {0}, durable[kActRaiserSramSize], disk[kActRaiserSramSize];
  Save_RecomputeChecksum(image);
  SaveError error = {{0}};
  CHECK(Save_WriteFile(format, path, image, &error));
  CHECK(SaveSystem_Attach(image, sizeof(image), backend, native, ini, &error));
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(SaveSystem_CopyDurableImage(durable));
  uint8_t sequence = 0;
  ArRegionalCampaign campaign;
  ArRegionalCampaign_Init(&campaign, 0, Identity, &sequence);
  SaveCommitHost host = ArRegionalCampaign_SaveHost(&campaign);
  CHECK(SaveSystem_SetCommitHost(&host));
  CHECK(ArRegionalCampaign_Continue(&campaign, path, durable, &error));
  CHECK(campaign.active.requested.costs.source[0] == kArRegionalSource_US);
  ArRegionalSession loaded;
  CHECK(ArRegionalSession_Load(&loaded, 0, path, durable, &error) == kSaveCheckpoint_Missing);
  ArRegionalCostPolicy jp, eu;
  ArRegionalCosts_Init(&jp, kArRegionalSource_Japan);
  ArRegionalCosts_Init(&eu, kArRegionalSource_Europe);
  CHECK(ArRegionalCampaign_NewGame(&campaign, &jp, &error));
  CHECK(ArRegionalSession_RequestTimers(&campaign.active, campaign.active.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestRetryScore(&campaign.active, campaign.active.revision, kArRegionalSource_Japan));
  ArRegionalSession saving = campaign.active;
  CHECK(ArRegionalSession_Load(&loaded, 0, path, durable, &error) == kSaveCheckpoint_Missing);
  CHECK(SaveSystem_BeginNativeWrite(&error));
  image[100] = 1;
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  Save_RecomputeChecksum(image);
  CHECK(SaveSystem_EndNativeWrite(true, &error));
  CHECK(campaign.pending_valid);
  CHECK(ArRegionalCampaign_NewGame(&campaign, &eu, &error));
  CHECK(campaign.active.requested.timers.source[0] == kArRegionalSource_US);
  CHECK(Save_LoadFile(format, path, disk, &error));
  CHECK(!memcmp(disk, durable, sizeof(disk)));
  char blocked[256];
  snprintf(blocked, sizeof(blocked), "%s.tmp", path);
  CHECK(MAKE_DIR(blocked) == 0);
  CHECK(!SaveSystem_AutoPersistIfChanged(&error));
  CHECK(campaign.pending_valid);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, durable, &error) == kSaveCheckpoint_Missing);
  CHECK(REMOVE_DIR(blocked) == 0);
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(!campaign.pending_valid);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded, &saving, sizeof(saving)));
  CHECK(campaign.active.requested.costs.source[0] == kArRegionalSource_Europe);
  /* A marker outside the checksum belongs to the durable JP game, not the
   * unsaved EU one. No direct Save_WriteFile bypass of companion rotation. */
  image[0x1ff0] ^= 1;
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(loaded.campaign, saving.campaign, 16));
  SaveEditRequest edits;
  SaveEditRequest_Clear(&edits); edits.master_level = 5;
  CHECK(SaveSystem_ApplyEdits(&edits, true, true, false, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.costs.source[0] == kArRegionalSource_Japan);
  CHECK(loaded.requested.timers.source[0] == kArRegionalSource_Japan);
  CHECK(loaded.requested.retry_score == kArRegionalSource_Japan);

  /* Foreign import gets its own identity/provenance, never the running game's. */
  memset(disk, 0, sizeof(disk)); Save_RecomputeChecksum(disk);
  CHECK(ArRegionalSession_Save(&campaign.active, kSaveFileFormat_NativeSrm, donor, NULL, disk, &error));
  CHECK(SaveSystem_Import(donor, false, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded, &campaign.active, sizeof(loaded)));
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(!campaign.active_valid && !campaign.pending_valid);
  CHECK(SaveSystem_CopyDurableImage(durable));
  CHECK(ArRegionalCampaign_Continue(&campaign, path, durable, &error));
  CHECK(campaign.active.requested.costs.source[0] == kArRegionalSource_Europe);

  /* Same-image story saves still commit pending metadata; SRAM memcmp alone
   * must not swallow a region-only change. */
  CHECK(ArRegionalSession_RequestCosts(&campaign.active, campaign.active.revision,
                                      kArRegionalCostGroup_Miracles, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestTimers(&campaign.active, campaign.active.revision, kArRegionalSource_Japan));
  CHECK(SaveSystem_BeginNativeWrite(&error));
  CHECK(SaveSystem_EndNativeWrite(true, &error));
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded, &campaign.active, sizeof(loaded)));
  disk[0] ^= 1; Save_RecomputeChecksum(disk);
  CHECK(!ArRegionalCampaign_Continue(&campaign, path, disk, &error));
  CHECK(!campaign.active_valid);
  Remove(native); Remove(ini); Remove(donor);
}

static void Acknowledge(SaveFileFormat format) {
  const char *path = "regional-adoption-test.srm";
  Remove(path);
  uint8_t image[kActRaiserSramSize] = {0}, disk[kActRaiserSramSize], sequence = 0;
  uint16_t stocks[kArRegionalLairCount];
  for (unsigned i = 0; i < kArRegionalLairCount; ++i) stocks[i] = i * 2900;
  Save_RecomputeChecksum(image);
  SaveError error = {{0}};
  ArRegionalCampaign campaign;
  ArRegionalCampaign_Init(&campaign, 0, Identity, &sequence);
  CHECK(Save_WriteFile(format, path, image, &error));
  CHECK(ArRegionalCampaign_Continue(&campaign, path, image, &error));
  CHECK(campaign.active.lairs.initialized_towns == 0);
  ArRegionalSession loaded;
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Missing);
  /* A failed metadata replacement is neither consent persisted nor permission
   * to continue. It never rewrites the native save, and retry can succeed. */
  CHECK(MAKE_DIR("regional-adoption-test.srm.archeckpoint.tmp") == 0);
  CHECK(!ArRegionalCampaign_AcknowledgeLairHistory(&campaign, format, path, image, stocks, &error));
  CHECK(!campaign.active_valid);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Missing);
  CHECK(REMOVE_DIR("regional-adoption-test.srm.archeckpoint.tmp") == 0);
  CHECK(ArRegionalCampaign_AcknowledgeLairHistory(&campaign, format, path, image, stocks, &error));
  CHECK(campaign.active_valid && campaign.active.lairs.approximate_towns == 0x3f);
  CHECK(campaign.active.lairs.initialized_towns == 0x3f);
  CHECK(!memcmp(campaign.active.lairs.stock[0], stocks, sizeof(stocks)));
  CHECK(Save_LoadFile(format, path, disk, &error) && !memcmp(disk, image, sizeof(image)));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded.lairs, &campaign.active.lairs, sizeof(loaded.lairs)));
  /* Cold Continue already has the estimate; a second acknowledgement is
   * idempotent, and exact/approximate distinctions are retained. */
  CHECK(ArRegionalCampaign_Continue(&campaign, path, image, &error));
  CHECK(ArRegionalCampaign_AcknowledgeLairHistory(&campaign, format, path, image, stocks, &error));
  CHECK(!memcmp(&loaded.lairs, &campaign.active.lairs, sizeof(loaded.lairs)));
  stocks[0] ^= 1;
  CHECK(!ArRegionalCampaign_AcknowledgeLairHistory(&campaign, format, path, image, stocks, &error));
  CHECK(!campaign.active_valid);
  stocks[0] ^= 1;
  CHECK(ArRegionalLairHistory_MarkDiverged(&loaded.lairs, 0));
  CHECK(ArRegionalSession_Save(&loaded, format, path, image, image, &error));
  CHECK(!ArRegionalCampaign_AcknowledgeLairHistory(&campaign, format, path, image, stocks, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.lairs.diverged_towns == 1);
  Remove(path);
  /* Partial histories retain their exact towns; only absent towns are adopted. */
  CHECK(Save_WriteFile(format, path, image, &error));
  CHECK(ArRegionalCampaign_Continue(&campaign, path, image, &error));
  CHECK(ArRegionalLairHistory_InitTown(&campaign.active.lairs, 0));
  for (unsigned i = 0; i < 4; ++i) stocks[i] = campaign.active.lairs.stock[0][i];
  CHECK(ArRegionalSession_Save(&campaign.active, format, path, image, image, &error));
  CHECK(ArRegionalCampaign_AcknowledgeLairHistory(&campaign, format, path, image, stocks, &error));
  CHECK(campaign.active.lairs.initialized_towns == 0x3f && campaign.active.lairs.approximate_towns == 0x3e);
  Remove(path);
  /* External save replacement while the prompt is open cannot be overwritten. */
  CHECK(Save_WriteFile(format, path, image, &error));
  image[10] = 1; Save_RecomputeChecksum(image);
  CHECK(!ArRegionalCampaign_AcknowledgeLairHistory(&campaign, format, path, image, stocks, &error));
  CHECK(!campaign.active_valid);
  CHECK(Save_LoadFile(format, path, disk, &error) && disk[10] == 0);
  Remove(path);
}

int main(void) {
  uint8_t a[16], b[16];
  CHECK(HostCampaignIdentity_Create(NULL, a));
  CHECK(HostCampaignIdentity_Create(NULL, b));
  CHECK(memcmp(a, b, sizeof(a)) != 0);
  CHECK((a[6] & 0xf0) == 0x40 && (a[8] & 0xc0) == 0x80);
  CHECK(!HostCampaignIdentity_Create(NULL, NULL));
  Run(kSaveBackend_NativeSrm); Run(kSaveBackend_Ini);
  Acknowledge(kSaveFileFormat_NativeSrm); Acknowledge(kSaveFileFormat_Ini);
  return failures ? 1 : 0;
}
