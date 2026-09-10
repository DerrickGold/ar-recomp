package tooling

import (
	"bytes"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func storedTargetFixture(t *testing.T, helper []byte) (ShadowAnalysisOptions, ShadowReport) {
	t.Helper()
	root := t.TempDir()
	image := make(romimage.Image, 0x10000)
	copy(image[0x100:], []byte{0xbd, 0x40, 0, 0x3a, 0x48, 0x60}) // field -> DEC -> PHA/RTS
	copy(image[0x200:], []byte{0xa9, 0, 0x85, 0x9d, 0x40, 0, 0x60})
	copy(image[0x300:], helper)
	copy(image[0x400:], []byte{0x20, 0, 0x83, 0x60})
	copy(image[0x410:], []byte{0x20, 0, 0x83, 0x60})
	image[0x500] = 0x60
	// Same expression in a different program bank is deliberately not joined.
	copy(image[0x8200:], []byte{0xa9, 0, 0x86, 0x9d, 0x40, 0, 0x60})
	options := ShadowAnalysisOptions{ROMPath: filepath.Join(root, "fixture.sfc"), CFGDir: filepath.Join(root, "recomp"), Jobs: 4}
	if err := os.WriteFile(options.ROMPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, filepath.Join(options.CFGDir, "bank00.cfg"), `bank = 00
func Consumer 8100 entry_mx:0,0
func LiteralWriter 8200 entry_mx:0,0
func Helper 8300 entry_mx:0,0
func Caller 8400 entry_mx:0,0
func AnotherCaller 8410 entry_mx:0,0
func KnownHandler 8500 entry_mx:0,0
hle_func 8500 HostReplacement
`)
	writeTestFile(t, filepath.Join(options.CFGDir, "bank01.cfg"), "bank = 01\nfunc OtherBankWriter 8200 entry_mx:0,0\n")
	report, err := AnalyzeAuthoredShadow(options)
	if err != nil {
		t.Fatal(err)
	}
	return options, report
}

func TestStoredTargetLiteralAndSavedReturnCandidates(t *testing.T) {
	options, report := storedTargetFixture(t, []byte{0xa3, 1, 0x1a, 0x9d, 0x40, 0, 0x60})
	site := inventorySite(t, report, 0x8105)
	if site.TargetSetStatus != "unproven" || len(site.StoredTargetFlows) != 1 {
		t.Fatalf("site = %+v", site)
	}
	flow := site.StoredTargetFlows[0]
	if flow.TargetAddend != 0 || flow.Field.Operand != 0x40 || flow.Field.Mode != "abs,x" || len(flow.Targets) != 3 || len(flow.Writers) != 2 {
		t.Fatalf("flow = %+v", flow)
	}
	want := map[uint32]string{0x8403: "continuation_candidate", 0x8413: "continuation_candidate", 0x8500: "unknown"}
	for _, target := range flow.Targets {
		if want[target.PC] != target.EntryKind {
			t.Fatalf("target = %+v", target)
		}
		if target.AuthoredEntry != (target.PC == 0x8500) {
			t.Fatalf("address overlap = %+v", target)
		}
	}
	if report.DispatchSummary.StoredTargetCandidates != 3 || report.DispatchSummary.StoredTargetAuthored != 1 || report.DispatchSummary.StoredTargetWriters != 2 {
		t.Fatal(report.DispatchSummary)
	}
	before, err := BuildStaticAnalysisDatabase(report)
	if err != nil {
		t.Fatal(err)
	}
	without := report
	without.DispatchSites = nil
	without.DispatchSummary = ShadowDispatchSummary{}
	after, err := BuildStaticAnalysisDatabase(without)
	if err != nil || !reflect.DeepEqual(before, after) {
		t.Fatalf("conditional targets changed proof: %v", err)
	}
	if report.EntryAblation.Summary.AuthoredHLEObligations != 1 {
		t.Fatal("HLE obligation was lost")
	}
	options.Jobs = 1
	second, err := AnalyzeAuthoredShadow(options)
	if err != nil || !reflect.DeepEqual(report, second) {
		t.Fatalf("non-deterministic report: %v", err)
	}
	var output bytes.Buffer
	if err := WriteShadowReport(&output, report, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, text := range []string{"memory-fed RTS targets:", "stored-target (report-only)", "candidate=$00:8403", "continuation_candidate", "same_memory_allocation_and_index"} {
		if !strings.Contains(output.String(), text) {
			t.Fatalf("missing %q", text)
		}
	}
}

func TestStoredTargetStackReadIsNotNecessarilyReturnAddress(t *testing.T) {
	for _, helper := range [][]byte{
		{0x08, 0xa3, 1, 0x1a, 0x9d, 0x40, 0, 0x28, 0x60}, // PHP changes stack origin
		{0x48, 0x68, 0x1a, 0x9d, 0x40, 0, 0x60},          // PLA is not at entry
		{0xa3, 3, 0x1a, 0x9d, 0x40, 0, 0x60},             // wrong stack offset
	} {
		_, report := storedTargetFixture(t, helper)
		flow := inventorySite(t, report, 0x8105).StoredTargetFlows[0]
		if len(flow.Targets) != 1 || flow.Targets[0].PC != 0x8500 {
			t.Fatalf("invented return provenance: %+v", flow.Targets)
		}
	}
}

func storedTargetGraph(t *testing.T, code []byte) ([]shadowStoredRead, []shadowStoredWrite) {
	t.Helper()
	image := make(romimage.Image, 0x8000)
	copy(image, code)
	graph, err := decoder.DecodeFunction(image, 0, 0x8000, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	return collectShadowStoredTargets(graph)
}

func TestStoredTargetWordAndAdjustmentContracts(t *testing.T) {
	for _, test := range []struct {
		name   string
		code   []byte
		addend int
		m      uint8
	}{
		{"word minus one", []byte{0xbd, 0x40, 0, 0x3a, 0x48, 0x60}, 0, 0},
		{"stored target minus one", []byte{0xb5, 0x30, 0x48, 0x60}, 1, 0},
		{"X register with Y-indexed field", []byte{0xbe, 0x60, 0, 0xca, 0xda, 0x60}, 0, 0},
		{"Y target via transfer", []byte{0xbd, 0x20, 0, 0x3a, 0xa8, 0x5a, 0x60}, 0, 0},
		{"long field plus one", []byte{0xbf, 0x40, 0, 0x7f, 0x1a, 0x48, 0x60}, 2, 0},
		{"live M changes after push", []byte{0xbd, 0x40, 0, 0x48, 0xe2, 0x20, 0x60}, 1, 1},
	} {
		t.Run(test.name, func(t *testing.T) {
			reads, _ := storedTargetGraph(t, test.code)
			if len(reads) != 1 || reads[0].flow.TargetAddend != test.addend || reads[0].flow.LiveMX.M != test.m {
				t.Fatalf("reads = %+v", reads)
			}
		})
	}
	for _, code := range [][]byte{
		{0xe2, 0x20, 0xbd, 0x40, 0, 0x48, 0x60},             // byte push
		{0xe2, 0x10, 0xbe, 0x40, 0, 0xda, 0x60},             // byte PHX
		{0xbd, 0x40, 0, 0x48, 0x08, 0x60},                   // intervening PHP
		{0xbd, 0x40, 0, 0x69, 1, 0, 0x48, 0x60},             // unknown carry/decimal
		{0xbd, 0x40, 0, 0x20, 0, 0x90, 0x48, 0x60},          // call clobber
		{0xbd, 0x40, 0, 0xe2, 0x10, 0xaa, 0x8a, 0x48, 0x60}, // truncated register
	} {
		reads, _ := storedTargetGraph(t, code)
		if len(reads) != 0 {
			t.Fatalf("invalid word source accepted: % X: %+v", code, reads)
		}
	}
}

func TestStoredTargetIncompleteWriterCensus(t *testing.T) {
	for _, test := range []struct {
		code  []byte
		kind  string
		width uint8
	}{
		{[]byte{0xe2, 0x20, 0xa9, 0x85, 0x9d, 0x40, 0, 0x60}, "partial_write", 1},
		{[]byte{0xfe, 0x40, 0, 0x60}, "read_modify_write", 2},
		{[]byte{0xa9, 0, 0x85, 0x69, 1, 0, 0x9d, 0x40, 0, 0x60}, "unknown_value", 2},
		{[]byte{0xbd, 0, 0x90, 0x9d, 0x40, 0, 0x60}, "memory_value", 2},
	} {
		_, writes := storedTargetGraph(t, test.code)
		if len(writes) != 1 || writes[0].writer.Kind != test.kind || writes[0].writer.Width != test.width || writes[0].writer.StoredValue != nil {
			t.Fatalf("unknown write disappeared or became finite: %+v", writes)
		}
	}
	_, writes := storedTargetGraph(t, []byte{0xa9, 0, 0, 0x3a, 0x9d, 0x40, 0, 0x60})
	if len(writes) != 1 || writes[0].writer.StoredValue == nil || *writes[0].writer.StoredValue != 0xffff {
		t.Fatalf("16-bit wrapping lost: %+v", writes)
	}
	_, writes = storedTargetGraph(t, []byte{0xa9, 0, 0x90, 0xa8, 0xa9, 2, 0, 0x29, 1, 0, 0x98, 0x9d, 0x40, 0, 0x60})
	if len(writes) != 1 || writes[0].writer.StoredValue == nil || *writes[0].writer.StoredValue != 0x9000 {
		t.Fatalf("unrelated A arithmetic clobbered tracked Y: %+v", writes)
	}
}

func TestStoredTargetUsesDecodedPredecessors(t *testing.T) {
	// The branch skips bytes that must not be mistaken for value/stack effects.
	reads, _ := storedTargetGraph(t, []byte{0xbd, 0x40, 0, 0x80, 3, 0x08, 0x42, 0xff, 0x48, 0x60})
	if len(reads) != 1 || reads[0].flow.LoadPC != 0x8000 {
		t.Fatalf("decoded branch source lost: %+v", reads)
	}
	// Both incoming paths load the same expression, but an ambiguous join
	// needs a multi-path value proof; lexical proximity is not that proof.
	reads, _ = storedTargetGraph(t, []byte{0xd0, 5, 0xbd, 0x40, 0, 0x80, 3, 0xbd, 0x40, 0, 0x48, 0x60})
	if len(reads) != 0 {
		t.Fatalf("ambiguous predecessor accepted: %+v", reads)
	}
}

func TestStoredTargetCandidatesRespectOwnershipWithoutCreatingRoots(t *testing.T) {
	for _, test := range []struct {
		target    uint16
		ownership string
		data      bool
	}{
		{0x8101, "instruction_interior_not_entry_proof", false},
		{0x8600, "unclaimed_rom_not_decoded", false},
		{0x8600, "authored_data", true},
		{0x0010, "not_rom_mapped", false},
	} {
		options, baseline := storedTargetFixture(t, []byte{0xa3, 1, 0x1a, 0x9d, 0x40, 0, 0x60})
		image, err := os.ReadFile(options.ROMPath)
		if err != nil {
			t.Fatal(err)
		}
		image[0x201], image[0x202] = byte(test.target), byte(test.target>>8)
		if err := os.WriteFile(options.ROMPath, image, 0o644); err != nil {
			t.Fatal(err)
		}
		if test.data {
			path := filepath.Join(options.CFGDir, "bank00.cfg")
			cfg, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			writeTestFile(t, path, string(cfg)+"data_region 00 8600 8602\n")
		}
		report, err := AnalyzeAuthoredShadow(options)
		if err != nil {
			t.Fatal(err)
		}
		if report.Summary.FinalVariants != baseline.Summary.FinalVariants {
			t.Fatal("candidate created a decode root")
		}
		found := false
		for _, target := range inventorySite(t, report, 0x8105).StoredTargetFlows[0].Targets {
			if target.StorePC == 0x8203 {
				found = true
				if target.PC != uint32(test.target) || target.Ownership != test.ownership || target.AuthoredEntry {
					t.Fatalf("ownership = %+v", target)
				}
			}
		}
		if !found {
			t.Fatal("candidate silently disappeared")
		}
	}
}

func TestStoredTargetEntryAtStoreKeepsUnknownAlternative(t *testing.T) {
	options, _ := storedTargetFixture(t, []byte{0xa3, 1, 0x1a, 0x9d, 0x40, 0, 0x60})
	path := filepath.Join(options.CFGDir, "bank00.cfg")
	cfg, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, path, string(cfg)+"func StoreEntry 8203 entry_mx:0,0\n")
	report, err := AnalyzeAuthoredShadow(options)
	if err != nil {
		t.Fatal(err)
	}
	unknown := false
	for _, writer := range inventorySite(t, report, 0x8105).StoredTargetFlows[0].Writers {
		if writer.StorePC != 0x8203 {
			continue
		}
		unknown = unknown || writer.Kind == "unknown_value"
	}
	if !unknown {
		t.Fatal("entry at store inherited a speculative constant")
	}
	// Depending on the authored sibling boundary, the prior root may stop
	// before the store. It is not valid to manufacture the missing edge just
	// to preserve a literal candidate.
}
