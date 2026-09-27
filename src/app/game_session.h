#ifndef AR_GAME_SESSION_H
#define AR_GAME_SESSION_H
/* One game lifetime, from subsystem/runner startup through ordered teardown.
 * Startup and soft reset use the same path. The application keeps ownership
 * of the ROM bytes, SDL, window and render device across these calls. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GameSessionConfig {
  const uint8_t *rom_data; /* Borrowed immutable source; runner copies it. */
  size_t rom_size;
  bool headless;
  bool headless_video;
} GameSessionConfig;

typedef struct GameSessionResult {
  bool restart; /* True only after a requested reset safely flushed saves. */
  int exit_code;
  bool settings_failed;
  bool save_failed;
} GameSessionResult;

/* Main thread only. Runs until quit, reset or fatal error, then stops all
 * session services. No session can restart after a fatal/persistence failure. */
GameSessionResult GameSession_Run(const GameSessionConfig *config);

#endif /* AR_GAME_SESSION_H */
