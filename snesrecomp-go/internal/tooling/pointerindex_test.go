package tooling

import (
	"bytes"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestPointerIndexLocalGuards(t *testing.T) {
	step := func(op, mode string, operand uint32) ShadowPointerPathStep {
		return ShadowPointerPathStep{Mnemonic: op, Mode: mode, Operand: operand}
	}
	branch := func(op string, taken bool) ShadowPointerPathStep {
		return ShadowPointerPathStep{Mnemonic: op, BranchTaken: &taken}
	}
	for _, test := range []struct {
		name  string
		value uint16
		path  []ShadowPointerPathStep
		want  string
	}{
		{"zero skipped", 0, []ShadowPointerPathStep{branch("BEQ", false), step("TAY", "imp", 0)}, "fails_local_guards"},
		{"nonzero transferred", 2, []ShadowPointerPathStep{branch("BEQ", false), step("TAY", "imp", 0)}, "passes_local_guards"},
		{"data word skipped", 0x1234, []ShadowPointerPathStep{branch("BPL", false)}, "fails_local_guards"},
		{"handler tag", 0x8500, []ShadowPointerPathStep{branch("BPL", false)}, "passes_local_guards"},
		{"terminator skipped", 0xffff, []ShadowPointerPathStep{branch("BPL", false), step("CMP", "imm", 0xffff), branch("BEQ", false)}, "fails_local_guards"},
		{"nonterminator", 0x8500, []ShadowPointerPathStep{branch("BPL", false), step("CMP", "imm", 0xffff), branch("BEQ", false)}, "passes_local_guards"},
		{"bound accepts", 3, []ShadowPointerPathStep{step("TAX", "imp", 0), step("CPX", "imm", 4), branch("BCC", true)}, "passes_local_guards"},
		{"bound rejects", 4, []ShadowPointerPathStep{step("TAX", "imp", 0), step("CPX", "imm", 4), branch("BCC", true)}, "fails_local_guards"},
		{"unrelated loop bound", 3, []ShadowPointerPathStep{step("CPX", "imm", 0x50), branch("BCC", true)}, "unknown_local_guard"},
		{"memory BIT clobbers sign", 0x8500, []ShadowPointerPathStep{step("BIT", "dp", 0x42), branch("BPL", false)}, "unknown_local_guard"},
		{"immediate BIT preserves sign", 0x8500, []ShadowPointerPathStep{step("BIT", "imm", 1), branch("BPL", false)}, "passes_local_guards"},
		{"mask comparison", 3, []ShadowPointerPathStep{step("BIT", "imm", 1), branch("BEQ", true)}, "fails_local_guards"},
		{"unknown carry", 3, []ShadowPointerPathStep{branch("BCS", true)}, "unknown_local_guard"},
		{"known carry", 3, []ShadowPointerPathStep{step("SEC", "imp", 0), branch("BCS", true)}, "passes_local_guards"},
		{"known overflow", 3, []ShadowPointerPathStep{step("CLV", "imp", 0), branch("BVC", true)}, "passes_local_guards"},
		{"unknown direction", 0, []ShadowPointerPathStep{step("BEQ", "rel", 0)}, "unknown_local_guard"},
		{"clobbered register", 1, []ShadowPointerPathStep{step("LDA", "dp", 0x20), branch("BEQ", false)}, "unknown_local_guard"},
		{"replacement literal", 1, []ShadowPointerPathStep{step("LDA", "imm", 0), branch("BEQ", true)}, "passes_local_guards"},
		{"unmodeled arithmetic", 1, []ShadowPointerPathStep{step("ADC", "imm", 1)}, "unknown_local_guard"},
	} {
		t.Run(test.name, func(t *testing.T) {
			if got := shadowPointerPathStatus(test.path, "A", test.value); got != test.want {
				t.Fatalf("status = %s, want %s", got, test.want)
			}
		})
	}
	truncated := []ShadowPointerPathStep{{Mnemonic: "TAX", MX: analysis.MXState{X: 1}}, step("CPX", "imm", 0), branch("BEQ", true)}
	if got := shadowPointerPathStatus(truncated, "A", 0); got != "unknown_local_guard" {
		t.Fatal("truncated transfer manufactured word proof:", got)
	}
}

func TestPointerIndexWriterInventoryAndSampling(t *testing.T) {
	root := t.TempDir()
	image := make(romimage.Image, 0x10000)
	// DB=$80; LDY $2000; LDA $0000,Y; BPL data; CMP #$FFFF;
	// BEQ terminator; STA $40; JSR trampoline. Base $0000 is not ROM.
	copy(image[0x100:], []byte{0xf4, 0x80, 0x80, 0xab, 0xac, 0, 0x20,
		0xb9, 0, 0, 0x10, 0x0a, 0xc9, 0xff, 0xff, 0xf0, 5, 0x85, 0x40, 0x20, 0, 0x82, 0x60})
	copy(image[0x200:], []byte{0x6c, 0x40, 0})
	// Writers live in a different program bank. Aliasing is not proven.
	copy(image[0x8100:], []byte{0xa9, 0, 0x90, 0x8d, 0, 0x20, 0x60})
	copy(image[0x8120:], []byte{0xa9, 2, 0x90, 0x8d, 0, 0x20, 0x60})
	copy(image[0x8140:], []byte{0xa9, 4, 0x90, 0x8d, 0, 0x20, 0x60})
	copy(image[0x8160:], []byte{0xe2, 0x20, 0xa9, 6, 0x8d, 0, 0x20, 0x60}) // partial writer
	copy(image[0x8180:], []byte{0xee, 0, 0x20, 0x60})                      // RMW
	copy(image[0x81a0:], []byte{0xad, 0, 0x30, 0x8d, 0, 0x20, 0x60})       // memory writer
	copy(image[0x1000:], []byte{0, 0x85, 0x34, 0x12, 0xff, 0xff})
	image[0x500] = 0x60
	options := ShadowAnalysisOptions{ROMPath: filepath.Join(root, "fixture.sfc"), CFGDir: filepath.Join(root, "recomp"), Jobs: 1}
	if err := os.WriteFile(options.ROMPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, filepath.Join(options.CFGDir, "bank00.cfg"), "bank = 00\nfunc Caller 8100 entry_mx:0,0\nfunc Handler 8500 entry_mx:0,0\nhle_func 8500 Replacement\nhle_dispatch 8200 HostDispatcher\n")
	writeTestFile(t, filepath.Join(options.CFGDir, "bank01.cfg"), "bank = 01\nfunc W0 8100 entry_mx:0,0\nfunc W1 8120 entry_mx:0,0\nfunc W2 8140 entry_mx:0,0\nfunc ByteWriter 8160 entry_mx:0,0\nfunc RMW 8180 entry_mx:0,0\nfunc MemoryWriter 81A0 entry_mx:0,0\n")
	report, err := AnalyzeAuthoredShadow(options)
	if err != nil {
		t.Fatal(err)
	}
	site := inventorySite(t, report, 0x8200)
	if len(site.PointerProducers) != 1 || site.TargetSetStatus != "unproven" || len(site.StaticTargets) != 0 || site.Routing != "hle" {
		t.Fatalf("site = %+v", site)
	}
	p := site.PointerProducers[0]
	e := p.IndexEvidence
	if e == nil || e.Field == nil || e.Field.Mode != "abs" || e.Field.Operand != 0x2000 || len(e.Writers) != 6 || len(e.Values) != 3 || len(e.Reads) != 3 {
		t.Fatalf("index = %+v", e)
	}
	if e.Writers[0].StorePC != 0x18103 || e.Writers[5].SourcePC != 0x181a0 {
		t.Fatalf("cross-bank writer provenance lost: %+v", e.Writers)
	}
	if len(p.LoadPath) != 3 || p.LoadPath[0].BranchTaken == nil || *p.LoadPath[0].BranchTaken || p.LoadPath[2].BranchTaken == nil || *p.LoadPath[2].BranchTaken {
		t.Fatalf("path = %+v", p.LoadPath)
	}
	for i, sample := range e.Reads {
		if i == 0 {
			if sample.Status != "word_passes_local_guards" || sample.ReadPC != 0x809000 || sample.TargetPC == nil || *sample.TargetPC != 0x8500 || !sample.AuthoredEntry || !sample.TargetROM {
				t.Fatalf("sample = %+v", sample)
			}
		} else if sample.Status != "word_fails_local_guards" || sample.TargetPC != nil {
			t.Fatalf("data/terminator became handler: %+v", sample)
		}
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
		t.Fatalf("samples affect proof: %v", err)
	}
	if report.EntryAblation.Summary.AuthoredHLEObligations != 1 {
		t.Fatal("HLE lost")
	}
	options.Jobs = 8
	second, err := AnalyzeAuthoredShadow(options)
	if err != nil || !reflect.DeepEqual(report, second) {
		t.Fatalf("non-deterministic report: %v", err)
	}
	var text bytes.Buffer
	if err := WriteShadowReport(&text, report, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"index-source (report-only)", "conditional-target=$00:8500", "word_fails_local_guards", "index_writer_alias_and_reaching_definition"} {
		if !strings.Contains(text.String(), want) {
			t.Fatalf("missing %q", want)
		}
	}
}

func TestPointerIndexSamplingBoundsAndUnknownBanks(t *testing.T) {
	image := make(romimage.Image, 0x8000)
	image[0x1000], image[0x1001] = 0, 0x85
	bank := func(value uint16) *uint16 { return &value }
	for _, test := range []struct {
		name        string
		bank        *uint16
		base, index uint16
		want        string
	}{
		{"effective ROM", bank(0x80), 0, 0x9000, "word_passes_local_guards"},
		{"unknown bank", nil, 0, 0x9000, ""},
		{"WRAM", bank(0x7e), 0, 0x9000, "not_rom_mapped"},
		{"low unmapped", bank(0), 0, 0x20, "not_rom_mapped"},
		{"word crosses bank", bank(0), 0, 0xffff, "bank_boundary_not_sampled"},
		{"index crosses bank", bank(0), 0x9000, 0x9000, "bank_boundary_not_sampled"},
	} {
		t.Run(test.name, func(t *testing.T) {
			p := ShadowPointerProducer{TrampolinePC: 0x18200, TableOffset: test.base, LoadRegister: "A", IndexEvidence: &ShadowPointerIndexEvidence{OriginRegister: "Y", Values: []ShadowPointerIndexValue{{Value: test.index}}}}
			p.DataBank.UnknownPaths = true // A local bank candidate never closes unknown paths.
			if test.bank != nil {
				p.DataBank.Constants = []ShadowRegisterConstant{{Value: *test.bank}}
			}
			site := &ShadowDispatchSite{PointerProducers: []ShadowPointerProducer{p}}
			attachShadowPointerIndexEvidence(image, nil, nil, map[uint32]*ShadowDispatchSite{0x18200: site})
			e := site.PointerProducers[0].IndexEvidence
			if test.want == "" {
				if len(e.Reads) != 0 {
					t.Fatal(e.Reads)
				}
				return
			}
			if len(e.Reads) != 1 || e.Reads[0].Status != test.want {
				t.Fatalf("samples = %+v", e.Reads)
			}
			if e.Reads[0].TargetPC != nil && *e.Reads[0].TargetPC != 0x18500 {
				t.Fatal("wrong target program bank")
			}
			if !site.PointerProducers[0].DataBank.UnknownPaths {
				t.Fatal("bank candidate became all-path proof")
			}
		})
	}
	p := ShadowPointerProducer{TableOffset: 0x9000, LoadRegister: "A", DataBank: ShadowRegisterEvidence{Constants: []ShadowRegisterConstant{{Value: 0x80}, {Value: 0}, {Value: 0x80}}}, IndexEvidence: &ShadowPointerIndexEvidence{OriginRegister: "Y"}}
	for i := range shadowPointerSampleLimit {
		p.IndexEvidence.Values = append(p.IndexEvidence.Values, ShadowPointerIndexValue{Value: uint16(i * 2)})
	}
	site := &ShadowDispatchSite{PointerProducers: []ShadowPointerProducer{p}}
	attachShadowPointerIndexEvidence(image, nil, nil, map[uint32]*ShadowDispatchSite{0: site})
	e := site.PointerProducers[0].IndexEvidence
	if len(e.Reads) != shadowPointerSampleLimit || !e.Truncated || e.Reads[0].DataBank != 0 || e.Reads[1].DataBank != 0x80 || e.Reads[2].IndexValue != 2 {
		t.Fatalf("uncapped or unsorted samples: %+v", e)
	}
}

func TestPointerIndexOriginRequiresWholeWord(t *testing.T) {
	for _, test := range []struct {
		name     string
		code     []byte
		evidence bool
	}{
		{"literal index", []byte{0xa0, 0, 0x90, 0xb9, 0, 0}, true},
		{"scalar dp", []byte{0xa4, 0x20, 0xb9, 0, 0}, true},
		{"zero guard and transfer", []byte{0xad, 0, 0x20, 0xf0, 9, 0xa8, 0xb9, 0, 0}, true},
		{"byte accumulator origin", []byte{0xe2, 0x20, 0xa5, 0x20, 0xa8, 0xc2, 0x20, 0xb9, 0, 0}, false},
		{"byte index origin", []byte{0xe2, 0x10, 0xa4, 0x20, 0xb9, 0, 0}, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			_, _, findings := pointerProducerFixture(t, pointerProducerCode(test.code, []byte{0x85, 0x40, 0x20, 0, 0x82, 0x60}), 0x40, nil)
			if len(findings) != 1 || (findings[0].IndexEvidence != nil) != test.evidence {
				t.Fatalf("findings = %+v", findings)
			}
			if test.name == "zero guard and transfer" {
				e := findings[0].IndexEvidence
				if len(e.Path) != 2 || shadowPointerPathStatus(e.Path, e.OriginRegister, 0) != "fails_local_guards" || shadowPointerPathStatus(e.Path, e.OriginRegister, 2) != "passes_local_guards" {
					t.Fatalf("path = %+v", e)
				}
			}
		})
	}
}

func TestPointerIndexUnknownGuardsNeverCreateTargets(t *testing.T) {
	image := make(romimage.Image, 0x8000)
	image[0x1000], image[0x1001] = 0, 0x85
	taken := true
	unknown := []ShadowPointerPathStep{{Mnemonic: "BCS", BranchTaken: &taken}}
	for _, onIndex := range []bool{false, true} {
		p := ShadowPointerProducer{TableOffset: 0x9000, LoadRegister: "A",
			DataBank:      ShadowRegisterEvidence{Constants: []ShadowRegisterConstant{{Value: 0}}},
			IndexEvidence: &ShadowPointerIndexEvidence{OriginRegister: "Y", Values: []ShadowPointerIndexValue{{Value: 0}}}}
		want := "word_unknown_local_guard"
		if onIndex {
			p.IndexEvidence.Path = unknown
			want = "index_unknown_local_guard"
		} else {
			p.LoadPath = unknown
		}
		site := &ShadowDispatchSite{PointerProducers: []ShadowPointerProducer{p}}
		attachShadowPointerIndexEvidence(image, nil, nil, map[uint32]*ShadowDispatchSite{0: site})
		e := site.PointerProducers[0].IndexEvidence
		if len(e.Reads) != 1 || e.Reads[0].Status != want || e.Reads[0].TargetPC != nil {
			t.Fatalf("unknown guard became target: %+v", e.Reads)
		}
	}
}
