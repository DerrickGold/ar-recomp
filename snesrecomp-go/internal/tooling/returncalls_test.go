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

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func returnCallsFixture(t *testing.T, code map[uint32][]byte, options decoder.Options) (*decoder.Graph, map[decoder.Variant]*decoder.Graph, returnCallLoader) {
	t.Helper()
	image := make(romimage.Image, 0x10000)
	for pc, data := range code {
		off, err := romimage.LoROMOffset(byte(pc>>16), uint16(pc))
		if err != nil {
			t.Fatal(err)
		}
		copy(image[off:], data)
	}
	programs := make(map[decoder.Variant]*decoder.Graph)
	for pc := range code {
		g, err := decoder.DecodeFunction(image, byte(pc>>16), uint16(pc), 0, 0, options)
		if err != nil {
			t.Fatal(err)
		}
		programs[decoder.Variant{Address: pc}] = g
	}
	load := func(v decoder.Variant) (*decoder.Graph, *config.Config, string) {
		if g := programs[v]; g != nil {
			return g, nil, ""
		}
		return nil, nil, "callee_exact_variant_not_in_shadow_closure"
	}
	return programs[decoder.Variant{Address: 0x8000}], programs, load
}

func TestReturnCallsCarryPhysicalFramesAndValues(t *testing.T) {
	for _, test := range []struct {
		name           string
		caller, callee []byte
		status         string
		addends        []uint16
	}{
		{"ordinary leaf", []byte{0x08, 0x20, 0, 0x81, 0x28, 0x60}, []byte{0x60}, "conditional_incoming_PC_preserved", []uint16{0}},
		{"balanced PHP SEP PLP", []byte{0x08, 0x20, 0, 0x81, 0x28, 0x60}, []byte{0x08, 0xe2, 0x30, 0xda, 0xfa, 0x28, 0x60}, "conditional_incoming_PC_preserved", []uint16{0}},
		{"return held in A", []byte{0x68, 0x20, 0, 0x81, 0x48, 0x60}, []byte{0xea, 0x60}, "conditional_incoming_PC_preserved", []uint16{0}},
		{"callee saves wide X before narrowing", []byte{0xfa, 0x20, 0, 0x81, 0xda, 0x60}, []byte{0xda, 0x08, 0xe2, 0x30, 0x28, 0xfa, 0x60}, "conditional_incoming_PC_preserved", []uint16{0}},
		{"callee changes A but caller restores", []byte{0x48, 0x20, 0, 0x81, 0x68, 0x60}, []byte{0xa9, 1, 0, 0x60}, "conditional_incoming_PC_preserved", []uint16{0}},
		{"binary carry crosses leaf", []byte{0xd8, 0x18, 0x20, 0, 0x81, 0x68, 0x69, 3, 0, 0x48, 0x60}, []byte{0xea, 0x60}, "conditional_constant_adjustment", []uint16{3}},
		{"callee establishes carry", []byte{0xd8, 0x20, 0, 0x81, 0x68, 0x69, 3, 0, 0x48, 0x60}, []byte{0x18, 0x60}, "conditional_constant_adjustment", []uint16{3}},
		{"two callee return paths", []byte{0x08, 0x20, 0, 0x81, 0x28, 0x60}, []byte{0xd0, 1, 0x60, 0x60}, "conditional_incoming_PC_preserved", []uint16{0}},
	} {
		t.Run(test.name, func(t *testing.T) {
			g, programs, load := returnCallsFixture(t, map[uint32][]byte{0x8000: test.caller, 0x8100: test.callee}, decoder.Options{})
			before := map[decoder.Variant]string{}
			for v, g := range programs {
				before[v] = shadowStoredJSONKey(g.Order) + shadowStoredJSONKey(g.Instructions[g.Entry])
			}
			r := auditShadowReturnCalls(g, nil, load)
			if r.Status != test.status || !reflect.DeepEqual(r.Adjustments, test.addends) || len(r.Blockers) != 0 {
				t.Fatalf("%+v", r)
			}
			if len(r.Calls) != 1 || len(r.Calls[0].MatchedExits) == 0 {
				t.Fatal(r)
			}
			for v, g := range programs {
				if before[v] != shadowStoredJSONKey(g.Order)+shadowStoredJSONKey(g.Instructions[g.Entry]) {
					t.Fatal("mutated graph")
				}
			}
		})
	}
}

func TestReturnCallsRejectIncompleteOrNonlocalContracts(t *testing.T) {
	for _, test := range []struct {
		name           string
		caller, callee []byte
		reason         string
	}{
		{"call itself overwrites pulled frame", []byte{0x68, 0xeb, 0x20, 0, 0x81, 0xe2, 0x20, 0xa3, 0, 0xeb, 0xc2, 0x20, 0x48, 0x60}, []byte{0x60}, "return_PC_not_incoming_PC_plus_constant"},
		{"callee clobbers held return", []byte{0x68, 0x20, 0, 0x81, 0x48, 0x60}, []byte{0xa9, 0, 0x90, 0x60}, "return_PC_not_incoming_PC_plus_constant"},
		{"PHP PLP does not restore X high byte", []byte{0xfa, 0x20, 0, 0x81, 0xda, 0x60}, []byte{0x08, 0xe2, 0x10, 0x28, 0x60}, "return_PC_not_incoming_PC_plus_constant"},
		{"callee writes caller return", []byte{0x20, 0, 0x81, 0x60}, []byte{0xa9, 0, 0, 0x83, 3, 0x60}, "return_PC_not_incoming_PC_plus_constant"},
		{"callee writes caller saved P", []byte{0x08, 0x20, 0, 0x81, 0x28, 0x60}, []byte{0xe2, 0x20, 0xa9, 0, 0x83, 3, 0xc2, 0x20, 0x60}, "PLP_value_not_known_saved_status"},
		{"adjusted callee return", []byte{0x20, 0, 0x81, 0x60}, []byte{0x68, 0x1a, 0x48, 0x60}, "callee_return_PC_adjusted_nonlocal_or_unknown"},
		{"constructed target", []byte{0x20, 0, 0x81, 0x60}, []byte{0xf4, 0, 0x90, 0x60}, "callee_return_not_at_matching_call_frame"},
		{"callee jumps out of frame", []byte{0x20, 0, 0x81, 0x60}, []byte{0xfa, 0x60}, "callee_return_not_at_matching_call_frame"},
		{"wrong return opcode", []byte{0x20, 0, 0x81, 0x60}, []byte{0x6b}, "callee_return_opcode_does_not_match_call_frame"},
		{"ordinary store not disjoint", []byte{0x20, 0, 0x81, 0x60}, []byte{0x85, 0x10, 0x60}, "memory_write_may_alias_return_frame"},
		{"callee sets decimal", []byte{0xd8, 0x18, 0x20, 0, 0x81, 0x68, 0x69, 1, 0, 0x48, 0x60}, []byte{0xf8, 0x60}, "return_arithmetic_requires_known_binary_mode_and_carry"},
		{"callee comparison clobbers carry", []byte{0xd8, 0x18, 0x20, 0, 0x81, 0x68, 0x69, 1, 0, 0x48, 0x60}, []byte{0xc9, 0, 0, 0x60}, "return_arithmetic_requires_known_binary_mode_and_carry"},
		{"callee exit width not decoded by caller", []byte{0x20, 0, 0x81, 0x60}, []byte{0xe2, 0x20, 0x60}, "call_exit_MX_or_continuation_absent_from_caller_decode"},
		{"unknown P from caller frame", []byte{0x20, 0, 0x81, 0x60}, []byte{0x28, 0x60}, "PLP_value_not_known_saved_status"},
		{"recursive base case cannot prove recursive path", []byte{0x20, 0, 0x81, 0x60}, []byte{0xd0, 3, 0x20, 0, 0x81, 0x60}, "abstract_call_depth_budget"},
		{"callee shares query state budget", []byte{0x20, 0, 0x81, 0x60}, []byte{0xa2, 0, 0, 0xe8, 0xd0, 0xfd, 0x60}, "state_budget"},
	} {
		t.Run(test.name, func(t *testing.T) {
			g, _, load := returnCallsFixture(t, map[uint32][]byte{0x8000: test.caller, 0x8100: test.callee}, decoder.Options{})
			r := auditShadowReturnCalls(g, nil, load)
			if r.Status != "unproven" || !slices.ContainsFunc(r.Blockers, func(b ShadowReturnValueBlocker) bool { return b.Reason == test.reason }) {
				t.Fatalf("%+v", r)
			}
		})
	}
}

func TestReturnCallsNestedFramesAndMultipleExitWidths(t *testing.T) {
	g, _, load := returnCallsFixture(t, map[uint32][]byte{0x8000: {0x08, 0x20, 0, 0x81, 0x28, 0x60}, 0x8100: {0x20, 0, 0x82, 0x60}, 0x8200: {0x08, 0xe2, 0x30, 0x28, 0x60}}, decoder.Options{})
	r := auditShadowReturnCalls(g, nil, load)
	if r.Status != "conditional_incoming_PC_preserved" || len(r.Calls) != 2 {
		t.Fatal(r)
	}
	// A nested callee may return normally itself while corrupting its parent's
	// return word. Returning normally is not a no-stack-write summary.
	g, _, load = returnCallsFixture(t, map[uint32][]byte{0x8000: {0x20, 0, 0x81, 0x60}, 0x8100: {0x20, 0, 0x82, 0x60}, 0x8200: {0xa9, 0, 0, 0x83, 3, 0x60}}, decoder.Options{})
	r = auditShadowReturnCalls(g, nil, load)
	if r.Status != "unproven" || r.Blockers[0].Reason != "callee_return_PC_adjusted_nonlocal_or_unknown" {
		t.Fatal(r)
	}
	options := decoder.Options{CalleeExitModes: map[decoder.Variant][]decoder.MX{{Address: 0x8100}: {{M: 0, X: 0}, {M: 1, X: 0}}}}
	g, _, load = returnCallsFixture(t, map[uint32][]byte{0x8000: {0x08, 0x20, 0, 0x81, 0xd0, 1, 0xea, 0x28, 0x60}, 0x8100: {0xd0, 2, 0xe2, 0x20, 0x60}}, options)
	r = auditShadowReturnCalls(g, nil, load)
	if r.Status != "conditional_incoming_PC_preserved" || len(r.Calls[0].MatchedExits) != 2 || r.Calls[0].MatchedExits[0].MX != (analysis.MXState{M: 0, X: 0}) || r.Calls[0].MatchedExits[1].MX != (analysis.MXState{M: 1, X: 0}) {
		t.Fatal(r)
	}
}

func TestReturnCallsLongBankAndWrap(t *testing.T) {
	g, _, load := returnCallsFixture(t, map[uint32][]byte{0x8000: {0x08, 0x22, 0, 0x81, 1, 0x28, 0x60}, 0x18100: {0x6b}}, decoder.Options{})
	if r := auditShadowReturnCalls(g, nil, load); r.Status != "conditional_incoming_PC_preserved" {
		t.Fatal(r)
	}
	g, _, load = returnCallsFixture(t, map[uint32][]byte{0x8000: {0x22, 0, 0x81, 1, 0x60}, 0x18100: {0xe2, 0x20, 0xa9, 2, 0x83, 3, 0xc2, 0x20, 0x6b}}, decoder.Options{})
	if r := auditShadowReturnCalls(g, nil, load); r.Status != "unproven" || r.Blockers[0].Reason != "callee_return_bank_changed_or_unknown" {
		t.Fatal(r)
	}
	for _, long := range []bool{false, true} {
		pc, length, op, mode := uint32(0x01fffd), uint8(3), byte(0x20), cpu65816.ABS
		ret := byte(0x60)
		if long {
			pc, length, op, mode, ret = 0x01fffc, 4, 0x22, cpu65816.LONG, 0x6b
		}
		entry, next := decoder.DecodeKey{PC: pc}, decoder.DecodeKey{PC: 0x010000}
		root := &decoder.Graph{Entry: entry, Order: []decoder.DecodeKey{entry, next}, Instructions: map[decoder.DecodeKey]*decoder.DecodedInstruction{
			entry: {Key: entry, Instruction: &cpu65816.Instruction{Opcode: op, Mnemonic: map[bool]string{false: "JSR", true: "JSL"}[long], Mode: mode, Operand: 0x018100, Length: length}, Successors: []decoder.DecodeKey{next}},
			next:  {Key: next, Instruction: &cpu65816.Instruction{Opcode: 0x60, Mnemonic: "RTS", Length: 1}},
		}}
		_, programs, loader := returnCallsFixture(t, map[uint32][]byte{0x18100: {ret}}, decoder.Options{})
		programs[decoder.Variant{Address: pc}] = root
		r := auditShadowReturnCalls(root, nil, loader)
		if r.Status != "conditional_incoming_PC_preserved" || r.Calls[0].ContinuationPC != 0x010000 {
			t.Fatal(r)
		}
	}
}

func TestReturnCallsExactVariantsAndHLEBarriers(t *testing.T) {
	g, programs, load := returnCallsFixture(t, map[uint32][]byte{0x8000: {0x08, 0x20, 0, 0x81, 0x28, 0x60}, 0x8100: {0x60}}, decoder.Options{})
	for _, cfg := range []*config.Config{
		{HLEFunctions: map[uint16]string{0x8100: "hook"}},
		{HLEFunctionsIf: map[uint16]config.HLEFunctionIf{0x8100: {}}},
		{HLEDispatch: map[uint16]string{0x8100: "hook"}},
		{HLESPCUpload: []uint16{0x8100}},
	} {
		r := auditShadowReturnCalls(g, cfg, func(v decoder.Variant) (*decoder.Graph, *config.Config, string) {
			p, _, reason := load(v)
			return p, cfg, reason
		})
		if r.Status != "unproven" || r.Blockers[0].Reason != "HLE_contract_not_inherited_from_ROM" {
			t.Fatal(r)
		}
	}
	leaf := programs[decoder.Variant{Address: 0x8100}]
	delete(programs, decoder.Variant{Address: 0x8100})
	programs[decoder.Variant{Address: 0x8100, M: 1}] = leaf
	r := auditShadowReturnCalls(g, nil, load)
	if r.Status != "unproven" || r.Blockers[0].Reason != "callee_exact_variant_not_in_shadow_closure" {
		t.Fatal(r)
	}
}

func TestReturnCallsShadowReportOnlyIntegration(t *testing.T) {
	root := t.TempDir()
	romPath, cfgDir := filepath.Join(root, "fixture.sfc"), filepath.Join(root, "recomp")
	image := make([]byte, 0x8000)
	copy(image, []byte{0x08, 0x20, 0, 0x81, 0x28, 0x60})
	image[0x100] = 0x60
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	path, authored := filepath.Join(cfgDir, "bank00.cfg"), "bank = 00\nfunc Caller 8000 entry_mx:0,0\nfunc Leaf 8100 entry_mx:0,0\n"
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
		t.Fatal("worker-dependent report")
	}
	if a.ReturnCalls.Statuses["conditional_incoming_PC_preserved"] != 1 || a.ReturnProvenance.Statuses["unproven"] != 1 {
		t.Fatalf("calls=%+v local=%+v", a.ReturnCalls, a.ReturnProvenance)
	}
	facts, rejected := SelectStaticProvenDatabaseDispatchFacts(a)
	a.ReturnCalls = ShadowReturnCalls{}
	other, otherRejected := SelectStaticProvenDatabaseDispatchFacts(a)
	if !reflect.DeepEqual(facts, other) || rejected != otherRejected {
		t.Fatal("call contracts entered production facts")
	}
	content, _ := os.ReadFile(path)
	entries, _ := os.ReadDir(root)
	if string(content) != authored || len(entries) != 2 {
		t.Fatal("analysis modified project")
	}
	var out bytes.Buffer
	writeShadowReturnCalls(&out, b.ReturnCalls, true)
	if !strings.Contains(out.String(), "[RETURN-CALL]") || !strings.Contains(out.String(), "not new roots") {
		t.Fatal(out.String())
	}
}

func TestReturnCallsDoNotPromoteContextToUniversalCalleeSummary(t *testing.T) {
	g, programs, load := returnCallsFixture(t, map[uint32][]byte{
		0x8000: {0x20, 0, 0x81, 0x60},
		0x8200: {0x20, 0, 0x81, 0x60},
		0x8100: {0x68, 0xa9, 2, 0x80, 0x48, 0x60},
	}, decoder.Options{})
	first := auditShadowReturnCalls(g, nil, load)
	second := auditShadowReturnCalls(programs[decoder.Variant{Address: 0x8200}], nil, load)
	if first.Status != "conditional_incoming_PC_preserved" || second.Status != "unproven" || second.Blockers[0].Reason != "callee_return_PC_adjusted_nonlocal_or_unknown" {
		t.Fatalf("first=%+v second=%+v", first, second)
	}
}

func TestReturnCallsLoaderKeepsOwnershipAndExactClosure(t *testing.T) {
	image := make(romimage.Image, 0x8000)
	copy(image, []byte{0x08, 0x20, 0, 0x81, 0x28, 0x60})
	image[0x100] = 0x60
	root := shadowDecodeResult{entry: decoder.Variant{Address: 0x8000}, bankRecipe: &shadowDBRecipe{}, returnValues: &ShadowReturnValueEntry{Blockers: []ShadowReturnValueBlocker{{PC: 0x8001, Reason: "callee_return_value_and_stack_contract"}}}}
	banks := []shadowBank{{ID: 0, Config: &config.Config{}}}
	// Bytes that decode as a perfect leaf are insufficient; this exact entry
	// must already exist in the input closure, without a guessed M/X variant.
	for _, results := range [][]shadowDecodeResult{
		{root},
		{root, {entry: decoder.Variant{Address: 0x8100, M: 1}, bankRecipe: &shadowDBRecipe{}}},
	} {
		r := collectShadowReturnCalls(image, banks, results)
		if r.Programs != 1 || r.Entries[0].Status != "unproven" || r.Entries[0].Blockers[0].Reason != "callee_exact_variant_not_in_shadow_closure" {
			t.Fatal(r)
		}
	}
	// Sibling boundaries apply to jump edges; ordinary fallthrough is still
	// allowed by the original decoder. Preserve that exact distinction.
	copy(image, []byte{0x08, 0x20, 0, 0x81, 0x80, 2, 0xea, 0xea, 0x28, 0x60})
	results := []shadowDecodeResult{root, {entry: decoder.Variant{Address: 0x8100}, bankRecipe: &shadowDBRecipe{}}, {entry: decoder.Variant{Address: 0x8008}, issue: &ShadowDecodeIssue{Error: "fixture failed sibling"}}}
	r := collectShadowReturnCalls(image, banks, results)
	if r.Entries[0].Status != "unproven" || r.Entries[0].Blockers[0].Reason != "external_or_missing_decoded_edge" {
		t.Fatal(r)
	}
	slices.Reverse(results)
	if !reflect.DeepEqual(r, collectShadowReturnCalls(image, banks, results)) {
		t.Fatal("query depends on worker order")
	}
}

func TestReturnCallsBudgetsAreExplicitAndDeterministic(t *testing.T) {
	image := make(romimage.Image, 0x8000)
	var results []shadowDecodeResult
	for n := range returnCallRootLimit + 1 {
		pc, a, b := uint32(0x8000+n*16), uint32(0xa000+n), uint32(0xb000+n)
		copy(image[pc-0x8000:], []byte{0x08, 0x20, byte(a), byte(a >> 8), 0x20, byte(b), byte(b >> 8), 0x28, 0x60})
		image[a-0x8000], image[b-0x8000] = 0x60, 0x60
		results = append(results, shadowDecodeResult{entry: decoder.Variant{Address: pc}, bankRecipe: &shadowDBRecipe{}, returnValues: &ShadowReturnValueEntry{Blockers: []ShadowReturnValueBlocker{{PC: pc + 1, Reason: "callee_return_value_and_stack_contract"}}}}, shadowDecodeResult{entry: decoder.Variant{Address: a}, bankRecipe: &shadowDBRecipe{}}, shadowDecodeResult{entry: decoder.Variant{Address: b}, bankRecipe: &shadowDBRecipe{}})
	}
	banks := []shadowBank{{ID: 0, Config: &config.Config{}}}
	r := collectShadowReturnCalls(image, banks, results)
	if r.RequestedEntries != returnCallRootLimit+1 || r.EntriesOmitted != 1 || len(r.Entries) != returnCallRootLimit || !r.ProgramBudgetHit || r.Programs != returnCallProgramLimit || r.Statuses["unproven"] == 0 {
		t.Fatalf("counts=%+v", r)
	}
	slices.Reverse(results)
	if !reflect.DeepEqual(r, collectShadowReturnCalls(image, banks, results)) {
		t.Fatal("budget depends on worker order")
	}
	var caller = []byte{0x08}
	for range returnCallCheckLimit + 1 {
		caller = append(caller, 0x20, 0, 0x81)
	}
	caller = append(caller, 0x28, 0x60)
	g, _, load := returnCallsFixture(t, map[uint32][]byte{0x8000: caller, 0x8100: {0x60}}, decoder.Options{})
	entry := auditShadowReturnCalls(g, nil, load)
	if entry.Status != "conditional_incoming_PC_preserved" || entry.CallsOmitted != 1 || len(entry.Calls) != returnCallCheckLimit {
		t.Fatal(entry)
	}
}

func TestReturnCallsSelectionDoesNotDependOnDisplayedBlockers(t *testing.T) {
	code := []byte{0x08}
	for range 10 {
		code = append(code, 0xd0, 3, 0x8d, 0, 0x10)
	}
	code = append(code, 0x20, 0, 0x81, 0x28, 0x60)
	g, _, _ := returnCallsFixture(t, map[uint32][]byte{0x8000: code, 0x8100: {0x60}}, decoder.Options{})
	local := auditShadowReturnValues(g, nil)
	if !local.hasBlockedCall || local.BlockersOmitted == 0 || slices.ContainsFunc(local.Blockers, func(b ShadowReturnValueBlocker) bool { return b.Reason == "callee_return_value_and_stack_contract" }) {
		t.Fatal(local)
	}
	image := make(romimage.Image, 0x8000)
	copy(image, code)
	image[0x100] = 0x60
	r := collectShadowReturnCalls(image, []shadowBank{{ID: 0, Config: &config.Config{}}}, []shadowDecodeResult{
		{entry: decoder.Variant{Address: 0x8000}, bankRecipe: &shadowDBRecipe{}, returnValues: &local},
		{entry: decoder.Variant{Address: 0x8100}, bankRecipe: &shadowDBRecipe{}},
	})
	if r.RequestedEntries != 1 || len(r.Entries[0].Calls) != 1 || r.Entries[0].Status != "unproven" {
		t.Fatal(r)
	}
}
