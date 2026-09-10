package regen

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
)

func TestNativeReturnTableRegeneration(t *testing.T) {
	dir := t.TempDir()
	cfg := filepath.Join(dir, "cfg")
	if err := os.Mkdir(cfg, 0700); err != nil {
		t.Fatal(err)
	}
	image := make([]byte, 0x10000)
	copy(image, []byte{0x22, 0, 0x83, 1, 8, 0x80, 0x10, 0x80, 0x6b})
	image[0x10] = 0x6b
	copy(image[0x8300:], []byte{0x08, 0xc2, 0x30, 0xda, 0x5a, 0x29, 0xff, 0, 0x0a, 0xa8, 0xc8, 0x0b, 0x3b, 0x5b, 0xb7, 8, 0x85, 8, 0xc6, 8, 0x2b, 0x7a, 0xfa, 0x28, 0x6b})
	rom := filepath.Join(dir, "fixture.sfc")
	for path, data := range map[string][]byte{rom: image, filepath.Join(cfg, "bank00.cfg"): []byte("bank = 00\nfunc Root 8000 entry_mx:0,0\n"), filepath.Join(cfg, "bank01.cfg"): []byte("bank = 01\n")} {
		if err := os.WriteFile(path, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	var control Report
	for _, jobs := range []int{1, 8} {
		r, err := Run(Options{ROMPath: rom, ConfigDir: cfg, OutputDir: filepath.Join(dir, string(rune('0'+jobs))), Jobs: jobs, AllowStubs: true, ExperimentalExactDirectCallMX: true})
		if err != nil {
			t.Fatal(err)
		}
		want := []NativeReturnTableSite{{SitePC: 0x8000, HelperPC: 0x018300, TablePC: 0x8004, ReturnPC: 0x018318, Targets: []uint32{0x8008, 0x8010}, Modes: [][2]uint8{{0, 0}}}}
		if !reflect.DeepEqual(r.NativeReturnTables, want) {
			t.Fatalf("sites %+v", r.NativeReturnTables)
		}
		if jobs == 1 {
			control = r
		} else if r.SemanticSourceSHA256 != control.SemanticSourceSHA256 || r.FinalEntries != control.FinalEntries {
			t.Fatal("worker-dependent generation")
		}
	}
	data, err := os.ReadFile(filepath.Join(cfg, "bank00.cfg"))
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != "bank = 00\nfunc Root 8000 entry_mx:0,0\n" {
		t.Fatal("authored configuration changed")
	}
}

func TestNativeReturnTableAuthoredBarriers(t *testing.T) {
	pc := uint16(0x8300)
	end := uint16(0x8319)
	for name, cfg := range map[string]*config.Config{
		"HLE":               {HLEFunctions: map[uint16]string{pc: "owned"}},
		"conditional HLE":   {HLEFunctionsIf: map[uint16]config.HLEFunctionIf{pc: {Function: "owned", Predicate: "when"}}},
		"SPC":               {HLESPCUpload: []uint16{pc}},
		"dispatch HLE":      {HLEDispatch: map[uint16]string{pc: "owned"}},
		"end":               {Entries: []config.Entry{{Start: pc, End: &end}}},
		"tail":              {Entries: []config.Entry{{Start: pc, TailCallPC: &end}}},
		"stack":             {Entries: []config.Entry{{Start: pc, EntrySOffset: 2}}},
		"exit":              {Entries: []config.Entry{{Start: pc, ExitMX: &config.MX{M: 1, X: 1}}}},
		"exclude":           {ExcludeRanges: []config.Range{{Start: pc, End: end}}},
		"site exit":         {ExitMXAt: []config.ExitMXAt{{Address: 0x018300}}},
		"forced variant":    {ForceVariantAt: map[uint32]config.MX{0x018300: {}}},
		"RTS override":      {RTSDispatch: []config.RTSDispatch{{SitePC: pc}}},
		"indirect override": {IndirectDispatch: []config.IndirectDispatch{{SitePC: pc}}},
	} {
		t.Run(name, func(t *testing.T) {
			r := nativeReturnConfigBarriers(1, cfg)
			if len(r) != 1 || r[0][0] != 0x018300 || r[0][1] < r[0][0] {
				t.Fatalf("barrier %+v", r)
			}
		})
	}
	if got := nativeReturnConfigBarriers(1, &config.Config{Entries: []config.Entry{{Name: "NativeRoot", Start: pc}}}); len(got) != 0 {
		t.Fatal("ordinary named root changes native semantics")
	}
}
