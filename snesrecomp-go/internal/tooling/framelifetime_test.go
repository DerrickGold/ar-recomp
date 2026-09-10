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

func TestFrameLifetimePreservedAndExtractedBytes(t *testing.T) {
	call := []byte{0x20, 0, 0x81, 0x60}
	for _, tt := range []struct {
		name        string
		prefix      []byte
		m, x        uint8
		mask, saved uint8
	}{
		{"ordinary call", nil, 0, 0, 0, 0},
		{"local register save", []byte{0x48, 0x68}, 0, 0, 0, 0},
		{"pull push restores identity", []byte{0x68, 0x48}, 0, 0, 0, 0},
		{"cross register restoration", []byte{0x7a, 0x98, 0x48}, 0, 0, 0, 0},
		{"D restoration", []byte{0x2b, 0x0b}, 0, 0, 0, 0},
		{"DB one byte restoration", []byte{0xab, 0x8b}, 0, 0, 0, 0},
		{"hidden B survives narrow load", []byte{0x68, 0xe2, 0x20, 0xa9, 0, 0xc2, 0x20, 0x48}, 0, 0, 1, 0},
		{"extracted word", []byte{0x68}, 0, 0, 3, 3},
		{"extracted byte", []byte{0x68}, 1, 0, 1, 1},
		{"equal height different value", []byte{0x68, 0xa9, 0, 0, 0x48}, 0, 0, 3, 0},
		{"saved register clobbered by arithmetic", []byte{0xfa, 0xe8, 0xda}, 0, 0, 3, 0},
		{"X truncation loses high byte", []byte{0xfa, 0xe2, 0x10, 0xc2, 0x10, 0xda}, 0, 0, 2, 0},
		{"incoming word relocated into local slot", []byte{0x68, 0x08, 0x48}, 0, 0, 3, 3},
		{"stack relative exact copy", []byte{0xa3, 1, 0x83, 1}, 0, 0, 0, 0},
		{"stack relative overwrite", []byte{0x83, 1}, 0, 0, 3, 0},
	} {
		t.Run(tt.name, func(t *testing.T) {
			code := append(slices.Clone(tt.prefix), call...)
			g := returnAuditFixture(t, code, tt.m, tt.x)
			before := shadowStoredJSONKey(g.Order) + shadowStoredJSONKey(g.Instructions[g.Entry])
			exit := decoder.AnalyzeExitMX(g, nil)
			r := auditShadowFrameLifetime(g, nil, 2)
			if tt.mask == 0 {
				if len(r.Boundaries) != 0 {
					t.Fatalf("false review: %+v", r)
				}
			} else {
				if len(r.Boundaries) != 1 {
					t.Fatalf("boundaries: %+v", r)
				}
				f := r.Boundaries[0]
				if f.Unprotected != tt.mask || f.SavedCopies != tt.saved {
					t.Fatalf("finding: %+v", f)
				}
			}
			if len(r.Blockers) != 0 {
				t.Fatalf("unexpected blocker: %+v", r)
			}
			if before != shadowStoredJSONKey(g.Order)+shadowStoredJSONKey(g.Instructions[g.Entry]) || exit != decoder.AnalyzeExitMX(g, nil) {
				t.Fatal("analysis mutated graph or exits")
			}
		})
	}
}

func TestFrameLifetimeConsumedCallerWithLaterArguments(t *testing.T) {
	// An early ordinary return and a frame-consuming transition share an
	// entry. The transition later passes two argument words to another call.
	// Synthetic addresses and constants, no commercial ROM bytes.
	code := []byte{
		0xd0, 1, 0x60, // BNE selected; RTS
		0x20, 0, 0x81, // prior unknown call
		0x68, 0x08, // PLA; PHP
		0xa9, 5, 0, 0x85, 0x20, // possible memory alias
		0x48, 0xa9, 0, 0, 0x48, // two arguments
		0x20, 0x20, 0x81, // JSR consumer at $8012, continuation $8015
		0x28, 0x4c, 0x30, 0x81, // restore P then transition
	}
	code = append(code, make([]byte, 0x131-len(code))...)
	code[0x130] = 0x60
	g := returnAuditFixture(t, code, 0, 0)
	r := auditShadowFrameLifetime(g, nil, 2)
	var found bool
	for _, f := range r.Boundaries {
		if f.PC != 0x8012 {
			continue
		}
		found = true
		if f.SPDelta != -3 || f.Unprotected != 3 || f.SavedCopies != 0 || !reflect.DeepEqual(f.PullPCs, []uint32{0x8006}) || f.Target == nil || *f.Target != 0x8120 || f.Continuation == nil || *f.Continuation != 0x8015 || f.InstructionBytes != "20 20 81" {
			t.Fatalf("late call: %+v", f)
		}
		if len(f.Obligations) != 2 {
			t.Fatalf("conditional assumptions lost: %+v", f)
		}
	}
	if !found {
		t.Fatalf("missed non-returning transition: %+v", r)
	}
	if len(r.Blockers) == 0 {
		t.Fatal("post-call cleanup and PLP must remain unproven")
	}
	// Replacing the incoming-word PLA with a balanced local save/pull must
	// remove the late-call finding, not simply classify every call as unsafe.
	code = append(slices.Clone(code[:6]), append([]byte{0x48}, code[6:]...)...)
	code[0x130] = 0x60
	r = auditShadowFrameLifetime(returnAuditFixture(t, code, 0, 0), nil, 2)
	if len(r.Boundaries) != 0 {
		t.Fatalf("local save became frame consumption: %+v", r)
	}
}

func TestFrameLifetimeDoesNotPreserveInactiveBytesAcrossCalls(t *testing.T) {
	// After PLA, a nested call overwrites the popped entry slots. Reading
	// them back with stack-relative addressing must not recover old tokens.
	g := returnAuditFixture(t, []byte{0x68, 0x20, 0, 0x81, 0xa3, 0, 0x48, 0x20, 0, 0x81, 0x60}, 0, 0)
	r := auditShadowFrameLifetime(g, nil, 2)
	if len(r.Boundaries) != 2 || r.Boundaries[1].SavedCopies != 0 || r.Boundaries[1].Unprotected != 3 {
		t.Fatalf("inactive bytes survived: %+v", r)
	}
}

func TestFrameLifetimeDoesNotUsePCZeroAsMissingProvenance(t *testing.T) {
	s := frameLifetimeState{}
	s.stack[frameLifetimeWindow+1], s.stack[frameLifetimeWindow+2] = 1, 2
	s.regs[0], _ = s.pull(2) // the abstract contract is not LoROM-address dependent
	s.key.PC = 1
	f, ok := frameLifetimeBoundary(s, &cpu65816.Instruction{Opcode: 0x20, Mnemonic: "JSR", Mode: cpu65816.ABS, Length: 3, Operand: 0x100}, 2)
	if !ok || f.Unprotected != 3 || !reflect.DeepEqual(f.PullPCs, []uint32{0}) {
		t.Fatalf("lost PC zero: %+v", f)
	}
}

func TestFrameLifetimeLongFramesPathsAndStatus(t *testing.T) {
	// Pull the return word and bank then make a long call. Only the word
	// hypothesis would overlook the bank byte, so retain the RTL hypothesis.
	g := returnAuditFixture(t, []byte{0x68, 0xab, 0x22, 0x20, 0x81, 2, 0x6b}, 0, 0)
	r := auditShadowFrameLifetimes(g, nil)
	if len(r) != 1 || r[0].FrameBytes != 3 || len(r[0].Boundaries) != 1 {
		t.Fatalf("long contract: %+v", r)
	}
	f := r[0].Boundaries[0]
	if f.Unprotected != 7 || f.SavedCopies != 7 || f.Target == nil || *f.Target != 0x028120 || f.Continuation == nil || *f.Continuation != 0x8006 {
		t.Fatalf("long edge: %+v", f)
	}
	// One branch restores the pulled word, one does not. Do not merge the
	// restored path into an unconditional claim about the other path.
	g = returnAuditFixture(t, []byte{0x68, 0xd0, 1, 0x48, 0x20, 0, 0x81, 0x60}, 0, 0)
	a := auditShadowFrameLifetime(g, nil, 2)
	if len(a.Boundaries) != 1 || a.Boundaries[0].SPDelta != 2 || a.Boundaries[0].SavedCopies != 3 {
		t.Fatalf("path states: %+v", a)
	}
	g = returnAuditFixture(t, []byte{0x08, 0xe2, 0x30, 0xda, 0xfa, 0x28, 0x20, 0, 0x81, 0x60}, 0, 0)
	a = auditShadowFrameLifetime(g, nil, 2)
	if len(a.Boundaries) != 0 || len(a.Blockers) != 0 {
		t.Fatalf("saved status: %+v", a)
	}
}

func TestFrameLifetimeBarriersAndBudgets(t *testing.T) {
	for _, tt := range []struct {
		name   string
		prefix []byte
		reason string
	}{
		{"stack reset", []byte{0x1b}, "unsupported_stack_or_control_effect"},
		{"emulation change", []byte{0xfb}, "unsupported_stack_or_control_effect"},
		{"unknown status", []byte{0x28}, "PLP_value_not_known_saved_status"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			code := append(slices.Clone(tt.prefix), 0x68, 0x20, 0, 0x81, 0x60)
			r := auditShadowFrameLifetime(returnAuditFixture(t, code, 0, 0), nil, 2)
			if len(r.Boundaries) != 0 || len(r.Blockers) != 1 || r.Blockers[0].Reason != tt.reason {
				t.Fatalf("barrier: %+v", r)
			}
		})
	}
	for _, cfg := range []*config.Config{
		{HLEFunctions: map[uint16]string{0x8000: "hook"}},
		{HLEFunctionsIf: map[uint16]config.HLEFunctionIf{0x8001: {}}},
		{HLEDispatch: map[uint16]string{0x8001: "hook"}},
		{HLESPCUpload: []uint16{0x8001}},
	} {
		r := auditShadowFrameLifetime(returnAuditFixture(t, []byte{0x68, 0x20, 0, 0x81, 0x60}, 0, 0), cfg, 2)
		if len(r.Boundaries) != 0 || len(r.Blockers) != 1 || r.Blockers[0].Reason != "HLE_contract_not_inherited_from_ROM" {
			t.Fatalf("HLE bypassed: %+v", r)
		}
	}
	g := returnAuditFixture(t, []byte{0x68, 0x20, 0, 0x81, 0x60}, 0, 0)
	g.Instructions[g.Entry].Instruction.DispatchKind = "rts_trick"
	if r := auditShadowFrameLifetime(g, nil, 2); len(r.Boundaries) != 0 || r.Blockers[0].Reason != "collapsed_dispatch_contract" {
		t.Fatalf("collapsed stack replayed: %+v", r)
	}
	g = returnAuditFixture(t, []byte{0x68, 0x20, 0, 0x81, 0x40}, 0, 0)
	for _, r := range auditShadowFrameLifetimes(g, nil) {
		if len(r.Boundaries) != 0 || len(r.Blockers) == 0 {
			t.Fatalf("interrupt as normal frame: %+v", r)
		}
	}
	g = returnAuditFixture(t, []byte{0x68, 0xd0, 0xfd, 0x20, 0, 0x81, 0x60}, 0, 0)
	if r := auditShadowFrameLifetime(g, nil, 2); !r.BudgetHit {
		t.Fatalf("unbounded pull loop: %+v", r)
	}
	// Exhaust a graph before reaching the only call. The missing finding
	// must not be presented as a complete absence of lifetime changes.
	g = &decoder.Graph{Entry: decoder.DecodeKey{PC: 0x8000}, Instructions: make(map[decoder.DecodeKey]*decoder.DecodedInstruction)}
	for n := 0; n <= frameLifetimeStates; n++ {
		k := decoder.DecodeKey{PC: 0x8000 + uint32(n)}
		g.Instructions[k] = &decoder.DecodedInstruction{Key: k, Instruction: &cpu65816.Instruction{Opcode: 0xea, Mnemonic: "NOP", Length: 1}, Successors: []decoder.DecodeKey{{PC: k.PC + 1}}}
	}
	if r := auditShadowFrameLifetime(g, nil, 2); !r.BudgetHit || len(r.Boundaries) != 0 {
		t.Fatalf("state budget: %+v", r)
	}
}

func TestFrameLifetimeReportOnlyAndDeterministic(t *testing.T) {
	root := t.TempDir()
	romPath, cfgDir := filepath.Join(root, "fixture.sfc"), filepath.Join(root, "recomp")
	image := make([]byte, 0x8000)
	copy(image, []byte{0x68, 0x08, 0x48, 0x48, 0x20, 0x20, 0x80, 0x60})
	image[0x20] = 0x60
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(cfgDir, "bank00.cfg")
	text := "bank = 00\nfunc Entry 8000 entry_mx:0,0\nfunc Leaf 8020 entry_mx:0,0\nhle_func 8020 leaf_hook\n"
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
	if a.FrameLifetimes.UniqueSourceSites != 1 {
		t.Fatalf("inventory: %+v", a.FrameLifetimes)
	}
	facts, rejected := SelectStaticProvenDatabaseDispatchFacts(a)
	auto, autoRejected := SelectStaticProvenAutomaticDispatchFacts(a)
	database, err := BuildStaticAnalysisDatabase(a)
	if err != nil {
		t.Fatal(err)
	}
	a.FrameLifetimes = ShadowFrameLifetimes{}
	withoutAudit, err := BuildStaticAnalysisDatabase(a)
	if err != nil || !reflect.DeepEqual(database, withoutAudit) {
		t.Fatalf("review evidence changed persisted entry/dispatch facts or templates: %v", err)
	}
	other, otherRejected := SelectStaticProvenDatabaseDispatchFacts(a)
	otherAuto, otherAutoRejected := SelectStaticProvenAutomaticDispatchFacts(a)
	if !reflect.DeepEqual(facts, other) || rejected != otherRejected || !reflect.DeepEqual(auto, otherAuto) || autoRejected != otherAutoRejected {
		t.Fatal("review evidence entered production facts")
	}
	if content, _ := os.ReadFile(path); string(content) != text {
		t.Fatal("cfg mutated")
	}
	if content, _ := os.ReadFile(romPath); !bytes.Equal(content, image) {
		t.Fatal("ROM mutated")
	}
	if files, _ := os.ReadDir(root); len(files) != 2 {
		t.Fatal("analysis wrote files")
	}
	var out bytes.Buffer
	writeShadowFrameLifetimes(&out, b.FrameLifetimes, true)
	if !strings.Contains(out.String(), "[FRAME-LIFETIME]") || !strings.Contains(out.String(), "not runtime failures") {
		t.Fatal(out.String())
	}
	entries := auditShadowFrameLifetimes(returnAuditFixture(t, []byte{0x68, 0x20, 0, 0x81, 0x60}, 0, 0), nil)
	result := shadowDecodeResult{frameLifetimes: entries}
	r := collectShadowFrameLifetimes([]shadowDecodeResult{result})
	if !reflect.DeepEqual(r, collectShadowFrameLifetimes([]shadowDecodeResult{result, result})) {
		t.Fatal("duplicate contexts inflate inventory")
	}
	slices.Reverse(entries)
	if !reflect.DeepEqual(r, collectShadowFrameLifetimes([]shadowDecodeResult{{frameLifetimes: entries}})) {
		t.Fatal("worker ordering changes inventory")
	}
}
