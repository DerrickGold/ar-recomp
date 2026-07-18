package config

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLoadRepresentativeConfig(t *testing.T) {
	path := filepath.Join(t.TempDir(), "bank01.cfg")
	text := `
bank = 1
auto_vectors
func Foo 8123 end:8234 entry_mx:0,1 exit_mx:1,0 tail_call:9000 entry_s_offset:-2
name 018999 NamedTarget
indirect_dispatch B8C0 26 idx:A tables:B8D0 ret:B8C2 sep:20
rts_dispatch 9000 9010 9020
hle_func 8123 HostFoo
force_variant_at 018155 0 1
exit_mx_at 018888 1 0
data_region 01 A000 A100
exclude_range B000 B100
`
	if err := os.WriteFile(path, []byte(text), 0o600); err != nil {
		t.Fatal(err)
	}
	cfg, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Bank != 1 || len(cfg.Entries) != 2 {
		t.Fatalf("bank/entries mismatch: bank=%d entries=%d", cfg.Bank, len(cfg.Entries))
	}
	entry := cfg.Entries[0]
	if entry.Name != "Foo" || entry.Start != 0x8123 || entry.End == nil || *entry.End != 0x8234 || entry.EntryMX != (MX{M: 0, X: 1}) {
		t.Fatalf("entry mismatch: %#v", entry)
	}
	if len(cfg.IndirectDispatch) != 1 || cfg.IndirectDispatch[0].IndexReg != "A" || cfg.IndirectDispatch[0].SEPMask != 0x20 {
		t.Fatalf("indirect dispatch mismatch: %#v", cfg.IndirectDispatch)
	}
	if len(cfg.RTSDispatch) != 1 || len(cfg.RTSDispatch[0].Targets) != 2 {
		t.Fatalf("RTS dispatch mismatch: %#v", cfg.RTSDispatch)
	}
}
