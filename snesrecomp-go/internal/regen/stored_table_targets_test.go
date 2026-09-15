package regen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestStoredRecordDiscoveryKeepsAuthoredBoundaries(t *testing.T) {
	for _, tt := range []struct {
		name, extra   string
		enabled, want bool
	}{
		{"default unchanged", "", false, false},
		{"table slot and subsequent direct closure", "", true, true},
		{"HLE setter", "hle_func 8100 HostSetter\n", true, false},
		{"conditional HLE setter", "hle_func_if 8100 HostSetter UseHost\n", true, false},
		{"HLE consumer", "hle_func 8000 HostReader\n", true, false},
		{"data boundary", "data_region 00 8400 8401\n", true, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			root := t.TempDir()
			cfg, romPath := filepath.Join(root, "cfg"), filepath.Join(root, "fixture.sfc")
			if err := os.Mkdir(cfg, 0700); err != nil {
				t.Fatal(err)
			}
			image := make([]byte, 0x8000)
			copy(image, []byte{0x22, 0, 0x81, 0, 0x6c, 0x42, 0})
			copy(image[0x100:], []byte{0x8b, 0x4b, 0xab, 0x0a, 0x0a, 0xa8, 0xbe, 0, 0x83, 0xb9, 2, 0x83, 0x8d, 0x42, 0, 0xab, 0x6b})
			copy(image[0x300:], []byte{0x34, 0x12, 0, 0x84, 0x78, 0x56, 0x10, 0x84})
			image[0x400] = 0x60
			copy(image[0x410:], []byte{0x20, 0, 0x85, 0x60})
			image[0x500] = 0x60
			if err := os.WriteFile(romPath, image, 0600); err != nil {
				t.Fatal(err)
			}
			config := "bank = 00\nfunc Root 8000 entry_mx:0,0\n" + tt.extra
			cfgPath := filepath.Join(cfg, "bank00.cfg")
			if err := os.WriteFile(cfgPath, []byte(config), 0600); err != nil {
				t.Fatal(err)
			}
			var hash string
			for _, jobs := range []int{1, 4} {
				out := filepath.Join(root, "gen")
				r, err := Run(Options{ROMPath: romPath, ConfigDir: cfg, OutputDir: out, Jobs: jobs, AllowStubs: true, ExperimentalStoredTargets: tt.enabled})
				if err != nil {
					t.Fatal(err)
				}
				if r.ObservedEntryRoots != 0 || (hash != "" && hash != r.SemanticSourceSHA256) {
					t.Fatal("non-static or order-dependent closure")
				}
				hash = r.SemanticSourceSHA256
				body, err := os.ReadFile(filepath.Join(out, "bank00_v2.c"))
				if err != nil {
					t.Fatal(err)
				}
				for _, name := range []string{"bank_00_8400_M0X0", "bank_00_8410_M0X0", "bank_00_8500_M0X0"} {
					if strings.Contains(string(body), "RecompReturn "+name+"(CpuState *cpu) {") != tt.want {
						t.Fatalf("cold body %s, wanted=%v", name, tt.want)
					}
				}
			}
			if actual, err := os.ReadFile(cfgPath); err != nil || string(actual) != config {
				t.Fatal("authored configuration changed")
			}
		})
	}
}
