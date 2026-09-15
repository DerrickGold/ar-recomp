package regen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestForwardedFieldLiteralGeneratesColdBody(t *testing.T) {
	for _, tt := range []struct {
		name, extra  string
		experimental bool
		want         bool
	}{
		{"default unchanged", "", false, false},
		{"open literal inventory", "", true, true},
		{"HLE writer preserved", "hle_func 8000 HostWriter\n", true, false},
		{"conditional HLE writer preserved", "hle_func_if 8000 HostWriter UseHost\n", true, false},
		{"HLE reader preserved", "hle_func 8100 HostReader\n", true, false},
		{"authored data remains data", "data_region 00 8200 8210\n", true, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			root := t.TempDir()
			rom := filepath.Join(root, "fixture.sfc")
			cfg := filepath.Join(root, "cfg")
			out := filepath.Join(root, "gen")
			if err := os.Mkdir(cfg, 0700); err != nil {
				t.Fatal(err)
			}
			image := make([]byte, 0x8000)
			copy(image, []byte{0xa9, 0, 0x82, 0x95, 0x38, 0x60})
			copy(image[0x100:], []byte{0xb5, 0x38, 0x85, 0x72, 0x6c, 0x72, 0})
			image[0x200] = 0x60
			if err := os.WriteFile(rom, image, 0600); err != nil {
				t.Fatal(err)
			}
			config := "bank = 00\nfunc Writer 8000 entry_mx:0,0\nfunc Reader 8100 entry_mx:0,0\n" + tt.extra
			cfgPath := filepath.Join(cfg, "bank00.cfg")
			if err := os.WriteFile(cfgPath, []byte(config), 0600); err != nil {
				t.Fatal(err)
			}
			r, err := Run(Options{ROMPath: rom, ConfigDir: cfg, OutputDir: out, Jobs: 2, AllowStubs: true, ExperimentalStoredTargets: tt.experimental})
			if err != nil {
				t.Fatal(err)
			}
			body, err := os.ReadFile(filepath.Join(out, "bank00_v2.c"))
			if err != nil {
				t.Fatal(err)
			}
			if strings.Contains(string(body), "RecompReturn bank_00_8200_M0X0(CpuState *cpu) {") != tt.want {
				t.Fatalf("body=%v entries=%d wanted=%v", string(body), r.FinalEntries, tt.want)
			}
			if tt.want {
				if !strings.Contains(string(body), "cpu_read16_bank_wrap(cpu, 0x00") || !strings.Contains(string(body), "cpu_dispatch_has_entry") {
					t.Fatal("native pointer/registry fallback replaced")
				}
			}
			actual, err := os.ReadFile(cfgPath)
			if err != nil || string(actual) != config {
				t.Fatal("authored config changed")
			}
		})
	}
}

func TestJoinedHandlerLiteralsGenerateOnlyOptInColdBodies(t *testing.T) {
	for _, tt := range []struct {
		name, extra   string
		enabled, want bool
	}{
		{"default unchanged", "", false, false},
		{"joined literals", "", true, true},
		{"authored data excluded", "data_region 00 8200 8210\n", true, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			root := t.TempDir()
			cfg := filepath.Join(root, "cfg")
			out := filepath.Join(root, "gen")
			rom := filepath.Join(root, "fixture.sfc")
			if err := os.Mkdir(cfg, 0700); err != nil {
				t.Fatal(err)
			}
			image := make([]byte, 0x8000)
			copy(image, []byte{0xd0, 5, 0xa9, 0, 0x82, 0x80, 3, 0xa9, 0x40, 0x82, 0x85, 0x42, 0x60})
			copy(image[0x100:], []byte{0x6c, 0x42, 0})
			image[0x200] = 0x60
			image[0x240] = 0x60
			if err := os.WriteFile(rom, image, 0600); err != nil {
				t.Fatal(err)
			}
			text := "bank = 00\nfunc Writer 8000 entry_mx:0,0\nfunc Reader 8100 entry_mx:0,0\n" + tt.extra
			path := filepath.Join(cfg, "bank00.cfg")
			if err := os.WriteFile(path, []byte(text), 0600); err != nil {
				t.Fatal(err)
			}
			if _, err := Run(Options{ROMPath: rom, ConfigDir: cfg, OutputDir: out, Jobs: 2, AllowStubs: true, ExperimentalStoredTargets: tt.enabled}); err != nil {
				t.Fatal(err)
			}
			body, err := os.ReadFile(filepath.Join(out, "bank00_v2.c"))
			if err != nil {
				t.Fatal(err)
			}
			if got := strings.Contains(string(body), "RecompReturn bank_00_8200_M0X0(CpuState *cpu) {"); got != tt.want {
				t.Fatalf("cold body=%v want=%v", got, tt.want)
			}
			if tt.enabled && (!strings.Contains(string(body), "cpu_dispatch_has_entry") || !strings.Contains(string(body), "cpu_read16_bank_wrap(cpu, 0x00")) {
				t.Fatal("lost actual pointer/live registry guard")
			}
			actual, err := os.ReadFile(path)
			if err != nil || string(actual) != text {
				t.Fatal("authored config changed")
			}
		})
	}
}
