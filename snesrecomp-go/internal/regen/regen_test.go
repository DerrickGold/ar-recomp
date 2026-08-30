package regen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestLintStubsIgnoresMarkerNameInHeaderComment(t *testing.T) {
	directory := t.TempDir()
	path := filepath.Join(directory, "unresolved_stubs_v2.c")
	header := "/* each stub chains into cpu_trace_unresolved_stub_trap */\n"
	if err := os.WriteFile(path, []byte(header), 0o644); err != nil {
		t.Fatal(err)
	}
	hits, err := lintStubs(directory)
	if err != nil {
		t.Fatal(err)
	}
	if hits != 0 {
		t.Fatalf("header-only marker hits = %d, want 0", hits)
	}
	call := header + "return cpu_trace_unresolved_stub_trap(cpu, 0, \"test\");\n"
	if err := os.WriteFile(path, []byte(call), 0o644); err != nil {
		t.Fatal(err)
	}
	hits, err = lintStubs(directory)
	if err != nil {
		t.Fatal(err)
	}
	if hits != 1 {
		t.Fatalf("call marker hits = %d, want 1", hits)
	}
}

func TestUnresolvedStubsFollowEmittedDemands(t *testing.T) {
	repo := &repository{
		image:      make(rom.Image, 0x10000),
		byBank:     make(map[byte]*bankState),
		unresolved: make(map[codegen.Variant]struct{}),
		allDataRegions: []decoder.DataRegion{
			{Bank: 0x00, Start: 0x9000, End: 0x9100},
		},
	}
	repo.byBank[0x00] = &bankState{ID: 0x00}
	variants := []codegen.Variant{
		{Address: 0x001234, M: 1, X: 1}, // below the LoROM window
		{Address: 0x028000, M: 1, X: 1}, // beyond this image
		{Address: 0x018000, M: 1, X: 1}, // valid ROM, missing cfg bank
		{Address: 0x009000, M: 1, X: 1}, // explicit data region
		{Address: 0x008000, M: 1, X: 1}, // emittable
	}
	context := codegen.NewContext()
	for _, variant := range variants {
		context.Demands[variant] = struct{}{}
	}
	repo.recordUnresolvedEmittedDemands([]*codegen.Context{context})
	for _, variant := range variants[:4] {
		if _, found := repo.unresolved[variant]; !found {
			t.Errorf("missing unresolved emitted demand %#v", variant)
		}
	}
	if _, found := repo.unresolved[variants[4]]; found {
		t.Errorf("emittable demand was marked unresolved: %#v", variants[4])
	}
}

func TestProvenDispatchDiscoveryDemandsOnlyExactPostSEPMX(t *testing.T) {
	key := decoder.DecodeKey{PC: 0x008000, M: 0, X: 0}
	instruction := &cpu65816.Instruction{
		Address: 0x008000, M: 0, X: 0,
		DispatchEntries:  []uint32{0x008200},
		DispatchKind:     "short",
		DispatchSEP:      0x20,
		DispatchMXProven: true,
	}
	graph := &decoder.Graph{Instructions: map[decoder.DecodeKey]*decoder.DecodedInstruction{
		key: {Key: key, Instruction: instruction},
	}}
	demands := discoverGraphDemands(graph, nil, false, nil)
	want := codegen.Variant{Address: 0x008200, M: 1, X: 0}
	if len(demands) != 1 {
		t.Fatalf("proven dispatch demands = %#v, want one exact target variant", demands)
	}
	if _, found := demands[want]; !found {
		t.Fatalf("proven dispatch demands = %#v, missing %#v", demands, want)
	}

	instruction.DispatchMXProven = false
	demands = discoverGraphDemands(graph, nil, false, nil)
	if len(demands) != 4 {
		t.Fatalf("ordinary dynamic dispatch demands = %#v, want all four variants", demands)
	}
}

func TestExperimentalDirectCallDiscoveryDemandsOnlyLiveMX(t *testing.T) {
	key := decoder.DecodeKey{PC: 0x008000, M: 0, X: 1}
	instruction := &cpu65816.Instruction{
		Address: 0x008000, Mnemonic: "JSR", Mode: cpu65816.ABS,
		Operand: 0x8200, M: 0, X: 1,
	}
	graph := &decoder.Graph{Instructions: map[decoder.DecodeKey]*decoder.DecodedInstruction{
		key: {Key: key, Instruction: instruction},
	}}
	demands := discoverGraphDemands(graph, nil, true, nil)
	want := codegen.Variant{Address: 0x008200, M: 0, X: 1}
	if len(demands) != 1 {
		t.Fatalf("exact direct call demands = %+v, want one variant", demands)
	}
	if _, found := demands[want]; !found {
		t.Fatalf("exact direct call demands = %+v, missing %+v", demands, want)
	}
	evidence := discoverGraphDemandEvidence(graph, nil, true, nil)[want]
	if !evidence.Canonical || !evidence.Static || evidence.Kind != analysis.EntryRoutine ||
		len(evidence.Sources) != 1 || evidence.Sources[0] != "static.direct_jsr" {
		t.Fatalf("exact direct call evidence = %+v", evidence)
	}
}

func TestInvalidDiscoveredDirectCallTrapsInlineWithoutDeadStub(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "game.sfc")
	cfgDir := filepath.Join(root, "recomp")
	outputDir := filepath.Join(root, "gen")
	image := make([]byte, 0x8000)
	copy(image, []byte{0x22, 0x34, 0x12, 0x00, 0x60}) // JSL $00:1234; RTS
	if err := os.WriteFile(romPath, image, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(cfgDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfgDir, "bank00.cfg"),
		[]byte("bank = 00\nfunc Entry 8000 end:8005 entry_mx:1,1\n"),
		0o600); err != nil {
		t.Fatal(err)
	}
	report, err := Run(Options{
		ROMPath: romPath, ConfigDir: cfgDir, OutputDir: outputDir, Jobs: 1,
		AllowStubs: true,
	})
	if err != nil {
		t.Fatal(err)
	}
	if report.StubHits != 1 {
		t.Fatalf("invalid direct call produced %d marker(s), want one inline trap",
			report.StubHits)
	}
	bank, err := os.ReadFile(filepath.Join(outputDir, "bank00_v2.c"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(bank),
		"cpu_trace_unresolved_stub_trap(cpu, 0x001234") {
		t.Fatalf("invalid direct call did not diagnose inline:\n%s", bank)
	}
	stubs, err := os.ReadFile(filepath.Join(outputDir, "unresolved_stubs_v2.c"))
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(stubs), "bank_00_1234_M") {
		t.Fatalf("inline invalid call produced unreferenced stubs:\n%s", stubs)
	}
}

func TestProvenAnalysisOverlayEmitsPHARTSDispatchWithoutWritingConfig(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "game.sfc")
	cfgDir := filepath.Join(root, "recomp")
	outputDir := filepath.Join(root, "gen")
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{
		0xBD, 0x00, 0x81, // LDA $8100,X
		0xA0, 0x08, 0x80, // LDY #$8008
		0x5A, // PHY
		0x48, // PHA at $8007
		0x60, // original dispatching RTS
		0x60, // continuation $8009
	})
	image[0x0100], image[0x0101] = 0xFF, 0x81 // handler-1 -> $8200
	image[0x0200] = 0x60
	if err := os.WriteFile(romPath, image, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(cfgDir, 0o755); err != nil {
		t.Fatal(err)
	}
	configPath := filepath.Join(cfgDir, "bank00.cfg")
	configText := "bank = 00\nfunc Entry 8000 entry_mx:0,0\nfunc Continue 8009 entry_mx:0,0\nfunc Handler 8200 entry_mx:0,0\n"
	if err := os.WriteFile(configPath, []byte(configText), 0o600); err != nil {
		t.Fatal(err)
	}
	returnPC := uint32(0x008009)
	fact := analysis.DispatchFact{
		SitePC: 0x008007, Mnemonic: "PHA", AddressingMode: "imp",
		LiveMX: []analysis.MXState{{M: 0, X: 0}}, Transfer: analysis.TransferCall,
		TargetEntryKind: analysis.EntryComputed, Targets: []uint32{0x008200}, TargetSetClosed: true,
		IndexRegister: "A", TableBases: []uint32{0x008100}, ReturnPC: &returnPC,
		Evidence: []analysis.Evidence{{Source: "static.fixture", Confidence: analysis.ConfidenceProven}},
	}
	report, err := Run(Options{
		ROMPath: romPath, ConfigDir: cfgDir, OutputDir: outputDir, Jobs: 1,
		AllowStubs: true, ProvenDispatchFacts: []analysis.DispatchFact{fact},
	})
	if err != nil {
		t.Fatal(err)
	}
	if report.AnalysisFactsApplied != 1 {
		t.Fatalf("applied analysis facts = %d, want 1", report.AnalysisFactsApplied)
	}
	if report.FinalEntries != report.InitialEntries {
		t.Fatalf("proven exact M/X dispatch grew variants: %d -> %d", report.InitialEntries, report.FinalEntries)
	}
	configAfter, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatal(err)
	}
	if string(configAfter) != configText {
		t.Fatalf("analysis overlay rewrote authored config:\n%s", configAfter)
	}
	generated, err := os.ReadFile(filepath.Join(outputDir, "bank00_v2.c"))
	if err != nil {
		t.Fatal(err)
	}
	for _, wanted := range []string{
		"PHA/RTS jump-table call", "case 0x81ff", "Handler",
		"#if SNESRECOMP_SEMANTIC_DISPATCH_TRACE",
		"cpu_trace_resolved_dispatch(cpu, 0x008200u, 0x008008u)",
		"goto L_8009_M0X0;",
	} {
		if !strings.Contains(string(generated), wanted) {
			t.Fatalf("generated overlay output missing %q:\n%s", wanted, generated)
		}
	}
	if strings.Contains(string(generated), "dispatch-ret tail-call: $8009 is registered func") {
		t.Fatalf("internal continuation resumed through a new C activation:\n%s", generated)
	}
}

func TestProvenContinuationMergesExactParentRegionAndRetainsExternalEntry(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "game.sfc")
	cfgDir := filepath.Join(root, "recomp")
	controlDir := filepath.Join(root, "control")
	overlayDir := filepath.Join(root, "overlay")
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x80, 0x0E}) // $8000: BRA $8010
	copy(image[0x0010:], []byte{0xEA, 0x60}) // $8010: NOP; RTS
	if err := os.WriteFile(romPath, image, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(cfgDir, 0o755); err != nil {
		t.Fatal(err)
	}
	configText := "bank = 00\n" +
		"func Root 8000 entry_mx:1,1\n" +
		"func bank_00_8010 8010 entry_mx:1,1\n"
	if err := os.WriteFile(filepath.Join(cfgDir, "bank00.cfg"), []byte(configText), 0o600); err != nil {
		t.Fatal(err)
	}
	control, err := Run(Options{
		ROMPath: romPath, ConfigDir: cfgDir, OutputDir: controlDir, Jobs: 1, AllowStubs: true,
	})
	if err != nil {
		t.Fatal(err)
	}
	fact := analysis.EntryFact{
		PC: 0x008010, EntryMX: analysis.MXState{M: 1, X: 1},
		Kind: analysis.EntryContinuation, TemplateFree: true,
		RegionOwners: []analysis.EntryVariant{{
			PC: 0x008000, EntryMX: analysis.MXState{M: 1, X: 1},
		}},
		ResumeEdges: []analysis.EntryEdge{{
			Source: analysis.EntryVariant{PC: 0x008000, EntryMX: analysis.MXState{M: 1, X: 1}},
			Target: analysis.EntryVariant{PC: 0x008010, EntryMX: analysis.MXState{M: 1, X: 1}},
		}},
		Evidence: []analysis.Evidence{{
			Source: "static.sibling_boundary_edge", Confidence: analysis.ConfidenceProven,
		}},
	}
	overlay, err := Run(Options{
		ROMPath: romPath, ConfigDir: cfgDir, OutputDir: overlayDir, Jobs: 1, AllowStubs: true,
		ProvenEntryFacts: []analysis.EntryFact{fact},
	})
	if err != nil {
		t.Fatal(err)
	}
	if overlay.AnalysisContinuationFactsApplied != 1 || overlay.AnalysisEntryFactsApplied != 0 ||
		overlay.AnalysisEntryFactsRediscovered != 0 {
		t.Fatalf("continuation/routine/rediscovered counts = %d/%d/%d",
			overlay.AnalysisContinuationFactsApplied, overlay.AnalysisEntryFactsApplied,
			overlay.AnalysisEntryFactsRediscovered)
	}
	if overlay.SharedRegionBodies != 1 || overlay.SharedRegionContinuationWrappers != 1 ||
		overlay.SharedRegionContinuationFallbacks != 0 {
		t.Fatalf("shared region bodies/wrappers/fallbacks = %d/%d/%d, want 1/1/0",
			overlay.SharedRegionBodies, overlay.SharedRegionContinuationWrappers,
			overlay.SharedRegionContinuationFallbacks)
	}
	if control.FinalEntries != overlay.FinalEntries || control.Functions != overlay.Functions {
		t.Fatalf("continuation overlay changed external entries/functions: control %d/%d overlay %d/%d",
			control.FinalEntries, control.Functions, overlay.FinalEntries, overlay.Functions)
	}
	controlSource, err := os.ReadFile(filepath.Join(controlDir, "bank00_v2.c"))
	if err != nil {
		t.Fatal(err)
	}
	overlaySource, err := os.ReadFile(filepath.Join(overlayDir, "bank00_v2.c"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(controlSource), "bank_00_8010_M1X1(cpu)") {
		t.Fatalf("control did not use the external sibling entry:\n%s", controlSource)
	}
	if !strings.Contains(string(overlaySource), "goto L_8010_M1X1;") ||
		strings.Contains(string(overlaySource), "tail-call past end: into bank_00_8010_M1X1") {
		t.Fatalf("continuation did not resume locally in its parent:\n%s", overlaySource)
	}
	if strings.Count(string(overlaySource), "RecompReturn bank_00_8010_M1X1(CpuState *cpu) {") != 1 {
		t.Fatalf("external continuation entry was not retained exactly once:\n%s", overlaySource)
	}
	for _, fragment := range []string{
		"static inline RecompReturn sr_region_00_8000_M1X1",
		"case 0: goto L_8000_M1X1;",
		"case 1: goto L_8010_M1X1;",
		"return sr_region_00_8000_M1X1(cpu, _entry_s, _hrv, 1);",
	} {
		if !strings.Contains(string(overlaySource), fragment) {
			t.Fatalf("shared continuation region is missing %q:\n%s", fragment, overlaySource)
		}
	}
	if strings.Count(string(overlaySource), "cpu_trace_block(cpu, 0x008010);") != 1 {
		t.Fatalf("continuation block body was duplicated:\n%s", overlaySource)
	}
	controlDispatch, err := os.ReadFile(filepath.Join(controlDir, "dispatch_v2.c"))
	if err != nil {
		t.Fatal(err)
	}
	overlayDispatch, err := os.ReadFile(filepath.Join(overlayDir, "dispatch_v2.c"))
	if err != nil {
		t.Fatal(err)
	}
	if string(controlDispatch) != string(overlayDispatch) {
		t.Fatalf("continuation overlay changed external registry:\n%s\n---\n%s", controlDispatch, overlayDispatch)
	}
}

func TestProvenContinuationRetainsStandaloneBodyWhenExternalClosureDiffers(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "game.sfc")
	cfgDir := filepath.Join(root, "recomp")
	outputDir := filepath.Join(root, "gen")
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x80, 0x0E}) // $8000: BRA $8010
	copy(image[0x0010:], []byte{0x80, 0xEE}) // $8010: BRA $8000
	if err := os.WriteFile(romPath, image, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(cfgDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfgDir, "bank00.cfg"), []byte(
		"bank = 00\nfunc Root 8000 entry_mx:1,1\nfunc bank_00_8010 8010 entry_mx:1,1\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	fact := analysis.EntryFact{
		PC: 0x008010, EntryMX: analysis.MXState{M: 1, X: 1},
		Kind: analysis.EntryContinuation, TemplateFree: true,
		RegionOwners: []analysis.EntryVariant{{
			PC: 0x008000, EntryMX: analysis.MXState{M: 1, X: 1},
		}},
		ResumeEdges: []analysis.EntryEdge{{
			Source: analysis.EntryVariant{PC: 0x008000, EntryMX: analysis.MXState{M: 1, X: 1}},
			Target: analysis.EntryVariant{PC: 0x008010, EntryMX: analysis.MXState{M: 1, X: 1}},
		}},
		Evidence: []analysis.Evidence{{
			Source: "static.sibling_boundary_edge", Confidence: analysis.ConfidenceProven,
		}},
	}
	report, err := Run(Options{
		ROMPath: romPath, ConfigDir: cfgDir, OutputDir: outputDir, Jobs: 1,
		AllowStubs: true, ProvenEntryFacts: []analysis.EntryFact{fact},
	})
	if err != nil {
		t.Fatal(err)
	}
	if report.SharedRegionBodies != 0 || report.SharedRegionContinuationWrappers != 0 ||
		report.SharedRegionContinuationFallbacks != 1 {
		t.Fatalf("shared region bodies/wrappers/fallbacks = %d/%d/%d, want 0/0/1",
			report.SharedRegionBodies, report.SharedRegionContinuationWrappers,
			report.SharedRegionContinuationFallbacks)
	}
	generated, err := os.ReadFile(filepath.Join(outputDir, "bank00_v2.c"))
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(generated), "sr_region_") ||
		strings.Count(string(generated), "cpu_trace_block(cpu, 0x008010);") != 2 {
		t.Fatalf("closure mismatch did not retain the standalone continuation body:\n%s", generated)
	}
}

func TestProvenContinuationFlattensNestedSingleOwnerTree(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "game.sfc")
	cfgDir := filepath.Join(root, "recomp")
	outputDir := filepath.Join(root, "gen")
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x80, 0x0E}) // $8000: BRA $8010
	copy(image[0x0010:], []byte{0x80, 0x0E}) // $8010: BRA $8020
	image[0x0020] = 0x60                     // $8020: RTS
	if err := os.WriteFile(romPath, image, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(cfgDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfgDir, "bank00.cfg"), []byte(
		"bank = 00\nfunc Root 8000 entry_mx:1,1\nfunc bank_00_8010 8010 entry_mx:1,1\nfunc bank_00_8020 8020 entry_mx:1,1\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	fact := func(pc, owner, source uint32) analysis.EntryFact {
		return analysis.EntryFact{
			PC: pc, EntryMX: analysis.MXState{M: 1, X: 1},
			Kind: analysis.EntryContinuation, TemplateFree: true,
			RegionOwners: []analysis.EntryVariant{{
				PC: owner, EntryMX: analysis.MXState{M: 1, X: 1},
			}},
			ResumeEdges: []analysis.EntryEdge{{
				Source: analysis.EntryVariant{PC: source, EntryMX: analysis.MXState{M: 1, X: 1}},
				Target: analysis.EntryVariant{PC: pc, EntryMX: analysis.MXState{M: 1, X: 1}},
			}},
			Evidence: []analysis.Evidence{{
				Source: "static.sibling_boundary_edge", Confidence: analysis.ConfidenceProven,
			}},
		}
	}
	report, err := Run(Options{
		ROMPath: romPath, ConfigDir: cfgDir, OutputDir: outputDir, Jobs: 1,
		AllowStubs: true,
		ProvenEntryFacts: []analysis.EntryFact{
			fact(0x008010, 0x008000, 0x008000),
			fact(0x008020, 0x008010, 0x008010),
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	if report.SharedRegionBodies != 1 || report.SharedRegionContinuationWrappers != 2 ||
		report.SharedRegionContinuationFallbacks != 0 {
		t.Fatalf("shared region bodies/wrappers/fallbacks = %d/%d/%d, want 1/2/0",
			report.SharedRegionBodies, report.SharedRegionContinuationWrappers,
			report.SharedRegionContinuationFallbacks)
	}
	generated, err := os.ReadFile(filepath.Join(outputDir, "bank00_v2.c"))
	if err != nil {
		t.Fatal(err)
	}
	for _, fragment := range []string{
		"case 1: goto L_8010_M1X1;",
		"case 2: goto L_8020_M1X1;",
		"return sr_region_00_8000_M1X1(cpu, _entry_s, _hrv, 1);",
		"return sr_region_00_8000_M1X1(cpu, _entry_s, _hrv, 2);",
	} {
		if !strings.Contains(string(generated), fragment) {
			t.Fatalf("flattened nested region is missing %q:\n%s", fragment, generated)
		}
	}
	if strings.Count(string(generated), "cpu_trace_block(cpu, 0x008010);") != 1 ||
		strings.Count(string(generated), "cpu_trace_block(cpu, 0x008020);") != 1 {
		t.Fatalf("nested continuation bodies were duplicated:\n%s", generated)
	}
}

func TestProvenContinuationFailsClosedOnSpecialEntryOrWrongOwner(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "game.sfc")
	cfgDir := filepath.Join(root, "recomp")
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x80, 0x0E})
	copy(image[0x0010:], []byte{0xEA, 0x60})
	if err := os.WriteFile(romPath, image, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(cfgDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfgDir, "bank00.cfg"), []byte(
		"bank = 00\nfunc Root 8000 entry_mx:1,1\nfunc Special 8010 entry_mx:1,1\nhle_func 8010 HostContinuation\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	fact := analysis.EntryFact{
		PC: 0x008010, EntryMX: analysis.MXState{M: 1, X: 1},
		Kind: analysis.EntryContinuation, TemplateFree: true,
		RegionOwners: []analysis.EntryVariant{{
			PC: 0x008000, EntryMX: analysis.MXState{M: 1, X: 1},
		}},
		ResumeEdges: []analysis.EntryEdge{{
			Source: analysis.EntryVariant{PC: 0x008000, EntryMX: analysis.MXState{M: 1, X: 1}},
			Target: analysis.EntryVariant{PC: 0x008010, EntryMX: analysis.MXState{M: 1, X: 1}},
		}},
		Evidence: []analysis.Evidence{{
			Source: "static.sibling_boundary_edge", Confidence: analysis.ConfidenceProven,
		}},
	}
	_, err := Run(Options{
		ROMPath: romPath, ConfigDir: cfgDir, OutputDir: filepath.Join(root, "special"), Jobs: 1,
		AllowStubs: true, ProvenEntryFacts: []analysis.EntryFact{fact},
	})
	if err == nil || !strings.Contains(err.Error(), "metadata blockers") || !strings.Contains(err.Error(), "hle_func") {
		t.Fatalf("special continuation error = %v", err)
	}

	if err := os.WriteFile(filepath.Join(cfgDir, "bank00.cfg"), []byte(
		"bank = 00\nfunc Root 8000 entry_mx:1,1\nfunc bank_00_8010 8010 entry_mx:1,1\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	fact.RegionOwners[0].EntryMX.M = 0
	_, err = Run(Options{
		ROMPath: romPath, ConfigDir: cfgDir, OutputDir: filepath.Join(root, "wrong-owner"), Jobs: 1,
		AllowStubs: true, ProvenEntryFacts: []analysis.EntryFact{fact},
	})
	if err == nil || !strings.Contains(err.Error(), "owner $008000 M0X1 has no active authored entry") {
		t.Fatalf("wrong-owner continuation error = %v", err)
	}

	fact.RegionOwners[0].EntryMX.M = 1
	fact.ResumeEdges[0].Source.PC = 0x008001
	_, err = Run(Options{
		ROMPath: romPath, ConfigDir: cfgDir, OutputDir: filepath.Join(root, "wrong-edge"), Jobs: 1,
		AllowStubs: true, ProvenEntryFacts: []analysis.EntryFact{fact},
	})
	if err == nil || !strings.Contains(err.Error(), "resume edge $008001 M1X1 -> $008010 M1X1 is not present") {
		t.Fatalf("wrong-edge continuation error = %v", err)
	}
}

func TestProvenRoutineRootsAreRediscoveredWithByteIdenticalOutput(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "game.sfc")
	cfgDir := filepath.Join(root, "recomp")
	normalDir := filepath.Join(root, "normal")
	overlayDir := filepath.Join(root, "overlay")
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x20, 0x00, 0x81, 0x60}) // $8000: JSR $8100; RTS
	copy(image[0x0100:], []byte{0x20, 0x00, 0x82, 0x60}) // $8100: JSR $8200; RTS
	image[0x0200] = 0x60                                 // $8200: RTS
	if err := os.WriteFile(romPath, image, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(cfgDir, 0o755); err != nil {
		t.Fatal(err)
	}
	configText := "bank = 00\n" +
		"func Root 8000 entry_mx:1,1\n" +
		"func bank_00_8100 8100 entry_mx:1,1\n" +
		"func bank_00_8200 8200 entry_mx:1,1\n"
	if err := os.WriteFile(filepath.Join(cfgDir, "bank00.cfg"), []byte(configText), 0o600); err != nil {
		t.Fatal(err)
	}
	baseOptions := Options{
		ROMPath: romPath, ConfigDir: cfgDir, Jobs: 1, AllowStubs: true,
		ExperimentalExactDirectCallMX: true,
	}
	normalOptions := baseOptions
	normalOptions.OutputDir = normalDir
	normal, err := Run(normalOptions)
	if err != nil {
		t.Fatal(err)
	}
	fact := func(pc uint32, caller string) analysis.EntryFact {
		return analysis.EntryFact{
			PC: pc, EntryMX: analysis.MXState{M: 1, X: 1}, Kind: analysis.EntryRoutine, TemplateFree: true,
			Evidence: []analysis.Evidence{{
				Source: "static.direct_jsr", Confidence: analysis.ConfidenceProven, Detail: caller,
			}},
		}
	}
	overlayOptions := baseOptions
	overlayOptions.OutputDir = overlayDir
	overlayOptions.ProvenEntryFacts = []analysis.EntryFact{
		fact(0x008100, "$00:8000"), fact(0x008200, "$00:8100"),
	}
	overlay, err := Run(overlayOptions)
	if err != nil {
		t.Fatal(err)
	}
	if overlay.AnalysisEntryFactsApplied != 2 || overlay.AnalysisEntryFactsRediscovered != 2 {
		t.Fatalf("entry overlay applied/rediscovered = %d/%d, want 2/2",
			overlay.AnalysisEntryFactsApplied, overlay.AnalysisEntryFactsRediscovered)
	}
	if overlay.AnalysisEntryTemplatesSynthesized != 2 {
		t.Fatalf("synthesized entry templates = %d, want 2", overlay.AnalysisEntryTemplatesSynthesized)
	}
	if normal.FinalEntries != overlay.FinalEntries || normal.Functions != overlay.Functions {
		t.Fatalf("normal entries/functions %d/%d, overlay %d/%d",
			normal.FinalEntries, normal.Functions, overlay.FinalEntries, overlay.Functions)
	}
	assertGeneratedDirectoriesEqual(t, normalDir, overlayDir)
}

func TestExactStaticCallSynthesizesConfigFreeCanonicalHLERoutine(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "game.sfc")
	controlCfg := filepath.Join(root, "control-cfg")
	prunedCfg := filepath.Join(root, "pruned-cfg")
	controlOut := filepath.Join(root, "control-gen")
	prunedOut := filepath.Join(root, "pruned-gen")
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x20, 0x00, 0x82, 0x60}) // $8000: JSR $8200; RTS, M0X0
	image[0x0200] = 0x60
	if err := os.WriteFile(romPath, image, 0o600); err != nil {
		t.Fatal(err)
	}
	write := func(directory, source string) {
		t.Helper()
		if err := os.MkdirAll(directory, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(directory, "bank00.cfg"), []byte(source), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	// Put the target before the caller so removing it necessarily changes raw
	// function order when discovery appends the synthesized entry.
	write(controlCfg, "bank = 00\nfunc bank_00_8200 8200 entry_mx:0,0\nfunc Root 8000 entry_mx:0,0\nhle_func 8200 HostRoutine\n")
	write(prunedCfg, "bank = 00\nfunc Root 8000 entry_mx:0,0\nhle_func 8200 HostRoutine\n")
	run := func(cfg, output string) Report {
		t.Helper()
		report, err := Run(Options{
			ROMPath: romPath, ConfigDir: cfg, OutputDir: output, Jobs: 1,
			AllowStubs: true, ExperimentalExactDirectCallMX: true,
		})
		if err != nil {
			t.Fatal(err)
		}
		return report
	}
	control := run(controlCfg, controlOut)
	pruned := run(prunedCfg, prunedOut)
	if len(control.StaticEntryDiscoveries) != 0 {
		t.Fatalf("authored control discoveries = %+v", control.StaticEntryDiscoveries)
	}
	if len(pruned.StaticEntryDiscoveries) != 1 {
		t.Fatalf("config-free discoveries = %+v", pruned.StaticEntryDiscoveries)
	}
	discovery := pruned.StaticEntryDiscoveries[0]
	if discovery.PC != 0x008200 || discovery.EntryMX != (analysis.MXState{M: 0, X: 0}) ||
		discovery.Kind != analysis.EntryRoutine || !discovery.CanonicalPromoted || len(discovery.Evidence) != 1 ||
		discovery.Evidence[0].Source != "static.direct_jsr" {
		t.Fatalf("config-free discovery = %+v", discovery)
	}
	if control.SemanticSourceSHA256 == "" || control.SemanticSourceSHA256 != pruned.SemanticSourceSHA256 {
		t.Fatalf("semantic hashes control=%s pruned=%s", control.SemanticSourceSHA256, pruned.SemanticSourceSHA256)
	}
	controlBank, err := os.ReadFile(filepath.Join(controlOut, "bank00_v2.c"))
	if err != nil {
		t.Fatal(err)
	}
	prunedBank, err := os.ReadFile(filepath.Join(prunedOut, "bank00_v2.c"))
	if err != nil {
		t.Fatal(err)
	}
	if string(controlBank) == string(prunedBank) {
		t.Fatal("raw generated bank unexpectedly retained authored ordering")
	}
	for _, source := range []string{string(controlBank), string(prunedBank)} {
		if !strings.Contains(source, "HostRoutine(cpu)") || !strings.Contains(source, "bank_00_8200_M0X0") {
			t.Fatalf("generated HLE routine was not preserved:\n%s", source)
		}
	}
	controlDispatch, err := os.ReadFile(filepath.Join(controlOut, "dispatch_v2.c"))
	if err != nil {
		t.Fatal(err)
	}
	prunedDispatch, err := os.ReadFile(filepath.Join(prunedOut, "dispatch_v2.c"))
	if err != nil {
		t.Fatal(err)
	}
	if string(controlDispatch) != string(prunedDispatch) {
		t.Fatalf("config-free canonical dispatch registry differs:\n%s\n---\n%s", controlDispatch, prunedDispatch)
	}
}

func TestProvenRoutineRootFailsClosedWhenNotStaticallyRediscovered(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "game.sfc")
	cfgDir := filepath.Join(root, "recomp")
	image := make([]byte, 0x8000)
	image[0x0000] = 0x60 // $8000: RTS
	image[0x0200] = 0x60 // $8200: RTS, but no static caller
	if err := os.WriteFile(romPath, image, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(cfgDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfgDir, "bank00.cfg"), []byte(
		"bank = 00\nfunc Root 8000 entry_mx:1,1\nfunc Lonely 8200 entry_mx:1,1\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	_, err := Run(Options{
		ROMPath: romPath, ConfigDir: cfgDir, OutputDir: filepath.Join(root, "gen"), Jobs: 1,
		AllowStubs: true, ExperimentalExactDirectCallMX: true,
		ProvenEntryFacts: []analysis.EntryFact{{
			PC: 0x008200, EntryMX: analysis.MXState{M: 1, X: 1}, Kind: analysis.EntryRoutine,
			Evidence: []analysis.Evidence{{Source: "static.direct_jsr", Confidence: analysis.ConfidenceProven}},
		}},
	})
	if err == nil || !strings.Contains(err.Error(), "were not rediscovered") || !strings.Contains(err.Error(), "$00:8200 M1X1") {
		t.Fatalf("unrecovered entry error = %v", err)
	}
}

func TestProvenRoutineRootRequiresExactMXRediscovery(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "game.sfc")
	cfgDir := filepath.Join(root, "recomp")
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x20, 0x00, 0x82, 0x60}) // M1X1 call
	image[0x0200] = 0x60
	if err := os.WriteFile(romPath, image, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(cfgDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfgDir, "bank00.cfg"), []byte(
		"bank = 00\nfunc Root 8000 entry_mx:1,1\nfunc WrongWidth 8200 entry_mx:0,0\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	_, err := Run(Options{
		ROMPath: romPath, ConfigDir: cfgDir, OutputDir: filepath.Join(root, "gen"), Jobs: 1,
		AllowStubs: true, ExperimentalExactDirectCallMX: true,
		ProvenEntryFacts: []analysis.EntryFact{{
			PC: 0x008200, EntryMX: analysis.MXState{M: 0, X: 0}, Kind: analysis.EntryRoutine,
			Evidence: []analysis.Evidence{{Source: "static.direct_jsr", Confidence: analysis.ConfidenceProven}},
		}},
	})
	if err == nil || !strings.Contains(err.Error(), "$00:8200 M0X0") {
		t.Fatalf("wrong-width rediscovery error = %v", err)
	}
}

func TestProvenRoutineRootRefusesAuthoredHLEObligations(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "game.sfc")
	cfgDir := filepath.Join(root, "recomp")
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x20, 0x00, 0x82, 0x60})
	image[0x0200] = 0x60
	if err := os.WriteFile(romPath, image, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(cfgDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfgDir, "bank00.cfg"), []byte(
		"bank = 00\nfunc Root 8000 entry_mx:1,1\nfunc HLERoutine 8200 entry_mx:1,1\nhle_func 8200 HostRoutine\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	_, err := Run(Options{
		ROMPath: romPath, ConfigDir: cfgDir, OutputDir: filepath.Join(root, "gen"), Jobs: 1,
		AllowStubs: true, ExperimentalExactDirectCallMX: true,
		ProvenEntryFacts: []analysis.EntryFact{{
			PC: 0x008200, EntryMX: analysis.MXState{M: 1, X: 1}, Kind: analysis.EntryRoutine,
			Evidence: []analysis.Evidence{{Source: "static.direct_jsr", Confidence: analysis.ConfidenceProven}},
		}},
	})
	if err == nil || !strings.Contains(err.Error(), "authored HLE obligations") ||
		!strings.Contains(err.Error(), "hle_func:HostRoutine") {
		t.Fatalf("HLE root suppression error = %v", err)
	}
}

func TestTemplateFreeRoutineRootRefusesCustomMetadata(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "game.sfc")
	cfgDir := filepath.Join(root, "recomp")
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x20, 0x00, 0x82, 0x60})
	image[0x0200] = 0x60
	if err := os.WriteFile(romPath, image, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(cfgDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfgDir, "bank00.cfg"), []byte(
		"bank = 00\nfunc Root 8000 entry_mx:1,1\nfunc MeaningfulName 8200 entry_mx:1,1\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	_, err := Run(Options{
		ROMPath: romPath, ConfigDir: cfgDir, OutputDir: filepath.Join(root, "gen"), Jobs: 1,
		AllowStubs: true, ExperimentalExactDirectCallMX: true,
		ProvenEntryFacts: []analysis.EntryFact{{
			PC: 0x008200, EntryMX: analysis.MXState{M: 1, X: 1}, Kind: analysis.EntryRoutine, TemplateFree: true,
			Evidence: []analysis.Evidence{{Source: "static.direct_jsr", Confidence: analysis.ConfidenceProven}},
		}},
	})
	if err == nil || !strings.Contains(err.Error(), "metadata blockers (custom_name)") {
		t.Fatalf("custom metadata ablation error = %v", err)
	}
}

func assertGeneratedDirectoriesEqual(t *testing.T, left, right string) {
	t.Helper()
	leftEntries, err := os.ReadDir(left)
	if err != nil {
		t.Fatal(err)
	}
	rightEntries, err := os.ReadDir(right)
	if err != nil {
		t.Fatal(err)
	}
	if len(leftEntries) != len(rightEntries) {
		t.Fatalf("generated file counts differ: %d != %d", len(leftEntries), len(rightEntries))
	}
	for index, leftEntry := range leftEntries {
		if leftEntry.Name() != rightEntries[index].Name() {
			t.Fatalf("generated filenames differ at %d: %q != %q", index, leftEntry.Name(), rightEntries[index].Name())
		}
		leftBytes, readErr := os.ReadFile(filepath.Join(left, leftEntry.Name()))
		if readErr != nil {
			t.Fatal(readErr)
		}
		rightBytes, readErr := os.ReadFile(filepath.Join(right, leftEntry.Name()))
		if readErr != nil {
			t.Fatal(readErr)
		}
		if string(leftBytes) != string(rightBytes) {
			t.Fatalf("generated file %s differs under entry overlay", leftEntry.Name())
		}
	}
}
