# ActRaiser semantic research map

This is the manually curated index of meanings learned while tracing and
decompiling ActRaiser. It records our observations; it is **not** generated,
and it is not yet an instruction to rename generated C. Detailed evidence and
field layouts remain in [SEAMS.md](SEAMS.md),
[rendering-engine.md](rendering-engine.md), [ram-map.md](ram-map.md), and
[rom-map.md](rom-map.md).

The goal is to make each investigation cumulative. When a trace establishes
what a generic `bank_XX_YYYY` routine does, add its address, a candidate name,
the confidence, and an evidence link here. We can promote stable names into the
bank cfg files after the recomp is complete and regression-tested.

## Confidence and promotion rules

| Status | Meaning | May rename generated symbol? |
|---|---|---|
| **Observed** | A call pattern or effect was seen, but the full contract is not mapped. | No |
| **Mapped** | Inputs, outputs, major state, and callers are understood well enough for a provisional semantic name. | Not during active recomp work |
| **Verified** | Static disassembly and runtime evidence agree, including important side effects and return behavior. | Candidate for the stabilization pass |
| **Stable** | Verified, regression-tested, and intentionally adopted as a public generated-code name. | Yes |

Rules:

1. The bank:address is the permanent identity; candidate names may improve.
2. Prefer a generic address over a confident-sounding but incomplete name.
3. Record a routine's contract, not only the bug or feature that exposed it.
4. Direct-page locations are often scratch with caller-specific meaning. Do
   not promote them to global variables unless their meaning is truly stable.
5. Rename cfg symbols separately from behavior changes, followed by regen and
   regression tests. Until that stabilization pass, handwritten hooks should
   retain the address in their comments even when they use semantic names.

## Function candidates

### Boot, frame service, and common dispatch

| Address | Candidate symbol | Status | Contract / observation | Evidence |
|---|---|---|---|---|
| `$00:8000` | `ResetHandler` | Stable | Reset and boot entry. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$00:8520` | `NmiHandler` | Stable | Per-frame NMI service entry. | [rendering §2](rendering-engine.md#2-the-nmi-graphics-chain-02abf0) |
| `$00:8525` | `IrqHandler` | Stable | IRQ entry. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$00:8526/$00:852F` | `CopRequestHandler` / `BrkSoundRequestHandler` | Verified | Software-interrupt request ports; write `$035A/$035B`. | [SEAMS syscall seam](SEAMS.md#logic-hardware-seam-inventory) |
| `$00:8418` | `WaitForVblank_Main` | Verified | Main-loop frame yield on RDNMI. HLE contract is established. | [rendering conversion table](rendering-engine.md#10-what-is-hled-vs-original) |
| `$02:A85E` | `WaitForVblank_Bank02` | Verified | Bank-02 frame-yield variant. | [rendering conversion table](rendering-engine.md#10-what-is-hled-vs-original) |
| `$00:8519` | `SetDataBankFromA` | Verified | Standard `PHA; PLB` helper; changes DB from A's low byte. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$00:80E5` | `MainLoop_RunSimulationFrame` | Mapped | Sim-mode dispatch entry reached when map group `$18` is zero. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:8000` | `SimulationBuilding_UpdateAll` | Mapped | Region-gated per-frame building/icon update path with several indirect tables. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$00:8465` | `HardwareSetup_Unknown8465` | Observed | Writes a hardware-register-style `$A1` value during the sim-dispatch tail; confirmed valid native-width code, but semantic target remains unknown. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$00:8241` | `MainLoopUnknown8241` | Observed | Called twice around the `$8418` vblank wait; role remains unknown. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$00:8059` | `MainLoop_Top` | Verified | Main-loop re-entry point; also the ending presenter's exit target — one of the ROM's only two RTL-long-jump destinations. | [rom map](rom-map.md), [bug ledger #20](bug-ledger.md) |
| `$02:AC4E` | `Nmi_ReadHeldButtons` | Verified | Per-frame NMI input read: `$00A0 = $4218 & $F4` (A/X/L/R held byte, masked by the input-enable byte `$F4`). | [SEAMS magic wiring](SEAMS.md#magic-system--full-wiring-map-2026-07-07-the-magic-dead-arc-debugmd-18) |
| `$01:9293/$01:92AA/$02:87F3/$02:9AC4/$02:BEBF/$03:B013/$03:E535` | `WaitForVblank_SpinSites` | Verified | The complete census of RDNMI (`$4210`) busy-spin yield points beyond `$8418`/`$A85E`; derived by `tools/find_yield_points.py` (includes long `AF`-form reads) and mirrored in the runtime's spin whitelist. | [SEAMS frame/timing](SEAMS.md#frame--timing--mostly-already-hled--the-model-is-understood) |
| `$01:93CB` | `Nmi_AckReenableBracket` | Verified | Single `LDA $004210` ack plus `$4200=#$A1` re-enable bracket; legitimately called twice per frame by sim effect paths, so it is **not** a vblank spin. | [SEAMS frame/timing](SEAMS.md#frame--timing--mostly-already-hled--the-model-is-understood) |

### Action objects, sprites, and HUD

| Address | Candidate symbol | Status | Contract / observation | Evidence |
|---|---|---|---|---|
| `$00:8915` | `ActionObject_UpdateAll` | Mapped | Walks active `$40`-byte action-object slots and dispatches each `+$12` handler. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$00:8C98` | `ActionObject_RebuildOam` | Verified | Clears OAM shadow, scans action objects, applies visibility/activation state, and invokes the component emitter. | [rendering §9](rendering-engine.md#9-oam--sprite-pipeline-action) |
| `$00:8D68` | `ActionObject_EmitComposition` | Verified | Walks a 7-byte sprite composition and emits OAM components using object bank/pointer fields. | [rendering §9](rendering-engine.md#9-oam--sprite-pipeline-action) |
| `$00:923A/$00:9258` | `ActionHud_EmitSprites` | Verified | Emits fixed-position HUD sprites from the bank-6 HUD table; does not use camera culling. | [rendering §9](rendering-engine.md#9-oam--sprite-pipeline-action) |
| `$00:9557` | `ActionObject_SpawnForMap` | Mapped | Dispatches the active map group's spawn list through the `$95DD` table. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$00:853D` | `ActionObject_AllocateSlot` | Observed | Object-slot allocation scan; its `CPY #$1AA0` bound proves the object table spans 80 slots (`$06A0`-`$1AA0`), not 24. | [SEAMS object model](SEAMS.md#object--spawn-handler-model-moved-from-debugmd-11-2026-07-06) |
| `$00:9832` | `Player_Update` | Mapped | Player object per-frame handler (object base `$08A0`, handler pointer at `$08B2`); at `$9843` tests held buttons `$A0` against `#$00C0` (A or X) and branches to the cast gate. | [SEAMS magic wiring](SEAMS.md#magic-system--full-wiring-map-2026-07-07-the-magic-dead-arc-debugmd-18) |
| `$00:9DE1` | `Magic_CastGate` | Verified | Cast gate: requires `$F8==0` (no cast active), `$02AC!=0` (magic equipped), player status `$08D0 BIT #$2008` clear (not hurt/invulnerable), and MP `$21>0`; on pass decrements `$21`, sets bit `$0010` of `$0030,X`, increments `$F9`, clears input-enable `$F4`. | [SEAMS magic wiring](SEAMS.md#magic-system--full-wiring-map-2026-07-07-the-magic-dead-arc-debugmd-18) |
| `$01:915D` | `EquipMenu_SelectMagic` | Mapped | Equip menu; writes the selected magic id to `$02AC` from the `$0299,X` HAVE flags (`AND #$7F`). | [SEAMS magic wiring](SEAMS.md#magic-system--full-wiring-map-2026-07-07-the-magic-dead-arc-debugmd-18) |
| `$02:84E0` | `Stats_LoadWorkingMp` | Mapped | Act-entry stat staging: loads the working MP copy `$21` from persistent `$0295` (`LDA $0295; STA $21`). | [ram map `$0295`](ram-map.md) |

### Object coroutine yields and spawn-record plumbing

The action-object engine is coroutine-based: an object's handler runs until it
calls a yield helper, which captures a continuation into an object field and
returns to the object loop. Every `JSR <helper>` return site is therefore a
dispatch entry the recompiler must register — this family caused the
action-freeze, bridge, and Death Heim bug classes.
`tools/find_yield_helpers.py` derives the helper list from the ROM by shape;
run it after any bank00.cfg handler work.

| Address | Candidate symbol | Status | Contract / observation | Evidence |
|---|---|---|---|---|
| `$00:8623/$00:8657/$00:8668/$00:8669` | `ActionObject_YieldToResume` family | Verified | Store their own JSR-return address as the object's `+$1E` resume handler and dispatch it; `$8669` additionally takes a parameter in A into field `+$38`. | [SEAMS object model](SEAMS.md#object--spawn-handler-model-moved-from-debugmd-11-2026-07-06) |
| `$00:86FA` | `ActionObject_YieldWaitFrames` | Verified | Wait-N yield: zeroes `+$06/+$08`, stores A (frame count) into `+$24`, pops the caller return and stores continuation into the `+$12` handler slot. Absent from the original folklore helper list; caused the Death Heim post-victory soft-lock. | [SEAMS object model](SEAMS.md#object--spawn-handler-model-moved-from-debugmd-11-2026-07-06), [bug ledger #20](bug-ledger.md) |
| `$00:A66A` | `ActionObject_YieldVariantA66A` | Mapped | Same peek-return idiom as the `$8657` family (reads the pushed return address and stores the next instruction as continuation); found by the shape census. | [SEAMS object model](SEAMS.md#object--spawn-handler-model-moved-from-debugmd-11-2026-07-06) |
| `$00:F778` | `ActionObject_YieldStashFrame` | Verified | Frame-hijacking yield: pops the caller's return frame into object field `+$3E`; `$00:F7C9` re-pushes it and `$00:F807`'s RTS consumes it. Never returns normally (exits via the yield system), so every `JSR $F778` site+3 is an RTS-dispatch entry and the paired-resume DO-NOT-REGISTER heuristic does **not** apply. | [SEAMS object model](SEAMS.md#object--spawn-handler-model-moved-from-debugmd-11-2026-07-06), [bug ledger #20](bug-ledger.md) |
| `$00:F8A6/$00:F8D2/$00:F977` | `ActionObject_DeepPeekHelpers` | Observed | Documented helper-shaped routines with deeper stack peeks; no JSR callers found by the census, so their dispatch role is unconfirmed. | [SEAMS object model](SEAMS.md#object--spawn-handler-model-moved-from-debugmd-11-2026-07-06) |

### Death Heim boss-rush flow

| Address | Candidate symbol | Status | Contract / observation | Evidence |
|---|---|---|---|---|
| `$00:FE89` | `DeathHeim_TeleportOutSequencer` | Verified | Boss-death victory driver (object slot 50, base `$1320`); reachable only via spawn-record data words, never by code reference. Chain: `$86FA` wait 64 → `$FE9D` (player cutscene state `$0800`, beam copies player position, COP `$A0`) → `$FEE6` → wait 32 → `$FEEC`. | [rom map](rom-map.md), [bug ledger #20](bug-ledger.md) |
| `$00:FEEC` | `DeathHeim_StageWarpToHub` | Verified | Writes rush progress `$0347 = $19 - 1`, stages the warp with a 16-bit `LDA #$0701; STA $1A`, sets the final-boss flag `$0334` when `$19==8`, and deallocates the driver. | [rom map](rom-map.md) |
| `$00:F3D4` | `DeathHeim_HubStageNextBoss` | Verified | Hub map `0701` spawn-record handler (record `$F3C8` in the `$F39A` table): stages `$1A = $0347 + 2`, `$1B = $18`; branches to the all-done path when `$0347==7`. | [rom map](rom-map.md) |
| `$00:A343/$00:A375` | `PostDeathHeim_RegionCompletionStager` | Mapped | Post-Death-Heim return flow (distinct from the per-boss warp): checks six-region completion over `$7F:6B18` on the `$1B=0` path. The all-regions-complete variant is the one flow not yet exercised in a verified run. | [bug ledger #20](bug-ledger.md) |

### Action camera, tile streaming, and NMI graphics

| Address | Candidate symbol | Status | Contract / observation | Evidence |
|---|---|---|---|---|
| `$02:B091` | `ActionCamera_Update` | Verified | Applies camera deltas, clamps against BG dimensions, derives BG2 parallax, and raises 16-pixel strip flags. | [rendering §4](rendering-engine.md#4-bg-tilemap-streaming-action-stages) |
| `$02:B127` | `ActionTileStream_Dispatch` | Verified | Test-and-clears `$93` and dispatches BG1/BG2 row or column builders. | [rendering §4](rendering-engine.md#4-bg-tilemap-streaming-action-stages) |
| `$02:B158` | `ActionTileStream_BuildColumn` | Verified | Marshals one two-column upload record for the selected layer. | [rendering §4](rendering-engine.md#4-bg-tilemap-streaming-action-stages) |
| `$02:B1AF` | `ActionTileStream_BuildRow` | Verified | Marshals one two-row upload record for the selected layer; decode is page-keyed. | [rendering §4](rendering-engine.md#4-bg-tilemap-streaming-action-stages) |
| `$02:B825` | `Tilemap_DecodeColumnRecord` | Verified | Decodes level map/metatiles into a caller-supplied column record. | [rendering §4](rendering-engine.md#4-bg-tilemap-streaming-action-stages) |
| `$02:B8A0` | `Tilemap_DecodeRowRecord` | Verified | Decodes level map/metatiles into a caller-supplied row record. | [rendering §4](rendering-engine.md#4-bg-tilemap-streaming-action-stages) |
| `$02:B90D` | `Tilemap_ExpandMetatile` | Verified | Expands a metatile ID through the live definition table and attributes. | [rendering §4](rendering-engine.md#4-bg-tilemap-streaming-action-stages) |
| `$02:B6F8-$02:B726` | `SkyPalace_SetupMetatilePage` | Mapped | Conditionally copies the Sky Palace source page at ROM `$07:D0A0` to WRAM `$7E:C200`. | [SEAMS UI seam](SEAMS.md#logic-hardware-seam-inventory) |
| `$02:B727` | `Tilemap_RebuildWholeMap` | Mapped | Performs whole-map record mega-bursts used by setup/UI recomposition paths. | [rendering §3](rendering-engine.md#3-the-upload-record-system-tilemap-writes-all-of-them) |
| `$02:BC56` | `ActionTileAnimation_Tick` | Mapped | Advances action tile animation and arms DMA descriptor slot 1. | [rendering §1](rendering-engine.md#1-frame-pipeline-overview) |
| `$02:BC82` | `ActionTimer_Tick` | Mapped | Decrements the action timer in packed BCD once per second. | [rendering §1](rendering-engine.md#1-frame-pipeline-overview) |
| `$02:ABF0` | `NmiGraphics_UploadFrame` | Verified | Complete graphics uploader: OAM, scroll, tilemap records, palettes, HUD, and descriptor DMAs. | [rendering §2](rendering-engine.md#2-the-nmi-graphics-chain-02abf0) |
| `$02:ACA3` | `NmiGraphics_UploadOam` | Verified | DMA of the 544-byte OAM shadow to `$2104`. | [rendering §2](rendering-engine.md#2-the-nmi-graphics-chain-02abf0) |
| `$02:ACC8/$02:ACE5` | `TileUpload_DrainRecords` | Verified | Drains fixed BG1/BG2 row/column records and clears their headers. | [rendering §3](rendering-engine.md#3-the-upload-record-system-tilemap-writes-all-of-them) |
| `$02:ADA8` | `VramDma_Upload64Bytes` | Verified | Shared channel-1 64-byte VRAM DMA helper. | [rendering §3](rendering-engine.md#3-the-upload-record-system-tilemap-writes-all-of-them) |
| `$02:ADC3` | `NmiGraphics_UploadScroll` | Verified | Uploads `$22-$2D` to BG scroll registers as 10-bit values. | [rendering §2](rendering-engine.md#2-the-nmi-graphics-chain-02abf0) |
| `$02:AEEB` | `NmiGraphics_UploadHudBg3` | Verified | Streams the BG3 HUD from `$7F:B000` every frame and the lower portion when `$F1` requests it. | [rendering §2](rendering-engine.md#2-the-nmi-graphics-chain-02abf0) |
| `$02:BF60` | `Dialog_DrawByType` | Mapped | Dispatches message-box/text composition by message ID; text and box occupy different BG layers. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |

### Simulation camera, records, actors, and events

| Address | Candidate symbol | Status | Contract / observation | Evidence |
|---|---|---|---|---|
| `$03:8053` | `Simulation_Enter` | Mapped | Sim-entry setup/graphics orchestration used by transitions and direct entry. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$03:8193` | `Simulation_RunFrame` | Mapped | Master town loop: town handlers, periodic work, and bank-01 actor/process passes. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$03:813F` | `SimulationRecords_RestoreStagedPositions` | Verified | Copies `$130` bytes from the selected bank-7F staging buffer into live records at `$0B30,Y`; odd overlapping writes are intentional. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:B4C6` | `SimulationCamera_Update` | Verified | Follows `$0AEE/$0AF0`, writes `$22/$24`, applies shake, and clamps the 512px town world. | [SEAMS town camera](SEAMS.md#town-camera-writer-01b4c6) |
| `$01:ACD9` | `SimulationOam_Rebuild` | Verified | Clears OAM and scans 48 fixed plus 44 world records before selecting an emitter. | [rendering §11.1](rendering-engine.md#111-town-camera-and-oam-pipeline) |
| `$01:ADAD` | `SimulationOam_EmitNormal` | Verified | Emits five-byte composition parts with normal attributes. | [rendering §11.1](rendering-engine.md#111-town-camera-and-oam-pipeline) |
| `$01:AE6F` | `SimulationOam_EmitAlternate` | Verified | Same geometry as `$ADAD` with the alternate attribute transform. | [rendering §11.1](rendering-engine.md#111-town-camera-and-oam-pipeline) |
| `$01:B473` | `AngelProjectile_CheckVisible` | Verified | Tests arrow lifetime/visibility against town world and camera bounds, returning cull via carry. | [rendering §11.1](rendering-engine.md#111-town-camera-and-oam-pipeline) |
| `$01:B898` | `SimulationRecord_DispatchByType` | Verified | Per-active-record value-keyed handler dispatch; requires the full 26-entry table. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$03:F5BE` | `Town_DispatchHandlers` | Verified | Selects the active town's packed handler list and dispatches its entries. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:AC36` | `TownProcess_AssignScript` | Mapped | Initializes a stride-12 process entry from the selected spawn-list variant. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:AC70` | `TownProcess_RunScript` | Mapped | Advances delay/frame scripts and handles hide, loop, and terminator opcodes. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:CFF2` | `TownProcess_AssignFromPackedSelection` | Mapped | Splits A into the list/sub-variant selectors and calls `$AC36`. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:AA56` | `TownProcess_RestoreAndAssignAll` | Mapped | Restores staged positions and walks town process entries to assign scripts. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:8029/$01:B1C7` | `TownProcess_Initialize` | Mapped | Town-init wrapper that assigns and begins a process script. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:B52F` | `TownProcess_SelectVariantAll` | Mapped | Switches all town processes to the requested variant. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:B6AE/$03:CFB3` | `TownProcess_HideAll` | Mapped | Hide sweeps used during town state changes and exit. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:D072` | `TownActor_Spawn` | Mapped | Initializes a stride-38 actor record from its type's behavior-script table. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:D04E-$01:D062` | `TownActor_DispatchBehavior` | Verified | Y-table RTS dispatch keyed by the record's masked selector. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:D063` | `TownActor_MarkInitialized` | Verified | Sets bit 15 in record `+$12`; standard one-time initialization latch. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:D08F` | `TownActor_RunScript` | Mapped | Advances an actor behavior script, including its countdown and terminator. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:CD0C` | `TownActor_DispatchBehaviorState` | Verified | Per-frame actor behavior-state dispatch (`LDY #$CD12; BRL $D04E`) keyed by record `+$12`: state 0 = paced delay (`$CD22`), 1 = walk-script executor (`$CD35`), 3 = paced walk (`$CEFA`), 7 = `$CFAA`. | [SEAMS town §6](SEAMS.md#6-town-actor-behavior--animation-system-bank-01-mapped-2026-07-04) |
| `$01:CD35` | `TownActor_RunWalkScript` | Verified | Walk-script executor: reads the next script byte via `$CFC7`; non-`$7F` bytes dispatch as commands through the `$CD6F` table (PHA/RTS at `$CD6B`). Command byte 3's handler `$CDCC` advances the state machine to paced walk — the step whose missing dispatch caused the 17×-fast "invisible people" bug. | [SEAMS town §6](SEAMS.md#6-town-actor-behavior--animation-system-bank-01-mapped-2026-07-04) |
| `$01:CFC7` | `TownActorScript_ReadAdvance` | Mapped | Advances the actor's `$7F`-bank walk/animation script pointer and returns the next script byte in A. | [SEAMS town §6](SEAMS.md#6-town-actor-behavior--animation-system-bank-01-mapped-2026-07-04) |
| `$01:9CD6` | `Reward_GrantMagicScroll` | Verified | Scroll-reward handler behind `$9C6F`: increments persistent MP `$0295` (long addressing) and the working copy `$21`, then posts the message. | [SEAMS reward web](SEAMS.md#sim-mode-reward-grant-web--019c6f-mapped-2026-07-07-the-lost-scroll-arc-debugmd-18b) |
| `$03:9390/$03:944B/$03:9505/$03:95B3` | `TownDev_CycleHandlers` | Mapped | Development-cycle (hourglass) phase handlers, installed via the RAM handler pointer at `$7C45/$7C47` — invisible to static call-graph scans. | [SEAMS town §5](SEAMS.md#5-the-development-cycle-hourglass--town-growth-mapped-2026-07-04) |
| `$03:9DE4/$03:9E5A` | `TownDev_BuildStepDispatch` | Mapped | Build-step web: seven outer handlers funnel through `$9ED3` into the per-building-type RTS table at `$9EF3` (49 targets `$A004`-`$A4B8`), with stateful mid-loop continuations `$9EF4` → `$9E32/$9EC5` (rts_dispatch class, never plain-func). | [SEAMS town §5](SEAMS.md#5-the-development-cycle-hourglass--town-growth-mapped-2026-07-04) |
| `$03:9CFB/$03:9D4D` | `SimScene_HandlerScanLoop` | Mapped | Scene-sequence handler scanners driven from `$03:8053` (site `$80B0`): push continuation `$9D3B/$9D8D` and `BRL` into handlers (`$9F1A`-`$9F73`, plus `$A4F7`); the loop-resume labels `$9D3C/$9D8E` are single-shot registrable entries. | [SEAMS town dispatch](SEAMS.md#sim-mode-dispatch-structure-mapped-2026-07-01-chasing-a-graphics-corruption-bug) |
| `$03:9FCD` | `SimScene_DispatcherSiblingFamily` | Observed | Untriaged sibling of the `$9D4D` dispatcher family flagged by `tools/find_tailcall_past_end.py`; prime suspect for the open multi-actor-cutscene bug (lair-seal attackers and the Bloodpool lightning pair render only one actor). | [SEAMS town dispatch](SEAMS.md#sim-mode-dispatch-structure-mapped-2026-07-01-chasing-a-graphics-corruption-bug) |
| `$03:F921` | `StoryEvent_RunVm` | Mapped | Interprets the story-event command stream used by town arcs. | [SEAMS story VM](SEAMS.md#story-event-system--the-03f921-event-vm-mapped-2026-07-06-the-rock-zapfire-arc) |
| `$01:9C6F` | `Reward_Dispatch` | Mapped | Value-keyed reward grant dispatcher used by town story events. | [SEAMS reward web](SEAMS.md#sim-mode-reward-grant-web--019c6f-mapped-2026-07-07-the-lost-scroll-arc-debugmd-18b) |
| `$03:F46E/$03:F479/$03:F484` | `TownLair_TestOpen` / `TownLair_SetOpen` / `TownLair_ClearOpen` | Mapped | Tests or mutates the current town's lair bit. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$03:F497` | `TownLair_ResolveBit` | Mapped | Resolves the current town/cell to a lair bit and WRAM bitset location. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:8819` | `TownEvent_Dispatch` | Mapped | Dispatches the event code in `$033E` through the `$F223` table. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$03:8700-$03:8711` | `TownState_Dispatch` | Mapped | Per-town state dispatcher keyed by `$7CC9[town]`. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$03:E1D2-$03:E1EB` | `TownEventHandler_Dispatch` | Mapped | Per-town, 32-entry event-handler dispatcher with shared continuation `$E1EC`. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |

### Save, title, transitions, and ending

| Address | Candidate symbol | Status | Contract / observation | Evidence |
|---|---|---|---|---|
| `$02:A622` | `Title_RunContinueMenu` | Mapped | Continue/new-game state machine; branches on save checksum validity. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$02:A88D` | `Save_VerifyChecksum` | Verified | ADD/XOR checksum of SRAM through `$701FEB`; returns validity via carry. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$02:84F3` | `Save_AccumulateChecksum` | Verified | Accumulates the ADD and XOR checksum words over the save-data span. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$03:9156` | `ActionToSimulation_Dispatch` | Mapped | Relocates the stack and dispatches the act-to-sim transition chain. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$03:AC8E` | `Transition_RunStep` | Observed | Counter-driven transition state step that calls `$97B0`; full role is not yet traced. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$02:AA9C` | `Ending_RunCredits` | Verified | Drives ending/credits entries, stamps the `ACT` completion marker into SRAM `$70:1FF0`, waits for Start, and RTL-jumps back to `$00:8059`. Relocates S to `$01FF` and never returns (dispatch-only class). | [rom map ending](rom-map.md) |
| `$00:82C3` | `Ending_EnterViaRtlJump` | Verified | Mode-8 (`$18=08`) main-loop entry into the ending: `LDA #$02; PHA; LDX #$AA9B; PHX; RTL` — a cross-bank RTL long-jump to `$02:AA9C`. Whole-ROM scan confirms this and the presenter's exit are the only two RTL-jump sites. | [bug ledger #20](bug-ledger.md) |

### Audio and SPC transport

| Address | Candidate symbol | Status | Contract / observation | Evidence |
|---|---|---|---|---|
| `$02:9964/$02:9A56` | `Spc_UploadImageFromDp` | Verified | Two-stage SPC uploaders (HLE'd): stage 1 streams a length/target block image, stage 2 installs BRR sample chunks from the ROM pool. The 24-bit source pointer lives at DP `+$A5` (`LDA [$A5],Y`), not DP `+0`. Post-boot uploads drive the game's own resident ARAM uploader at `$0F0E`. | [SEAMS audio](SEAMS.md#audio--closest-to-a-clean-interface--start-here) |
| `$02:9ACD` | `Spc_BootImageDriver` | Mapped | Boot-time upload driver; installs the sound engine at ARAM `$0400` and jumps it there. | [SEAMS audio](SEAMS.md#audio--closest-to-a-clean-interface--start-here) |
| `$00:A3FE-$00:A454` | `BossMusic_LoadHandshake` | Verified | Six-step blocking CPU↔SPC handshake: send `$F1` → `$A410` wait for echo → send `$01` → `$A427` wait for port clear → `$A431` send `$F0` → completion at `$A454`. Timing-sensitive; HLE'd upload plus native handshake is the working combination. | [SEAMS audio](SEAMS.md#audio--closest-to-a-clean-interface--start-here) |
| `$02:AC33` | `Audio_ConsumeCopRequest` | Verified | Reads the COP event id from `$035A`, clears it, and writes it to APU port `$2142` — COP event ids are **audio/event commands**, not spawn requests (a proven red herring). | [SEAMS audio](SEAMS.md#audio--closest-to-a-clean-interface--start-here) |
| `$02:B63B/$02:B66C` | `Audio_CommandConsumer` | Observed | Post-upload playback command consumer (start/stop/fade port writes); the instrumentation point for track-level music replacement. | [SEAMS audio](SEAMS.md#audio--closest-to-a-clean-interface--start-here) |

## Data and table candidates

These are data identities rather than functions. Preserve that distinction in
the eventual symbol format.

| Address / range | Candidate symbol | Status | Meaning | Evidence |
|---|---|---|---|---|
| `$01:A227` | `TownProcess_SpawnListTable` | Mapped | Maps list ID to five process-script variants. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:D128+` | `TownActor_PlacementRecords` | Mapped | Six-byte placement records: actor type and world position. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:E099` | `TownActor_BehaviorScriptTable` | Verified | Actor-type-indexed behavior-script pointers. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$01:E7D9` | `TownActor_SpriteFrameTable` | Verified | Actor-type-indexed sprite-frame/composition pointers. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$03:F5ED-$03:F620` | `Town_HandlerLists` | Verified | Six-town outer table plus packed handler lists consumed by `$F5BE`. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$03:DCA2/$03:DCAE` | `Town_OpenLairMaskPointers` / `Town_SpawnedLairMaskPointers` | Mapped | Per-town pointers to lair bitsets in WRAM. | [SEAMS function roles](SEAMS.md#function-roles-discovered-decomp-groundwork) |
| `$00:96AF-$00:F39A` | `ActionMap_SpawnTables` | Mapped | The full eight-table per-map-group battery: `$96AF/$A8F6/$B449/$C11E/$CD9B/$D928/$E722/$F39A`. Each points to per-object spawn records: base`+0x0C` = init handler (JSR-shaped), base`+0x0F` = steady-state handler, record`[0x0A]` = the **polymorphic** field-`$14` slot (handler for some object types, plain data for others — not bulk-derivable). | [SEAMS spawn tables](SEAMS.md#per-level-handler-tables--spawn-dispatcher) |
| `$00:F39A+` | `DeathHeim_SpawnTable` | Verified | Distinct hub, six boss arenas, and final-boss spawn table. | [rom map](rom-map.md) |
| `$01:CD12` | `TownActor_BehaviorStateTable` | Verified | Eight behavior-state handler entries consumed by `$01:CD0C`. | [SEAMS town §6](SEAMS.md#6-town-actor-behavior--animation-system-bank-01-mapped-2026-07-04) |
| `$01:CD6F` | `TownActorScript_CommandTable` | Verified | Eighteen script-command handlers (`$CD93`-`$CEE5`) dispatched by `$CD35`'s PHA/RTS at `$CD6B`. | [SEAMS town §6](SEAMS.md#6-town-actor-behavior--animation-system-bank-01-mapped-2026-07-04) |
| `$01:B8D0` | `SimulationRecord_TypeHandlerTable` | Verified | The 26-entry (real bound `$B904`) per-record-type table behind `$01:B898`; town actor types `0x12/0x13` sit past the old 16-entry cap. | [SEAMS town dispatch](SEAMS.md#sim-mode-dispatch-structure-mapped-2026-07-01-chasing-a-graphics-corruption-bug) |
| `$01:9C94` | `Reward_HandlerTable` | Verified | Twenty reward-grant handler entries consumed by `$01:9C6F`'s PHX/PHA/RTS dispatch. | [SEAMS reward web](SEAMS.md#sim-mode-reward-grant-web--019c6f-mapped-2026-07-07-the-lost-scroll-arc-debugmd-18b) |
| `$03:F99A` | `StoryEvent_HandlerTable` | Verified | Six-byte records mapping story-event ids to 11 handlers; walked statically to register the `$F921` VM's targets. | [SEAMS story VM](SEAMS.md#story-event-system--the-03f921-event-vm-mapped-2026-07-06-the-rock-zapfire-arc) |
| `$02:C7E5` | `Song_PointerTable` | Verified | Seventeen 3-byte song-image pointers; the stage-1 upload source address is a stable track identity (more arrive via inline `[$A2]` script pointers through `$02:B4C0`). | [SEAMS audio](SEAMS.md#audio--closest-to-a-clean-interface--start-here) |
| ROM `$08:8000` | `BrrSampleChunkPool` | Verified | Length-prefixed `[len16][BRR data]` instrument chunks; the chunk index is a stable ROM-wide instrument ID (per-sample HD-swap seam). | [SEAMS audio](SEAMS.md#audio--closest-to-a-clean-interface--start-here) |
| ARAM `$0F0E/$0F12/$0F48` | `SpcResident_Uploader` | Verified | The sound engine's own resident uploader (SPC-side, not SNES bus): entry `$0F0E`, `$CC`-wait spin `$0F12`, finalize tail `$0F48` (`MOV X,#$31; MOV $F1,X; RET`). The HLE completes it by jumping `spc->pc` to the tail. | [SEAMS audio](SEAMS.md#audio--closest-to-a-clean-interface--start-here) |
| ROM `$07:D0A0` | `SkyPalace_MetatilePage` | Verified | 16×16 source metatile page copied by Sky Palace setup. | [rendering Sky Palace](rendering-engine.md#6-sky-palace-bg2-dialog-staging) |

## Stable state anchors used by handwritten code

This is deliberately a short cross-reference, not a duplicate of
[ram-map.md](ram-map.md). Names here are appropriate for shared constants;
polymorphic fields and temporary direct-page values should remain local.

| WRAM | Semantic name | Meaning |
|---|---|---|
| `$7E:0018` | `MapGroup` | Non-action, six action kingdoms, Death Heim, or ending group. |
| `$7E:0019` | `CurrentMap` | Town/submode or action-map number within the group. |
| `$7E:001A/$001B` | `DestinationMap` / `DestinationMapGroup` | Pending transition destination. |
| `$7E:0022/$0024` | `Bg1CameraX/Y` | BG1 and primary camera coordinates. |
| `$7E:0026/$0028` | `Bg2CameraX/Y` | BG2/parallax coordinates. |
| `$7E:002E/$0030` | `Bg1Width/Height` | BG1 world dimensions. |
| `$7E:0032/$0034` | `Bg2Width/Height` | BG2 world dimensions. |
| `$7E:0088` | `GameFrame` | NMI-maintained game-frame counter. |
| `$7E:0093` | `TileStreamRequestFlags` | BG1/BG2 row/column crossing flags. |
| `$7E:0380-$059F` | `OamShadow` | 512-byte low table plus 32-byte high table. |
| `$7E:06A0+` | `ActionObjectTable` / `SimFixedRecords` | Mode-dependent resident object/record array. **80** stride-`$40` slots (`$06A0`-`$1AA0`; allocator bound in `$853D`), not 24. Key fields: `+$12` handler, `+$1E` yield-resume, `+$24` wait counter, `+$3E` stashed return frame. |
| `$7E:08A0+` | `PlayerObject` | Action player slot inside the action-object table. |
| `$7E:08D0` | `PlayerStatusFlags` | Player status word: `$2000` = invulnerable, `$0008` = hurt; the magic cast gate vetoes on `BIT #$2008`. |
| `$7E:00A0` | `HeldButtons` | NMI-latched `$4218 & $F4` held-button byte (`#$00C0` = A or X = cast). |
| `$7E:0021` | `WorkingMp` | Act-mode working MP/scroll count; loaded from persistent MP at act entry. |
| `$7E:0295` | `PersistentMp` | Persistent MP/scroll count (save-backed); written only via long addressing by reward grants. |
| `$7E:02AC` | `SelectedMagic` | Equipped magic id chosen in the equip menu; zero blocks casting. |
| `$7E:0334` | `DeathHeimEndState` | Final-boss/ending sequencing flag (1 = final-boss teleport-out ran). |
| `$7E:0347` | `DeathHeimProgress` | Boss-rush progress (`$19 - 1` after each boss); `7` = all bosses beaten. |
| `$7E:035A/$035B` | `CopEventRequest` / `BrkSfxRequest` | Software-interrupt request ports (music/event id, SFX id). |
| `$7F:6B18` | `RegionCompletionFlags` | Six-region completion bitset checked by the post-Death-Heim return stager. |
| `$7E:0A00+` | `SimWorldRecords` | 44 stride-38 simulation world records. |
| `$7E:0AEE/$0AF0` | `SimCameraTargetX/Y` | Town camera-follow target. |
| `$7E:3900/$3A02` | `Bg1ColumnRecord` / `Bg1RowRecord` | Fixed BG1 tile-upload records. |
| `$7E:3B04/$3C06` | `Bg2ColumnRecord` / `Bg2RowRecord` | Fixed BG2 tile-upload records. |
| `$7F:B000+` | `HudBg3TilemapBuffer` | HUD/dialog BG3 tilemap staging. |

## How to extend this map

For each newly understood routine or table:

1. Capture the immutable address and all known callers/callees.
2. Describe inputs, outputs, state writes, width/DB assumptions, and return
   convention in the subsystem document.
3. Add or update one row here with the most conservative useful name.
4. Increase confidence only when the evidence supports it; note unresolved
   parts explicitly.
5. During the final stabilization pass, copy only **Verified** candidates into
   `recomp/bank*.cfg`, regenerate once, and review the resulting symbol-only
   diff independently from gameplay changes.

This map should grow whenever decompilation work answers a question, even when
the immediate recomp fix needs no new hook. That is how one investigation
prevents the next from retracing the same code.
