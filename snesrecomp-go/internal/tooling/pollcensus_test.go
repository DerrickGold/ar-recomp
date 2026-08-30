package tooling

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPollCensusClassifiesLoopAndSingleRead(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image, []byte{
		0xad, 0x10, 0x42, // LDA $4210
		0x10, 0xfb, // BPL $8000
		0xad, 0x12, 0x42, // LDA $4212
		0xea, 0x60, // NOP; RTS
	})
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), "bank = 00\nfunc Root 8000 entry_mx:1,1\n")
	report, err := BuildPollCensus(PollCensusOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 2})
	if err != nil {
		t.Fatal(err)
	}
	populatePollInstructionBytes(image, &report)
	if report.Summary.Sites != 2 || report.Summary.PollLoops != 1 || report.Summary.SingleReads != 1 {
		t.Fatalf("summary=%+v sites=%+v", report.Summary, report.Sites)
	}
	if report.Sites[0].PC != 0x008000 || report.Sites[0].Classification != "poll_loop" || report.Sites[0].BranchPC == nil || *report.Sites[0].BranchPC != 0x008003 {
		t.Fatalf("poll site = %+v", report.Sites[0])
	}
	if report.Sites[1].PC != 0x008005 || report.Sites[1].Classification != "single_read" {
		t.Fatalf("single site = %+v", report.Sites[1])
	}
	var output bytes.Buffer
	if err := WritePollCensus(&output, report, "text"); err != nil {
		t.Fatal(err)
	}
	for _, fragment := range []string{"poll=1", "$00:8000", "$4210", "bytes=AD 10 42"} {
		if !strings.Contains(output.String(), fragment) {
			t.Fatalf("poll output missing %q:\n%s", fragment, output.String())
		}
	}
}

func TestPollCensusReportsTruthfulHLECoverage(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	for _, offset := range []int{0x00, 0x10, 0x20, 0x30} {
		copy(image[offset:], []byte{
			0xad, 0x10, 0x42, // LDA $4210
			0x10, 0xfb, // BPL to the LDA
			0x60, // RTS
		})
	}
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), strings.Join([]string{
		"bank = 00",
		"func Whole 8000 entry_mx:1,1",
		"func Conditional 8010 entry_mx:1,1",
		"func Upload 8020 entry_mx:1,1",
		"func Dispatch 8030 entry_mx:1,1",
		"hle_func 8000 HostWhole",
		"hle_func_if 8010 HostConditional HostConditionalEnabled",
		"hle_spc_upload 8020",
		"hle_dispatch 8030 HostDispatch",
		"",
	}, "\n"))
	report, err := BuildPollCensus(PollCensusOptions{
		ROMPath: romPath, CFGDir: cfgDir, Jobs: 2,
	})
	if err != nil {
		t.Fatal(err)
	}
	coverage := make(map[uint32]PollHLECoverage)
	for _, site := range report.Sites {
		coverage[site.PC] = site.HLECoverage
	}
	if coverage[0x008000] != PollHLECoverageWholeBody ||
		coverage[0x008010] != PollHLECoverageConditional ||
		coverage[0x008020] != PollHLECoverageWholeBody ||
		coverage[0x008030] != PollHLECoverageNone {
		t.Fatalf("coverage=%v sites=%+v", coverage, report.Sites)
	}
	if report.Version != 2 || report.Summary.HLEWholeBody != 2 ||
		report.Summary.HLEConditional != 1 ||
		report.Summary.LivePollLoops != 2 {
		t.Fatalf("summary=%+v version=%d", report.Summary, report.Version)
	}
}

func TestPollCensusAcceptsArbitraryHardwareRegister(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image, []byte{0xad, 0x40, 0x21, 0x60}) // LDA $2140; RTS
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"),
		"bank = 00\nfunc Root 8000 entry_mx:1,1\n")
	report, err := BuildPollCensus(PollCensusOptions{
		ROMPath: romPath, CFGDir: cfgDir, Jobs: 1,
		Registers: []uint16{0x2140},
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(report.Sites) != 1 || report.Sites[0].Register != 0x2140 {
		t.Fatalf("sites=%+v", report.Sites)
	}
}
