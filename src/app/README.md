# Application lifecycle

Start at `main.c` → `Application_Run()` → `GameSession_Run()` → `GameLoop_Run()`.
These calls run on the main thread. A soft reset ends one session and starts
another inside the same application; it never reruns process startup.

| Owner | What belongs here |
| --- | --- |
| `application.c` | Launch arguments and paths, settings load, immutable source ROM, SDL/window creation, reset loop and final error reporting. |
| `game_session.c` | Ordered subsystem startup, runner/save initialization, first-run save setup wiring, and the matching session teardown. |
| `game_loop.c` | Events, paused menus, emulation/replay ticks, presentation pacing, post-tick work and graphics-device reset handling. |
| `runtime_settings.c` | Settings actions request reset/exit; the session decides whether persistence allows a reset. |

`GameSessionConfig` borrows the application's immutable ROM bytes and launch
mode. The runner copies the ROM; randomizer changes belong to that session.
`GameSessionResult` returns persistence results and whether a safe reset was
requested. SDL, the window and render device survive that result.

## Adding or changing a session service

Keep startup and teardown together in `game_session.c`, with the actual state
and cleanup in the subsystem that owns them. Follow the existing dependency
order rather than a generic registration list:

- Resolve paths/settings and create the window before session resources.
- Install settings actions before input. `HostInput_BeginSession/EndSession`
  own the mapper and clear pause, turbo and held controls on every new session.
- Create the runner before attaching runner observers. Load and validate the
  selected save and bind replay before starting asynchronous audio.
- Stop audio before reading/removing observers or destroying the runner.
- Flush persistence before releasing the save collection; a failed reset
  preserves the pending request for recovery. Fatal sessions cannot restart.
- Release textures and frame history while the process-owned device is alive.

`AudioSession_Begin/End` reset audio pause/diagnostic state and own output
shutdown. `RuntimeSettings_BeginSession/EndSession` reset lifecycle requests and
attach/detach the settings callbacks. Game-native state resets through the
runner's `ActRaiser_InitializeGame()` lifecycle callback.

Frame work belongs in `game_loop.c` only when it coordinates owners. Feature
logic stays in its own subsystem. Developer schedules belong to the process so
a consumed reset command cannot re-arm itself in the next session.

## Local validation

`tests/save_slots_boot_test.py` runs the actual game against temporary profiles
and a local ROM. It covers repeated resets in one window, prepared campaigns,
save import, legacy adoption and first-run setup at frame zero. It also checks
that the audio diagnostic observer binds to each replacement runner.

```sh
python3 tests/save_slots_boot_test.py build/ActRaiserRecomp build/actraiser_save_slots_test ar.sfc
```

The audio-session and host-input unit tests cover repeated session ownership.
Run the normal local quality checks and CTest suite after changing the ordering.
