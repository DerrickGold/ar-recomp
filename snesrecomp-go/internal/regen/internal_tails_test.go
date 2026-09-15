package regen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestInternalTailsExposeExistingBlocksOnly(t *testing.T) {
	if _, err := Run(Options{ExperimentalInternalTails: true, OnlyBanks: map[byte]struct{}{0: {}}}); err == nil || !strings.Contains(err.Error(), "full regeneration") {
		t.Fatalf("partial regeneration was not rejected before reading inputs: %v", err)
	}
	for _, tt := range []struct {
		name, extra string
		callback    []byte
		want        int
	}{
		{"ordinary", "", []byte{0xe6, 0x12, 0x4c, 0, 0x83}, 1},
		{"multiple identical owners", "func Second 8250 entry_mx:0,0\n", []byte{0xe6, 0x12, 0x4c, 0, 0x83}, 1},
		{"HLE owner", "hle_func 8200 HostCallback\n", []byte{0xe6, 0x12, 0x4c, 0, 0x83}, 0},
		{"HLE target", "hle_func 8300 HostCleanup\n", []byte{0xe6, 0x12, 0x4c, 0, 0x83}, 0},
		{"existing root", "func Cleanup 8300 entry_mx:0,0\n", []byte{0xe6, 0x12, 0x4c, 0, 0x83}, 0},
		{"data target", "data_region 00 8300 8302\n", []byte{0xe6, 0x12, 0x4c, 0, 0x83}, 0},
		{"saved PHP history", "", []byte{0x08, 0x4c, 0, 0x83}, 0},
		{"conditional is not tail", "", []byte{0xd0, 0x03, 0x60, 0xea, 0xea, 0x60}, 0},
	} {
		t.Run(tt.name, func(t *testing.T) {
			root := t.TempDir()
			romPath := filepath.Join(root, "fixture.sfc")
			cfgDir := filepath.Join(root, "cfg")
			image := make([]byte, 0x8000)
			copy(image, []byte{0x20, 0, 0x81, 0xe6, 0x10, 0x60})
			copy(image[0x100:], []byte{0x5a, 0x6c, 0x40, 0})
			copy(image[0x200:], tt.callback)
			copy(image[0x250:], []byte{0x4c, 0, 0x83})
			copy(image[0x300:], []byte{0x7a, 0x60})
			if err := os.WriteFile(romPath, image, 0600); err != nil {
				t.Fatal(err)
			}
			if err := os.Mkdir(cfgDir, 0700); err != nil {
				t.Fatal(err)
			}
			cfg := "bank = 00\nfunc Outer 8000 entry_mx:0,0\nfunc Callback 8200 entry_mx:0,0\n" + tt.extra
			cfgPath := filepath.Join(cfgDir, "bank00.cfg")
			if err := os.WriteFile(cfgPath, []byte(cfg), 0600); err != nil {
				t.Fatal(err)
			}
			var hashes []string
			for _, jobs := range []int{1, 2} {
				out := filepath.Join(root, "out")
				r, err := Run(Options{ROMPath: romPath, ConfigDir: cfgDir, OutputDir: out, Jobs: jobs, AllowStubs: true,
					ExperimentalInternalTails: true, ChunkThresholdBytes: 1, ChunkPCSpan: 0x100})
				if err != nil {
					t.Fatal(err)
				}
				if r.InternalTailEntries != tt.want {
					t.Fatalf("cold entries=%d want=%d", r.InternalTailEntries, tt.want)
				}
				hashes = append(hashes, r.SemanticSourceSHA256)
				if tt.want == 1 {
					source, err := os.ReadFile(filepath.Join(out, "bank00_part02_v2.c"))
					if err != nil {
						t.Fatal(err)
					}
					for _, want := range []string{"cold internal-tail entry", "goto L_8300_M0X0;", "return sr_region_cold_00_8200_M0X0(cpu, _entry_s, _hrv, 1);"} {
						if !strings.Contains(string(source), want) {
							t.Fatalf("missing %q", want)
						}
					}
					if strings.Contains(string(source), "void bank_00_8300") {
						t.Fatal("continuation became routine alias")
					}
					dispatch, err := os.ReadFile(filepath.Join(out, "dispatch_v2.c"))
					if err != nil {
						t.Fatal(err)
					}
					if !strings.Contains(string(dispatch), "0x008300u, { bank_00_8300_M0X0, NULL, NULL, NULL }") {
						t.Fatal("registry lost exact width")
					}
				}
			}
			if hashes[0] != hashes[1] {
				t.Fatal("parallel output changed")
			}
			actual, err := os.ReadFile(cfgPath)
			if err != nil || string(actual) != cfg {
				t.Fatal("authored config changed")
			}
			if tt.want == 1 {
				r, err := Run(Options{ROMPath: romPath, ConfigDir: cfgDir, OutputDir: filepath.Join(root, "normal"), Jobs: 1, AllowStubs: true})
				if err != nil {
					t.Fatal(err)
				}
				if r.InternalTailEntries != 0 {
					t.Fatal("opt-in changed default")
				}
				dispatch, err := os.ReadFile(filepath.Join(root, "normal", "dispatch_v2.c"))
				if err != nil {
					t.Fatal(err)
				}
				if strings.Contains(string(dispatch), "bank_00_8300_M0X0") {
					t.Fatal("default unexpectedly exposed internal block")
				}
			}
		})
	}
}
