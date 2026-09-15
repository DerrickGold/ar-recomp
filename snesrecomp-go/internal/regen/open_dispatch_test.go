package regen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestOpenDispatchExitInferenceConverges(t *testing.T) {
	root := t.TempDir()
	cfg, imagePath := filepath.Join(root, "cfg"), filepath.Join(root, "fixture.sfc")
	if err := os.Mkdir(cfg, 0700); err != nil {
		t.Fatal(err)
	}
	image := make([]byte, 0x8000)
	copy(image, []byte{0x22, 0, 0x81, 0, 0x85, 0x50, 0x86, 0x54, 0xc2, 0x30, 0x60})
	copy(image[0x100:], []byte{0x7c, 0, 0x83}) // JMP ($8300,X), not a closed selector domain
	copy(image[0x200:], []byte{0x6b, 0xea})
	copy(image[0x300:], []byte{0, 0x82})
	if err := os.WriteFile(imagePath, image, 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfg, "bank00.cfg"), []byte("bank = 00\nfunc Caller 8000 entry_mx:0,0\n"), 0600); err != nil {
		t.Fatal(err)
	}
	var hash string
	for _, jobs := range []int{1, 4, 1} {
		out := filepath.Join(root, "gen")
		report, err := Run(Options{ROMPath: imagePath, ConfigDir: cfg, OutputDir: out, AllowStubs: true, Jobs: jobs})
		if err != nil {
			t.Fatal(err)
		}
		if hash != "" && report.SemanticSourceSHA256 != hash {
			t.Fatal("open-target and width worklists depend on worker order or prior output")
		}
		hash = report.SemanticSourceSHA256
		body, err := os.ReadFile(filepath.Join(out, "bank00_v2.c"))
		if err != nil {
			t.Fatal(err)
		}
		for _, part := range []string{"live post-call M/X", "goto L_8004_M0X0;", "goto L_8004_M0X1;", "goto L_8004_M1X0;", "goto L_8004_M1X1;", "cpu_trace_trapped_dispatch"} {
			if !strings.Contains(string(body), part) {
				t.Fatalf("missing %q", part)
			}
		}
	}
}

func TestOpenRecordDiscoveryFeedsSubsequentStaticPasses(t *testing.T) {
	root := t.TempDir()
	cfg, imagePath := filepath.Join(root, "cfg"), filepath.Join(root, "fixture.sfc")
	if err := os.Mkdir(cfg, 0700); err != nil {
		t.Fatal(err)
	}
	image := make([]byte, 0x8000)
	copy(image, []byte{0xa5, 0x40, 0x0a, 0x0a, 0xaa, 0x7c, 0, 0x81})
	copy(image[0x100:], []byte{0, 0x82, 1, 0, 0, 0x83, 0, 0}) // pointer + metadata records
	image[0x200] = 0x60
	copy(image[0x300:], []byte{0x20, 0, 0x84, 0x60})                      // newly inventoried handler exposes a direct call
	copy(image[0x400:], []byte{0xa9, 0, 0x85, 0x85, 0x42, 0x6c, 0x42, 0}) // next pass exposes a stored target
	image[0x500] = 0x60
	if err := os.WriteFile(imagePath, image, 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfg, "bank00.cfg"), []byte("bank = 00\nfunc Root 8000 entry_mx:0,0\n"), 0600); err != nil {
		t.Fatal(err)
	}
	var hash string
	for _, jobs := range []int{1, 4} {
		out := filepath.Join(root, "gen")
		r, err := Run(Options{ROMPath: imagePath, ConfigDir: cfg, OutputDir: out, Jobs: jobs, AllowStubs: true, ExperimentalStoredTargets: true})
		if err != nil {
			t.Fatal(err)
		}
		if r.ObservedEntryRoots != 0 || (hash != "" && hash != r.SemanticSourceSHA256) {
			t.Fatal("static closure used observations or depends on worker order")
		}
		hash = r.SemanticSourceSHA256
		body, err := os.ReadFile(filepath.Join(out, "bank00_v2.c"))
		if err != nil {
			t.Fatal(err)
		}
		for _, name := range []string{"bank_00_8300_M0X0", "bank_00_8400_M0X0", "bank_00_8500_M0X0"} {
			if !strings.Contains(string(body), "RecompReturn "+name+"(CpuState *cpu) {") {
				t.Fatalf("new root did not feed the next analysis layer: %s", name)
			}
		}
	}
}
