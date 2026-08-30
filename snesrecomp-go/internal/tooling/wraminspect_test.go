package tooling

import (
	"os"
	"path/filepath"
	"testing"
)

func TestWRAMSymbolsAndOperations(t *testing.T) {
	root := t.TempDir()
	symbolPath := filepath.Join(root, "ram-map.md")
	if err := os.WriteFile(symbolPath, []byte("| $7E:0021 | 1 | **Magic points** — working copy |\n| `$7F:0002` | Town value |\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	symbols, err := LoadWRAMSymbols(symbolPath)
	if err != nil {
		t.Fatal(err)
	}
	address, err := ParseWRAMOffset("magic-points", symbols)
	if err != nil || address != 0x21 {
		t.Fatalf("address=%#x err=%v symbols=%+v", address, err, symbols.Symbols)
	}
	before := make([]byte, 0x20000)
	after := make([]byte, 0x20000)
	before[0x21], before[0x22] = 3, 4
	copy(after, before)
	after[0x21], after[0x10002] = 5, 9
	beforePath, afterPath := filepath.Join(root, "before.bin"), filepath.Join(root, "after.bin")
	if err := os.WriteFile(beforePath, before, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(afterPath, after, 0o644); err != nil {
		t.Fatal(err)
	}
	get, err := BuildWRAMGet([]string{afterPath}, []string{"magic-points", "7F:0002"}, symbols)
	if err != nil || get.Files[0].Values[0].Byte != 5 || get.Files[0].Values[1].Byte != 9 {
		t.Fatalf("get=%+v err=%v", get, err)
	}
	diff, err := BuildWRAMDiff(beforePath, afterPath, 0x800, false, 20, symbols)
	if err != nil || diff.Differences != 2 || len(diff.Rows) != 1 || len(diff.Clusters) != 1 {
		t.Fatalf("diff=%+v err=%v", diff, err)
	}
	scan, err := BuildWRAMScan(afterPath, 2, 0x0405, 0, 0x1ffff, 20, symbols)
	if err != nil || len(scan.Matches) != 1 || scan.Matches[0].Address != 0x21 {
		t.Fatalf("scan=%+v err=%v", scan, err)
	}
}
