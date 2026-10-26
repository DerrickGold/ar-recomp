package regen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDuplicateEntryFailsBeforeParallelEmission(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "fixture.sfc")
	image := make([]byte, 0x8000)
	image[0] = 0x60
	if err := os.WriteFile(romPath, image, 0600); err != nil {
		t.Fatal(err)
	}
	cfgPath := filepath.Join(root, "bank00.cfg")
	for _, mx := range []string{"0,0", "1,0"} {
		cfg := "bank = 00\nfunc First 8000 entry_mx:0,0\nfunc Second 8000 entry_mx:" + mx + "\n"
		if err := os.WriteFile(cfgPath, []byte(cfg), 0600); err != nil {
			t.Fatal(err)
		}
		_, err := Run(Options{ROMPath: romPath, ConfigDir: root,
			OutputDir: filepath.Join(root, "out"), Jobs: 2, AllowStubs: true})
		if mx == "0,0" {
			if err == nil || !strings.Contains(err.Error(), "duplicate function entry $008000 M0X0") {
				t.Fatalf("duplicate entry error = %v", err)
			}
		} else if err != nil {
			t.Fatalf("distinct M/X entry was rejected: %v", err)
		}
	}
}
