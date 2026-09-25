package tooling

import (
	"reflect"
	"testing"
)

func TestCommandPathOutcomesCollapseOnlyProofDetail(t *testing.T) {
	read := func(delta int) *ShadowCommandCursorRead {
		return &ShadowCommandCursorRead{LoadPC: 0x8123, Offset: 2, BankSource: "entry_db", Delta: delta}
	}
	base := ShadowCommandCallbackPath{Status: "owned_native_refetch", EntryDecimalCondition: "clear", CursorDelta: 6, CursorKnown: true, DBSource: "entry_db",
		StreamCursorRead: read(1), CursorPublications: []ShadowCommandCursorStore{{Mode: "dp,x", Operand: 0x46, CursorDelta: 6}},
		Path: []uint32{0x8000, 0x8010}, Obligations: []string{"first"}, StopPC: 0x8010}
	twin := base
	twin.StreamCursorRead = read(1) // equal value behind a different pointer
	twin.Path, twin.Branches, twin.Obligations, twin.StopPC = []uint32{0x8000, 0x8020}, []ShadowCommandBranch{{}}, []string{"second"}, 0x8020
	variants := []func(*ShadowCommandCallbackPath){
		func(p *ShadowCommandCallbackPath) { p.Status = "opaque_caller_stack_read" },
		func(p *ShadowCommandCallbackPath) { p.EntryDecimalCondition = "set" },
		func(p *ShadowCommandCallbackPath) { p.CursorDelta = 7 },
		func(p *ShadowCommandCallbackPath) { p.CursorKnown = false },
		func(p *ShadowCommandCallbackPath) { p.StackBytes = 3 },
		func(p *ShadowCommandCallbackPath) { p.DBSource = "constant" },
		func(p *ShadowCommandCallbackPath) { p.StreamCursorRead = read(2) },
		func(p *ShadowCommandCallbackPath) { p.StreamCursorRead = nil },
		func(p *ShadowCommandCallbackPath) {
			p.CursorPublications = []ShadowCommandCursorStore{{Mode: "dp,x", Operand: 0x46, CursorDelta: 4}}
		},
		func(p *ShadowCommandCallbackPath) { p.CursorPublications = nil },
		func(p *ShadowCommandCallbackPath) {
			p.Returns = []ShadowCommandCallbackReturn{{PC: 0x8030, TargetPC: 0x8040, Source: "matched_jsr"}}
		},
	}
	paths := []ShadowCommandCallbackPath{base, twin}
	for _, change := range variants {
		p := base
		change(&p)
		paths = append(paths, p, p)
	}
	got := distinctShadowCommandPathOutcomes(paths)
	if len(got) != 1+len(variants) {
		t.Fatalf("outcomes=%d want %d", len(got), 1+len(variants))
	}
	if !reflect.DeepEqual(got[0], base) {
		t.Fatalf("first representative not kept: %+v", got[0])
	}
	for n := range variants {
		if !reflect.DeepEqual(got[1+n], paths[2+2*n]) {
			t.Fatalf("variant %d reordered or merged: %+v", n, got[1+n])
		}
	}
	if len(distinctShadowCommandPathOutcomes(nil)) != 0 {
		t.Fatal("empty input produced outcomes")
	}
}
