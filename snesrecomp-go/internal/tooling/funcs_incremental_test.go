package tooling

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestSyncFuncsKeepsUnchangedHeaderTimestamp(t *testing.T) {
	cfg, output := t.TempDir(), filepath.Join(t.TempDir(), "funcs.h")
	if _, err := SyncFuncs(cfg, output); err != nil {
		t.Fatal(err)
	}
	old := time.Unix(1234567890, 0)
	if err := os.Chtimes(output, old, old); err != nil {
		t.Fatal(err)
	}
	if _, err := SyncFuncs(cfg, output); err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(output)
	if err != nil || !info.ModTime().Equal(old) {
		t.Fatalf("unchanged header rewritten: %v %v", info, err)
	}
	if err := os.WriteFile(filepath.Join(cfg, "bank00.cfg"), []byte("bank = 00\nfunc Entry 8000 entry_mx:1,1\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := SyncFuncs(cfg, output); err != nil {
		t.Fatal(err)
	}
	info, err = os.Stat(output)
	if err != nil || !info.ModTime().After(old) {
		t.Fatalf("changed header not updated: %v %v", info, err)
	}
}
