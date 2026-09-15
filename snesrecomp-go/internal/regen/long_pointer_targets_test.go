package regen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestOverlappingLongPointerColdRootsPreserveOverrides(t *testing.T) {
	for _, tt := range []struct {
		name, extra   string
		enabled, want bool
	}{
		{"default unchanged", "", false, false},
		{"overlapping literal writes", "", true, true},
		{"HLE writer", "hle_func 8000 HostWriter\n", true, false},
		{"conditional HLE writer", "hle_func_if 8000 HostWriter UseHost\n", true, false},
		{"HLE reader", "hle_func 8100 HostReader\n", true, false},
		{"authored data", "data_region 00 8210 8211\n", true, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			root := t.TempDir()
			cfg := filepath.Join(root, "cfg")
			out := filepath.Join(root, "gen")
			romPath := filepath.Join(root, "fixture.sfc")
			if err := os.Mkdir(cfg, 0700); err != nil {
				t.Fatal(err)
			}
			image := make([]byte, 0x8000)
			copy(image, []byte{0xa9, 0x82, 0, 0x8d, 0x41, 0, 0xa9, 0x10, 0x82, 0x8d, 0x40, 0, 0x60})
			copy(image[0x100:], []byte{0xdc, 0x40, 0})
			image[0x210] = 0x6b
			if err := os.WriteFile(romPath, image, 0600); err != nil {
				t.Fatal(err)
			}
			config := "bank = 00\nfunc Writer 8000 entry_mx:0,0\nfunc Reader 8100 entry_mx:0,0\n" + tt.extra
			cfgPath := filepath.Join(cfg, "bank00.cfg")
			if err := os.WriteFile(cfgPath, []byte(config), 0600); err != nil {
				t.Fatal(err)
			}
			var hash string
			for _, jobs := range []int{1, 4} {
				r, err := Run(Options{ROMPath: romPath, ConfigDir: cfg, OutputDir: out, Jobs: jobs, AllowStubs: true, ExperimentalStoredTargets: tt.enabled})
				if err != nil {
					t.Fatal(err)
				}
				if r.ObservedEntryRoots != 0 || (hash != "" && hash != r.SemanticSourceSHA256) {
					t.Fatal("non-static or non-deterministic roots")
				}
				hash = r.SemanticSourceSHA256
				body, err := os.ReadFile(filepath.Join(out, "bank00_v2.c"))
				if err != nil {
					t.Fatal(err)
				}
				if strings.Contains(string(body), "RecompReturn bank_00_8210_M0X0(CpuState *cpu) {") != tt.want {
					t.Fatalf("cold target wanted=%v", tt.want)
				}
			}
			if actual, err := os.ReadFile(cfgPath); err != nil || string(actual) != config {
				t.Fatal("authored configuration changed")
			}
		})
	}
}
