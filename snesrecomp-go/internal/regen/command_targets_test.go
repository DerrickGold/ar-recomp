package regen

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestCommandColdTargetJoinsDiscoveryFixedPoint(t *testing.T) {
	for _, tt := range []struct {
		name, extra   string
		enabled, want bool
		shape         string
	}{
		{"default unchanged", "", false, false, ""},
		{"native conditional operand", "", true, true, ""},
		{"HLE caller", "hle_func 8000 HostCaller\n", true, false, ""},
		{"conditional HLE caller", "hle_func_if 8000 HostCaller UseHost\n", true, false, ""},
		{"HLE engine", "hle_func 8200 HostEngine\n", true, false, ""},
		{"authored data target", "data_region 00 9200 9204\n", true, false, ""},
		{"published cursor default unchanged", "", false, false, "published"},
		{"published cursor then subsequent direct closure", "", true, true, "published"},
		{"published cursor HLE reload", "hle_func 8B00 HostReload\n", true, false, "published"},
		{"ROM cursor default unchanged", "", false, false, "cursor"},
		{"ROM cursor then callback and callee", "", true, true, "cursor"},
		{"ROM cursor HLE command", "hle_func 8940 HostCommand\n", true, false, "cursor"},
		{"ROM cursor data target", "data_region 00 9200 9204\n", true, false, "cursor"},
		{"callback prefix default unchanged", "", false, false, "prefix"},
		{"callback prefix then callback and callee", "", true, true, "prefix"},
		{"callback prefix HLE command", "hle_func 8940 HostCommand\n", true, false, "prefix"},
		{"callback prefix HLE reload", "hle_func 8B00 HostReload\n", true, false, "prefix"},
		{"rebased field default unchanged", "", false, false, "rebased"},
		{"rebased cursor then callback and callee", "", true, true, "rebased"},
		{"rebased cursor HLE store", "hle_func 8940 HostStore\n", true, false, "rebased"},
		{"rebased cursor HLE reload", "hle_func 8980 HostReload\n", true, false, "rebased"},
		{"rebased cursor authored data", "data_region 00 9200 9204\n", true, false, "rebased"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			dir := t.TempDir()
			cfg := filepath.Join(dir, "cfg")
			if err := os.Mkdir(cfg, 0700); err != nil {
				t.Fatal(err)
			}
			image := make([]byte, 4*0x8000)
			copy(image, []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60})
			copy(image[0x100:], []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60})
			copy(image[0x200:], []byte{0xf4, 2, 2, 0xab, 0xab, 0x0a, 0x0a, 0xa8, 0xb9, 0, 0x90, 0xa8, 0x88,
				0xb9, 0, 0, 0x30, 1, 0x60, 0xc8, 0xc8, 0xeb, 0x29, 1, 0, 0x0a, 0xaa, 0x7c, 0, 0x88})
			copy(image[0x800:], []byte{0, 0x89, 0x10, 0x89})
			copy(image[0x900:], []byte{0xb9, 0, 0, 0x85, 0x72, 0x6c, 0x72, 0})
			image[0x910] = 0x60
			copy(image[0x1100c:], []byte{0x20, 0xa0})
			copy(image[0x1201f:], []byte{0, 0x80, 0, 0x92})
			copy(image[0x1200:], []byte{0x20, 0, 0x94, 0x60})
			image[0x1400] = 0x60
			if tt.shape == "published" {
				copy(image, []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x20, 0, 0x8b, 0x60})
				copy(image[0x800:], []byte{0x40, 0x89})
				copy(image[0x940:], []byte{0x98, 0x18, 0x69, 3, 0, 0x95, 0x46, 0x60})
				copy(image[0x910:], []byte{0xb9, 0, 0, 0x85, 0x72, 0x6c, 0x72, 0})
				copy(image[0xb00:], []byte{0xb4, 0x46, 0x88, 0x4c, 0x0d, 0x82})
				copy(image[0x1201f:], []byte{0, 0x80, 0, 0, 0, 0x81, 0, 0x92})
			}
			if tt.shape == "cursor" {
				copy(image[0x800:], []byte{0x40, 0x89})
				copy(image[0x940:], []byte{0xb9, 0, 0, 0xa8, 0x88, 0x4c, 0x0d, 0x82})
				copy(image[0x910:], []byte{0xb9, 0, 0, 0x85, 0x72, 0x6c, 0x72, 0})
				copy(image[0x1201f:], []byte{0, 0x80, 1, 0xa1})
				copy(image[0x12100:], []byte{0, 0x81, 0, 0x92})
			}
			if tt.shape == "prefix" {
				copy(image, []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x20, 0, 0x8b, 0x60})
				copy(image[0x800:], []byte{0x40, 0x89})
				copy(image[0x940:], []byte{0xb9, 0, 0, 0x85, 0x72, 0xc8, 0xc8, 0x94, 0x46, 0x5a, 0x8b, 0x4b, 0xab, 0xf4, 0x55, 0x89, 0x6c, 0x72, 0})
				copy(image[0x910:], []byte{0xb9, 0, 0, 0x85, 0x72, 0x6c, 0x72, 0})
				copy(image[0xb00:], []byte{0xb4, 0x46, 0x88, 0x4c, 0x0d, 0x82})
				copy(image[0x1600:], []byte{0xdc, 0x98, 0})
				copy(image[0x1201f:], []byte{0, 0x80, 0, 0x96, 0x81, 0, 0x92})
			}
			if tt.shape == "rebased" {
				image[0x217] = 3 // selector mask, four command entries
				copy(image[0x800:], []byte{0x40, 0x89, 0x10, 0x89, 0x80, 0x89, 0x30, 0x89})
				image[0x930] = 0x60
				copy(image[0x940:], []byte{0xb9, 0, 0, 0x29, 0xff, 0, 0x18, 0x65, 0x70, 0xaa, 0xc8, 0xb9, 0, 0, 0xc8, 0xc8, 0x94, 0, 0xa8, 0xa6, 0x70, 0x88, 0x4c, 0x0d, 0x82})
				copy(image[0x980:], []byte{0xb9, 0, 0, 0x29, 0xff, 0, 0x18, 0x65, 0x70, 0xaa, 0xb4, 0, 0xa6, 0x70, 0x88, 0x4c, 0x0d, 0x82})
				copy(image[0x910:], []byte{0xb9, 0, 0, 0x85, 0x72, 0x6c, 0x72, 0})
				copy(image[0x1201f:], []byte{0, 0x80, 0x2c, 0x40, 0xa0, 0x81, 0, 0x92})
				copy(image[0x1203f:], []byte{0, 0x82, 0x2c, 0})
			}
			romPath := filepath.Join(dir, "fixture.sfc")
			if err := os.WriteFile(romPath, image, 0600); err != nil {
				t.Fatal(err)
			}
			authored := "bank = 00\nfunc Root 8000 entry_mx:0,0\n" + tt.extra
			for bank := 0; bank < 4; bank++ {
				body := fmt.Sprintf("bank = %02X\n", bank)
				if bank == 0 {
					body = authored
				}
				if err := os.WriteFile(filepath.Join(cfg, fmt.Sprintf("bank%02x.cfg", bank)), []byte(body), 0600); err != nil {
					t.Fatal(err)
				}
			}
			var hash string
			for _, jobs := range []int{1, 4} {
				out := filepath.Join(dir, "gen")
				r, err := Run(Options{ROMPath: romPath, ConfigDir: cfg, OutputDir: out, Jobs: jobs, AllowStubs: true, ExperimentalStoredTargets: tt.enabled})
				if err != nil {
					t.Fatal(err)
				}
				if r.ObservedEntryRoots != 0 || (hash != "" && hash != r.SemanticSourceSHA256) {
					t.Fatal("observed or order-dependent discovery")
				}
				hash = r.SemanticSourceSHA256
				body, err := os.ReadFile(filepath.Join(out, "bank00_v2.c"))
				if err != nil {
					t.Fatal(err)
				}
				for _, pc := range []string{"9200", "9400"} {
					if got := strings.Contains(string(body), "RecompReturn bank_00_"+pc+"_M0X0(CpuState *cpu) {"); got != tt.want {
						t.Fatalf("target %s generated=%v want=%v", pc, got, tt.want)
					}
				}
			}
			if actual, err := os.ReadFile(filepath.Join(cfg, "bank00.cfg")); err != nil || string(actual) != authored {
				t.Fatal("authored configuration changed")
			}
		})
	}
}
