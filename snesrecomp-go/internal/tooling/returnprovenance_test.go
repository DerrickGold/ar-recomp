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
)

func TestReturnValueContracts(t *testing.T) {
	for _, test := range []struct {
		name    string
		code    []byte
		m, x    uint8
		status  string
		addends []uint16
	}{
		{"plain RTS", []byte{0x60}, 0, 0, "conditional_incoming_PC_preserved", []uint16{0}},
		{"plain RTL", []byte{0x6b}, 0, 0, "conditional_incoming_PC_preserved", []uint16{0}},
		{"pop and restore", []byte{0x68, 0x48, 0x60}, 0, 0, "conditional_incoming_PC_preserved", []uint16{0}},
		{"PLX INX PHX", []byte{0xfa, 0xe8, 0xe8, 0xda, 0x60}, 0, 0, "conditional_constant_adjustment", []uint16{2}},
		{"PLY DEY PHY wraps", []byte{0x7a, 0x88, 0x5a, 0x60}, 0, 0, "conditional_constant_adjustment", []uint16{65535}},
		{"binary ADC", []byte{0x68, 0xd8, 0x18, 0x69, 3, 0, 0x48, 0x60}, 0, 0, "conditional_constant_adjustment", []uint16{3}},
		{"binary SBC", []byte{0x68, 0xd8, 0x38, 0xe9, 3, 0, 0x48, 0x60}, 0, 0, "conditional_constant_adjustment", []uint16{65533}},
		{"REP clears decimal and carry", []byte{0x68, 0xc2, 9, 0x69, 3, 0, 0x48, 0x60}, 0, 0, "conditional_constant_adjustment", []uint16{3}},
		{"saved carry decimal", []byte{0xd8, 0x18, 0x08, 0xf8, 0x38, 0x28, 0x68, 0x69, 3, 0, 0x48, 0x60}, 0, 0, "conditional_constant_adjustment", []uint16{3}},
		{"stack-relative slot", []byte{0xa3, 1, 0x1a, 0x83, 1, 0x60}, 0, 0, "conditional_constant_adjustment", []uint16{1}},
		{"slot below local save", []byte{0xda, 0xa3, 3, 0x1a, 0x83, 3, 0xfa, 0x60}, 0, 0, "conditional_constant_adjustment", []uint16{1}},
		{"status below local save", []byte{0x08, 0xc2, 0x20, 0xa3, 2, 0x1a, 0x83, 2, 0x28, 0x60}, 1, 0, "conditional_constant_adjustment", []uint16{1}},
		{"PHP protects width", []byte{0x08, 0xe2, 0x30, 0xda, 0xfa, 0x28, 0x60}, 0, 0, "conditional_incoming_PC_preserved", []uint16{0}},
		{"JSL bank preserved", []byte{0x68, 0x1a, 0x48, 0x6b}, 0, 0, "conditional_constant_adjustment", []uint16{1}},
		{"PHX PLX reroutes value", []byte{0x68, 0xaa, 0xda, 0xa2, 0, 0, 0xfa, 0xe8, 0x8a, 0x48, 0x60}, 0, 0, "conditional_constant_adjustment", []uint16{1}},
		{"eight-bit byte roundtrip", []byte{0x68, 0x48, 0x60}, 1, 1, "conditional_incoming_PC_preserved", []uint16{0}},
		{"TAX transfers hidden B when X is wide", []byte{0xfa, 0x8a, 0xe2, 0x20, 0xaa, 0xc2, 0x20, 0xda, 0x60}, 0, 0, "conditional_incoming_PC_preserved", []uint16{0}},
		{"two paths same value", []byte{0xfa, 0xd0, 3, 0xe8, 0x80, 1, 0xe8, 0xda, 0x60}, 0, 0, "conditional_constant_adjustment", []uint16{1}},
		{"two paths different values", []byte{0xfa, 0xd0, 1, 0xe8, 0xda, 0x60}, 0, 0, "path_dependent_adjustment", []uint16{0, 1}},
		{"same-stack loop is not termination proof", []byte{0xd0, 0xfe, 0x60}, 0, 0, "conditional_incoming_PC_preserved", []uint16{0}},
	} {
		t.Run(test.name, func(t *testing.T) {
			g := returnAuditFixture(t, test.code, test.m, test.x)
			before := shadowStoredJSONKey(g.Order) + shadowStoredJSONKey(g.Instructions[g.Entry])
			exit := decoder.AnalyzeExitMX(g, nil)
			r := auditShadowReturnValues(g, nil)
			if r.Status != test.status || !reflect.DeepEqual(r.Adjustments, test.addends) || len(r.Blockers) != 0 {
				t.Fatalf("%+v", r)
			}
			if exit != decoder.AnalyzeExitMX(g, nil) || before != shadowStoredJSONKey(g.Order)+shadowStoredJSONKey(g.Instructions[g.Entry]) {
				t.Fatal("graph or exit behavior changed")
			}
		})
	}
}

func TestReturnValueRejectsUnprovenContracts(t *testing.T) {
	for _, test := range []struct {
		name   string
		code   []byte
		reason string
	}{
		{"entry decimal unknown", []byte{0x68, 0x18, 0x69, 3, 0, 0x48, 0x60}, "return_arithmetic_requires_known_binary_mode_and_carry"},
		{"decimal set", []byte{0x68, 0xf8, 0x18, 0x69, 3, 0, 0x48, 0x60}, "return_arithmetic_requires_known_binary_mode_and_carry"},
		{"carry unknown", []byte{0x68, 0xd8, 0x69, 3, 0, 0x48, 0x60}, "return_arithmetic_requires_known_binary_mode_and_carry"},
		{"compare clobbers carry", []byte{0x68, 0xd8, 0x18, 0xc9, 0, 0, 0x69, 3, 0, 0x48, 0x60}, "return_arithmetic_requires_known_binary_mode_and_carry"},
		{"PLP restores unknown decimal", []byte{0x08, 0xd8, 0x18, 0x28, 0x68, 0x69, 3, 0, 0x48, 0x60}, "return_arithmetic_requires_known_binary_mode_and_carry"},
		{"constant replaces return", []byte{0x68, 0xa9, 0, 0x81, 0x48, 0x60}, "return_PC_not_incoming_PC_plus_constant"},
		{"memory replaces return", []byte{0x68, 0xbd, 0x1e, 0, 0x48, 0x60}, "return_PC_not_incoming_PC_plus_constant"},
		{"partial byte overwrite", []byte{0xe2, 0x20, 0xa9, 0, 0x83, 1, 0xc2, 0x20, 0x60}, "return_PC_not_incoming_PC_plus_constant"},
		{"narrowing X loses high byte", []byte{0xfa, 0xe2, 0x10, 0xc2, 0x10, 0xda, 0x60}, "return_PC_not_incoming_PC_plus_constant"},
		{"address pulled at wrong width", []byte{0xe2, 0x20, 0x68, 0xc2, 0x20, 0x48, 0x60}, "return_not_at_entry_frame"},
		{"wrong local save slot", []byte{0xda, 0xa3, 1, 0x1a, 0x83, 3, 0xfa, 0x60}, "return_PC_not_incoming_PC_plus_constant"},
		{"newly pushed target", []byte{0xf4, 0, 0x81, 0x60}, "return_not_at_entry_frame"},
		{"bank overwritten", []byte{0xe2, 0x20, 0xa9, 0, 0x83, 3, 0x6b}, "return_bank_not_preserved"},
		{"unknown callee", []byte{0x20, 0, 0x81, 0x60}, "callee_return_value_and_stack_contract"},
		{"callee after arithmetic", []byte{0xfa, 0xe8, 0xda, 0x20, 0, 0x81, 0x60}, "callee_return_value_and_stack_contract"},
		{"DP aliases stack", []byte{0x68, 0x85, 0x10, 0x48, 0x60}, "memory_write_may_alias_return_frame"},
		{"unknown store aliases stack", []byte{0x9d, 0, 0, 0x60}, "memory_write_may_alias_return_frame"},
		{"block move", []byte{0x54, 0, 0, 0x60}, "memory_write_may_alias_return_frame"},
		{"unknown status", []byte{0x28, 0x60}, "PLP_value_not_known_saved_status"},
		{"saved status overwritten", []byte{0x08, 0xa9, 0, 0, 0x83, 1, 0x28, 0x60}, "PLP_value_not_known_saved_status"},
		{"stack reset", []byte{0x1b, 0x60}, "unsupported_stack_or_control_effect"},
		{"emulation transition", []byte{0xfb, 0x60}, "unsupported_stack_or_control_effect"},
		{"mixed return kinds", []byte{0xd0, 1, 0x60, 0x6b}, "mixed_RTS_RTL_entry_frame_contract"},
		{"unproven branch poisons whole contract", []byte{0xd0, 3, 0x20, 0, 0x81, 0x60}, "callee_return_value_and_stack_contract"},
		{"growing stack loop", []byte{0x48, 0xd0, 0xfd, 0x60}, "stack_window_budget"},
		{"changing return loop", []byte{0xfa, 0xe8, 0xd0, 0xfd, 0xda, 0x60}, "state_budget"},
	} {
		t.Run(test.name, func(t *testing.T) {
			r := auditShadowReturnValues(returnAuditFixture(t, test.code, 0, 0), nil)
			if r.Status != "unproven" || !slices.ContainsFunc(r.Blockers, func(b ShadowReturnValueBlocker) bool { return b.Reason == test.reason }) {
				t.Fatalf("%+v", r)
			}
		})
	}
}

func TestReturnValueHLEAndGraphBarriers(t *testing.T) {
	for _, cfg := range []*config.Config{
		{HLEFunctions: map[uint16]string{0x8000: "hook"}},
		{HLEFunctions: map[uint16]string{0x8001: "hook"}},
		{HLEFunctionsIf: map[uint16]config.HLEFunctionIf{0x8000: {}}},
		{HLEDispatch: map[uint16]string{0x8000: "hook"}},
		{HLESPCUpload: []uint16{0x8000}},
	} {
		r := auditShadowReturnValues(returnAuditFixture(t, []byte{0xfa, 0xe8, 0xda, 0x60}, 0, 0), cfg)
		if r.Status != "unproven" || r.Blockers[0].Reason != "HLE_contract_not_inherited_from_ROM" {
			t.Fatal(r)
		}
	}
	g := returnAuditFixture(t, []byte{0xfa, 0xe8, 0xda, 0x60}, 0, 0)
	g.Instructions[g.Entry].Instruction.DispatchKind = "rts_trick"
	if r := auditShadowReturnValues(g, nil); r.Status != "unproven" || r.Blockers[0].Reason != "collapsed_dispatch_contract" {
		t.Fatal(r)
	}
	g = returnAuditFixture(t, []byte{0xea, 0x60}, 0, 0)
	delete(g.Instructions, g.Instructions[g.Entry].Successors[0])
	if r := auditShadowReturnValues(g, nil); r.Status != "unproven" || r.Blockers[0].Reason != "external_or_missing_decoded_edge" {
		t.Fatal(r)
	}
	g = returnAuditFixture(t, []byte{0x08, 0x28, 0x60}, 0, 0)
	key := g.Instructions[g.Entry].Successors[0]
	g.Instructions[key].Successors[0].M = 1
	if r := auditShadowReturnValues(g, nil); r.Status != "unproven" || r.Blockers[0].Reason != "decoded_MX_disagrees_with_saved_status" {
		t.Fatal(r)
	}
	// An unsupported effect is a blocker, not evidence that these bytes are data.
	g = returnAuditFixture(t, []byte{0xea, 0x60}, 0, 0)
	g.Instructions[g.Entry].Instruction = &cpu65816.Instruction{Mnemonic: "FUTURE", Length: 1}
	if r := auditShadowReturnValues(g, nil); r.Status != "unproven" || r.Blockers[0].Reason != "unsupported_instruction_effect" {
		t.Fatal(r)
	}
}

func TestReturnValueBlockerLimitRetainsIncompleteness(t *testing.T) {
	var code []byte
	for range 12 {
		code = append(code, 0xd0, 3, 0x20, 0, 0x90)
	}
	code = append(code, 0x60)
	r := auditShadowReturnValues(returnAuditFixture(t, code, 0, 0), nil)
	if r.Status != "unproven" || len(r.Blockers) != 8 || r.BlockersOmitted != 4 || !reflect.DeepEqual(r.Adjustments, []uint16{0}) {
		t.Fatal(r)
	}
}

func TestReturnValueReportSelectionAndDeterminism(t *testing.T) {
	if collectShadowReturnValues(returnAuditFixture(t, []byte{0x60}, 0, 0), nil) != nil {
		t.Fatal("plain leaf is outside the focused inventory")
	}
	a := collectShadowReturnValues(returnAuditFixture(t, []byte{0xfa, 0xe8, 0xda, 0x60}, 0, 0), nil)
	b := collectShadowReturnValues(returnAuditFixture(t, []byte{0xfa, 0xe8, 0xda, 0x60}, 1, 1), nil)
	results := []shadowDecodeResult{{returnValues: a}, {returnValues: b}, {returnValues: a}}
	r := collectShadowReturnProvenance(results)
	slices.Reverse(results)
	if r.EntryVariants != 2 || !reflect.DeepEqual(r, collectShadowReturnProvenance(results)) {
		t.Fatal(r)
	}
	var out bytes.Buffer
	writeShadowReturnProvenance(&out, r, true)
	if !strings.Contains(out.String(), "[RETURN-VALUE]") || !strings.Contains(out.String(), "not inline-data") {
		t.Fatal(out.String())
	}
	report := ShadowReport{ReturnProvenance: r}
	facts, rejected := SelectStaticProvenDatabaseDispatchFacts(report)
	report.ReturnProvenance = ShadowReturnProvenance{}
	other, otherRejected := SelectStaticProvenDatabaseDispatchFacts(report)
	if !reflect.DeepEqual(facts, other) || rejected != otherRejected {
		t.Fatal("return-value evidence entered production facts")
	}
}

func TestReturnValueShadowIntegration(t *testing.T) {
	root := t.TempDir()
	romPath, cfgDir := filepath.Join(root, "fixture.sfc"), filepath.Join(root, "recomp")
	image := make([]byte, 0x8000)
	// Read the actual stacked PC, increment it, and replace the same word.
	copy(image, []byte{0xa3, 1, 0x1a, 0x83, 1, 0x60})
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	path, authored := filepath.Join(cfgDir, "bank00.cfg"), "bank = 00\nfunc Adjust 8000 entry_mx:0,0\n"
	writeTestFile(t, path, authored)
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
		t.Fatal("worker-dependent shadow report")
	}
	if a.ReturnProvenance.Statuses["conditional_constant_adjustment"] != 1 {
		t.Fatal(a.ReturnProvenance)
	}
	facts, rejected := SelectStaticProvenDatabaseDispatchFacts(a)
	a.ReturnProvenance = ShadowReturnProvenance{}
	other, otherRejected := SelectStaticProvenDatabaseDispatchFacts(a)
	if !reflect.DeepEqual(facts, other) || rejected != otherRejected {
		t.Fatal("report-only adjustment became production evidence")
	}
	content, _ := os.ReadFile(path)
	entries, _ := os.ReadDir(root)
	if string(content) != authored || len(entries) != 2 {
		t.Fatal("analysis modified the project")
	}
}
