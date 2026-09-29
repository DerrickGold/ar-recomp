package main

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/artifact"
	"github.com/DerrickGold/snesrecomp-go/internal/tooling"
)

func TestAnalysisFlagsDoNotImplicitlySelectCallWidths(t *testing.T) {
	root := t.TempDir()
	romPath, cfg := filepath.Join(root, "game.sfc"), filepath.Join(root, "recomp")
	image := make([]byte, 0x8000)
	copy(image, []byte{0x20, 0, 0x81, 0x60})
	image[0x100] = 0x60
	if err := os.WriteFile(romPath, image, 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(cfg, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfg, "bank00.cfg"), []byte("bank = 00\nfunc Root 8000 entry_mx:1,1\n"), 0600); err != nil {
		t.Fatal(err)
	}
	shadow, err := tooling.AnalyzeAuthoredShadow(tooling.ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfg, Jobs: 1})
	if err != nil {
		t.Fatal(err)
	}
	db, err := tooling.BuildStaticAnalysisDatabase(shadow)
	if err != nil {
		t.Fatal(err)
	}
	if len(db.DispatchFacts) != 0 || len(db.EntryFacts) != 0 {
		t.Fatal("fixture is not a zero-fact database")
	}
	dbPath := filepath.Join(root, "facts.json")
	if err := tooling.WriteStaticAnalysisDatabaseFile(dbPath, db); err != nil {
		t.Fatal(err)
	}
	var control artifact.Manifest
	for i, flags := range [][]string{nil, {"--analysis-db", dbPath}, {"--experimental-proven-analysis"}, {"--analysis-db", dbPath, "--experimental-exact-direct-call-mx"}} {
		out := filepath.Join(root, []string{"normal", "database", "automatic", "explicit"}[i])
		args := append([]string{"--rom", romPath, "--cfg-dir", cfg, "--out-dir", out, "--jobs", "1", "--allow-stubs"}, flags...)
		if err := regenerate(args); err != nil {
			t.Fatal(err)
		}
		files, err := artifact.FromDir(out)
		if err != nil {
			t.Fatal(err)
		}
		if i == 0 {
			control = files
			continue
		}
		if reflect.DeepEqual(control, files) != (i != 3) {
			t.Fatalf("mode %d did not retain its explicit width policy", i)
		}
	}
}

func TestExactCallWidthsRefuseDefaultOutput(t *testing.T) {
	err := regenerate([]string{"--experimental-exact-direct-call-mx"})
	if err == nil || !strings.Contains(err.Error(), "refusing to replace src/gen") {
		t.Fatalf("got %v", err)
	}
}
