package regen

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestFiniteTableTargetDiscoveryClosure(t *testing.T) {
	for _, tt := range []struct {
		name, extra   string
		enabled, want bool
	}{
		{"default unchanged", "", false, false},
		{"finite records and subsequent direct closure", "", true, true},
		{"HLE writer", "hle_func 8100 Host\n", true, false},
		{"HLE consumer", "hle_func 8200 Host\n", true, false},
		{"authored target data", "data_region 00 9400 9424\n", true, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			dir := t.TempDir()
			cfg := filepath.Join(dir, "cfg")
			if err := os.Mkdir(cfg, 0700); err != nil {
				t.Fatal(err)
			}
			image := make([]byte, 0x8000)
			copy(image, []byte{0x20, 0, 0x81, 0x20, 0, 0x82, 0x60})
			copy(image[0x100:], []byte{0xa5, 0x72, 0x29, 3, 0, 0x0a, 0x0a, 0x0a, 0xa8,
				0x4b, 0xab, 0xb9, 0, 0x90, 0x8f, 0, 0x12, 0x7e,
				0xb9, 6, 0x90, 0x85, 0x42, 0x60})
			copy(image[0x200:], []byte{0x6c, 0x42, 0})
			copy(image[0x1006:], []byte{0, 0x94})
			copy(image[0x1016:], []byte{0x20, 0x94})
			image[0x1400] = 0x60
			copy(image[0x1420:], []byte{0x20, 0, 0x95, 0x60})
			image[0x1500] = 0x60
			romPath := filepath.Join(dir, "fixture.sfc")
			if err := os.WriteFile(romPath, image, 0600); err != nil {
				t.Fatal(err)
			}
			authored := "bank = 00\nfunc Root 8000 entry_mx:0,0\n" + tt.extra
			cfgPath := filepath.Join(cfg, "bank00.cfg")
			if err := os.WriteFile(cfgPath, []byte(authored), 0600); err != nil {
				t.Fatal(err)
			}
			var hash string
			for _, jobs := range []int{1, 4} {
				out := filepath.Join(dir, "gen")
				r, err := Run(Options{ROMPath: romPath, ConfigDir: cfg, OutputDir: out, Jobs: jobs, AllowStubs: true, ExperimentalStoredTargets: tt.enabled})
				if err != nil {
					t.Fatal(err)
				}
				if r.ObservedEntryRoots != 0 || (hash != "" && hash != r.SemanticSourceSHA256) {
					t.Fatal("observed or nondeterministic closure")
				}
				hash = r.SemanticSourceSHA256
				body, err := os.ReadFile(filepath.Join(out, "bank00_v2.c"))
				if err != nil {
					t.Fatal(err)
				}
				for _, pc := range []uint16{0x9400, 0x9420, 0x9500} {
					for m := 0; m < 2; m++ {
						for x := 0; x < 2; x++ {
							name := fmt.Sprintf("RecompReturn bank_00_%04X_M%dX%d(CpuState *cpu) {", pc, m, x)
							if strings.Contains(string(body), name) != tt.want {
								t.Fatalf("%s: wanted=%v", name, tt.want)
							}
						}
					}
				}
			}
			if actual, err := os.ReadFile(cfgPath); err != nil || string(actual) != authored {
				t.Fatal("authored config changed")
			}
		})
	}
}
