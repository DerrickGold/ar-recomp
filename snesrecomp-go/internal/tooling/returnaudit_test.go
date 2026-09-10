package tooling

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"slices"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func returnAuditFixture(t *testing.T, code []byte, m, x uint8) *decoder.Graph {
	t.Helper()
	image := make(romimage.Image, 0x8000)
	copy(image, code)
	g, err := decoder.DecodeFunction(image, 0, 0x8000, m, x, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	return g
}

func TestReturnAuditFrameShapes(t *testing.T) {
	for _, test := range []struct {
		name  string
		code  []byte
		shape string
		depth int16
	}{
		{"plain RTS", []byte{0x60}, "entry_frame_position_only", 0},
		{"plain RTL", []byte{0x6b}, "entry_frame_position_only", 0},
		{"register save restore", []byte{0xda, 0xfa, 0x60}, "entry_frame_position_only", 0},
		{"PHP width bracket", []byte{0x08, 0xe2, 0x30, 0xda, 0xfa, 0x28, 0x60}, "entry_frame_position_only", 0},
		{"PEA RTS", []byte{0xf4, 0x20, 0x80, 0x60}, "locally_pushed_frame", 2},
		{"PEI RTS", []byte{0xd4, 0x10, 0x60}, "locally_pushed_frame", 2},
		{"PER RTS", []byte{0x62, 0x20, 0, 0x60}, "locally_pushed_frame", 2},
		{"PHK PEA RTL", []byte{0x4b, 0xf4, 0x20, 0x80, 0x6b}, "locally_pushed_frame", 3},
		{"PHB RTS", []byte{0x8b, 0x60}, "mixed_local_and_entry_frame", 1},
		{"PHX RTL", []byte{0xda, 0x6b}, "mixed_local_and_entry_frame", 2},
		{"past entry", []byte{0xfa, 0x60}, "past_entry_frame", -2},
		{"rewritten frame equal height", []byte{0x68, 0x18, 0x69, 3, 0, 0x48, 0x60}, "entry_frame_written", 0},
		{"direct return slot write", []byte{0x83, 1, 0x60}, "entry_frame_written", 0},
		{"return slot below local save", []byte{0xda, 0x83, 3, 0xfa, 0x60}, "entry_frame_written", 0},
		{"only local slot written", []byte{0xda, 0x83, 1, 0xfa, 0x60}, "entry_frame_position_only", 0},
		{"RTI", []byte{0x40}, "interrupt_frame_not_normal_call_exit", 0},
		{"TXS", []byte{0x9a, 0x60}, "unknown_stack_position", returnAuditUnknown},
		{"TCS", []byte{0x1b, 0x60}, "unknown_stack_position", returnAuditUnknown},
		{"XCE", []byte{0xfb, 0x60}, "unknown_stack_position", returnAuditUnknown},
	} {
		t.Run(test.name, func(t *testing.T) {
			g := returnAuditFixture(t, test.code, 0, 0)
			// Audit must not rewrite decode metadata, legacy exits, or graph edges.
			before := shadowStoredJSONKey(g.Order) + shadowStoredJSONKey(g.Instructions[g.Entry])
			exitBefore := decoder.AnalyzeExitMX(g, nil)
			r := auditShadowReturns(g, nil)
			if len(r.sites) != 1 {
				t.Fatalf("sites=%+v", r.sites)
			}
			c := r.sites[0].Contexts[0]
			if !slices.Contains(c.Shapes, test.shape) {
				t.Fatalf("context=%+v", c)
			}
			if test.depth == returnAuditUnknown {
				if !c.UnknownDepth {
					t.Fatal("unknown height discarded")
				}
			} else if !reflect.DeepEqual(c.Depths, []int16{test.depth}) {
				t.Fatalf("depths=%v", c.Depths)
			}
			if decoder.AnalyzeExitMX(g, nil) != exitBefore || before != shadowStoredJSONKey(g.Order)+shadowStoredJSONKey(g.Instructions[g.Entry]) {
				t.Fatal("audit mutated graph or legacy exit behavior")
			}
		})
	}
}

func TestReturnAuditPathSetsAndBudgets(t *testing.T) {
	// One branch pushes a word; another reaches exactly the same RTS without it.
	g := returnAuditFixture(t, []byte{0xd0, 3, 0xf4, 0x20, 0x80, 0x60}, 0, 0)
	r := auditShadowReturns(g, nil)
	if len(r.sites) != 1 || !reflect.DeepEqual(r.sites[0].Contexts[0].Depths, []int16{0, 2}) || len(r.sites[0].Contexts[0].Shapes) != 2 {
		t.Fatalf("merged=%+v", r)
	}
	// An accumulating push loop must reach unknown, never clamp to a valid
	// finite depth that would manufacture a normal-return proof.
	g = returnAuditFixture(t, []byte{0x8b, 0xd0, 0xfd, 0x60}, 0, 0)
	r = auditShadowReturns(g, nil)
	if !r.budgetHit || !r.sites[0].Contexts[0].UnknownDepth || !r.sites[0].Contexts[0].Incomplete {
		t.Fatalf("depth budget=%+v", r)
	}
	// A graph can exceed the state budget before reaching a lexical return.
	g = &decoder.Graph{Entry: decoder.DecodeKey{PC: 0x8000}, Instructions: make(map[decoder.DecodeKey]*decoder.DecodedInstruction)}
	for i := 0; i <= returnAuditStateLimit; i++ {
		k := decoder.DecodeKey{PC: 0x8000 + uint32(i)}
		d := &decoder.DecodedInstruction{Key: k, Instruction: &cpu65816.Instruction{Address: k.PC, Opcode: 0xea, Mnemonic: "NOP", Length: 1}, Successors: []decoder.DecodeKey{{PC: k.PC + 1}}}
		if i == returnAuditStateLimit {
			d.Instruction.Opcode = 0x60
			d.Instruction.Mnemonic = "RTS"
			d.Successors = nil
		}
		g.Order = append(g.Order, k)
		g.Instructions[k] = d
	}
	r = auditShadowReturns(g, nil)
	if !r.budgetHit || !r.sites[0].Contexts[0].Incomplete || !slices.Contains(r.sites[0].Contexts[0].Shapes, "not_reached_in_audit") {
		t.Fatalf("state budget=%+v", r)
	}
}

func TestReturnAuditExplicitObligations(t *testing.T) {
	for _, test := range []struct {
		code       []byte
		obligation string
	}{
		{[]byte{0x20, 0, 0x81, 0x60}, "callee_normal_return_and_stack_effects"},
		{[]byte{0x85, 0x10, 0x60}, "memory_writes_may_alias_return_frame"},
		{[]byte{0x54, 0, 0, 0x60}, "memory_writes_may_alias_return_frame"},
	} {
		r := auditShadowReturns(returnAuditFixture(t, test.code, 0, 0), nil)
		if len(r.sites) != 1 || !slices.Contains(r.sites[0].Contexts[0].Obligations, test.obligation) {
			t.Fatalf("obligation lost: %+v", r)
		}
	}
	for _, cfg := range []*config.Config{
		{HLEFunctions: map[uint16]string{0x8000: "hook"}},
		{HLEFunctions: map[uint16]string{0x8001: "hook"}},
		{HLEFunctionsIf: map[uint16]config.HLEFunctionIf{0x8000: {}}},
		{HLEDispatch: map[uint16]string{0x8000: "hook"}},
		{HLESPCUpload: []uint16{0x8000}},
	} {
		r := auditShadowReturns(returnAuditFixture(t, []byte{0xea, 0x60}, 0, 0), cfg)
		c := r.sites[0].Contexts[0]
		if !c.UnknownDepth || !slices.Contains(c.Obligations, "HLE_contract_not_inherited_from_ROM") {
			t.Fatalf("HLE contract inherited: %+v", c)
		}
	}
	g := returnAuditFixture(t, []byte{0x48, 0x60}, 0, 0)
	g.Instructions[g.Entry].Instruction.DispatchKind = "rts_trick"
	r := auditShadowReturns(g, nil)
	if !r.sites[0].Contexts[0].UnknownDepth {
		t.Fatal("collapsed carrier interpreted as ordinary push")
	}
	for _, d := range g.Instructions {
		if d.Instruction.Mnemonic == "RTS" {
			d.Instruction.DispatchKind = "rts_trick"
		}
	}
	r = auditShadowReturns(g, nil)
	if r.sites[0].Contexts[0].LegacyExitCandidate {
		t.Fatal("recognized RTS dispatch counted as legacy normal exit")
	}
}

func TestReturnAuditDeduplicatesSitesWithoutDiscardingMX(t *testing.T) {
	a := auditShadowReturns(returnAuditFixture(t, []byte{0x60}, 0, 0), nil)
	b := auditShadowReturns(returnAuditFixture(t, []byte{0x60}, 1, 1), nil)
	results := []shadowDecodeResult{{returnAudit: a}, {returnAudit: b}, {returnAudit: a}}
	before := shadowStoredJSONKey(a.sites) + shadowStoredJSONKey(b.sites)
	r := collectShadowReturnAudit(results)
	if r.RawReturnContexts != 3 || r.UniqueSourceSites != 1 || r.SourceMXSites != 2 || len(r.Sites[0].Contexts) != 1 || r.ShapeSourceSites["entry_frame_position_only"] != 1 {
		t.Fatalf("counts=%+v", r)
	}
	if before != shadowStoredJSONKey(a.sites)+shadowStoredJSONKey(b.sites) {
		t.Fatal("aggregation mutated source contexts")
	}
	slices.Reverse(results)
	if !reflect.DeepEqual(r, collectShadowReturnAudit(results)) {
		t.Fatal("aggregation depends on worker order")
	}
}

func TestReturnAuditReportOnlyIntegration(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "fixture.sfc")
	cfgDir := filepath.Join(root, "recomp")
	image := make([]byte, 0x8000)
	copy(image, []byte{0x08, 0xf4, 0x20, 0x80, 0x60})
	image[0x20] = 0x60
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(cfgDir, "bank00.cfg")
	text := "bank = 00\nfunc Constructed 8000 entry_mx:0,0\nfunc Leaf 8020 entry_mx:0,0\nhle_func 8020 leaf_hook\n"
	writeTestFile(t, path, text)
	a, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 1})
	if err != nil {
		t.Fatal(err)
	}
	b, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 8})
	if err != nil {
		t.Fatal(err)
	}
	ja, _ := json.Marshal(a)
	jb, _ := json.Marshal(b)
	if !bytes.Equal(ja, jb) {
		t.Fatal("worker-dependent report")
	}
	if a.ReturnAudit.UniqueSourceSites < 2 || a.ReturnAudit.CandidateReviewSites < 1 {
		t.Fatalf("audit=%+v", a.ReturnAudit)
	}
	facts, rejected := SelectStaticProvenDatabaseDispatchFacts(a)
	a.ReturnAudit = ShadowReturnAudit{}
	other, otherRejected := SelectStaticProvenDatabaseDispatchFacts(a)
	if !reflect.DeepEqual(facts, other) || rejected != otherRejected {
		t.Fatal("audit fed production fact selection")
	}
	content, _ := os.ReadFile(path)
	if string(content) != text {
		t.Fatal("authored cfg changed")
	}
	entries, _ := os.ReadDir(root)
	if len(entries) != 2 {
		t.Fatal("audit wrote files")
	}
	var out bytes.Buffer
	writeShadowReturnAudit(&out, b.ReturnAudit, true)
	if !strings.Contains(out.String(), "[RETURN-FRAME]") || !strings.Contains(out.String(), "not runtime failures") {
		t.Fatal(out.String())
	}
}
