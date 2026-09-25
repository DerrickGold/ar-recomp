package regen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestIndirectValuesFeedNormalDiscoveryWithoutObservedRoots(t *testing.T) {
	for _, tt := range []struct {
		name, extra   string
		enabled, want bool
	}{
		{"default", "", false, false},
		{"cold closure", "", true, true},
		{"HLE producer", "hle_func 8000 Host\n", true, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			dir := t.TempDir()
			cfg := filepath.Join(dir, "cfg")
			if err := os.Mkdir(cfg, 0700); err != nil {
				t.Fatal(err)
			}
			image := make([]byte, 0x8000)
			// The producer publishes the table field and seeds the state; the
			// consumer's state reload therefore has no local definition.
			copy(image, []byte{0x4b, 0xab, 0xd8, 0xa5, 0x70, 0x29, 1, 0,
				0x0a, 0x0a, 0x0a, 0x0a, 0x18, 0x69, 0, 0x90, 0x8d, 0, 4,
				0xac, 0, 4, 0xb9, 0x0b, 0, 0x8d, 2, 4, 0xa9, 2, 0, 0x8d, 4, 4, 0x60})
			copy(image[0x100:], []byte{0x4b, 0xab, 0xd8,
				0xad, 4, 4, 0x0a, 0x18, 0x6d, 4, 4, 0x6d, 2, 4, 0x85, 0x42,
				0xa0, 2, 0, 0xb1, 0x42, 0x29, 0xff, 0, 0x8d, 4, 4,
				0xb2, 0x42, 0x85, 0x42, 0xf4, 0xff, 0x81, 0x6c, 0x42, 0})
			copy(image[0x100b:], []byte{0, 0x91})
			copy(image[0x101b:], []byte{0, 0x92})
			for n := range 2 {
				copy(image[0x1100+n*0x100:], []byte{byte(n * 32), 0x94, 2, 0, 0, 0, byte(n*32 + 16), 0x94, 0, 0})
				for _, p := range []int{0x1400 + n*32, 0x1410 + n*32} {
					copy(image[p:], []byte{0x20, 0, 0x95, 0x60})
				}
			}
			image[0x1500] = 0x60
			romPath := filepath.Join(dir, "fixture.sfc")
			if err := os.WriteFile(romPath, image, 0600); err != nil {
				t.Fatal(err)
			}
			authored := "bank = 00\nfunc Producer 8000 entry_mx:0,0\nfunc Consumer 8100 entry_mx:0,0\n" + tt.extra
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
					t.Fatal("observed roots or order-dependent generation")
				}
				hash = r.SemanticSourceSHA256
				body, err := os.ReadFile(filepath.Join(out, "bank00_v2.c"))
				if err != nil {
					t.Fatal(err)
				}
				for _, name := range []string{"bank_00_9400_M0X0", "bank_00_9430_M0X0", "bank_00_9500_M0X0"} {
					if strings.Contains(string(body), "RecompReturn "+name+"(CpuState *cpu) {") != tt.want {
						t.Fatalf("%s wanted=%v", name, tt.want)
					}
				}
			}
			if b, err := os.ReadFile(cfgPath); err != nil || string(b) != authored {
				t.Fatal("authored configuration changed")
			}
		})
	}
}
