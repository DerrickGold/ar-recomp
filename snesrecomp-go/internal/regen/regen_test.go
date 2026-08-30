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
		"func RoutineA 8100 entry_mx:1,1\n" +
		"func RoutineB 8200 entry_mx:1,1\n"
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
			PC: pc, EntryMX: analysis.MXState{M: 1, X: 1}, Kind: analysis.EntryRoutine,
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
	if normal.FinalEntries != overlay.FinalEntries || normal.Functions != overlay.Functions {
		t.Fatalf("normal entries/functions %d/%d, overlay %d/%d",
			normal.FinalEntries, normal.Functions, overlay.FinalEntries, overlay.Functions)
	}
	assertGeneratedDirectoriesEqual(t, normalDir, overlayDir)
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
