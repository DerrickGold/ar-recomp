package regen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestComputedReturnExitInferenceRoutesLiveContinuation(t *testing.T) {
	for _, entry := range []string{"0,0", "1,1"} {
		t.Run(entry, func(t *testing.T) {
			root := t.TempDir()
			cfg, out, rom := filepath.Join(root, "cfg"), filepath.Join(root, "gen"), filepath.Join(root, "fixture.sfc")
			if err := os.Mkdir(cfg, 0700); err != nil {
				t.Fatal(err)
			}
			image := make([]byte, 0x8000)
			copy(image, []byte{0x22, 0, 0x81, 0, 0x85, 0x50, 0x86, 0x54, 0xc2, 0x30, 0x60})
			copy(image[0x100:], []byte{0xe2, 0x20, 0xd4, 0x40, 0x60})
			if err := os.WriteFile(rom, image, 0600); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(filepath.Join(cfg, "bank00.cfg"), []byte("bank = 00\nfunc Caller 8000 entry_mx:"+entry+"\n"), 0600); err != nil {
				t.Fatal(err)
			}
			if _, err := Run(Options{ROMPath: rom, ConfigDir: cfg, OutputDir: out, AllowStubs: true, Jobs: 2}); err != nil {
				t.Fatal(err)
			}
			body, err := os.ReadFile(filepath.Join(out, "bank00_v2.c"))
			if err != nil {
				t.Fatal(err)
			}
			for _, part := range []string{"live post-call M/X", "goto L_8004_M0X0;", "goto L_8004_M0X1;", "goto L_8004_M1X0;", "goto L_8004_M1X1;"} {
				if !strings.Contains(string(body), part) {
					t.Fatalf("missing %q:\n%s", part, body)
				}
			}
		})
	}
}
