#include "campaign_identity.h"

#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <stdio.h>
#endif

bool HostCampaignIdentity_Create(void *unused, uint8_t id[16]) {
  (void)unused;
  if (!id) return false;
  uint8_t bytes[16];
#ifdef _WIN32
  /* Use the OS generator without adding a static-library dependency to each
   * portable frontend. System32 lookup cannot load a game-directory DLL. */
  HMODULE library = LoadLibraryExW(L"bcrypt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!library) return false;
  typedef NTSTATUS (WINAPI *RandomFn)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);
  RandomFn random = (RandomFn)GetProcAddress(library, "BCryptGenRandom");
  bool ok = random && random(NULL, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
  FreeLibrary(library);
#else
  FILE *source = fopen("/dev/urandom", "rb");
  if (!source) return false;
  bool ok = fread(bytes, 1, sizeof(bytes), source) == sizeof(bytes);
  if (fclose(source)) ok = false;
#endif
  if (!ok) return false;
  /* UUID v4/variant bits also guarantee the codec's nonzero-ID invariant. */
  bytes[6] = (uint8_t)((bytes[6] & 0x0f) | 0x40);
  bytes[8] = (uint8_t)((bytes[8] & 0x3f) | 0x80);
  memcpy(id, bytes, sizeof(bytes));
  return true;
}
