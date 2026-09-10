package tooling

import (
	"bytes"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/emitter"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func shadowHLEInventoryFixture(t *testing.T) (ShadowAnalysisOptions, romimage.Image) {
	t.Helper()
	root := t.TempDir()
	image := make(romimage.Image, 0x8000)
	copy(image[0x10:], []byte{0x6c, 0x10, 0x00})
	copy(image[0x20:], []byte{0x6c, 0x20, 0x00})
	copy(image[0x30:], []byte{0x6c, 0x30, 0x00})
	options := ShadowAnalysisOptions{ROMPath: filepath.Join(root, "fixture.sfc"), CFGDir: filepath.Join(root, "recomp"), Jobs: 4}
	if err := os.WriteFile(options.ROMPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, filepath.Join(options.CFGDir, "bank00.cfg"), `bank = 00
func Routed16 8010 entry_mx:0,0
func Routed8 8010 entry_mx:1,1
func Unrouted 8030 entry_mx:0,0
hle_dispatch 8010 HostDispatch
hle_dispatch 8020 HostUnreachedDispatch
`)
	return options, image
}

func inventorySite(t *testing.T, report ShadowReport, pc uint32) ShadowDispatchSite {
	t.Helper()
	for _, site := range report.DispatchSites {
		if site.SitePC == pc {
			return site
		}
	}
	t.Fatalf("missing dispatch site $%06X: %+v", pc, report.DispatchSites)
	return ShadowDispatchSite{}
}

func TestShadowDispatchInventoryPreservesHLEWithoutCreatingTrapsOrRoots(t *testing.T) {
	options, image := shadowHLEInventoryFixture(t)
	cfgPath := filepath.Join(options.CFGDir, "bank00.cfg")
	before, err := os.ReadFile(cfgPath)
	if err != nil {
		t.Fatal(err)
	}
	report, err := AnalyzeAuthoredShadow(options)
	if err != nil {
		t.Fatal(err)
	}
	if report.Version != shadowReportVersion || report.Summary.RawUnresolvedEmissions != 1 || report.Summary.UniqueUnresolvedSites != 1 || len(report.Unresolved) != 1 {
		t.Fatalf("HLE routes changed unresolved totals: %+v", report.Summary)
	}
	if report.DispatchSummary.UniqueSites != 3 || report.DispatchSummary.HLERoutedSites != 2 || report.DispatchSummary.HLEUnprovenTargetSites != 2 {
		t.Fatalf("inventory summary = %+v", report.DispatchSummary)
	}
	routed := inventorySite(t, report, 0x008010)
	if routed.Routing != "hle" || routed.HLEFunction != "HostDispatch" || routed.TargetSetStatus != "unproven" || routed.StaticStatus != "unresolved" ||
		routed.InstructionBytes != "6C 10 00" || len(routed.Callers) != 2 || routed.DecodedOccurrences != 2 {
		t.Fatalf("decoded HLE = %+v", routed)
	}
	unreached := inventorySite(t, report, 0x008020)
	if unreached.Reachability != "authored_only_not_decoded" || unreached.StaticStatus != "not_analyzed" || len(unreached.Callers) != 0 || unreached.InstructionBytes != "" || unreached.DecodedOccurrences != 0 {
		t.Fatalf("HLE-only declaration manufactured code/width/reachability: %+v", unreached)
	}
	for _, mx := range [][2]uint8{{0, 0}, {1, 1}} {
		result, err := emitter.EmitFunction(image, 0, 0x8010, mx[0], mx[1], emitter.FunctionOptions{
			Name: "Routed", Decode: decoder.Options{HLEDispatch: map[uint16]string{0x8010: "HostDispatch"}},
			HLEDispatch: map[uint16]string{0x8010: "HostDispatch"},
		})
		if err != nil {
			t.Fatalf("HLE emission needs --allow-stubs: %v", err)
		}
		if len(result.UnresolvedIndirects) != 0 || !strings.Contains(result.Source, "HostDispatch(cpu)") || strings.Contains(result.Source, "cpu_trace_unresolved_indirect_jump") {
			t.Fatalf("HLE generation changed: %+v", result)
		}
	}
	database, err := BuildStaticAnalysisDatabase(report)
	if err != nil {
		t.Fatal(err)
	}
	if len(database.DispatchFacts) != 0 || len(database.EntryFacts) != 0 {
		t.Fatalf("report-only HLE inventory created proof: %+v", database)
	}
	options.Jobs = 1
	second, err := AnalyzeAuthoredShadow(options)
	if err != nil {
		t.Fatal(err)
	}
	var a, b bytes.Buffer
	if err := WriteShadowReport(&a, report, "json", false); err != nil {
		t.Fatal(err)
	}
	if err := WriteShadowReport(&b, second, "json", false); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(a.Bytes(), b.Bytes()) {
		t.Fatal("inventory differs by worker count")
	}
	after, err := os.ReadFile(cfgPath)
	if err != nil || !bytes.Equal(before, after) {
		t.Fatalf("analysis changed cfg: %v", err)
	}
}

func TestShadowDispatchInventoryJoinsHLEAndObservedOnlyCensus(t *testing.T) {
	options, _ := shadowHLEInventoryFixture(t)
	report, err := AnalyzeAuthoredShadow(options)
	if err != nil {
		t.Fatal(err)
	}
	before, err := BuildStaticAnalysisDatabase(report)
	if err != nil {
		t.Fatal(err)
	}
	evidence := DispatchCensusReport{
		Version: 2, ROMHash: report.ROM.SHA256, TraceHash: strings.Repeat("a", 64),
		Provenance: "snesrecomp-runtime-dispatch-census-v2", Overflow: true,
		Observations: []DispatchObservation{
			{SitePC: 0x008010, TargetPC: 0x809100, M: 1, X: 1, Found: true, Mirrored: true, ObservationCount: 7},
			{SitePC: 0x008010, TargetPC: 0x809100, M: 0, X: 0, Trapped: true, ObservationCount: 20000000},
			{SitePC: 0x008010, TargetPC: 0x008004, Continuation: true, ObservationCount: 9},
			{SitePC: 0x008040, TargetPC: 0x009200, Emulation: true, Trapped: true, ObservationCount: 11},
			{SitePC: 0x008050, TargetPC: 0x008060, Continuation: true, ObservationCount: 12},
		},
	}
	for pass := 0; pass < 2; pass++ {
		if err := applyShadowDispatchEvidence(&report, evidence); err != nil {
			t.Fatal(err)
		}
		if report.Summary.UniqueUnresolvedSites != 1 || report.Summary.ObservedUnresolvedSites != 0 || report.Summary.UnobservedUnresolvedSites != 1 {
			t.Fatalf("legacy unresolved counts changed: %+v", report.Summary)
		}
		if report.DispatchSummary.UniqueSites != 5 || report.DispatchSummary.ObservedSites != 3 || report.DispatchSummary.UnobservedSites != 2 ||
			report.DispatchSummary.ObservedOnlySites != 2 || report.DispatchSummary.ObservedMissingBodySites != 2 || report.DispatchSummary.ObservedTrappedSites != 2 {
			t.Fatalf("coverage summary pass %d = %+v", pass, report.DispatchSummary)
		}
		site := inventorySite(t, report, 0x008010)
		if site.RuntimeStatus != shadowRuntimeObservedTrappedMissing || site.RuntimeObservationCount != 20000016 || site.TargetSetStatus != "unproven" || len(site.RuntimeObservations) != 3 {
			t.Fatalf("HLE evidence lost or promoted: %+v", site)
		}
		unknown := inventorySite(t, report, 0x008040)
		if unknown.Reachability != "observed_only" || unknown.Routing != "unknown" || unknown.InstructionBytes != "" || len(unknown.Callers) != 0 || !unknown.RuntimeObservations[0].Emulation {
			t.Fatalf("runtime-only source invented static evidence: %+v", unknown)
		}
		continuation := inventorySite(t, report, 0x008050)
		if continuation.RuntimeStatus != shadowRuntimeObservedResolved {
			t.Fatalf("continuation counted as missing: %+v", continuation)
		}
	}
	after, err := BuildStaticAnalysisDatabase(report)
	if err != nil || !reflect.DeepEqual(before, after) {
		t.Fatalf("census changed static database: %v", err)
	}
	var output bytes.Buffer
	if err := WriteShadowReport(&output, report, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"[DISPATCH-SITE] $00:8010 routing=hle:HostDispatch", "target_set=unproven", "observed-target=$80:9100 M0X0", "continuation=true", "overflow=true", "No classified blockers is not a coverage certificate"} {
		if !strings.Contains(output.String(), want) {
			t.Errorf("missing %q in text report", want)
		}
	}
	evidence.ROMHash = strings.Repeat("f", 64)
	if err := applyShadowDispatchEvidence(&report, evidence); err == nil {
		t.Fatal("accepted mismatched ROM census")
	}
}

func TestShadowDispatchInventorySingleBankDoesNotImportOtherBanks(t *testing.T) {
	options, _ := shadowHLEInventoryFixture(t)
	report, err := AnalyzeAuthoredShadow(options)
	if err != nil {
		t.Fatal(err)
	}
	options.DispatchAnalysisPath = filepath.Join(t.TempDir(), "census.json")
	if err := WriteDispatchCensusFile(options.DispatchAnalysisPath, DispatchCensusReport{
		Version: 2, ROMHash: report.ROM.SHA256, Provenance: "snesrecomp-runtime-dispatch-census-v2",
		Observations: []DispatchObservation{{SitePC: 0x008010, TargetPC: 0x009000, Found: true, ObservationCount: 3}, {SitePC: 0x018010, TargetPC: 0x019000, ObservationCount: 4}},
	}); err != nil {
		t.Fatal(err)
	}
	bank := byte(0)
	options.OnlyBank = &bank
	report, err = AnalyzeAuthoredShadow(options)
	if err != nil {
		t.Fatal(err)
	}
	if report.DispatchSummary.UniqueSites != 3 || report.DispatchSummary.ObservedOnlySites != 0 || report.DispatchEvidence.Observations != 1 {
		t.Fatalf("single-bank scope leaked: %+v", report.DispatchSummary)
	}
}

func TestShadowDispatchInventoryKeepsRoutedStreamPatternReportOnly(t *testing.T) {
	options, image := shadowHLEInventoryFixture(t)
	copy(image, []byte{
		0xac, 0x34, 0x12, // LDY $1234
		0xb9, 0x00, 0x00, // LDA $0000,Y
		0x10, 0x07, // BPL $800F
		0x85, 0x98, // STA $98
		0x20, 0x20, 0x80, // JSR $8020
		0x80, 0xf1, // BRA $8000
		0x60,
	})
	copy(image[0x20:], []byte{0x6c, 0x98, 0x00})
	if err := os.WriteFile(options.ROMPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, filepath.Join(options.CFGDir, "bank00.cfg"), "bank = 00\nfunc Interpreter 8000 entry_mx:0,0\nhle_dispatch 8020 HostStreamDispatch\n")
	report, err := AnalyzeAuthoredShadow(options)
	if err != nil {
		t.Fatal(err)
	}
	site := inventorySite(t, report, 0x008020)
	if site.Routing != "hle" || site.Classification != shadowUnresolvedTaggedStreamDispatch || site.Priority != shadowPriorityLikelyBlocker || site.StreamDispatch == nil || site.StreamDispatch.InterpreterEntryPC != 0x008000 {
		t.Fatalf("HLE routing hid stream evidence: %+v", site)
	}
	if site.TargetSetStatus != "unproven" || len(report.Unresolved) != 0 || report.Summary.RawUnresolvedEmissions != 0 || report.Summary.LikelyBlockingUnresolvedSites != 0 {
		t.Fatalf("routed heuristic changed legacy trap reporting: %+v", report.Summary)
	}
	if len(site.PointerProducers) != 1 || site.PointerProducers[0].TableOffset != 0 || len(site.PointerProducers[0].ROMBaseCandidates) != 0 ||
		site.PointerProducers[0].IndexOrigin == nil || site.PointerProducers[0].IndexOrigin.Operand != 0x1234 {
		t.Fatalf("variable stream pointer became a fixed ROM table: %+v", site.PointerProducers)
	}
}
