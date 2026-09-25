package regen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A record-selected state machine: the initializer is reached with unknown DB,
// reuses its record-pointer slot across a call and publishes the table base
// and initial state; the dispatcher combines both through three-byte cells.
// Handlers and their direct callee are discovered with no observed roots and
// no authored dispatch, and planted garbage cells are never emitted.
func TestRecordStateTablesFeedDiscoveryWithoutObservedRoots(t *testing.T) {
	for _, tt := range []struct {
		name    string
		enabled bool
	}{{"default", false}, {"cold closure", true}} {
		t.Run(tt.name, func(t *testing.T) {
			dir := t.TempDir()
			cfg := filepath.Join(dir, "cfg")
			if err := os.Mkdir(cfg, 0700); err != nil {
				t.Fatal(err)
			}
			image := make([]byte, 0x8000)
			for n := 0x1000; n < 0x2000; n++ {
				image[n] = 0x60
			}
			copy(image, []byte{0xa5, 0x70, 0x29, 0xff, 0, 0x0a, 0x0a, 0x0a, 0x0a, 0x18, 0x69, 0, 0xa0,
				0x8d, 0x61, 0x1c, 0x20, 0, 0x88, 0xac, 0x61, 0x1c, 0xb9, 0x0b, 0, 0x8d, 0x7f, 0x1c,
				0xb9, 3, 0, 0x29, 0x7f, 0, 0x8d, 0x3b, 0x1c, 0x20, 0, 0x88,
				0x4b, 0xab, 0xac, 0x61, 0x1c, 0xb9, 2, 0, 0x60})
			copy(image[0x200:], []byte{0x4b, 0xab, 0xd8, 0xad, 0x3b, 0x1c, 0x30, 0x1f, 0x0a, 0x18,
				0x6d, 0x3b, 0x1c, 0x6d, 0x7f, 0x1c, 0x85, 0x42, 0xa0, 2, 0, 0xb1, 0x42,
				0x29, 0xff, 0, 0x8d, 0x3b, 0x1c, 0xb2, 0x42, 0x85, 0x42, 0xf4, 0x26, 0x82,
				0x6c, 0x42, 0, 0x60})
			image[0x800] = 0x60
			copy(image[0x2003:], []byte{1})
			copy(image[0x200b:], []byte{0x30, 0xa0})
			copy(image[0x2013:], []byte{3})
			copy(image[0x201b:], []byte{0x38, 0xa0})
			copy(image[0x202b:], []byte{0x38, 0xa0})
			copy(image[0x2030:], []byte{0, 0x91, 1, 0x10, 0x91, 0})
			copy(image[0x2036:], []byte{0x40, 0x93})
			copy(image[0x2038:], []byte{0, 0x92, 0, 0x10, 0x92, 0, 0x20, 0x92, 1, 0x30, 0x92, 2})
			copy(image[0x204b:], []byte{0x50, 0xa0})
			copy(image[0x2050:], []byte{0xee, 0x9e, 0, 0xee, 0x9e, 0, 0xee, 0x9e, 0, 0xee, 0x9e, 0})
			for _, handler := range []int{0x1100, 0x1110, 0x1200, 0x1210, 0x1220, 0x1230} {
				copy(image[handler:], []byte{0x20, 0, 0x96, 0x60})
			}
			romPath := filepath.Join(dir, "fixture.sfc")
			if err := os.WriteFile(romPath, image, 0600); err != nil {
				t.Fatal(err)
			}
			authored := "bank = 00\nfunc Initializer 8000 entry_mx:0,0\nfunc Dispatcher 8200 entry_mx:0,0\n"
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
				for _, pc := range []string{"9100", "9110", "9200", "9210", "9220", "9230", "9600"} {
					if strings.Contains(string(body), "RecompReturn bank_00_"+pc+"_M0X0(CpuState *cpu) {") != tt.enabled {
						t.Fatalf("handler %s wanted=%v", pc, tt.enabled)
					}
				}
				for _, pc := range []string{"9340", "9EEE"} {
					if strings.Contains(string(body), "RecompReturn bank_00_"+pc+"_M0X0(CpuState *cpu) {") {
						t.Fatalf("garbage cell %s was emitted", pc)
					}
				}
			}
			if b, err := os.ReadFile(cfgPath); err != nil || string(b) != authored {
				t.Fatal("authored configuration changed")
			}
		})
	}
}
