package regen

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestNullableTableReentersColdDiscovery(t *testing.T) {
	for _, tt := range []struct {
		name, extra   string
		enabled, want bool
	}{
		{"default", "", false, false},
		{"handler then callee", "", true, true},
		{"HLE dispatcher", "hle_func 8000 Host\n", true, false},
		{"data handler", "data_region 00 8220 8224\n", true, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			dir := t.TempDir()
			cfg := filepath.Join(dir, "cfg")
			if err := os.Mkdir(cfg, 0700); err != nil {
				t.Fatal(err)
			}
			image := make([]byte, 0x8000)
			copy(image, []byte{0xa4, 0x70, 0x0a, 0xaa, 0x7c, 0, 0x81})
			copy(image[0x100:], []byte{0, 0x82, 0x10, 0x82, 0, 0, 0x20, 0x82})
			image[0x200], image[0x210], image[0x300] = 0x60, 0x60, 0x60
			copy(image[0x220:], []byte{0x20, 0, 0x83, 0x60})
			romPath := filepath.Join(dir, "fixture.sfc")
			if err := os.WriteFile(romPath, image, 0600); err != nil {
				t.Fatal(err)
			}
			cfgPath := filepath.Join(cfg, "bank00.cfg")
			authored := "bank = 00\nfunc Root 8000 entry_mx:0,0\n" + tt.extra
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
				for _, pc := range []uint16{0x8220, 0x8300} {
					for m := 0; m < 2; m++ {
						for x := 0; x < 2; x++ {
							name := fmt.Sprintf("RecompReturn bank_00_%04X_M%dX%d(CpuState *cpu) {", pc, m, x)
							if strings.Contains(string(body), name) != tt.want {
								t.Fatalf("%s want %v", name, tt.want)
							}
						}
					}
				}
			}
			if got, err := os.ReadFile(cfgPath); err != nil || string(got) != authored {
				t.Fatal("configuration mutated")
			}
		})
	}
}
