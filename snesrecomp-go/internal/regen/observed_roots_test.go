package regen

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/tooling"
)

func TestObservedCensusSeedsWithoutAuthoringOrStaticProof(t *testing.T) {
	root := t.TempDir()
	romPath, cfgDir := filepath.Join(root, "fixture.sfc"), filepath.Join(root, "cfg")
	image := make([]byte, 0x8000)
	image[0], image[0x100], image[0x200], image[0x300] = 0x60, 0x60, 0x60, 0x60
	if err := os.WriteFile(romPath, image, 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(cfgDir, 0700); err != nil {
		t.Fatal(err)
	}
	cfg := []byte("bank = 00\nfunc Authored 8000 entry_mx:0,0\ndata_region 00 8400 8500\n")
	cfgPath := filepath.Join(cfgDir, "bank00.cfg")
	if err := os.WriteFile(cfgPath, cfg, 0600); err != nil {
		t.Fatal(err)
	}
	hash := sha256.Sum256(image)
	base := tooling.DispatchCensusReport{Version: 2, Provenance: "snesrecomp-runtime-dispatch-census-v2",
		ROMHash: hex.EncodeToString(hash[:]), TraceHash: strings.Repeat("1", 64),
		Observations: []tooling.DispatchObservation{
			{SitePC: 0x8000, TargetPC: 0x808100, M: 0, X: 0, ObservationCount: 3, Trapped: true},
			{SitePC: 0x8000, TargetPC: 0x8200, M: 0, X: 0, ObservationCount: 2, Continuation: true},
			{SitePC: 0x8000, TargetPC: 0x8300, M: 0, X: 0, ObservationCount: 2, Found: true},
		}}
	for _, tt := range []struct {
		name   string
		change func(*tooling.DispatchCensusReport)
		bad    bool
	}{
		{"valid", func(*tooling.DispatchCensusReport) {}, false},
		{"wrong ROM", func(r *tooling.DispatchCensusReport) { r.ROMHash = strings.Repeat("0", 64) }, true},
		{"overflow", func(r *tooling.DispatchCensusReport) { r.Overflow = true }, true},
		{"missing trace identity", func(r *tooling.DispatchCensusReport) { r.TraceHash = "" }, true},
		{"invalid width", func(r *tooling.DispatchCensusReport) { r.Observations[0].M = 2 }, true},
		{"zero observations", func(r *tooling.DispatchCensusReport) { r.Observations[0].ObservationCount = 0 }, true},
		{"authored data collision", func(r *tooling.DispatchCensusReport) { r.Observations[0].TargetPC = 0x8400 }, true},
		{"unmapped target", func(r *tooling.DispatchCensusReport) { r.Observations[0].TargetPC = 0x001234 }, true},
		{"emulation", func(r *tooling.DispatchCensusReport) { r.Observations[0].Emulation = true }, true},
		{"valid then invalid is atomic", func(r *tooling.DispatchCensusReport) {
			r.Observations = append(r.Observations, tooling.DispatchObservation{
				SitePC: 0x8000, TargetPC: 0x8400, M: 0, X: 0, ObservationCount: 1})
		}, true},
	} {
		t.Run(tt.name, func(t *testing.T) {
			r := base
			r.Observations = append([]tooling.DispatchObservation(nil), base.Observations...)
			tt.change(&r)
			path := filepath.Join(root, "census.json")
			if err := tooling.WriteDispatchCensusFile(path, r); err != nil {
				t.Fatal(err)
			}
			repo, err := loadRepository(romPath, cfgDir)
			if err != nil {
				t.Fatal(err)
			}
			count, err := repo.applyObservedDispatchCensus(romPath, []string{path, path})
			if (err != nil) != tt.bad {
				t.Fatalf("count=%d err=%v", count, err)
			}
			if tt.bad {
				if len(repo.banks[0].Config.Entries) != 1 || len(repo.observedVariants) != 0 {
					t.Fatal("rejected evidence partially changed entries")
				}
				return
			}
			if count != 1 || len(repo.banks[0].Config.Entries) != 2 || len(repo.staticEntryDiscoveries) != 0 {
				t.Fatalf("observation became authored/static proof: %+v", repo.banks[0].Config.Entries)
			}
			v := codegen.Variant{Address: 0x8100, M: 0, X: 0}
			dirty := map[codegen.Variant]struct{}{v: {}}
			emitted := map[codegen.Variant]struct{}{v: {}, {Address: 0x8100, M: 1, X: 1}: {}}
			if got := repo.computePrunable(dirty, emitted, nil); len(got) != 0 {
				t.Fatal("observed exact width pruned")
			}
			actual, err := os.ReadFile(cfgPath)
			if err != nil || string(actual) != string(cfg) {
				t.Fatal("authored cfg changed")
			}
		})
	}
}
