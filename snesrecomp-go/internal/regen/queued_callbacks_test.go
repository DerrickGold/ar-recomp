package regen

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestQueuedCallbackReentersDiscovery(t *testing.T) {
	for _, tt := range []struct {
		name, extra   string
		bank          int
		enabled, want bool
	}{
		{"default unchanged", "", 0, false, false},
		{"callback and direct callee", "", 0, true, true},
		{"HLE caller", "hle_func 8000 Host\n", 0, true, false},
		{"HLE setter", "hle_func_if 8100 Host UseHost\n", 1, true, false},
		{"HLE consumer", "hle_func 8300 Host\n", 2, true, false},
		{"data target", "data_region 00 9200 9210\n", 0, true, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			dir := t.TempDir()
			cfg := filepath.Join(dir, "cfg")
			if err := os.Mkdir(cfg, 0700); err != nil {
				t.Fatal(err)
			}
			image := make([]byte, 0x18000)
			copy(image, []byte{0xa9, 0, 0x92, 0x22, 0, 0x81, 1, 0x22, 0, 0x83, 2, 0x60})
			copy(image[0x8100:], []byte{0x4c, 0, 0x82})
			copy(image[0x8200:], []byte{0xac, 0, 0x10, 0x99, 0, 0x12, 0xa3, 3, 0x99, 2, 0x12, 0xa5, 0x70, 0x99, 3, 0x12, 0x6b})
			copy(image[0x10300:], []byte{0xac, 0, 0x10, 0xb9, 0, 0x12, 0x85, 0x42, 0xb9, 2, 0x12, 0x85, 0x44, 0xbe, 3, 0x12, 0x86, 0x70, 0x4b, 0xf4, 0x18, 0x83, 0xdc, 0x42, 0, 0x6b})
			copy(image[0x1200:], []byte{0x20, 0, 0x94, 0x6b})
			image[0x1400] = 0x60
			romPath := filepath.Join(dir, "fixture.sfc")
			if err := os.WriteFile(romPath, image, 0600); err != nil {
				t.Fatal(err)
			}
			authored := map[string]string{}
			for bank := 0; bank < 3; bank++ {
				text := fmt.Sprintf("bank = %02X\n", bank)
				if bank == 0 {
					text += "func Root 8000 entry_mx:0,0\n"
				}
				if bank == tt.bank {
					text += tt.extra
				}
				path := filepath.Join(cfg, fmt.Sprintf("bank%02X.cfg", bank))
				if err := os.WriteFile(path, []byte(text), 0600); err != nil {
					t.Fatal(err)
				}
				authored[path] = text
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
				for _, pc := range []uint16{0x9200, 0x9400} {
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
			for path, want := range authored {
				if got, err := os.ReadFile(path); err != nil || string(got) != want {
					t.Fatal("authored configuration changed")
				}
			}
		})
	}
}
