package regen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
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
