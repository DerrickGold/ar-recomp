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
