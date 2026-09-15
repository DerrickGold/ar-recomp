package regen

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestImmediateContinuationReentersDiscoveryAfterBudgetStop(t *testing.T) {
	for _, tt := range []struct {
		name, extra   string
		enabled, want bool
	}{
		{"default unchanged", "", false, false},
		{"continuation then callee and next continuation", "", true, true},
		{"HLE software caller", "hle_func 8100 Host\n", true, false},
		{"conditional HLE software caller", "hle_func_if 8100 Host UseHost\n", true, false},
		{"data continuation", "data_region 00 8200 8210\n", true, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			dir := t.TempDir()
			cfg := filepath.Join(dir, "cfg")
			if err := os.Mkdir(cfg, 0700); err != nil {
				t.Fatal(err)
			}
			image := make([]byte, 0x8000)
			copy(image, []byte{0xa9, 0, 0x85, 0x85, 0x98, 0x38, 0x20, 0, 0x81, 0x60})
			copy(image[0x100:], []byte{0xb0, 0x0b, 0xf4, 0xff, 0x83, 0xa2, 0, 0x40, 0xca, 0xd0, 0xfd, 0x60, 0xea,
				0xf4, 0xff, 0x81, 0x6c, 0x98, 0})
			copy(image[0x200:], []byte{0x20, 0, 0x83, 0xf4, 8, 0x82, 0x6c, 0x98, 0, 0x60})
			image[0x300], image[0x400], image[0x500] = 0x60, 0x60, 0x60
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
					t.Fatal("observed or order-dependent closure")
				}
				hash = r.SemanticSourceSHA256
				body, err := os.ReadFile(filepath.Join(out, "bank00_v2.c"))
				if err != nil {
					t.Fatal(err)
				}
				for _, pc := range []uint16{0x8200, 0x8209, 0x8300} {
					for m := 0; m < 2; m++ {
						for x := 0; x < 2; x++ {
							name := fmt.Sprintf("RecompReturn bank_00_%04X_M%dX%d(CpuState *cpu) {", pc, m, x)
							if strings.Contains(string(body), name) != tt.want {
								t.Fatalf("%s: want %v", name, tt.want)
							}
						}
					}
				}
			}
			if actual, err := os.ReadFile(cfgPath); err != nil || string(actual) != authored {
				t.Fatal("authored configuration changed")
			}
		})
	}
}
