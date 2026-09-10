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
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestReturnAliasFullWriteFootprints(t *testing.T) {
	for _, tc := range []struct {
		name  string
		code  []byte
		m, x  uint8
		db    int // -1 unknown
		index int // -1 unknown; set both X and Y
		safe  bool
		span  [2]uint32
	}{
		{"first unmirrored word", []byte{0x8f, 0, 0x20, 0x7e}, 0, 0, -1, -1, true, [2]uint32{0x7e2000, 0x7e2001}},
		{"bank crossing stays WRAM", []byte{0x8f, 0xff, 0xff, 0x7e}, 0, 0, -1, -1, true, [2]uint32{0x7effff, 0x7f0000}},
		{"last unmirrored byte", []byte{0x8f, 0xff, 0xff, 0x7f}, 1, 0, -1, -1, true, [2]uint32{0x7fffff, 0x7fffff}},
		{"last word reaches bank80 mirror", []byte{0x8f, 0xff, 0xff, 0x7f}, 0, 0, -1, -1, false, [2]uint32{0x7fffff, 0x800000}},
		{"first byte aliases", []byte{0x8f, 0xff, 0x1f, 0x7e}, 0, 0, -1, -1, false, [2]uint32{0x7e1fff, 0x7e2000}},
		{"entire low mirror", []byte{0x8f, 0, 1, 0x7e}, 0, 0, -1, -1, false, [2]uint32{0x7e0100, 0x7e0101}},
		{"full X domain safe", []byte{0x9f, 0, 0x20, 0x7e}, 0, 0, -1, -1, true, [2]uint32{0x7e2000, 0x7f2000}},
		{"full X domain escapes", []byte{0x9f, 0x40, 0, 0x7f}, 0, 0, -1, -1, false, [2]uint32{0x7f0040, 0x800040}},
		{"narrow X domain safe", []byte{0x9f, 0x40, 0, 0x7f}, 0, 1, -1, -1, true, [2]uint32{0x7f0040, 0x7f0140}},
		{"known full X safe", []byte{0x9f, 0x40, 0, 0x7f}, 0, 0, -1, 0x100, true, [2]uint32{0x7f0140, 0x7f0141}},
		{"known X bank carry unsafe", []byte{0x9f, 0x40, 0, 0x7f}, 0, 0, -1, 0xffff, false, [2]uint32{0x80003f, 0x800040}},
		{"24 bit wrap rejected", []byte{0x9f, 0xff, 0xff, 0xff}, 0, 0, -1, 1, false, [2]uint32{0x1000000, 0x1000001}},
		{"absolute DB known", []byte{0x8d, 0, 0x20}, 0, 0, 0x7e, -1, true, [2]uint32{0x7e2000, 0x7e2001}},
		{"absolute DB unknown", []byte{0x8d, 0, 0x20}, 0, 0, -1, -1, false, [2]uint32{}},
		{"absolute X carries bank", []byte{0x9d, 0xff, 0xff}, 0, 0, 0x7e, 1, true, [2]uint32{0x7f0000, 0x7f0001}},
		{"absolute Y carries bank", []byte{0x99, 0xff, 0xff}, 0, 0, 0x7f, 1, false, [2]uint32{0x800000, 0x800001}},
		{"STX uses X not M", []byte{0x8e, 0xff, 0xff}, 1, 0, 0x7f, -1, false, [2]uint32{0x7fffff, 0x800000}},
		{"STY narrow despite wide A", []byte{0x8c, 0xff, 0xff}, 0, 1, 0x7f, -1, true, [2]uint32{0x7fffff, 0x7fffff}},
		{"STZ uses M", []byte{0x9c, 0xff, 0xff}, 0, 1, 0x7f, -1, false, [2]uint32{0x7fffff, 0x800000}},
		{"direct ignores DB", []byte{0x85, 0x20}, 0, 0, 0x7f, -1, false, [2]uint32{}},
		{"indirect pointer not a destination", []byte{0x97, 0x20}, 0, 1, 0x7f, -1, false, [2]uint32{}},
		{"WRAM data port can overwrite stack", []byte{0x8f, 0x80, 0x21, 0}, 0, 0, -1, -1, false, [2]uint32{0x2180, 0x2181}},
		{"DMA can overwrite stack", []byte{0x8f, 0x0b, 0x42, 0}, 1, 0, -1, -1, false, [2]uint32{0x420b, 0x420b}},
		{"cartridge write not assumed inert", []byte{0x8f, 0, 0x80, 0x70}, 0, 0, -1, -1, false, [2]uint32{0x708000, 0x708001}},
		{"RMW not an ordinary store", []byte{0xee, 0, 0x20}, 0, 0, 0x7e, -1, false, [2]uint32{}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			g := returnAuditFixture(t, append(slices.Clone(tc.code), 0x60), tc.m, tc.x)
			s := returnValueState{key: g.Entry}
			if tc.db >= 0 {
				s.db = returnSymbol(returnValueConstant, uint16(tc.db))[0]
			}
			if tc.index >= 0 {
				s.regs[1], s.regs[2] = returnSymbol(returnValueConstant, uint16(tc.index)), returnSymbol(returnValueConstant, uint16(tc.index))
			}
			w := auditReturnWrite(s, g.Instructions[g.Entry].Instruction)
			if (w.Status == "disjoint_unmirrored_WRAM") != tc.safe {
				t.Fatalf("%+v", w)
			}
			if tc.span != [2]uint32{} && (w.ByteRange == nil || *w.ByteRange != tc.span) {
				t.Fatalf("span=%v want=%v", w.ByteRange, tc.span)
			}
		})
	}
}

func TestReturnAliasesFollowDBAndKeepRealStackEffects(t *testing.T) {
	for _, tc := range []struct {
		name           string
		caller, callee []byte
		status, reason string
	}{
		{"callee store is disjoint", []byte{0x08, 0x20, 0, 0x81, 0x28, 0x60}, []byte{0x8f, 0, 0x20, 0x7e, 0x60}, "conditional_incoming_PC_preserved", ""},
		{"PEA PLB establishes DB across call", []byte{0xf4, 0x7e, 0x7e, 0xab, 0xab, 0x20, 0, 0x81, 0x60}, []byte{0x8d, 0, 0x20, 0x60}, "conditional_incoming_PC_preserved", ""},
		{"callee changes DB for caller", []byte{0x20, 0, 0x81, 0x8d, 0, 0x20, 0x60}, []byte{0xf4, 0x7e, 0x7e, 0xab, 0xab, 0x60}, "conditional_incoming_PC_preserved", ""},
		{"PHB restores actual DB", []byte{0xf4, 0x7f, 0x7f, 0xab, 0xab, 0x8b, 0x20, 0, 0x81, 0xab, 0x8d, 0, 0x10, 0x60}, []byte{0xf4, 0x7e, 0x7e, 0xab, 0xab, 0x60}, "conditional_incoming_PC_preserved", ""},
		{"PLB unknown kills prior DB", []byte{0xf4, 0x7f, 0x7f, 0xab, 0xab, 0x20, 0, 0x81, 0x8d, 0, 0x20, 0x60}, []byte{0x0b, 0xab, 0xab, 0x60}, "unproven", "memory_write_may_alias_return_frame"},
		{"PHK uses active bank not WRAM guess", []byte{0x4b, 0xab, 0x20, 0, 0x81, 0x60}, []byte{0x8d, 0, 0x20, 0x60}, "unproven", "memory_write_may_alias_return_frame"},
		{"known X used through PHX PLX", []byte{0xa2, 0, 1, 0xda, 0xa2, 0xff, 0xff, 0xfa, 0x20, 0, 0x81, 0x60}, []byte{0x9f, 0x40, 0, 0x7f, 0x60}, "conditional_incoming_PC_preserved", ""},
		{"safe store does not hide caller frame corruption", []byte{0x20, 0, 0x81, 0x60}, []byte{0x8f, 0, 0x20, 0x7e, 0xa9, 0, 0, 0x83, 3, 0x60}, "unproven", "return_PC_not_incoming_PC_plus_constant"},
		{"other reads still unknown", []byte{0x68, 0x8f, 0, 0x20, 0x7e, 0xaf, 0, 0x20, 0x7e, 0x48, 0x60}, []byte{0x60}, "unproven", "return_PC_not_incoming_PC_plus_constant"},
		{"one safe path cannot close aliasing branch", []byte{0x20, 0, 0x81, 0x60}, []byte{0xd0, 5, 0x8f, 0, 0x20, 0x7e, 0x60, 0x8f, 0, 1, 0x7e, 0x60}, "unproven", "memory_write_may_alias_return_frame"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			g, _, load := returnCallsFixture(t, map[uint32][]byte{0x8000: tc.caller, 0x8100: tc.callee}, decoder.Options{})
			before := auditShadowReturnCalls(g, nil, load)
			r := auditShadowReturnCallMode(g, nil, load, true)
			if r.Status != tc.status || (tc.reason != "" && !slices.ContainsFunc(r.Blockers, func(b ShadowReturnValueBlocker) bool { return b.Reason == tc.reason })) {
				t.Fatalf("%+v", r)
			}
			if len(r.Writes) == 0 {
				t.Fatal("missing write evidence")
			}
			if !reflect.DeepEqual(before, auditShadowReturnCalls(g, nil, load)) || len(before.Writes) != 0 {
				t.Fatal("changed old query")
			}
		})
	}
}

func TestReturnAliasesHLEAndDisplayLimits(t *testing.T) {
	caller := []byte{0x08, 0x20, 0, 0x81, 0x28, 0x60}
	callee := []byte{}
	for range returnAliasWriteLimit + 1 {
		callee = append(callee, 0x8f, 0, 0x20, 0x7e)
	}
	callee = append(callee, 0x85, 0, 0x60)
	g, _, load := returnCallsFixture(t, map[uint32][]byte{0x8000: caller, 0x8100: callee}, decoder.Options{})
	r := auditShadowReturnCallMode(g, nil, load, true)
	if r.Status != "unproven" || len(r.Writes) != returnAliasWriteLimit || r.WritesOmitted != 2 || !r.hasBlockedWrite {
		t.Fatal(r)
	}
	if r.WriteStatuses["disjoint_unmirrored_WRAM"] != returnAliasWriteLimit+1 || r.WriteStatuses["may_alias_or_unmodeled"] != 1 || len(r.DisjointWritePCs) != returnAliasWriteLimit+1 {
		t.Fatal("display limit hid write totals", r)
	}
	cfg := &config.Config{HLEFunctions: map[uint16]string{0x8104: "fixture_hook"}}
	r = auditShadowReturnCallMode(g, nil, func(v decoder.Variant) (*decoder.Graph, *config.Config, string) {
		g, _, why := load(v)
		return g, cfg, why
	}, true)
	if r.Status != "unproven" || r.Blockers[0].Reason != "HLE_contract_not_inherited_from_ROM" || len(r.Writes) != 1 {
		t.Fatal(r)
	}
}

func TestReturnAliasesReportOnlyIntegration(t *testing.T) {
	root := t.TempDir()
	romPath, cfgDir := filepath.Join(root, "fixture.sfc"), filepath.Join(root, "recomp")
	image := make(romimage.Image, 0x8000)
	copy(image, []byte{0x08, 0x20, 0, 0x81, 0x28, 0x60})
	copy(image[0x100:], []byte{0x8f, 0, 0x20, 0x7e, 0x60})
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
		t.Fatal("worker dependent report")
	}
	if a.ReturnAliases.Statuses["conditional_incoming_PC_preserved"] != 1 || a.ReturnCalls.Statuses["unproven"] != 1 || a.ReturnAliases.Programs != 2 {
		t.Fatalf("aliases=%+v calls=%+v", a.ReturnAliases, a.ReturnCalls)
	}
	facts, rejected := SelectStaticProvenDatabaseDispatchFacts(a)
	a.ReturnAliases = ShadowReturnCalls{}
	other, otherRejected := SelectStaticProvenDatabaseDispatchFacts(a)
	if !reflect.DeepEqual(facts, other) || rejected != otherRejected {
		t.Fatal("alias contracts entered production facts")
	}
	content, _ := os.ReadFile(path)
	entries, _ := os.ReadDir(root)
	if string(content) != authored || len(entries) != 2 {
		t.Fatal("analysis modified project")
	}
	var out bytes.Buffer
	writeShadowReturnAliases(&out, b.ReturnAliases, true)
	if !strings.Contains(out.String(), "[RETURN-ALIAS]") || !strings.Contains(out.String(), "bytes=8F00207E") || !strings.Contains(out.String(), "not new roots") {
		t.Fatal(out.String())
	}
}

func TestReturnAliasSelectionRetainsHiddenMemoryBlockers(t *testing.T) {
	image := make(romimage.Image, 0x8000)
	image[0] = 0x60
	previous := ShadowReturnCalls{Entries: []ShadowReturnCallEntry{{ShadowReturnValueEntry: ShadowReturnValueEntry{EntryPC: 0x8000, hasBlockedWrite: true, Blockers: []ShadowReturnValueBlocker{{PC: 0x8000, Reason: "other_displayed_reason"}}, BlockersOmitted: 1}}}}
	r := collectShadowReturnAliases(image, []shadowBank{{ID: 0, Config: &config.Config{}}}, []shadowDecodeResult{{entry: decoder.Variant{Address: 0x8000}, bankRecipe: &shadowDBRecipe{}}}, previous)
	if r.RequestedEntries != 1 || r.Statuses["conditional_incoming_PC_preserved"] != 1 {
		t.Fatal(r)
	}
}
