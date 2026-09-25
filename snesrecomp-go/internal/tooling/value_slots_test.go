package tooling

import (
	"encoding/json"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func decodeColdFixture(t *testing.T, image rom.Image, entries ...uint16) []*decoder.Graph {
	t.Helper()
	var graphs []*decoder.Graph
	for _, pc := range entries {
		g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{MaxInstructions: 128})
		if err != nil {
			t.Fatal(err)
		}
		graphs = append(graphs, g)
	}
	return graphs
}

// Two lifetimes of one record-pointer slot: a loader stores table-derived
// pointers and dispatches through them; an initializer stores an arithmetic
// record address. Both reload the slot after calling the same helper.
func slotLifetimeFixture(t *testing.T, helper []byte) (rom.Image, []*decoder.Graph) {
	t.Helper()
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0x4b, 0xab, 0xa5, 0x70, 0x29, 2, 0, 0xa8, 0xbe, 0, 0xa0,
		0x8e, 0x61, 0x1c, 0x20, 0, 0x88, 0xae, 0x61, 0x1c, 0xfc, 6, 0, 0x60})
	copy(image[0x100:], []byte{0x4b, 0xab, 0xa5, 0x72, 0x29, 1, 0, 0x0a, 0x0a, 0x0a, 0x0a,
		0x18, 0x69, 0, 0xb0, 0x8d, 0x61, 0x1c, 0x20, 0, 0x88, 0xac, 0x61, 0x1c, 0xb9, 6, 0, 0x85, 0x50, 0x60})
	copy(image[0x800:], helper)
	copy(image[0x2000:], []byte{0, 0xa1, 0x10, 0xa1})
	copy(image[0x2106:], []byte{0, 0x91})
	copy(image[0x2116:], []byte{0x10, 0x91})
	copy(image[0x3006:], []byte{0, 0x92})
	copy(image[0x3016:], []byte{0x10, 0x92})
	for _, at := range []int{0x1100, 0x1110, 0x1200, 0x1210} {
		image[at] = 0x60
	}
	return image, decodeColdFixture(t, image, 0x8000, 0x8100, 0x8800)
}

func TestColdSlotLifetimesUseReachingDefinitions(t *testing.T) {
	image, graphs := slotLifetimeFixture(t, []byte{0x60})
	got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
	if want := []uint32{0x009100, 0x009110}; !got.Converged || !slices.Equal(got.IndexedTargets, want) {
		t.Fatalf("mixed slot lifetimes: got %X want %X; %s", got.IndexedTargets, want, got.Summary())
	}
	for _, condition := range []string{"local_slot_reaching_definition", "slot_definition_survives_calls"} {
		if !slices.Contains(got.Conditions, condition) {
			t.Fatalf("missing condition %s: %v", condition, got.Conditions)
		}
	}
}

func TestColdSlotDefinitionStopsAtWritingCallee(t *testing.T) {
	// The helper clears the slot. Its callers cannot claim their earlier store
	// as the reaching definition, so the reload keeps the global publication
	// union rather than guessing which lifetime survives.
	image, graphs := slotLifetimeFixture(t, []byte{0x9c, 0x61, 0x1c, 0x60})
	got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
	if want := []uint32{0x009100, 0x009110, 0x009200, 0x009210}; !slices.Equal(got.IndexedTargets, want) {
		t.Fatalf("got %X want %X; %s", got.IndexedTargets, want, got.Summary())
	}
	if slices.Contains(got.Conditions, "local_slot_reaching_definition") {
		t.Fatalf("claimed a definition across a writing callee: %v", got.Conditions)
	}
}

// A record-selected state machine. The initializer is reached with unknown DB,
// selects a 16-byte record arithmetically, saves and reloads its address across
// a call and publishes the table-base (+11) and initial-state (+3) fields. It
// then reads the same record after PHK/PLB. The dispatcher combines both slots
// through three-byte handler/next-state cells. Records past the pointed state
// tables and misaligned cells hold decodable garbage that must not be admitted.
func recordStateFixture(t *testing.T, helper []byte, witness bool) (rom.Image, []*decoder.Graph) {
	t.Helper()
	image := make(rom.Image, 0x8000)
	for n := 0x1000; n < 0x2000; n++ {
		image[n] = 0x60 // every $9xxx address decodes
	}
	initializer := []byte{0xa5, 0x70, 0x29, 0xff, 0, 0x0a, 0x0a, 0x0a, 0x0a, 0x18, 0x69, 0, 0xa0,
		0x8d, 0x61, 0x1c, 0x20, 0, 0x88, 0xac, 0x61, 0x1c, 0xb9, 0x0b, 0, 0x8d, 0x7f, 0x1c,
		0xb9, 3, 0, 0x29, 0x7f, 0, 0x8d, 0x3b, 0x1c, 0x20, 0, 0x88,
		0x4b, 0xab, 0xac, 0x61, 0x1c, 0xb9, 2, 0, 0x60}
	if !witness {
		initializer[40], initializer[41] = 0xea, 0xea
	}
	copy(image, initializer)
	copy(image[0x200:], []byte{0x4b, 0xab, 0xd8, 0xad, 0x3b, 0x1c, 0x30, 0x1f, 0x0a, 0x18,
		0x6d, 0x3b, 0x1c, 0x6d, 0x7f, 0x1c, 0x85, 0x42, 0xa0, 2, 0, 0xb1, 0x42,
		0x29, 0xff, 0, 0x8d, 0x3b, 0x1c, 0xb2, 0x42, 0x85, 0x42, 0xf4, 0x26, 0x82,
		0x6c, 0x42, 0, 0x60})
	copy(image[0x800:], helper)
	// Records: initial state at +3, state table at +11.
	copy(image[0x2003:], []byte{1})
	copy(image[0x200b:], []byte{0x30, 0xa0})
	copy(image[0x2013:], []byte{3})
	copy(image[0x201b:], []byte{0x38, 0xa0})
	copy(image[0x202b:], []byte{0x38, 0xa0})
	// State tables directly after the three records, with a gap between them.
	copy(image[0x2030:], []byte{0, 0x91, 1, 0x10, 0x91, 0})
	copy(image[0x2036:], []byte{0x40, 0x93})
	copy(image[0x2038:], []byte{0, 0x92, 0, 0x10, 0x92, 0, 0x20, 0x92, 1, 0x30, 0x92, 2})
	// A record past the tables would point at this garbage table.
	copy(image[0x204b:], []byte{0x50, 0xa0})
	copy(image[0x2050:], []byte{0xee, 0x9e, 0, 0xee, 0x9e, 0, 0xee, 0x9e, 0, 0xee, 0x9e, 0})
	return image, decodeColdFixture(t, image, 0x8000, 0x8200, 0x8800)
}

func TestColdRecordStateTablesCorrelatePublishedFields(t *testing.T) {
	image, graphs := recordStateFixture(t, []byte{0x60}, true)
	before, _ := json.Marshal(graphs)
	want := []uint32{0x009100, 0x009110, 0x009200, 0x009210, 0x009220, 0x009230}
	var first ColdValueInventory
	for n := range 2 {
		got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
		if !got.Converged || !slices.Equal(got.IndirectTargets, want) {
			t.Fatalf("got %X want %X; %s", got.IndirectTargets, want, got.Summary())
		}
		for _, condition := range []string{"same_index_definition_bank", "self_delimiting_record_family", "local_slot_reaching_definition"} {
			if !slices.Contains(got.Conditions, condition) {
				t.Fatalf("missing condition %s: %v", condition, got.Conditions)
			}
		}
		if slices.Contains(got.Boundaries, "value_correlation_budget") || slices.Contains(got.Boundaries, "value_cardinality_budget") {
			t.Fatalf("correlation was dropped: %v", got.Boundaries)
		}
		if n == 0 {
			first = got
		} else {
			a, _ := json.Marshal(first)
			b, _ := json.Marshal(got)
			if string(a) != string(b) {
				t.Fatal("query order changed the result")
			}
		}
		slices.Reverse(graphs)
	}
	after, _ := json.Marshal(graphs)
	if string(before) != string(after) {
		t.Fatal("query changed decoded instructions")
	}
}

func TestColdRecordStateTableBarriers(t *testing.T) {
	for _, tt := range []struct {
		name    string
		helper  []byte
		witness bool
	}{
		// Without a known-DB read of the same definition, neither the record
		// reads nor their publications acquire a bank.
		{"no known-bank witness", []byte{0x60}, false},
		// A helper that rewrites the record-pointer slot breaks the reload's
		// local definition; the unknown-DB publication is not guessed.
		{"slot rewritten by callee", []byte{0x9c, 0x61, 0x1c, 0x60}, true},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, graphs := recordStateFixture(t, tt.helper, tt.witness)
			got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
			if len(got.IndirectTargets) != 0 {
				t.Fatalf("admitted %X; %s", got.IndirectTargets, got.Summary())
			}
		})
	}
}

func TestColdRecordFamilyBoundUsesAlignedForwardPointers(t *testing.T) {
	image, graphs := recordStateFixture(t, []byte{0x60}, true)
	e := newColdValueEngine(image, nil, graphs, graphs)
	e.consumers()
	e.indirectConsumers()
	e.solve()
	var bound uint16
	families := 0
	for _, st := range e.families {
		if st.has {
			families++
			bound = st.bound
		}
	}
	if families != 1 || bound != 0xa030 {
		t.Fatalf("family bound %04X across %d families, want $A030", bound, families)
	}
	// A forward word that is not on a record boundary is not pointed storage.
	st := &coldRecordFamily{fields: map[[2]uint16]bool{{0, 3}: true}, users: map[int]bool{}}
	copy(image[0x2003:], []byte{0x31, 0xa0})
	records := []coldWordValue{{word: 0xa000}, {word: 0xa010}, {word: 0xa020}}
	if _, has := e.recordFamilyBound(st, records); has {
		t.Fatal("misaligned field word bounded the family")
	}
}

// A callee that can reach a spelled writer of the reloaded slot, directly or
// through a resolved jump table, or that is itself undecoded, sends the reload
// back to the global union. Code the analysis cannot see into and aliased
// writes that could touch the slot keep the local answer, under explicit
// conditions the union shares.
func TestColdSlotDefinitionCallEffects(t *testing.T) {
	local := []uint32{0x009100, 0x009110}
	union := []uint32{0x009100, 0x009110, 0x009200, 0x009210}
	for _, tt := range []struct {
		name    string
		helper  []byte
		setup   func(image rom.Image)
		roots   []uint16
		want    []uint32
		aliased bool
		hidden  bool
	}{
		// LDX #$9F00; LDY #$1C61; LDA #1; MVN $00,$00; RTS
		{"block move into low WRAM", []byte{0xa2, 0x00, 0x9f, 0xa0, 0x61, 0x1c, 0xa9, 0x01, 0x00, 0x54, 0x00, 0x00, 0x60}, nil, nil, local, true, false},
		// The same block move into a ROM bank cannot write the slot.
		{"block move into ROM", []byte{0xa2, 0x00, 0x9f, 0xa0, 0x61, 0x1c, 0xa9, 0x01, 0x00, 0x54, 0xc0, 0x00, 0x60}, nil, nil, local, false, false},
		// LDX #0; STA $1C00,X; RTS: the index can reach the slot.
		{"indexed store below the slot", []byte{0xa2, 0x00, 0x00, 0x9d, 0x00, 0x1c, 0x60}, nil, nil, local, true, false},
		// LDX #0; STA $1D00,X; RTS: starts above the slot.
		{"indexed store above the slot", []byte{0xa2, 0x00, 0x00, 0x9d, 0x00, 0x1d, 0x60}, nil, nil, local, false, false},
		// LDX #0; STA ($30),Y; RTS: a pointer-based write may reach anything.
		{"pointer-based store", []byte{0xa2, 0x00, 0x00, 0x91, 0x30, 0x60}, nil, nil, local, true, false},
		// LDX #0; JSR ($1E00,X); RTS: the pointer lives in RAM, callees unknown.
		{"indirect call through RAM", []byte{0xa2, 0x00, 0x00, 0xfc, 0x00, 0x1e, 0x60},
			func(image rom.Image) { copy(image[0x900:], []byte{0x9c, 0x61, 0x1c, 0x60}) }, []uint16{0x8800, 0x8900}, local, false, true},
		// JSR $8A00; RTS where $8A00 writes the slot but is never decoded.
		{"undecoded downstream callee", []byte{0x20, 0x00, 0x8a, 0x60},
			func(image rom.Image) { copy(image[0xa00:], []byte{0x9c, 0x61, 0x1c, 0x60}) }, nil, local, false, true},
		// The helper the reload's own path calls is itself undecoded.
		{"undecoded direct callee", []byte{0x60}, nil, []uint16{}, union, false, false},
		// LDX #0; JSR ($9000,X); RTS: the in-bank table target, a writer, is
		// decoded into the helper itself.
		{"in-bank jump table to a writer", []byte{0xa2, 0x00, 0x00, 0xfc, 0x00, 0x90, 0x60},
			func(image rom.Image) {
				copy(image[0x1000:], []byte{0x00, 0x89})
				copy(image[0x900:], []byte{0x9c, 0x61, 0x1c, 0x60})
			}, []uint16{0x8800, 0x8900}, union, false, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, _ := slotLifetimeFixture(t, tt.helper)
			if tt.setup != nil {
				tt.setup(image)
			}
			roots := tt.roots
			if roots == nil {
				roots = []uint16{0x8800}
			}
			graphs := decodeColdFixture(t, image, append([]uint16{0x8000, 0x8100}, roots...)...)
			got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
			if !slices.Equal(got.IndexedTargets, tt.want) {
				t.Fatalf("got %X want %X; %s", got.IndexedTargets, tt.want, got.Summary())
			}
			claimed := slices.Contains(got.Conditions, "local_slot_reaching_definition")
			if claimed != slices.Equal(tt.want, local) {
				t.Fatalf("local definition claimed=%v; conditions %v", claimed, got.Conditions)
			}
			if aliased := slices.Contains(got.Conditions, "slot_definition_assumes_no_aliased_write"); aliased != tt.aliased {
				t.Fatalf("aliased-write condition recorded=%v want %v", aliased, tt.aliased)
			}
			if hidden := slices.Contains(got.Conditions, "slot_definition_assumes_no_hidden_write"); hidden != tt.hidden {
				t.Fatalf("hidden-write condition recorded=%v want %v", hidden, tt.hidden)
			}
		})
	}
}

// A resolved long jump-table entry in another bank stays a separate graph,
// not part of the dispatching helper. A writer reached only through it still
// ends the local answer.
func TestColdSlotDefinitionCrossBankTableWriter(t *testing.T) {
	// LDX #0; JSR ($9000,X); RTS with a long table entry $01:8900.
	image, _ := slotLifetimeFixture(t, []byte{0xa2, 0x00, 0x00, 0xfc, 0x00, 0x90, 0x60})
	image = append(image, make(rom.Image, 0x8000)...)
	copy(image[0x1000:], []byte{0x00, 0x89, 0x01})
	copy(image[0x8900:], []byte{0x9c, 0x61, 0x1c, 0x6b}) // $01:8900: STZ $1C61; RTL
	graphs := decodeColdFixture(t, image, 0x8000, 0x8100)
	helper, err := decoder.DecodeFunction(image, 0, 0x8800, 0, 0, decoder.Options{MaxInstructions: 128,
		IndirectCallTables: map[uint32]decoder.IndirectCallTable{0x008803: {Base: 0x9000, Count: 1, Kind: "long"}}})
	if err != nil {
		t.Fatal(err)
	}
	writer, err := decoder.DecodeFunction(image, 1, 0x8900, 0, 0, decoder.Options{MaxInstructions: 128})
	if err != nil {
		t.Fatal(err)
	}
	if d := helper.Instructions[decoder.DecodeKey{PC: 0x008803}]; d == nil || !slices.Equal(d.Instruction.DispatchEntries, []uint32{0x018900}) {
		t.Fatalf("fixture: helper dispatch not resolved to $01:8900")
	}
	if _, inside := helper.Instructions[decoder.DecodeKey{PC: 0x018900}]; inside {
		t.Fatalf("fixture: the cross-bank entry was folded into the helper graph")
	}
	graphs = append(graphs, helper, writer)
	got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
	if want := []uint32{0x009100, 0x009110, 0x009200, 0x009210}; !slices.Equal(got.IndexedTargets, want) {
		t.Fatalf("got %X want %X; %s", got.IndexedTargets, want, got.Summary())
	}
	if slices.Contains(got.Conditions, "local_slot_reaching_definition") {
		t.Fatalf("a local definition survived a table-reached writer: %v", got.Conditions)
	}
}

// The same holds on the caller's own path between the store and the reload,
// where both callers replace the helper call with the write, except that a
// block move there ends the local answer.
func TestColdSlotDefinitionCallerPathAliases(t *testing.T) {
	local := []uint32{0x009100, 0x009110}
	union := []uint32{0x009100, 0x009110, 0x009200, 0x009210}
	for _, tt := range []struct {
		name    string
		store   []byte
		want    []uint32
		aliased bool
	}{
		{"indexed store can reach the slot", []byte{0x9d, 0x00, 0x1c}, local, true},
		{"indexed store above the slot", []byte{0x9d, 0x00, 0x1d}, local, false},
		{"block move into low WRAM", []byte{0x54, 0x00, 0x00}, union, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, _ := slotLifetimeFixture(t, []byte{0x60})
			copy(image[14:], tt.store)    // loader: JSR $8800 -> the write
			copy(image[0x112:], tt.store) // initializer: JSR $8800 -> the write
			graphs := decodeColdFixture(t, image, 0x8000, 0x8100)
			got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
			if !slices.Equal(got.IndexedTargets, tt.want) {
				t.Fatalf("got %X want %X; %s", got.IndexedTargets, tt.want, got.Summary())
			}
			if aliased := slices.Contains(got.Conditions, "slot_definition_assumes_no_aliased_write"); aliased != tt.aliased {
				t.Fatalf("aliased-write condition recorded=%v want %v", aliased, tt.aliased)
			}
		})
	}
}

// Linked records point forward to their successors. That forward pointer is a
// pointed-storage bound for a speculative record scan only: independently
// established records are read regardless.
func TestColdRecordFamilyBoundKeepsEstablishedRecords(t *testing.T) {
	image := make(rom.Image, 0x8000)
	image[0] = 0x60
	copy(image[0x2000:], []byte{0x10, 0xa0, 0x11, 0x11}) // $A000: next=$A010, payload
	copy(image[0x2010:], []byte{0x20, 0xa0, 0x22, 0x22}) // $A010: next=$A020, payload
	copy(image[0x2020:], []byte{0x00, 0x00, 0x33, 0x33}) // $A020: next=null, payload
	graphs := decodeColdFixture(t, image, 0x8000)
	for _, speculative := range []bool{false, true} {
		e := newColdValueEngine(image, nil, graphs, graphs)
		family := coldReadFamily{key: decoder.DecodeKey{PC: 0x8000}}
		var records []coldWordValue
		for _, word := range []uint16{0xa000, 0xa010, 0xa020} {
			records = append(records, coldWordValue{word: word, speculative: speculative})
		}
		e.noteRecordFamily(family, []byte{0}, 0, records)
		if st := e.families[family]; !st.has || st.bound != 0xa010 {
			t.Fatalf("expected a $A010 pointed-storage bound, got %+v", st)
		}
		want := 3
		if speculative {
			want = 1
		}
		if got := e.readWords(0, 2, records, family); len(got) != want {
			t.Fatalf("speculative=%v: read %d records, want %d", speculative, len(got), want)
		}
	}
}
