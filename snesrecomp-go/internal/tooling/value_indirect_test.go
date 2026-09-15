package tooling

import (
	"encoding/json"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func indirectValueFixture(t *testing.T, extra []byte) (rom.Image, []*decoder.Graph) {
	t.Helper()
	image := make(rom.Image, 0x8000)
	// Select a record arithmetically, save/reload its address, and publish a
	// table field. No authored dispatch entries or observed targets are used.
	copy(image, []byte{0x4b, 0xab, 0xd8, 0xa5, 0x70, 0x29, 1, 0,
		0x0a, 0x0a, 0x0a, 0x0a, 0x18, 0x69, 0, 0x90, 0x8d, 0, 4,
		0xac, 0, 4, 0xb9, 0x0b, 0, 0x8d, 2, 4, 0x60})
	// Three-byte state records: word handler / byte next state. A scratch
	// pointer is overwritten with the handler only AFTER the indirect reads.
	code := []byte{0x4b, 0xab, 0xd8, 0xa9, 2, 0, 0x8d, 4, 4,
		0xad, 4, 4, 0x0a, 0x18, 0x6d, 4, 4, 0x6d, 2, 4, 0x85, 0x42,
		0xa0, 2, 0, 0xb1, 0x42, 0x29, 0xff, 0, 0x8d, 4, 4}
	code = append(code, extra...)
	code = append(code, 0xb2, 0x42, 0x85, 0x42, 0xf4, 0xff, 0x81, 0x6c, 0x42, 0)
	copy(image[0x100:], code)
	copy(image[0x100b:], []byte{0, 0x91})
	copy(image[0x101b:], []byte{0, 0x92})
	for n := range 2 {
		copy(image[0x1100+n*0x100:], []byte{byte(n * 32), 0x94, 2, 0, 0, 0, byte(n*32 + 16), 0x94, 0, 0})
		image[0x1400+n*32], image[0x1410+n*32] = 0x60, 0x60
	}
	var graphs []*decoder.Graph
	for _, pc := range []uint16{0x8000, 0x8100} {
		g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{MaxInstructions: 128})
		if err != nil {
			t.Fatal(err)
		}
		graphs = append(graphs, g)
	}
	return image, graphs
}

func TestColdIndirectValueClosure(t *testing.T) {
	image, graphs := indirectValueFixture(t, nil)
	before, _ := json.Marshal(graphs)
	want := []uint32{0x009400, 0x009410, 0x009420, 0x009430}
	var first ColdValueInventory
	for n := range 2 {
		got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
		if !got.Converged || !slices.Equal(got.IndirectTargets, want) {
			t.Fatalf("got %X want %X: %s", got.IndirectTargets, want, got.Summary())
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

func TestColdIndirectPointerBarriers(t *testing.T) {
	for _, tt := range []struct {
		name  string
		extra []byte
	}{
		{"partial overwrite", []byte{0xe2, 0x20, 0x85, 0x43, 0xc2, 0x20}},
		{"indexed alias", []byte{0x95, 0x40}},
		{"indirect alias", []byte{0x92, 0x70}},
		{"RMW alias", []byte{0xe6, 0x42}},
		{"call", []byte{0x20, 0, 0x83, 0xc2, 0x30}},
		{"D changed", []byte{0xa9, 1, 0, 0x5b}},
		{"unknown DB", []byte{0xab}},
		{"hardware write", []byte{0x8d, 0x0b, 0x42}},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, graphs := indirectValueFixture(t, tt.extra)
			got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
			if len(got.IndirectTargets) != 0 {
				t.Fatalf("crossed %s: %X", tt.name, got.IndirectTargets)
			}
		})
	}
}

func TestColdIndirectValueOwnership(t *testing.T) {
	for _, kind := range []string{"HLE producer", "data target", "decimal", "unknown carry"} {
		t.Run(kind, func(t *testing.T) {
			image, graphs := indirectValueFixture(t, nil)
			configs := map[byte]*config.Config{}
			switch kind {
			case "HLE producer":
				configs[0] = &config.Config{HLEFunctions: map[uint16]string{0x8000: "Host"}}
			case "data target":
				configs[0] = &config.Config{DataRegions: []config.DataRegion{{Bank: 0, Start: 0x9400, End: 0x9430}}}
			case "decimal", "unknown carry":
				if kind == "decimal" {
					image[2] = 0xf8
				} else {
					image[12] = 0x28 // PLP, not a guessed clear carry
				}
				g, err := decoder.DecodeFunction(image, 0, 0x8000, 0, 0, decoder.Options{MaxInstructions: 128})
				if err != nil {
					t.Fatal(err)
				}
				graphs[0] = g
			}
			got := AnalyzeColdValueProvenance(image, configs, graphs, graphs)
			if len(got.IndirectTargets) != 0 {
				t.Fatalf("crossed %s: %X", kind, got.IndirectTargets)
			}
		})
	}
}

func TestColdIndirectMaskCycleNeedsAnIndependentSeed(t *testing.T) {
	image, graphs := indirectValueFixture(t, nil)
	copy(image[0x103:], []byte{0xad, 4, 4}) // self-copy, not literal state 2
	g, err := decoder.DecodeFunction(image, 0, 0x8100, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	graphs[1] = g
	got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
	if !got.Converged || len(got.IndirectTargets) != 0 {
		t.Fatalf("unseeded next-state byte manufactured a root: %s", got.Summary())
	}
}

func TestColdIndirectReadUsesMapperAndRejectsBankCrossing(t *testing.T) {
	for _, wrap := range []bool{false, true} {
		image := make(rom.Image, 0x400000)
		copy(image[0xffc0:], "SYNTHETIC HIROM TEST  ")
		image[0xffd5], image[0xffdc], image[0xffdd], image[0xfffd] = 0x31, 0xff, 0xff, 0x81
		pointer := uint16(0x1200)
		if wrap {
			pointer = 0xfffe
		}
		copy(image[0x8100:], []byte{0x4b, 0xab, 0xa9, 0, 0, 0x5b,
			0xa9, byte(pointer), byte(pointer >> 8), 0x85, 0x42, 0xa0, 2, 0,
			0xb1, 0x42, 0x85, 0x42, 0x6c, 0x42, 0})
		copy(image[0x1202:], []byte{0, 0x15})
		image[0x1500] = 0x60
		g, err := decoder.DecodeFunction(image, 0xc0, 0x8100, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		got := AnalyzeColdValueProvenance(image, nil, []*decoder.Graph{g}, []*decoder.Graph{g})
		if wrap && len(got.IndirectTargets) != 0 || !wrap && !slices.Equal(got.IndirectTargets, []uint32{0xc01500}) {
			t.Fatalf("wrap=%t mapped indirect read: %X; %s", wrap, got.IndirectTargets, got.Summary())
		}
	}
}

func TestColdValueADCChainedCarry(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0xd8, 0xa9, 0xff, 0xff, 0x18, 0x69, 2, 0, 0x69, 3, 0, 0x60})
	g, err := decoder.DecodeFunction(image, 0, 0x8000, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	e := newColdValueEngine(image, nil, []*decoder.Graph{g}, []*decoder.Graph{g})
	node := e.node(coldValueQuery{coldValueSite{0, decoder.DecodeKey{PC: 0x800b, M: 0, X: 0}}, "A", 0})
	e.solve()
	if got := e.nodes[node].values; len(got) != 1 || got[0].word != 5 {
		t.Fatalf("lost carry from preceding ADC: %+v", got)
	}
}

func TestColdValueGrowingCycleStaysUnknownAfterSaturation(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0xa9, 0, 0, 0x85, 0x70, 0x60})
	copy(image[0x100:], []byte{0xd8, 0xa5, 0x70, 0x18, 0x69, 1, 0, 0x85, 0x70, 0x60})
	var graphs []*decoder.Graph
	for _, pc := range []uint16{0x8000, 0x8100} {
		g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		graphs = append(graphs, g)
	}
	e := newColdValueEngine(image, nil, graphs, graphs)
	node := e.node(coldValueQuery{coldValueSite{1, decoder.DecodeKey{PC: 0x8107, M: 0, X: 0}}, "A", 0})
	e.solve()
	saturated := false
	for _, n := range e.nodes {
		saturated = saturated || n.saturated
	}
	if e.failed || !saturated || len(e.nodes[node].values) != 0 || e.work > 10000 {
		t.Fatalf("growing cycle reseeded after overflow: failed=%t node=%+v work=%d", e.failed, e.nodes[node], e.work)
	}
}
