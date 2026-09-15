package tooling

import (
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestColdValueSeedIsNotPreviousLoopState(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0xb9, 0, 0x90, 0xc8, 0x80, 0xfa})
	copy(image[0x1000:], []byte{0x34, 0x12, 0x56})
	g, err := decoder.DecodeFunction(image, 0, 0x8000, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	graphs := []*decoder.Graph{g}
	e := newColdValueEngine(image, nil, graphs, graphs)
	e.addEntrySeed(coldValueEntrySeed{graph: 0, y: 0, bank: 0, parentFetch: 0x9100})
	unbound := e.node(coldValueQuery{coldValueSite{0, g.Entry}, "Y", 0})
	read := e.node(coldValueQuery{coldValueSite{0, g.Entry}, "read", e.entrySeeds[0][0]})
	e.solve()
	if len(e.nodes[unbound].values) != 0 {
		t.Fatal("loop invented an entry input")
	}
	if v := e.nodes[read].values; len(v) != 1 || v[0].word != 0x1234 {
		t.Fatalf("lost external seed or used next iteration: %v", v)
	}
}
