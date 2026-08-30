package tooling

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func writeTestFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestSyncFuncs(t *testing.T) {
	root := t.TempDir()
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), "bank = 00\nfunc First 8000 entry_mx:1,1\nname 018100 Alias\n")
	writeTestFile(t, filepath.Join(cfgDir, "bank01.cfg"), "bank = 01\nfunc Second 9000 entry_mx:1,1\n")
	output := filepath.Join(root, "funcs.h")
	count, err := SyncFuncs(cfgDir, output)
	if err != nil {
		t.Fatal(err)
	}
	if count != 3 {
		t.Fatalf("count = %d, want 3", count)
	}
	data, err := os.ReadFile(output)
	if err != nil {
		t.Fatal(err)
	}
	source := string(data)
	for _, wanted := range []string{
		"void First(CpuState *cpu);  /* $00:8000 alias */",
		"RecompReturn Alias_M0X1(CpuState *cpu);",
		"void Second(CpuState *cpu);  /* $01:9000 alias */",
	} {
		if !strings.Contains(source, wanted) {
			t.Errorf("header missing %q", wanted)
		}
	}
}

func TestGenerateMetadata(t *testing.T) {
	root := t.TempDir()
	genDir, cfgDir := filepath.Join(root, "gen"), filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(genDir, "bank00_v2.c"), "RecompReturn bank_00_8000_M1X1(CpuState *cpu) {\n  L_8000_M1X1:\n  return RECOMP_RETURN_NORMAL; /* tail-call past end: into bank_00_8010_M1X1 at $8010 */\n}\n")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), "bank = 00\nfunc bank_00_8000 8000\nrts_dispatch 8005 8010\n")
	output := filepath.Join(root, "meta.json")
	now := time.Date(2026, 7, 18, 1, 2, 3, 0, time.Local)
	report, err := GenerateMetadata(genDir, cfgDir, output, now)
	if err != nil {
		t.Fatal(err)
	}
	if report.Functions != 1 || report.Labels != 1 || report.TailCalls != 1 || report.CFGCounts["func"] != 1 {
		t.Fatalf("unexpected report: %+v", report)
	}
	var metadata GeneratedMetadata
	data, err := os.ReadFile(output)
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(data, &metadata); err != nil {
		t.Fatal(err)
	}
	if metadata.GeneratedAt != "2026-07-18 01:02:03" || metadata.Functions["008000"][0] != "_M1X1" || metadata.TailCalls[0].TargetPC != "008010" {
		t.Fatalf("unexpected metadata: %+v", metadata)
	}
}

func TestCensusRTSWebs(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	image[0], image[1], image[2], image[3] = 0xA9, 0x0F, 0x80, 0x48
	image[0x10], image[0x11], image[0x12] = 0xEA, 0xEA, 0x60
	image[0x20], image[0x21] = 0x48, 0x60
	romPath := filepath.Join(root, "test.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), "bank = 00\n")
	var output bytes.Buffer
	report, err := CensusRTSWebs(RTSCensusOptions{ROMPath: romPath, CFGDir: cfgDir, Output: &output})
	if err != nil {
		t.Fatal(err)
	}
	if report.UncoveredPushes != 1 || report.UncoveredSites != 1 {
		t.Fatalf("unexpected report: %+v\n%s", report, output.String())
	}
	if !strings.Contains(output.String(), "push @00:8000") || !strings.Contains(output.String(), "PHA;RTS dispatch @00:8021") {
		t.Fatalf("unexpected output:\n%s", output.String())
	}
}

func TestCensusRTSWebsFindsStackCapturedJSRContinuation(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	image[0], image[1], image[2] = 0x20, 0x00, 0x81 // JSR $8100.
	copy(image[0x100:], []byte{0xA3, 0x01, 0x1A, 0x9D, 0x12, 0x00, 0x60})
	romPath := filepath.Join(root, "test.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), "bank = 00\n")
	var output bytes.Buffer
	report, err := CensusRTSWebs(RTSCensusOptions{ROMPath: romPath, CFGDir: cfgDir, YieldHelpers: true, Suggest: true, Output: &output})
	if err != nil {
		t.Fatal(err)
	}
	if report.YieldHelpers != 1 || report.YieldContinuations != 1 || report.UncoveredContinuations != 1 {
		t.Fatalf("unexpected report: %+v\n%s", report, output.String())
	}
	if !strings.Contains(output.String(), "helper 00:8100") || !strings.Contains(output.String(), "continuation 00:8003 [UNCOVERED]") {
		t.Fatalf("unexpected output:\n%s", output.String())
	}
}

func TestCensusStubsCollapsesVariants(t *testing.T) {
	root := t.TempDir()
	writeTestFile(t, filepath.Join(root, "bank00_v2.c"), "return cpu_trace_unresolved_goto_trap(cpu, 0x008000, 0x008100, \"Fn_M0X0\", \"L\");\nreturn cpu_trace_unresolved_goto_trap(cpu, 0x008000, 0x008100, \"Fn_M1X1\", \"L\");\nreturn cpu_trace_dispatch_oob(cpu, 0x008200, 0xffff);\n(void)cpu_trace_unresolved_stub_trap(cpu, 0x001234, \"bad\");\nreturn cpu_trace_unresolved_indirect_jump(cpu, 0x008300);\n")
	writeTestFile(t, filepath.Join(root, "unresolved_stubs_v2.c"), "return cpu_trace_unresolved_stub_trap(cpu, 0x011234, \"Stub_M0X0\");\nreturn cpu_trace_unresolved_stub_trap(cpu, 0x011234, \"Stub_M1X1\");\n")
	var output bytes.Buffer
	report, err := CensusStubs(root, true, &output)
	if err != nil {
		t.Fatal(err)
	}
	if report.LogicalGotos != 1 || report.GotoEmissions != 2 ||
		report.LogicalDispatches != 1 || report.DispatchEmissions != 1 ||
		report.LogicalTargets != 2 || report.TargetEmissions != 3 ||
		report.LogicalIndirects != 1 || report.IndirectEmissions != 1 ||
		report.LogicalTotal() != 5 {
		t.Fatalf("unexpected report: %+v", report)
	}
}

func TestAnalyzeAuthoredShadowIsReadOnlyAndClassifiesComparisons(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{
		0xBD, 0x00, 0x81, // LDA $8100,X
		0xA0, 0x08, 0x80, // LDY #$8008 (RTS returns to $8009)
		0x5A, // PHY
		0x48, // PHA -- authored indirect site $8007
		0x60, // RTS
	})
	copy(image[0x0010:], []byte{0xA9, 0x1F, 0x80, 0x48, 0x60}) // push $801F; RTS at $8014 -> $8020
	copy(image[0x0030:], []byte{0x6C, 0x00, 0x20})             // unresolved JMP ($2000)
	image[0x0040] = 0x60                                       // authored-only RTS site
	image[0x0100], image[0x0101] = 0xFF, 0x81                  // handler-1 -> $8200
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	configText := `bank = 00
func PHAEntry 8000 entry_mx:0,0
func ImmediateEntry 8010 entry_mx:0,0
func UnresolvedEntry 8030 entry_mx:0,0
func PlainRTS 8040 entry_mx:0,0
indirect_dispatch 8007 1 idx:A tables:8100 ret:8009
rts_dispatch 8014 8020
rts_dispatch 8040 8050
`
	configPath := filepath.Join(cfgDir, "bank00.cfg")
	writeTestFile(t, configPath, configText)
	before, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatal(err)
	}

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 2})
	if err != nil {
		t.Fatal(err)
	}
	if !report.NoWrite || report.Summary.AuthoredFacts != 3 || report.Summary.InferredFacts != 2 ||
		report.Summary.ExactMatches != 1 || report.Summary.PartialMatches != 1 ||
		report.Summary.AuthoredOnly != 1 || report.Summary.Conflicts != 0 {
		t.Fatalf("unexpected shadow summary: %+v", report.Summary)
	}
	if report.Summary.RawUnresolvedEmissions != 1 || report.Summary.UniqueUnresolvedSites != 1 {
		t.Fatalf("unexpected unresolved summary: %+v", report.Summary)
	}
	after, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(before, after) {
		t.Fatal("shadow analysis modified authored configuration")
	}
	entries, err := os.ReadDir(root)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 2 { // fixture.sfc and recomp/
		t.Fatalf("shadow analysis created output files: %v", entries)
	}
	var output bytes.Buffer
	if err := WriteShadowReport(&output, report, "json", false); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(output.String(), `"mode": "compare_authored"`) || !strings.Contains(output.String(), `"no_write": true`) {
		t.Fatalf("unexpected JSON report:\n%s", output.String())
	}
	second, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 1})
	if err != nil {
		t.Fatal(err)
	}
	var secondOutput bytes.Buffer
	if err := WriteShadowReport(&secondOutput, second, "json", false); err != nil {
		t.Fatal(err)
	}
	if output.String() != secondOutput.String() {
		t.Fatalf("JSON report changed with worker count:\njobs=2:\n%s\njobs=1:\n%s", output.String(), secondOutput.String())
	}
}

func TestAnalyzeAuthoredShadowPrioritizesTaggedStreamHandlerDispatch(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{
		0xAC, 0x34, 0x12, // LDY $1234 -- variable stream pointer
		0xB9, 0x00, 0x00, // LDA $0000,Y -- tagged stream word
		0x10, 0x07, // BPL $800F -- non-negative word takes data path
		0x85, 0x98, // STA $98 -- stage handler address
		0x20, 0x20, 0x80, // JSR $8020 -- trampoline
		0x80, 0xF1, // BRA $8000
		0x60, // data-path return
	})
	copy(image[0x0020:], []byte{0x6C, 0x98, 0x00}) // JMP ($0098)
	image[0x003F] = 0x60
	copy(image[0x0040:], []byte{0xA9, 0x01, 0x00, 0x8D, 0x34, 0x12, 0x60})
	image[0x004F] = 0x6B
	copy(image[0x0050:], []byte{0xA9, 0x02, 0x00, 0x9D, 0x34, 0x12, 0x60})
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), "bank = 00\nfunc Interpreter 8000 entry_mx:0,0\n")

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 2})
	if err != nil {
		t.Fatal(err)
	}
	if report.Summary.UniqueUnresolvedSites != 1 || report.Summary.LikelyBlockingUnresolvedSites != 1 || len(report.Unresolved) != 1 {
		t.Fatalf("unexpected unresolved summary: %+v sites=%+v", report.Summary, report.Unresolved)
	}
	site := report.Unresolved[0]
	if site.SitePC != 0x008020 || site.Classification != shadowUnresolvedTaggedStreamDispatch || site.Priority != shadowPriorityLikelyBlocker || site.StreamDispatch == nil {
		t.Fatalf("unresolved site = %+v", site)
	}
	pattern := site.StreamDispatch
	if pattern.InterpreterEntryPC != 0x008000 || pattern.StreamPointer != 0x1234 || pattern.TargetSlot != 0x98 || pattern.StreamWordLoadPC != 0x008003 || pattern.SignTestPC != 0x008006 || pattern.TargetSlotStorePC != 0x008008 || pattern.TrampolineCallPC != 0x00800A {
		t.Fatalf("stream pattern = %+v", pattern)
	}
	wantCandidates := []uint32{0x008040, 0x008050}
	if len(site.StructuralHandlerCandidates) != len(wantCandidates) {
		t.Fatalf("structural candidates = %v, want %v", site.StructuralHandlerCandidates, wantCandidates)
	}
	for index, want := range wantCandidates {
		if site.StructuralHandlerCandidates[index] != want {
			t.Fatalf("structural candidates = %v, want %v", site.StructuralHandlerCandidates, wantCandidates)
		}
	}
	var output bytes.Buffer
	if err := WriteShadowReport(&output, report, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"likely bring-up blockers=1", "class=tagged_stream_handler_dispatch", "$00:8040", "confirm with dispatch census"} {
		if !strings.Contains(output.String(), want) {
			t.Fatalf("verbose report missing %q:\n%s", want, output.String())
		}
	}
}

func TestAnalyzeAuthoredShadowReportsDispatchCodeIslandAfterBRA(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x7c, 0x00, 0x81}) // JMP ($8100,X)
	copy(image[0x0100:], []byte{0x00, 0x82, 0x40, 0x82})
	copy(image[0x0200:], []byte{0x80, 0x2e}) // BRA $8230
	copy(image[0x0202:], []byte{0xa9, 0x01, 0x85, 0x10, 0x60})
	copy(image[0x0230:], []byte{0x60, 0xea, 0xea, 0xea})
	copy(image[0x0240:], []byte{0xa9, 0x02, 0x85, 0x11, 0x60})
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func Dispatcher 8000 entry_mx:1,1
func FirstHandler 8200 entry_mx:1,1
func NextHandler 8240 entry_mx:1,1
indirect_dispatch 8000 2 idx:X tables:8100 transfer:tail
`)

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{
		ROMPath: romPath, CFGDir: cfgDir, Jobs: 2,
	})
	if err != nil {
		t.Fatal(err)
	}
	if report.Summary.DispatchCodeIslands != 1 || len(report.DispatchCodeIslands) != 1 {
		t.Fatalf("dispatch islands=%+v summary=%+v", report.DispatchCodeIslands, report.Summary)
	}
	if report.Summary.LandingCandidates != 0 || len(report.LandingCandidates) != 0 {
		t.Fatalf("dispatch island duplicated as boundary landing: %+v", report.LandingCandidates)
	}
	island := report.DispatchCodeIslands[0]
	if island.SitePC != 0x008000 || island.PreviousTargetPC != 0x008200 ||
		island.NextTargetPC != 0x008240 || island.CandidateEntryPC != 0x008202 ||
		island.EndExclusive != 0x008207 || island.PrecededBy != "BRA" ||
		island.Confidence != analysis.ConfidenceProbable {
		t.Fatalf("unexpected dispatch code island: %+v", island)
	}
	var output bytes.Buffer
	if err := WriteShadowReport(&output, report, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"dispatch gap sweep: 1", "[DISPATCH-CODE-ISLAND]", "$00:8202", "preceded_by=BRA"} {
		if !strings.Contains(output.String(), want) {
			t.Fatalf("verbose report missing %q:\n%s", want, output.String())
		}
	}
}

func TestAnalyzeAuthoredShadowReportsBoundaryLandingWithExactMX(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0200:], []byte{0x80, 0x2e})                   // BRA $8230
	copy(image[0x0202:], []byte{0xa9, 0x01, 0x85, 0x10, 0x60}) // M=1: LDA #$01; STA $10; RTS
	copy(image[0x0230:], []byte{0x60})                         // owned branch target
	copy(image[0x0240:], []byte{0xa9, 0x02, 0x85, 0x11, 0x60}) // next confirmed entry
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func First 8200 entry_mx:1,1
func Anchor 8240 entry_mx:1,1
`)

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{
		ROMPath: romPath, CFGDir: cfgDir, Jobs: 2,
	})
	if err != nil {
		t.Fatal(err)
	}
	if report.Summary.LandingCandidates != 1 || len(report.LandingCandidates) != 1 {
		t.Fatalf("landing candidates=%+v summary=%+v", report.LandingCandidates, report.Summary)
	}
	candidate := report.LandingCandidates[0]
	if candidate.AnchorPC != 0x008240 || candidate.CandidateEntryPC != 0x008202 || candidate.PrecededBy != "BRA" ||
		candidate.Ownership != shadowLandingUnclaimed || candidate.Confidence != analysis.ConfidenceProbable || len(candidate.Variants) != 1 {
		t.Fatalf("unexpected landing candidate: %+v", candidate)
	}
	variant := candidate.Variants[0]
	if variant.EndExclusive != 0x008207 || variant.Termination != "clean_return" || variant.InstructionCount != 3 || variant.Confidence != analysis.ConfidenceProbable {
		t.Fatalf("unexpected landing variant: %+v", variant)
	}
	wantMX := []analysis.MXState{{M: 1, X: 0}, {M: 1, X: 1}}
	if len(variant.EntryMX) != len(wantMX) {
		t.Fatalf("entry M/X = %+v, want %+v", variant.EntryMX, wantMX)
	}
	for index := range wantMX {
		if variant.EntryMX[index] != wantMX[index] {
			t.Fatalf("entry M/X = %+v, want %+v", variant.EntryMX, wantMX)
		}
	}
	var output bytes.Buffer
	if err := WriteShadowReport(&output, report, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"boundary landing sweep: 1 unclaimed entry candidate(s): 1 probable, 0 speculative", "[LANDING-CANDIDATE]", "$00:8202", "entry_mx=M1X0,M1X1", "termination=clean_return"} {
		if !strings.Contains(output.String(), want) {
			t.Fatalf("verbose report missing %q:\n%s", want, output.String())
		}
	}
}

func TestBoundaryLandingDowngradesCandidateTableButRejectsConfirmedTable(t *testing.T) {
	image := make([]byte, 0x8000)
	copy(image[0x0202:], []byte{0xa9, 0x01, 0x85, 0x10, 0x60})
	end := uint16(0x8240)
	graph, err := decoder.DecodeFunction(image, 0, 0x8202, 1, 0, decoder.Options{End: &end})
	if err != nil {
		t.Fatal(err)
	}
	span := ShadowTableSpan{
		StartPC: 0x008202, EndExclusive: 0x008207,
		Ownership: shadowTableOwnershipCandidate, Confidence: analysis.ConfidenceProbable,
	}
	shape, ok := validateShadowLandingGraph(graph, 0x008202, 0x008240, map[uint32]struct{}{}, nil, []ShadowTableSpan{span})
	if !ok || shape.Ownership != shadowLandingTableConflict || shape.Confidence != analysis.ConfidenceSpeculative {
		t.Fatalf("candidate table landing shape=%+v ok=%t", shape, ok)
	}
	span.Ownership = shadowTableOwnershipConfirmed
	if _, ok := validateShadowLandingGraph(graph, 0x008202, 0x008240, map[uint32]struct{}{}, nil, []ShadowTableSpan{span}); ok {
		t.Fatal("confirmed table bytes were accepted as a landing candidate")
	}
}

func TestAnalyzeAuthoredShadowDeduplicatesWidthDependentLandingShapes(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0200:], []byte{0x80, 0x2e})
	copy(image[0x0202:], []byte{0xa9, 0x60, 0xea, 0x60}) // M=0: LDA #$EA60; RTS. M=1: LDA #$60; NOP; RTS.
	copy(image[0x0230:], []byte{0x60})
	copy(image[0x0240:], []byte{0x60})
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func First 8200 entry_mx:1,1
func Anchor 8240 entry_mx:1,1
`)

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 2})
	if err != nil {
		t.Fatal(err)
	}
	if report.Summary.LandingCandidates != 1 || len(report.LandingCandidates) != 1 || len(report.LandingCandidates[0].Variants) != 2 {
		t.Fatalf("width variants inflated landing sites: summary=%+v candidates=%+v", report.Summary, report.LandingCandidates)
	}
	if report.LandingCandidates[0].Confidence != analysis.ConfidenceProbable || report.Summary.ProbableLandingCandidates != 1 || report.Summary.SpeculativeLandingCandidates != 0 {
		t.Fatalf("aggregate landing confidence=%+v summary=%+v", report.LandingCandidates[0], report.Summary)
	}
	if report.LandingCandidates[0].Variants[0].InstructionCount != 2 || report.LandingCandidates[0].Variants[1].InstructionCount != 3 {
		t.Fatalf("width-dependent shapes=%+v", report.LandingCandidates[0].Variants)
	}
}

func TestAnalyzeAuthoredShadowClassifiesWeakLandingAsSpeculative(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0200:], []byte{0x80, 0x2e})       // BRA $8230
	copy(image[0x0202:], []byte{0x89, 0x00, 0x40}) // M=1: BIT #$00; RTI
	copy(image[0x0230:], []byte{0x60})
	copy(image[0x0240:], []byte{0x60})
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func First 8200 entry_mx:1,1
func Anchor 8240 entry_mx:1,1
`)

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 1})
	if err != nil {
		t.Fatal(err)
	}
	if len(report.LandingCandidates) != 1 || report.LandingCandidates[0].Confidence != analysis.ConfidenceSpeculative || len(report.LandingCandidates[0].Variants) != 1 || report.LandingCandidates[0].Variants[0].Confidence != analysis.ConfidenceSpeculative {
		t.Fatalf("landing candidates=%+v", report.LandingCandidates)
	}
	if report.Summary.ProbableLandingCandidates != 0 || report.Summary.SpeculativeLandingCandidates != 1 {
		t.Fatalf("landing confidence summary=%+v", report.Summary)
	}
}

func TestAnalyzeAuthoredShadowBoundaryLandingCanJoinAnchor(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0200:], []byte{0x80, 0x2e})             // BRA $8230
	copy(image[0x0202:], []byte{0xa9, 0x01, 0x80, 0x3a}) // LDA #$01; BRA $8240
	copy(image[0x0230:], []byte{0x60})
	copy(image[0x0240:], []byte{0x60})
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func First 8200 entry_mx:1,1
func Anchor 8240 entry_mx:1,1
`)

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 1})
	if err != nil {
		t.Fatal(err)
	}
	if len(report.LandingCandidates) != 1 || len(report.LandingCandidates[0].Variants) != 1 || report.LandingCandidates[0].Variants[0].Termination != "join_anchor" || report.LandingCandidates[0].Variants[0].EndExclusive != 0x008206 {
		t.Fatalf("landing candidates=%+v", report.LandingCandidates)
	}
}

func TestAnalyzeAuthoredShadowBoundaryLandingRejectsDeclaredData(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0200:], []byte{0x80, 0x2e})
	copy(image[0x0202:], []byte{0xa9, 0x01, 0x85, 0x10, 0x60})
	copy(image[0x0230:], []byte{0x60})
	copy(image[0x0240:], []byte{0x60})
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func First 8200 entry_mx:1,1
func Anchor 8240 entry_mx:1,1
data_region 00 8202 8207
`)

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 1})
	if err != nil {
		t.Fatal(err)
	}
	if report.Summary.LandingCandidates != 0 || len(report.LandingCandidates) != 0 {
		t.Fatalf("declared data produced landing candidates: %+v", report.LandingCandidates)
	}
}

func TestAnalyzeAuthoredShadowReportsVectorRootEntryRecoveryClasses(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{
		0x20, 0x00, 0x81, // JSR $8100: exact authored M/X
		0x20, 0x10, 0x81, // JSR $8110: address recovered with different M/X
		0x4c, 0x20, 0x81, // JMP $8120: owned internally, not a new call entry
	})
	image[0x0100] = 0x60
	image[0x0110] = 0x60
	image[0x0120] = 0x60
	image[0x0200] = 0x60
	copy(image[0x0300:], []byte{0x00, 0x81, 0x20, 0x81, 0x00, 0x82}) // probable same-bank entry table
	for _, offset := range []int{0x7fea, 0x7fee, 0x7ffc} {
		image[offset], image[offset+1] = 0x00, 0x80
	}
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func Reset 8000 entry_mx:1,1
func Direct 8100 entry_mx:1,1
func WrongMX 8110 entry_mx:0,0
func Tail 8120 entry_mx:1,1
func Hidden 8200 entry_mx:1,1
`)

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 2})
	if err != nil {
		t.Fatal(err)
	}
	summary := report.EntryRecovery.Summary
	if summary.AuthoredEntries != 5 || summary.ExactVariants != 2 || summary.AddressOtherMX != 1 ||
		summary.InternalOwned != 1 || summary.ProvenContinuations != 0 || summary.NotRecovered != 1 ||
		summary.InitialRootVariants != 1 || summary.FinalRootVariants != 3 || summary.DecodeIssues != 0 ||
		summary.EntriesWithPointerClusters != 3 || summary.ProbablePointerClusterEntries != 3 || summary.NotRecoveredWithPointerClusters != 1 {
		t.Fatalf("entry recovery summary=%+v", summary)
	}
	statuses := make(map[string]string)
	for _, entry := range report.EntryRecovery.Entries {
		statuses[entry.Name] = entry.Status
	}
	want := map[string]string{
		"Reset": shadowEntryRecoveryExact, "Direct": shadowEntryRecoveryExact,
		"WrongMX": shadowEntryRecoveryAddressOnly, "Tail": shadowEntryRecoveryInternal,
		"Hidden": shadowEntryRecoveryMissing,
	}
	for name, status := range want {
		if statuses[name] != status {
			t.Fatalf("entry %s status=%q, want %q; all=%v", name, statuses[name], status, statuses)
		}
	}
	for _, name := range []string{"Direct", "Tail", "Hidden"} {
		var found *ShadowEntryRecoveryRecord
		for index := range report.EntryRecovery.Entries {
			if report.EntryRecovery.Entries[index].Name == name {
				found = &report.EntryRecovery.Entries[index]
				break
			}
		}
		if found == nil || len(found.PointerClusters) != 1 || found.PointerClusters[0].StartPC != 0x008300 ||
			found.PointerClusters[0].EndExclusive != 0x008306 || found.PointerClusters[0].Confidence != analysis.ConfidenceProbable {
			t.Fatalf("entry %s pointer clusters=%+v", name, found)
		}
	}
	var output bytes.Buffer
	if err := WriteShadowReport(&output, report, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, fragment := range []string{"vector-root entry recovery: 5 authored", "exact=2", "address-other-M/X=1", "internal-owned=1", "not-recovered=1", "authored-entry ROM pointer clusters: 3 declaration(s), 3 probable", "[ENTRY-ADDRESS-OTHER-MX]", "[ENTRY-INTERNAL-OWNED]", "[ENTRY-NOT-RECOVERED]", "pointer-cluster=$00:8300..$00:8306"} {
		if !strings.Contains(output.String(), fragment) {
			t.Fatalf("verbose recovery report missing %q:\n%s", fragment, output.String())
		}
	}
}

func TestAnalyzeAuthoredShadowReportsStaticEntryAblationRoots(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x20, 0x00, 0x81, 0x60}) // Reset -> Direct1
	copy(image[0x0100:], []byte{0x20, 0x00, 0x82, 0x60}) // Direct1 -> Direct2
	copy(image[0x0200:], []byte{0x20, 0x00, 0x83, 0x60}) // Direct2 -> Direct3
	copy(image[0x0300:], []byte{0x20, 0x00, 0x87, 0x60}) // demands D at the wrong authored M/X
	copy(image[0x0400:], []byte{0x20, 0x00, 0x85, 0x60}) // cycle A -> B
	copy(image[0x0500:], []byte{0x20, 0x00, 0x84, 0x60}) // cycle B -> A
	image[0x0600] = 0x60                                 // independent C
	image[0x0700] = 0x60                                 // independent D M0X0
	copy(image[0x0800:], []byte{0x4c, 0x00, 0x89})       // external TailCaller -> TailTarget
	image[0x0900] = 0x60
	for _, offset := range []int{0x7fea, 0x7fee, 0x7ffc} {
		image[offset], image[offset+1] = 0x00, 0x80
	}
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func Reset 8000 entry_mx:1,1
func Direct1 8100 entry_mx:1,1
func Direct2 8200 entry_mx:1,1
func Direct3 8300 entry_mx:1,1
func CycleA 8400 entry_mx:1,1
func CycleB 8500 entry_mx:1,1
func IndependentC 8600 entry_mx:1,1
func WrongWidthD 8700 entry_mx:0,0
func TailCaller 8800 entry_mx:1,1
func TailTarget 8900 entry_mx:1,1
`)

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 2})
	if err != nil {
		t.Fatal(err)
	}
	summary := report.EntryAblation.Summary
	if summary.AuthoredDeclarations != 10 || summary.UniqueAuthoredVariants != 10 || summary.StaticDependencyEdges != 7 ||
		summary.VectorCoveredDeclarations != 4 || summary.IndividuallyRecoverable != 7 || summary.BatchRecoverable != 2 ||
		summary.RetainedRootDeclarations != 4 || summary.RetainedUniqueRootVariants != 4 || summary.BatchRoutineTargets != 1 ||
		summary.BatchTailTargets != 1 || summary.BatchComputedTargets != 0 || summary.BatchInternalContinuations != 0 {
		t.Fatalf("entry ablation summary=%+v", summary)
	}
	statuses := make(map[string]ShadowEntryAblationRecord)
	for _, entry := range report.EntryAblation.Entries {
		statuses[entry.Name] = entry
	}
	for _, name := range []string{"Reset", "Direct1", "Direct2", "Direct3"} {
		if statuses[name].Status != shadowEntryAblationVectorCovered || !statuses[name].IndividuallyRecoverable {
			t.Fatalf("vector entry %s=%+v", name, statuses[name])
		}
	}
	if statuses["CycleA"].Status != shadowEntryAblationRetained || !statuses["CycleA"].IndividuallyRecoverable ||
		statuses["CycleB"].Status != shadowEntryAblationRecoverable || !statuses["CycleB"].IndividuallyRecoverable || statuses["CycleB"].EntryKindHint != "routine" {
		t.Fatalf("cycle statuses A=%+v B=%+v", statuses["CycleA"], statuses["CycleB"])
	}
	if len(statuses["CycleA"].Incoming) != 1 || len(statuses["CycleA"].Incoming[0].Kinds) != 1 || statuses["CycleA"].Incoming[0].Kinds[0] != "direct_jsr" {
		t.Fatalf("cycle A incoming provenance=%+v", statuses["CycleA"].Incoming)
	}
	for _, name := range []string{"IndependentC", "WrongWidthD"} {
		if statuses[name].Status != shadowEntryAblationRetained || statuses[name].IndividuallyRecoverable {
			t.Fatalf("independent entry %s=%+v", name, statuses[name])
		}
	}
	if statuses["TailCaller"].Status != shadowEntryAblationRetained || statuses["TailTarget"].Status != shadowEntryAblationRecoverable ||
		statuses["TailTarget"].EntryKindHint != "tail_target" || len(statuses["TailTarget"].Incoming) != 1 ||
		statuses["TailTarget"].Incoming[0].Kinds[0] != "direct_tail_jump" {
		t.Fatalf("tail statuses caller=%+v target=%+v", statuses["TailCaller"], statuses["TailTarget"])
	}
	var output bytes.Buffer
	if err := WriteShadowReport(&output, report, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, fragment := range []string{"static entry ablation: 10 declaration(s)/10 unique variant(s), 7 dependency edge(s)", "individually recoverable=7", "inclusion-minimal roots=4 declaration(s)/4 variant(s)", "[ABLATION-RETAINED-STATIC-ROOT] $00:8400", "[ABLATION-RETAINED-STATIC-ROOT] $00:8700"} {
		if !strings.Contains(output.String(), fragment) {
			t.Fatalf("verbose ablation report missing %q:\n%s", fragment, output.String())
		}
	}
}

func TestStaticEntryAblationInventoriesHLEOnlyObligations(t *testing.T) {
	cfg := &config.Config{
		Entries: []config.Entry{
			{Name: "Root", Start: 0x8000, EntryMX: config.MX{M: 1, X: 1}},
			{Name: "ExplicitHLE", Start: 0x8100, EntryMX: config.MX{M: 1, X: 1}},
		},
		HLEFunctions: map[uint16]string{0x8100: "HostWhole"},
		HLEFunctionsIf: map[uint16]config.HLEFunctionIf{
			0x8200: {Function: "HostConditional", Predicate: "HostConditionalEnabled"},
		},
		HLESPCUpload: []uint16{0x8300},
	}
	report := analyzeShadowEntryAblation(make([]byte, 0x8000), []shadowBank{{ID: 0, Config: cfg}}, nil)
	if report.Summary.AuthoredHLEObligations != 3 || report.Summary.HLEOnlyObligations != 2 {
		t.Fatalf("HLE obligation summary=%+v", report.Summary)
	}
	if len(report.HLEObligations) != 3 {
		t.Fatalf("HLE obligations=%+v", report.HLEObligations)
	}
	want := []ShadowEntryHLEObligation{
		{PC: 0x008100, Directive: "hle_func", Function: "HostWhole", AuthoredEntry: true},
		{PC: 0x008200, Directive: "hle_func_if", Function: "HostConditional", Predicate: "HostConditionalEnabled", AuthoredEntry: false},
		{PC: 0x008300, Directive: "hle_spc_upload", AuthoredEntry: false},
	}
	for index := range want {
		if report.HLEObligations[index] != want[index] {
			t.Fatalf("HLE obligation %d=%+v, want %+v", index, report.HLEObligations[index], want[index])
		}
	}
	if len(report.Entries) != 2 || len(report.Entries[1].AuthoredHLE) != 1 || report.Entries[1].AuthoredHLE[0] != "hle_func:HostWhole" {
		t.Fatalf("explicit HLE entry annotation missing: %+v", report.Entries)
	}
}

func TestShadowEntryPointerOwnershipKeepsEvidenceClassesSeparate(t *testing.T) {
	start, end := uint32(0x008100), uint32(0x008106)
	confirmed := []ShadowTableSpan{{StartPC: start, EndExclusive: end, Ownership: shadowTableOwnershipConfirmed}}
	if ownership, confidence := shadowEntryPointerOwnership(start, end, 3, nil, confirmed); ownership != shadowTableOwnershipConfirmed || confidence != analysis.ConfidenceProven {
		t.Fatalf("confirmed table ownership=%s confidence=%s", ownership, confidence)
	}
	candidate := []ShadowTableSpan{{StartPC: start, EndExclusive: end, Ownership: shadowTableOwnershipCandidate}}
	if ownership, confidence := shadowEntryPointerOwnership(start, end, 3, nil, candidate); ownership != shadowTableOwnershipCandidate || confidence != analysis.ConfidenceProbable {
		t.Fatalf("candidate table ownership=%s confidence=%s", ownership, confidence)
	}
	owned := map[uint32]struct{}{start + 2: {}}
	if ownership, confidence := shadowEntryPointerOwnership(start, end, 3, owned, nil); ownership != "decoded_code_overlap" || confidence != analysis.ConfidenceSpeculative {
		t.Fatalf("decoded overlap ownership=%s confidence=%s", ownership, confidence)
	}
	if ownership, confidence := shadowEntryPointerOwnership(start, end-2, 2, nil, nil); ownership != "unclaimed_rom" || confidence != analysis.ConfidenceSpeculative {
		t.Fatalf("short unclaimed ownership=%s confidence=%s", ownership, confidence)
	}
}

func TestAnalyzeAuthoredShadowFindsTableFirstInteriorAndEdgeTarget(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0200:], []byte{0x80, 0x2e})                   // BRA $8230
	copy(image[0x0202:], []byte{0xa9, 0x01, 0x85, 0x10, 0x60}) // unconfigured, stack-balanced routine
	image[0x0230] = 0x60
	image[0x0240] = 0x60
	copy(image[0x0300:], []byte{0x00, 0x82, 0x02, 0x82, 0x40, 0x82}) // missing interior target
	copy(image[0x0310:], []byte{0x00, 0x82, 0x40, 0x82, 0x02, 0x82}) // missing trailing target
	copy(image[0x0320:], []byte{0x00, 0x82, 0x50, 0x82, 0x40, 0x82}) // pointer shape without landing evidence
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func First 8200 entry_mx:1,1
func Anchor 8240 entry_mx:1,1
`)

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 2})
	if err != nil {
		t.Fatal(err)
	}
	if report.Summary.TableFirstTargets != 1 || report.Summary.ProbableTableFirstTargets != 1 ||
		report.Summary.SpeculativeTableFirstTargets != 0 || len(report.TableFirstTargets) != 1 {
		t.Fatalf("table-first summary=%+v targets=%+v", report.Summary, report.TableFirstTargets)
	}
	target := report.TableFirstTargets[0]
	if target.TargetPC != 0x008202 || target.AnchorPC != 0x008240 || target.Confidence != analysis.ConfidenceProbable || len(target.Sources) != 2 {
		t.Fatalf("table-first target=%+v", target)
	}
	kinds := make(map[string]bool)
	for _, source := range target.Sources {
		kinds[source.Kind] = true
		if source.KnownEntries != 2 || source.DistinctKnownTargets != 2 || source.Confidence != analysis.ConfidenceProbable {
			t.Fatalf("table-first source=%+v", source)
		}
	}
	if !kinds[shadowTableFirstMissingInterior] || !kinds[shadowTableFirstMissingAfter] {
		t.Fatalf("table-first source kinds=%v", kinds)
	}
	for _, candidate := range report.TableFirstTargets {
		if candidate.TargetPC == 0x008250 {
			t.Fatalf("uncorroborated pointer shape became a finding: %+v", candidate)
		}
	}
	var output bytes.Buffer
	if err := WriteShadowReport(&output, report, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, fragment := range []string{"table-first target discovery: 2 pointer seed(s) (absolute=2 base+offset=0; base arithmetic=0 base(s)/0 site(s)); 1 accepted target(s) (absolute=1 base+offset=0): 1 new landing(s), 0 address-taken internal; 1 probable, 0 speculative", "post-terminator=1", "rejected=1", "[TABLE-FIRST-TARGET] $00:8202", "class=new_landing", "source=missing_interior", "source=missing_after"} {
		if !strings.Contains(output.String(), fragment) {
			t.Fatalf("verbose table-first report missing %q:\n%s", fragment, output.String())
		}
	}
}

func TestAnalyzeAuthoredShadowUsesPointerWindowAsBoundedLandingSeed(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	image[0x0200] = 0x60
	copy(image[0x0210:], []byte{0xa9, 0x01, 0x85, 0x10, 0x60}) // no decoded predecessor boundary
	image[0x0240] = 0x60
	image[0x0280] = 0x60
	copy(image[0x0250:], []byte{0x84, 0x9b, 0xa2, 0x13, 0x40})       // plausible data record ending in RTI
	copy(image[0x0300:], []byte{0x00, 0x82, 0x10, 0x82, 0x40, 0x82}) // valid pointer-seeded target
	copy(image[0x0310:], []byte{0x00, 0x82, 0x50, 0x82, 0x40, 0x82}) // RTI-ending target must stay rejected
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func First 8200 entry_mx:1,1
func Anchor 8240 entry_mx:1,1
func Last 8280 entry_mx:1,1
`)

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 2})
	if err != nil {
		t.Fatal(err)
	}
	if len(report.LandingCandidates) != 0 {
		t.Fatalf("fixture unexpectedly supplied a post-terminator landing: %+v", report.LandingCandidates)
	}
	if len(report.TableFirstTargets) != 1 || report.TableFirstTargets[0].TargetPC != 0x008210 ||
		report.TableFirstTargets[0].LandingSeed != "pointer_window" || report.TableFirstTargets[0].Confidence != analysis.ConfidenceProbable {
		t.Fatalf("pointer-seeded findings=%+v", report.TableFirstTargets)
	}
	var rtiRejection *ShadowTableFirstRejection
	for index := range report.TableFirstRejections {
		if report.TableFirstRejections[index].TargetPC == 0x008250 {
			rtiRejection = &report.TableFirstRejections[index]
			break
		}
	}
	if rtiRejection == nil || rtiRejection.Reason != "rti_not_valid_for_table_pointer" {
		t.Fatalf("pointer-seeded rejections=%+v", report.TableFirstRejections)
	}
}

func TestAnalyzeAuthoredShadowFindsBasePlusOffsetTableTarget(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x18, 0x69, 0x00, 0x82, 0x60}) // CLC; ADC #$8200; RTS
	image[0x0200] = 0x60
	copy(image[0x0210:], []byte{0xa9, 0x01, 0x85, 0x10, 0x60})
	image[0x0220] = 0x60
	image[0x0240] = 0x60
	copy(image[0x0300:], []byte{0x00, 0x00, 0x10, 0x00, 0x20, 0x00, 0x40, 0x00}) // offsets from $8200
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func BaseEvidence 8000 entry_mx:0,0
func Base 8200 entry_mx:1,1
func Middle 8220 entry_mx:1,1
func Anchor 8240 entry_mx:1,1
`)

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 1})
	if err != nil {
		t.Fatal(err)
	}
	var found *ShadowTableFirstTarget
	for index := range report.TableFirstTargets {
		if report.TableFirstTargets[index].TargetPC == 0x008210 {
			found = &report.TableFirstTargets[index]
			break
		}
	}
	if found == nil || found.Classification != "new_landing" || found.LandingSeed != "pointer_window" ||
		found.Confidence != analysis.ConfidenceProbable || report.Summary.TableFirstBaseOffsetTargets < 1 ||
		report.Summary.TableFirstBaseEvidenceBases != 1 || report.Summary.TableFirstBaseEvidenceSites != 1 {
		t.Fatalf("base+offset findings=%+v summary=%+v", report.TableFirstTargets, report.Summary)
	}
	baseSource := false
	for _, source := range found.Sources {
		if source.Encoding == shadowTableFirstBasePlusU16 && source.BasePC == 0x008200 && source.WordPC == 0x008302 &&
			len(source.BaseEvidencePCs) == 1 && source.BaseEvidencePCs[0] == 0x008001 {
			baseSource = true
		}
	}
	if !baseSource {
		t.Fatalf("base+offset source missing: %+v", found.Sources)
	}
}

func TestAnalyzeAuthoredShadowClassifiesAddressTakenInternalBlock(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0200:], []byte{0x80, 0x0e}) // BRA $8210
	copy(image[0x0210:], []byte{0xa9, 0x01, 0x60})
	image[0x0240] = 0x60
	copy(image[0x0300:], []byte{0x00, 0x82, 0x10, 0x82, 0x40, 0x82})
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func First 8200 entry_mx:1,1
func Anchor 8240 entry_mx:1,1
`)

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 1})
	if err != nil {
		t.Fatal(err)
	}
	if len(report.TableFirstTargets) != 1 || report.Summary.TableFirstInternalTargets != 1 ||
		report.Summary.TableFirstNewLandings != 0 || report.TableFirstTargets[0].TargetPC != 0x008210 ||
		report.TableFirstTargets[0].Classification != "address_taken_internal" ||
		report.TableFirstTargets[0].LandingSeed != "decoded_instruction" || report.TableFirstTargets[0].Confidence != analysis.ConfidenceProbable {
		t.Fatalf("internal target findings=%+v summary=%+v", report.TableFirstTargets, report.Summary)
	}
}

func TestAnalyzeAuthoredShadowReportsPostZeroTableCandidateWithoutChangingCompiledBound(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x7c, 0x00, 0x81}) // JMP ($8100,X)
	for index := 0; index < 72; index++ {
		target := uint16(0x8190)
		if index >= 12 && index <= 15 {
			target = 0
		}
		image[0x100+index*2] = byte(target)
		image[0x101+index*2] = byte(target >> 8)
	}
	copy(image[0x0190:], []byte{0xa9, 0x01, 0x85, 0x10, 0x60,
		0xea, 0xea, 0xea, 0xea, 0xea, 0xea, 0xea, 0xea, 0xea, 0xea, 0xea})
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func Dispatcher 8000 entry_mx:1,0
func Handler 8190 entry_mx:1,0
indirect_dispatch 8000 72 idx:X tables:8100 transfer:tail
`)

	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{
		ROMPath: romPath, CFGDir: cfgDir, Jobs: 2,
	})
	if err != nil {
		t.Fatal(err)
	}
	var comparison *analysis.Comparison
	for index := range report.Comparisons {
		if report.Comparisons[index].SitePC == 0x008000 {
			comparison = &report.Comparisons[index]
			break
		}
	}
	if comparison == nil || comparison.Status != analysis.ComparisonPartial ||
		comparison.Inferred == nil || comparison.Inferred.TargetSetClosed ||
		len(comparison.Inferred.TargetCandidates) != 72 {
		t.Fatalf("zero-hole comparison=%+v summary=%+v", comparison, report.Summary)
	}
	detail := comparison.Inferred.Evidence[0].Detail
	if !strings.Contains(detail, "conservative count=12") ||
		!strings.Contains(detail, "post-zero candidate count=72") {
		t.Fatalf("candidate evidence=%q", detail)
	}
	foundSpan := false
	for _, span := range report.TableSpans {
		if span.SitePC == 0x008000 && span.EntryCount == 72 {
			for _, provenance := range span.Provenance {
				if provenance == "static.decoder.auto_dispatch" {
					foundSpan = true
				}
			}
		}
	}
	if !foundSpan {
		t.Fatalf("missing 72-entry candidate table span: %+v", report.TableSpans)
	}
}

func TestSelectStaticProvenAutomaticDispatchFactsRejectsOpenAndObservedFacts(t *testing.T) {
	proven := analysis.DispatchFact{
		SitePC: 0x008100, Mnemonic: "RTS", Transfer: analysis.TransferResume,
		TargetEntryKind: analysis.EntryContinuation, Targets: []uint32{0x008200}, TargetSetClosed: true,
		Evidence: []analysis.Evidence{{Source: "static.fixture", Confidence: analysis.ConfidenceProven}},
	}
	open := analysis.DispatchFact{
		SitePC: 0x008110, Mnemonic: "JMP", Transfer: analysis.TransferTail,
		TargetEntryKind: analysis.EntryComputed, TargetCandidates: []uint32{0x008300},
		UnknownFields: []string{"targets"},
		Evidence:      []analysis.Evidence{{Source: "static.fixture", Confidence: analysis.ConfidenceProbable}},
	}
	observed := analysis.DispatchFact{
		SitePC: 0x008120, Mnemonic: "RTS", Transfer: analysis.TransferResume,
		TargetEntryKind: analysis.EntryContinuation, Targets: []uint32{0x008400}, TargetSetClosed: true,
		Evidence: []analysis.Evidence{{Source: "replay.fixture", Confidence: analysis.ConfidenceObserved}},
	}
	misclassifiedProfile := analysis.DispatchFact{
		SitePC: 0x008130, Mnemonic: "RTS", Transfer: analysis.TransferResume,
		TargetEntryKind: analysis.EntryContinuation, Targets: []uint32{0x008500}, TargetSetClosed: true,
		Evidence: []analysis.Evidence{{Source: "profile.fixture", Confidence: analysis.ConfidenceProven}},
	}
	report := ShadowReport{Comparisons: []analysis.Comparison{
		{SitePC: proven.SitePC, Status: analysis.ComparisonAutomatic, Inferred: &proven},
		{SitePC: open.SitePC, Status: analysis.ComparisonAutomatic, Inferred: &open},
		{SitePC: observed.SitePC, Status: analysis.ComparisonAutomatic, Inferred: &observed},
		{SitePC: misclassifiedProfile.SitePC, Status: analysis.ComparisonAutomatic, Inferred: &misclassifiedProfile},
	}}
	selected, rejected := SelectStaticProvenAutomaticDispatchFacts(report)
	if len(selected) != 1 || selected[0].SitePC != proven.SitePC || rejected != 3 {
		t.Fatalf("selection = %#v, rejected=%d; want only proven static fact", selected, rejected)
	}
}

func TestSelectStaticProvenRoutineEntryFactsExcludesContinuationAndRetainedRoots(t *testing.T) {
	report := ShadowReport{EntryAblation: ShadowEntryAblationReport{Entries: []ShadowEntryAblationRecord{
		{
			PC: 0x008100, AuthoredMX: analysis.MXState{M: 0, X: 1},
			Status: shadowEntryAblationRecoverable, EntryKindHint: "routine",
			Incoming: []ShadowEntryAblationSource{{
				PC: 0x008000, EntryMX: analysis.MXState{M: 0, X: 1}, Kinds: []string{"direct_jsr"},
			}},
		},
		{
			PC: 0x008200, AuthoredMX: analysis.MXState{M: 0, X: 1},
			Status: shadowEntryAblationRecoverable, EntryKindHint: "internal_continuation",
			DecodedInstructions: 2,
			Incoming: []ShadowEntryAblationSource{{
				PC: 0x008100, EntryMX: analysis.MXState{M: 0, X: 1}, Kinds: []string{"sibling_boundary_edge"},
				Edges: []analysis.EntryEdge{{
					Source: analysis.EntryVariant{PC: 0x008110, EntryMX: analysis.MXState{M: 0, X: 1}},
					Target: analysis.EntryVariant{PC: 0x008200, EntryMX: analysis.MXState{M: 0, X: 1}},
				}},
			}},
		},
		{
			PC: 0x008300, AuthoredMX: analysis.MXState{M: 0, X: 1},
			Status: shadowEntryAblationRetained, EntryKindHint: "routine",
			Incoming: []ShadowEntryAblationSource{{
				PC: 0x008000, EntryMX: analysis.MXState{M: 0, X: 1}, Kinds: []string{"direct_jsl"},
			}},
		},
		{
			PC: 0x008400, AuthoredMX: analysis.MXState{M: 0, X: 1},
			AuthoredHLE: []string{"hle_func:HostRoutine"},
			Status:      shadowEntryAblationRecoverable, EntryKindHint: "routine",
			Incoming: []ShadowEntryAblationSource{{
				PC: 0x008000, EntryMX: analysis.MXState{M: 0, X: 1}, Kinds: []string{"direct_jsr"},
			}},
		},
	}}}
	facts := SelectStaticProvenRoutineEntryFacts(report)
	if len(facts) != 1 || facts[0].PC != 0x008100 || facts[0].EntryMX != (analysis.MXState{M: 0, X: 1}) {
		t.Fatalf("selected entry facts = %+v, want only exact routine $00:8100 M0X1", facts)
	}
	if facts[0].Kind != analysis.EntryRoutine || len(facts[0].Evidence) != 1 ||
		!facts[0].TemplateFree ||
		facts[0].Evidence[0].Source != "static.direct_jsr" ||
		facts[0].Evidence[0].Confidence != analysis.ConfidenceProven {
		t.Fatalf("selected entry fact lacks normalized static proof: %+v", facts[0])
	}
}

func TestSelectStaticProvenContinuationEntryFactsRequiresPlainSiblingOnlyEntry(t *testing.T) {
	report := ShadowReport{EntryAblation: ShadowEntryAblationReport{Entries: []ShadowEntryAblationRecord{
		{
			PC: 0x008200, AuthoredMX: analysis.MXState{M: 0, X: 1},
			Status: shadowEntryAblationRecoverable, EntryKindHint: "internal_continuation",
			DecodedInstructions: 2,
			Incoming: []ShadowEntryAblationSource{{
				PC: 0x008100, EntryMX: analysis.MXState{M: 0, X: 1}, Kinds: []string{"sibling_boundary_edge"},
				Edges: []analysis.EntryEdge{{
					Source: analysis.EntryVariant{PC: 0x008110, EntryMX: analysis.MXState{M: 0, X: 1}},
					Target: analysis.EntryVariant{PC: 0x008200, EntryMX: analysis.MXState{M: 0, X: 1}},
				}},
			}},
		},
		{
			PC: 0x008300, AuthoredMX: analysis.MXState{M: 0, X: 1},
			Status: shadowEntryAblationRecoverable, EntryKindHint: "internal_continuation",
			DecodedInstructions: 2,
			TemplateBlockers:    []string{"hle_func"},
			Incoming: []ShadowEntryAblationSource{{
				PC: 0x008100, EntryMX: analysis.MXState{M: 0, X: 1}, Kinds: []string{"sibling_boundary_edge"},
			}},
		},
		{
			PC: 0x018400, AuthoredMX: analysis.MXState{M: 0, X: 1},
			Status: shadowEntryAblationRecoverable, EntryKindHint: "internal_continuation",
			DecodedInstructions: 2,
			Incoming: []ShadowEntryAblationSource{{
				PC: 0x008100, EntryMX: analysis.MXState{M: 0, X: 1}, Kinds: []string{"sibling_boundary_edge"},
			}},
		},
		{
			PC: 0x008500, AuthoredMX: analysis.MXState{M: 0, X: 1},
			Status: shadowEntryAblationRecoverable, EntryKindHint: "internal_continuation",
			DecodedInstructions: 200,
			Incoming: []ShadowEntryAblationSource{{
				PC: 0x008100, EntryMX: analysis.MXState{M: 0, X: 1}, Kinds: []string{"sibling_boundary_edge"},
				Edges: []analysis.EntryEdge{{
					Source: analysis.EntryVariant{PC: 0x008120, EntryMX: analysis.MXState{M: 0, X: 1}},
					Target: analysis.EntryVariant{PC: 0x008500, EntryMX: analysis.MXState{M: 0, X: 1}},
				}},
			}},
		},
	}}}
	facts := SelectStaticProvenContinuationEntryFacts(report)
	if len(facts) != 2 {
		t.Fatalf("continuation facts = %+v, want two plain same-bank facts independent of region size", facts)
	}
	fact := facts[0]
	if fact.PC != 0x008200 || fact.Kind != analysis.EntryContinuation || !fact.TemplateFree ||
		len(fact.RegionOwners) != 1 || fact.RegionOwners[0] != (analysis.EntryVariant{
		PC: 0x008100, EntryMX: analysis.MXState{M: 0, X: 1},
	}) || len(fact.ResumeEdges) != 1 || fact.ResumeEdges[0].Source.PC != 0x008110 ||
		len(fact.Evidence) != 1 || fact.Evidence[0].Source != "static.sibling_boundary_edge" {
		t.Fatalf("continuation fact = %+v", fact)
	}
	if facts[1].PC != 0x008500 || len(facts[1].ResumeEdges) != 1 || facts[1].ResumeEdges[0].Source.PC != 0x008120 {
		t.Fatalf("large continuation fact was not retained: %+v", facts[1])
	}
}

func TestSelectStaticProvenContinuationEntryFactsAllowsAcyclicMultiOwnerOverlaps(t *testing.T) {
	record := func(pc uint32, owners ...uint32) ShadowEntryAblationRecord {
		result := ShadowEntryAblationRecord{
			PC: pc, AuthoredMX: analysis.MXState{M: 1, X: 1},
			Status: shadowEntryAblationRecoverable, EntryKindHint: "internal_continuation",
			DecodedInstructions: 20,
		}
		for _, owner := range owners {
			result.Incoming = append(result.Incoming, ShadowEntryAblationSource{
				PC: owner, EntryMX: analysis.MXState{M: 1, X: 1}, Kinds: []string{"sibling_boundary_edge"},
				Edges: []analysis.EntryEdge{{
					Source: analysis.EntryVariant{PC: owner + 2, EntryMX: analysis.MXState{M: 1, X: 1}},
					Target: analysis.EntryVariant{PC: pc, EntryMX: analysis.MXState{M: 1, X: 1}},
				}},
			})
		}
		return result
	}
	report := ShadowReport{EntryAblation: ShadowEntryAblationReport{Entries: []ShadowEntryAblationRecord{
		record(0x008200, 0x008000, 0x008100), // isolated multi-owner target
		record(0x008500, 0x008300, 0x008400), // acyclic: owner $8300 is another continuation
		record(0x008300, 0x008000),
		record(0x008600, 0x008700, 0x008800), // acyclic: target $8600 owns another continuation
		record(0x008900, 0x008600),
		record(0x008A00, 0x008B00, 0x008C00), // cyclic: target reaches owner $8B00
		record(0x008B00, 0x008A00),
	}}}
	facts := SelectStaticProvenContinuationEntryFacts(report)
	wantedMulti := map[uint32]bool{0x008200: false, 0x008500: false, 0x008600: false}
	for index := range facts {
		if _, wanted := wantedMulti[facts[index].PC]; wanted {
			wantedMulti[facts[index].PC] = true
			if len(facts[index].RegionOwners) != 2 || len(facts[index].ResumeEdges) != 2 {
				t.Fatalf("multi-owner fact = %+v", facts[index])
			}
			for _, edge := range facts[index].ResumeEdges {
				if edge.RegionOwner == nil {
					t.Fatalf("multi-owner resume edge lacks explicit owner: %+v", edge)
				}
			}
		}
		if facts[index].PC == 0x008A00 {
			t.Fatalf("cyclic multi-owner continuation was selected: %+v", facts[index])
		}
	}
	for pc, found := range wantedMulti {
		if !found {
			t.Fatalf("acyclic multi-owner fact $%06X was not selected: %+v", pc, facts)
		}
	}
	multiOwner, acyclicOverlap, cyclicOverlap := classifyStaticProvenContinuationOverlaps(report)
	if multiOwner != 4 || acyclicOverlap != 2 || cyclicOverlap != 1 {
		t.Fatalf("multi-owner/acyclic/cyclic overlap counts = %d/%d/%d, want 4/2/1", multiOwner, acyclicOverlap, cyclicOverlap)
	}
}

func TestAuthoredTransferUsesOpcodeBeforeLegacyReturnMetadata(t *testing.T) {
	returnPC := uint16(0x849b)
	dispatch := config.IndirectDispatch{ReturnPC: &returnPC}
	jmp := &cpu65816.Instruction{Mnemonic: "JMP"}
	if got := authoredTransferForInstruction(jmp, dispatch); got != analysis.TransferTail {
		t.Fatalf("JMP transfer = %s, want tail", got)
	}
	pha := &cpu65816.Instruction{Mnemonic: "PHA"}
	if got := authoredTransferForInstruction(pha, dispatch); got != analysis.TransferCall {
		t.Fatalf("PHA/RTS transfer = %s, want call", got)
	}
	dispatch.Transfer = config.IndirectTransferTail
	if got := authoredTransferForInstruction(pha, dispatch); got != analysis.TransferTail {
		t.Fatalf("explicit PHA transfer = %s, want tail", got)
	}
}

func TestFormatShadowConflictsNamesSitesAndReasons(t *testing.T) {
	authored := analysis.DispatchFact{SitePC: 0x008498, InstructionBytes: "7C 9B 84", Mnemonic: "JMP"}
	report := ShadowReport{Comparisons: []analysis.Comparison{
		{
			SitePC: 0x008498, Status: analysis.ComparisonConflict, Authored: &authored,
			Differences: []string{"return PC differs: authored=$00849B inferred=none"},
		},
	}}
	got := FormatShadowConflicts(report)
	for _, wanted := range []string{"$00:8498", "7C 9B 84 JMP", "return PC differs"} {
		if !strings.Contains(got, wanted) {
			t.Fatalf("conflict summary %q missing %q", got, wanted)
		}
	}
}

func TestAnalyzeAuthoredShadowReproducesWRAMRTSContinuation(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{
		0xA9, 0x1F, 0x80, // LDA #$801F (handler minus one)
		0x8D, 0x45, 0x7C, // STA $7C45
		0xA9, 0x0F, 0x80, // LDA #$800F (continuation minus one)
		0x48,             // PHA
		0xAD, 0x45, 0x7C, // LDA $7C45
		0x48, // PHA
		0x60, // RTS dispatcher at $800E
		0xEA, // padding
		0x60, // continuation at $8010
	})
	image[0x0020] = 0x60 // handler exits to $8010
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func Root 8000 entry_mx:0,0
rts_dispatch 800E 8020
rts_dispatch 8020 8010
`)
	report, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 2})
	if err != nil {
		t.Fatal(err)
	}
	if report.Summary.ExactMatches != 2 || report.Summary.Conflicts != 0 || report.Summary.AuthoredOnly != 0 {
		t.Fatalf("unexpected shadow summary: %+v", report.Summary)
	}
}

func TestShadowSummaryClassifiesOpenOverlappingDispatchAsGarbageOnly(t *testing.T) {
	fact := analysis.DispatchFact{
		SitePC: 0x008001, FunctionEntries: []uint32{0x008100},
		Mnemonic: "JMP", Transfer: analysis.TransferTail,
		TargetEntryKind: analysis.EntryComputed, UnknownFields: []string{"targets"},
		Evidence: []analysis.Evidence{{
			Source: "static.decoder.auto_dispatch", Confidence: analysis.ConfidenceProbable,
		}},
	}
	results := []shadowDecodeResult{{
		facts: []analysis.DispatchFact{fact},
		spans: []shadowDecodedSpan{{PC: 0x008000, FunctionEntry: 0x008200, Length: 3}},
	}}
	facts, _, _, _ := summarizeShadowResults(make(romimage.Image, 0x8000), results)
	if len(facts) != 1 || facts[0].CodeOwnership != analysis.OwnershipGarbageOnly {
		t.Fatalf("facts = %+v, want garbage-only ownership", facts)
	}
	comparisons, summary := analysis.CompareDispatchFacts(nil, facts)
	if summary.Automatic != 0 || summary.GarbageOnly != 1 || comparisons[0].Status != analysis.ComparisonGarbageOnly {
		t.Fatalf("summary=%+v comparisons=%+v", summary, comparisons)
	}
}

func TestShadowDirectCallDemandPreservesExactLiveMX(t *testing.T) {
	key := decoder.DecodeKey{PC: 0x008000, M: 0, X: 1}
	instruction := &cpu65816.Instruction{
		Address: 0x008000, Mnemonic: "JSR", Mode: cpu65816.ABS,
		Operand: 0x8200, M: 0, X: 1,
	}
	graph := &decoder.Graph{Instructions: map[decoder.DecodeKey]*decoder.DecodedInstruction{
		key: {Key: key, Instruction: instruction},
	}}
	demands := discoverShadowDemands(0, graph, nil)
	want := decoder.Variant{Address: 0x008200, M: 0, X: 1}
	if len(demands) != 1 {
		t.Fatalf("direct call demands = %+v, want one exact M/X variant", demands)
	}
	if _, found := demands[want]; !found {
		t.Fatalf("direct call demands = %+v, missing %+v", demands, want)
	}
}

func TestRecoverSelfDelimitedWordTable(t *testing.T) {
	image := make(romimage.Image, 0x8000)
	// Outer pointers at $8100 end exactly at their earliest referenced list.
	image[0x0100], image[0x0101] = 0x04, 0x81
	image[0x0102], image[0x0103] = 0x08, 0x81
	pointers, ok := recoverSelfDelimitedWordTable(image, 0, 0x8100, false)
	if !ok || len(pointers) != 2 || pointers[0] != 0x008104 || pointers[1] != 0x008108 {
		t.Fatalf("outer table = %v, %t", pointers, ok)
	}
	// The packed inner list stores handler-minus-one and an $FFFF terminator;
	// its first handler at $8108 proves the two-word bound.
	image[0x0104], image[0x0105] = 0x07, 0x81
	image[0x0106], image[0x0107] = 0xFF, 0xFF
	targets, ok := recoverSelfDelimitedWordTable(image, 0, 0x8104, true)
	if !ok || len(targets) != 2 || targets[0] != 0x008108 || targets[1] != 0 {
		t.Fatalf("inner table = %v, %t", targets, ok)
	}
}
