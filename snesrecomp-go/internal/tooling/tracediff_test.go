package tooling

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestTraceDiffFinalSequenceAndAligned(t *testing.T) {
	root := t.TempDir()
	oraclePath := filepath.Join(root, "oracle.jsonl")
	recompPath := filepath.Join(root, "recomp.jsonl")
	oracle := strings.Join([]string{
		`{"f":1,"adr":"88","val":"01"}`,
		`{"f":1,"adr":"200","val":"10"}`,
		`{"f":2,"adr":"88","val":"02"}`,
		`{"f":2,"adr":"200","val":"20"}`,
		`{"f":2,"adr":"201","val":"33"}`,
	}, "\n")
	recomp := strings.Join([]string{
		`{"f":10,"adr":"88","val":"01"}`,
		`{"f":10,"adr":"200","val":"10"}`,
		`{"f":11,"adr":"88","val":"02"}`,
		`{"f":11,"adr":"200","val":"21"}`,
		`{"f":11,"adr":"202","val":"44"}`,
	}, "\n")
	if err := os.WriteFile(oraclePath, []byte(oracle), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(recompPath, []byte(recomp), 0o644); err != nil {
		t.Fatal(err)
	}
	for _, mode := range []string{"final", "sequence", "aligned"} {
		report, err := BuildTraceDiff(TraceDiffOptions{Mode: mode, OraclePath: oraclePath, RecompPath: recompPath, Top: 20, High: 0x1ffff})
		if err != nil {
			t.Fatalf("%s: %v", mode, err)
		}
		if report.Summary.DivergentAddresses != 1 {
			t.Fatalf("%s summary=%+v rows=%+v", mode, report.Summary, report.Rows)
		}
		if report.Rows[0].Address != 0x200 || report.Rows[0].OracleValue != 0x20 || report.Rows[0].RecompValue != 0x21 {
			t.Fatalf("%s row=%+v", mode, report.Rows[0])
		}
		if mode == "aligned" && (report.Summary.FirstDivergenceGF == nil || *report.Summary.FirstDivergenceGF != 2) {
			t.Fatalf("aligned first divergence=%v", report.Summary.FirstDivergenceGF)
		}
	}
}
