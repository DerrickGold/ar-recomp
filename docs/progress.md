# ActRaiser Recompilation Progress

## Phase 1: Environment & Tooling Setup

- [x] ROM header analysis (LoROM, 1MB, 8KB SRAM, no coprocessors)
- [x] ROM checksum verified (0x83DB)
- [x] Project directory structure created
- [x] snesrecomp framework cloned and symlinked
- [x] MegaManXSNESRecomp cloned as template reference
- [x] ROM analysis tool created (tools/rom_info.py)
- [x] LZSS decompressor created (tools/lzss_decompress.py)
- [x] Bank 00 config created (recomp/bank00.cfg)
- [x] **snesrecomp successfully processes bank 00** (40,928 lines of C generated)
- [x] Regen script created (tools/regen.sh)
- [x] macOS build script created (tools/build-macos.sh)
- [ ] Install Mesen2 emulator/debugger
- [ ] Install DiztinGUIsh disassembler
- [x] Install SDL2 and build dependencies
- [x] Create remaining bank configs (banks 01-1C)
- [x] Create game-specific runtime files (main.c, cpu_infra.c, etc.)
- [x] Create CMakeLists.txt

## Phase 2: ROM Analysis & Trace Generation
- [x] ROM map documented (docs/rom-map.md)
- [x] RAM map documented (docs/ram-map.md)
- [x] Bank layout analyzed (32 banks, 29 with content)
- [x] JSL cross-bank targets identified
- [ ] Generate trace logs in Mesen2 (all game states)
- [ ] Build annotated disassembly in DiztinGUIsh
- [ ] Document game architecture (main loop, state machine, NMI handler)

## Phase 3: Automated Recompilation
- [x] Bank 00 recompilation successful (proof of concept)
- [x] Create configs for remaining code banks
- [x] Full recompilation of all banks (29 banks, 44 stubs for indirect dispatch)
- [x] First compile of generated C (builds and runs without crashes)
- [ ] Title screen rendering
- [ ] First level playable

## Phase 4: Progressive Manual Replacement
- [ ] Custom PPU renderer
- [ ] Mode 7 renderer
- [ ] Audio system decisions
- [ ] Input & save system

## Phase 5: Enhancements
- [ ] Widescreen support
- [ ] Resolution scaling
- [ ] Shader support
