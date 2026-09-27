package regen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestHLEFallbackDiscoversInstructionAlignedBoundaryExit(t *testing.T) {
	for _, data := range []bool{false, true} {
		t.Run(map[bool]string{false: "exact continuation", true: "data stays excluded"}[data], func(t *testing.T) {
			root := t.TempDir()
			cfg, out, imagePath := filepath.Join(root, "cfg"), filepath.Join(root, "gen"), filepath.Join(root, "test.sfc")
			if err := os.Mkdir(cfg, 0700); err != nil {
				t.Fatal(err)
			}
			image := make([]byte, 0x8000)
			// M1 executes LDA #$11; NOP; RTS. M0 executes LDA #$EA11;
			// RTS. Both are valid native streams, with different continuations.
			copy(image, []byte{0xa9, 0x11, 0xea, 0x60})
			copy(image[0x100:], []byte{0x20, 0x00, 0x80, 0x60})
			text := "bank = 00\nfunc Prefix 8000 end:8002 entry_mx:1,0\n" +
				"hle_func_if 8000 Hook Enabled\nfunc Suffix 8002 end:8003 entry_mx:1,0\n" +
				"func Caller 8100 entry_mx:0,0\n"
			if data {
				text += "data_region 00 8003 8004\n"
			}
			if err := os.WriteFile(imagePath, image, 0600); err != nil {
				t.Fatal(err)
			}
			cfgPath := filepath.Join(cfg, "bank00.cfg")
			if err := os.WriteFile(cfgPath, []byte(text), 0600); err != nil {
				t.Fatal(err)
			}
			report, err := Run(Options{ROMPath: imagePath, ConfigDir: cfg, OutputDir: out, AllowStubs: true, Jobs: 2})
			if err != nil {
				t.Fatal(err)
			}
			body, err := os.ReadFile(filepath.Join(out, "bank00_v2.c"))
			if err != nil {
				t.Fatal(err)
			}
			for _, width := range []string{"M0X0", "M1X0"} {
				present := strings.Contains(string(body), "RecompReturn bank_00_8003_"+width+"(CpuState *cpu) {")
				if present == data {
					t.Fatalf("continuation %s present=%v data=%v", width, present, data)
				}
			}
			if (report.StubHits > 0) != data {
				for _, line := range strings.Split(string(body), "\n") {
					if strings.Contains(line, "unresolved") {
						t.Log(line)
					}
				}
				t.Fatalf("stub hits=%d data=%v", report.StubHits, data)
			}
			actual, err := os.ReadFile(cfgPath)
			if err != nil || string(actual) != text {
				t.Fatal("regeneration mutated authored configuration")
			}
		})
	}
}
