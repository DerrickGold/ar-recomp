package tooling

import (
	"os"
	"path/filepath"
	"testing"
)

func TestMXDiffAlignmentAndUnresolvedOracle(t *testing.T) {
	root := t.TempDir()
	recompPath, oraclePath := filepath.Join(root, "recomp.txt"), filepath.Join(root, "oracle.txt")
	if err := os.WriteFile(recompPath, []byte("100 1 1\n101 1 0\n102 0 0\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(oraclePath, []byte("90 1 1 7\n91 0 0 8\n92 -1 -1 9\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	report, err := BuildMXDiff(MXDiffOptions{RecompPath: recompPath, OraclePath: oraclePath, Offset: 10, Context: 2, Top: 10})
	if err != nil {
		t.Fatal(err)
	}
	if report.CommonFrames != 3 || report.ComparedFrames != 2 || report.UnresolvedOracle != 1 || report.DivergentFrames != 1 {
		t.Fatalf("report=%+v", report)
	}
	if report.FirstDivergence == nil || *report.FirstDivergence != 101 {
		t.Fatalf("first=%v", report.FirstDivergence)
	}
}
