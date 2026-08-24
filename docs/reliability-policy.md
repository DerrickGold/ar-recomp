# Runtime reliability policy

ActRaiser Recompiled is a game, not a fault-tolerant service. Recovery is useful
only when it preserves an experience a player would reasonably choose to keep
playing. A fallback that avoids a crash by silently removing sound, changing
the selected rendering mode, resetting the game, or risking save progress is a
failure disguised as resilience.

This policy applies to the hand-written runtime under `src/`. Generated
recompiler output remains governed by the generator and its dispatch contracts.

## Decision rule

Classify a failure at the point where the owning subsystem still has enough
context to name it:

1. **Optional feature is absent before use:** keep running. Missing replacement
   art, a replacement track that does not decode, or a developer diagnostic
   that was never armed may leave the complete authentic implementation in
   charge.
2. **Expected game state has no enhanced representation:** keep the complete
   authentic frame. Loading fades, unsupported native effects, and content for
   which an enhancement was never authored are presentation states, not host
   failures.
3. **Recoverable platform event:** make one bounded recovery attempt while the
   game is paused. A renderer target reset may rebuild resources; a save write
   may retry briefly. Success restores the same selected experience.
4. **Selected experience or supported-path invariant is broken:** request an
   orderly fatal-session shutdown. Do not substitute a lower-quality mode,
   publish stale data, soft-reset emulation, or freeze indefinitely.
5. **The host cannot be reached safely:** abort remains appropriate. Examples
   are failure while switching from the game coroutine back to the host or a
   guard-page stack fault. Orderly shutdown is not honest when control flow or
   memory integrity is already unknown.

The test is not whether *some* pixels or audio can still be produced. The test
is whether the result still fulfils the player's selected mode and remains
enjoyable without risking their data.

## Orderly fatal-session contract

`SessionFatal_Request` latches the first actionable error on the main/game
thread. Callers must return toward the host loop; they must not continue
mutating game state in the hope that a later layer repairs the problem.

The host then:

- stops emulation at the next loop boundary;
- writes the live settings registry, excluding session-only state;
- flushes changed battery SRAM unless a diagnostic replay forbids writes;
- performs the normal reverse-dependency teardown;
- reports the initiating error and any persistence failure to stderr and, for
  an interactive run, an SDL error dialog;
- exits with a failure status.

The first error wins. Secondary errors during teardown are reported separately
and cannot replace the condition the player can act on.

Boot errors still use `Die()`: no playable session or new user progress exists
yet. A rejected existing save is a boot error, never permission to start with
fresh SRAM. A missing save file remains the normal new-game case.

## 2026-08 audit

The audit searched the hand-written runtime for fallback, disable-on-error,
unavailable-resource, abort, and direct-exit paths, then inspected their owners
and callers. Literal uses of “fallback” were not treated as defects; many name
valid content or arithmetic defaults.

| Area | Policy result |
|---|---|
| Active save load | Changed: a corrupt, unreadable, wrong-sized, or checksum-invalid existing save stops boot with its path. It can no longer be shadowed as fresh SRAM and overwritten later. |
| Audio output | Changed: failure to create or resume the selected stream stops the session. Muting remains a transport-synchronised user choice, not an audio-device fallback. |
| Authentic comparison camera | Changed: removed fixed and per-line relative-camera fallback APIs. Only an exact BG1/BG2 native-camera frame receives a fresh publish serial; capture failure or a two-second frozen wait stops the session. |
| Comparison surface | Changed: absence is harmless while the control is unused, but selecting comparison without a valid surface stops the session instead of returning to Enhanced. |
| Hang watchdog | Changed: a trapped hang stops the session. It no longer abandons the coroutine and silently power-cycles the game. |
| Coroutine setup | Changed: missing page-size/guard-page/coroutine support stops the session. There is no unguarded-stack compatibility mode. |
| CRT processing | Changed: when selected, shader or target failure stops the session. CRT can no longer disappear from Authentic or Enhanced comparison output. |
| World-navigation renderer | Changed: a valid selected navigation frame that cannot render stops the session. Sticky allocation-failed flags and the flat presentation fallback were removed. |
| Presentation stage outcomes | Changed: shared outcomes distinguish a complete frame, optional omitted polish, and an unusable core view. World-navigation weather and Diorama supersampling may degrade; a core Diorama failure now requests orderly shutdown instead of silently ending the frame. |
| Temporary presentation targets | Changed: SIM shadow, blur, and rim passes capture the caller's exact target/viewport/clip/draw state. Failure to enter an optional pass omits it before mutation; failure to restore ownership makes the selected SIM frame unusable. |
| SIM extension freshness | Changed: the town canvas is drawable only after a complete upload of the slot's exact serial. A failed dirty upload invalidates the mixed-generation texture and gets one bounded full retry. The optional underlay blur carries its own serial and can no longer outlive the sharp world map it represents. |
| Diorama draw submissions | Changed: selected skybox and main-plane submissions are core; shoebox, stack, thickness, shadow, and shader polish report optional omission. Target, viewport, texture-address, and GPU-state restoration failures remain core because later drawing ownership is unknown. |
| Hand-written HLE invariants | Changed: invalid CPU entry modes, data contracts, and HLE allocation failures use one non-returning coroutine-to-host fatal trampoline. The host returns before NMI or further game-state execution, while a missing/incorrect escape still aborts rather than continuing through invalid CPU state. |
| SIM 3D core textures/billboard atlas | Changed: absence remains harmless while SIM 3D is off; booting or switching on a mode that needs the missing resource stops with an actionable error. |
| Renderer device loss/reset | Changed: device loss stops with an actionable error; a reset gets one rebuild pass and failure to restore the controls overlay stops the session. |
| Battery-save writes | Changed: transient failures retry for five seconds. A sustained failure stops play rather than accumulating progress that cannot be saved. Shutdown reports if the final flush also fails. |
| Randomizer snapshot | Changed: snapshot failure is harmless while unused. If Randomizer is configured on, boot stops instead of silently running stock content; an unavailable runtime hides the master control. |
| SIM 3D frame capture | Changed: core plane allocation, renderer binding, and impossible loss of both object sources stop the selected session. Expected picker/effect states may still use their complete authentic view. |
| HD replacement art | Retained: absent art and optional decode failures leave the complete ROM graphics in charge. No player-authored setting is mutated. |
| Replacement music | Retained: a missing/failed optional replacement releases the authentic sequencer, which remains complete and time-aligned. |
| Action BG/room HLE | Retained: unsupported content hands complete ownership back to the native PPU path. Partial authentic comparison camera output is no longer one of these fallbacks. |
| Loading/fade/picker frames | Retained: these are explicit authentic presentation states, not recovery from a host failure. |
| Cosmetic particles, shadows, rim/DOF, and similar stages | Retained for now: losing one cosmetic stage does not make the core selected view unplayable. Capability rows should hide unsupported choices where detection is possible. |
| Diagnostic tools and replay protections | Retained: unarmed diagnostics are optional, and replay no-write behavior protects the user's live settings and save. |

## Remaining boundaries

Some presentation modules still combine core geometry resources and cosmetic
resources without publishing their outcome. Do not convert those wholesale to
fatal errors. First split “core selected view unavailable” from “optional
cosmetic stage unavailable,” then apply the decision rule above. SIM town
canvas/underlay allocation remains intentionally optional because the live town
ground retains ownership; validation captures at the supported camera extremes
should precede any stricter policy. Cloud, shadow, rim, Diorama shoebox/depth
polish, world-navigation weather, and Diorama supersampling remain cosmetic.
Allocation and setup failures for large optional target resources are latched
until renderer reset so a graceful visual downgrade cannot become a per-frame
allocation or setup-failure loop.

Generated recompiler dispatch guards still contain generator-owned aborts.
They are outside this hand-written-runtime audit and must be changed in the
generator, with its dispatch contract tests, rather than patched in generated C.

## Review checklist

For every new failure branch:

- Is the feature genuinely optional and unused, or has the player selected it?
- Does the fallback retain a complete authentic or explicitly supported view?
- Can a bounded retry restore the same contract without advancing gameplay?
- Could continuing risk overwriting or losing user data?
- Does the error name the failed subsystem and a practical next action?
- Is the branch covered by a state/serial/capacity assertion or a focused test?
- If orderly return is unsafe, is the reason for `abort()` documented at the
  call site?

Do not add a permanent `*_failed` latch merely to suppress retries. Either the
absence is a stable capability known before selection, the retry is bounded, or
the selected session ends through `SessionFatal_Request`.
