package cfg

import (
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func decodeROM(t *testing.T, data []byte) *decoder.Graph {
	t.Helper()
	image := make(rom.Image, 0x8000)
	copy(image, data)
	graph, err := decoder.DecodeFunction(image, 0, 0x8000, 1, 1, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	return graph
}

func keyAt(graph *Graph, pc uint32) decoder.DecodeKey {
	for _, key := range graph.Order {
		if key.PC == pc {
			return key
		}
	}
	return decoder.DecodeKey{}
}

func TestLinearFunctionIsOneBlock(t *testing.T) {
	decoded := decodeROM(t, []byte{0xA9, 0x05, 0x85, 0x00, 0x60})
	graph := Build(decoded)
	if len(graph.Blocks) != 1 {
		t.Fatalf("blocks = %d, want 1", len(graph.Blocks))
	}
	block := graph.Blocks[decoded.Entry]
	if block == nil || len(block.Instructions) != 3 {
		t.Fatalf("entry block = %#v", block)
	}
	if graph.Dominators[decoded.Entry] != decoded.Entry {
		t.Errorf("entry does not dominate itself")
	}
}

func TestModeSplitMakesDistinctBlocks(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0xB0, 0x0A, 0xC2, 0x30, 0x80, 0x06})
	copy(image[0x0C:], []byte{0xEA, 0x60})
	decoded, err := decoder.DecodeFunction(image, 0, 0x8000, 1, 1, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	graph := Build(decoded)
	seen := make(map[[2]uint8]bool)
	for key := range graph.Blocks {
		if key.PC == 0x800C {
			seen[[2]uint8{key.M, key.X}] = true
			block := graph.Blocks[key]
			if len(block.Instructions) != 2 || block.Last().Instruction.Mnemonic != "RTS" {
				t.Errorf("unexpected block at $800C: %#v", block)
			}
		}
	}
	if len(seen) != 2 || !seen[[2]uint8{0, 0}] || !seen[[2]uint8{1, 1}] {
		t.Fatalf("mode blocks at $800C = %v", seen)
	}
}

func TestDiamondDominanceFrontier(t *testing.T) {
	decoded := decodeROM(t, []byte{
		0xF0, 0x04,
		0xA9, 0x01,
		0x80, 0x02,
		0xA9, 0x02,
		0x60,
	})
	graph := Build(decoded)
	left, right, join := keyAt(graph, 0x8002), keyAt(graph, 0x8006), keyAt(graph, 0x8008)
	if _, found := graph.DominanceFrontier[left][join]; !found {
		t.Errorf("left-arm frontier does not contain join")
	}
	if _, found := graph.DominanceFrontier[right][join]; !found {
		t.Errorf("right-arm frontier does not contain join")
	}
	if len(graph.DominanceFrontier[decoded.Entry]) != 0 {
		t.Errorf("entry frontier = %v, want empty", graph.DominanceFrontier[decoded.Entry])
	}
}
