package regen

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestIndexedRecordCallsReenterStaticClosure(t *testing.T) {
	for _, tt := range []struct {
		name, extra   string
		enabled, want bool
	}{
		{"default", "", false, false}, {"closure", "", true, true},
		{"HLE writer", "hle_func 8100 Host\n", true, false},
		{"HLE consumer", "hle_func 8200 Host\n", true, false},
		{"data target", "data_region 00 9400 9403\n", true, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			dir := t.TempDir()
			cfg := filepath.Join(dir, "cfg")
			if err := os.Mkdir(cfg, 0700); err != nil {
				t.Fatal(err)
			}
			image := make([]byte, 0x8000)
			copy(image, []byte{0x20, 0, 0x81, 0x20, 0, 0x82, 0x60})
			copy(image[0x100:], []byte{0x4b, 0xab, 0xa5, 0x70, 0x29, 1, 0, 0x0a, 0xa8, 0xbe, 0, 0x90, 0x8e, 0x40, 0, 0x60})
			copy(image[0x200:], []byte{0xae, 0x40, 0, 0xfc, 6, 0, 0x60})
			copy(image[0x1000:], []byte{0, 0x92, 0x10, 0x92})
			copy(image[0x1206:], []byte{0, 0x94})
			copy(image[0x1216:], []byte{0, 0x94})
			copy(image[0x1400:], []byte{0x20, 0, 0x95, 0x60})
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
				if r.ObservedEntryRoots != 0 || hash != "" && hash != r.SemanticSourceSHA256 {
					t.Fatal("observed or nondeterministic closure")
				}
				hash = r.SemanticSourceSHA256
				body, err := os.ReadFile(filepath.Join(out, "bank00_v2.c"))
				if err != nil {
					t.Fatal(err)
				}
				for _, pc := range []uint16{0x9400, 0x9500} {
					for m := 0; m < 2; m++ {
						for x := 0; x < 2; x++ {
							name := fmt.Sprintf("RecompReturn bank_00_%04X_M%dX%d(CpuState *cpu) {", pc, m, x)
							if strings.Contains(string(body), name) != tt.want {
								t.Fatalf("%s wanted=%v", name, tt.want)
							}
						}
					}
				}
			}
			if b, err := os.ReadFile(cfgPath); err != nil || string(b) != authored {
				t.Fatal("config changed")
			}
		})
	}
}
