package decoder

import (
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func bank0(chunks map[uint16][]byte) rom.Image {
	image := make(rom.Image, 0x8000)
	for address, data := range chunks {
		copy(image[int(address)-0x8000:], data)
	}
	return image
}

func mustDecode(t *testing.T, image rom.Image, start uint16, m, x uint8) *Graph {
	t.Helper()
	graph, err := DecodeFunction(image, 0, start, m, x, Options{})
	if err != nil {
		t.Fatalf("DecodeFunction: %v", err)
	}
	return graph
}

func TestModeSplitPreservesBothStates(t *testing.T) {
	graph := mustDecode(t, bank0(map[uint16][]byte{
		0x8000: {0xC2, 0x30, 0xB0, 0x0C, 0xE2, 0x30, 0x80, 0x08},
		0x8010: {0xEA, 0x60},
	}), 0x8000, 1, 1)

	keys := graph.KeysAtPC(0x8010)
	if len(keys) != 2 {
		t.Fatalf("keys at $8010 = %v, want two mode variants", keys)
	}
	want := map[[2]uint8]bool{{0, 0}: true, {1, 1}: true}
	for _, key := range keys {
		if !want[[2]uint8{key.M, key.X}] {
			t.Errorf("unexpected mode at $8010: M=%d X=%d", key.M, key.X)
		}
		instruction := graph.Instructions[key].Instruction
		if instruction.M != key.M || instruction.X != key.X {
			t.Errorf("instruction state M=%d X=%d differs from key %v", instruction.M, instruction.X, key)
		}
	}
}

func TestPHPPLPRestoresMode(t *testing.T) {
	graph := mustDecode(t, bank0(map[uint16][]byte{
		0x8000: {0x08, 0xE2, 0x30, 0x28, 0x60},
	}), 0x8000, 0, 0)

	plp := DecodeKey{PC: 0x8003, M: 1, X: 1, PStack: 0, PDepth: 1}
	if graph.Instructions[plp] == nil {
		t.Fatalf("missing PLP with saved M0X0: keys=%v", graph.Order)
	}
	rts := DecodeKey{PC: 0x8004, M: 0, X: 0}
	if graph.Instructions[rts] == nil {
		t.Fatalf("missing RTS with PLP-restored M0X0: keys=%v", graph.Order)
	}
}

func TestConstantZFoldPrunesDeadPath(t *testing.T) {
	graph := mustDecode(t, bank0(map[uint16][]byte{
		0x8000: {
			0xA2, 0x01,
			0xD0, 0x04,
			0xA9, 0xFF,
			0xEA,
			0x60,
			0xEA,
			0x60,
		},
	}), 0x8000, 1, 1)

	branch := graph.Instructions[DecodeKey{PC: 0x8002, M: 1, X: 1}]
	if branch == nil || len(branch.Successors) != 1 || branch.Successors[0].PC != 0x8008 {
		t.Fatalf("BNE successors = %#v, want only $8008", branch)
	}
	for _, deadPC := range []uint32{0x8004, 0x8006, 0x8007} {
		if keys := graph.KeysAtPC(deadPC); len(keys) != 0 {
			t.Errorf("dead PC $%04X was not pruned: %v", deadPC, keys)
		}
	}
	if len(graph.ConstantZFolds) != 1 {
		t.Fatalf("constant-Z folds = %d, want 1", len(graph.ConstantZFolds))
	}
	fold := graph.ConstantZFolds[0]
	if fold.BranchMnemonic != "BNE" || fold.PreviousMnemonic != "LDX" || fold.PreviousImmediate != 1 || fold.LivePC != 0x8008 {
		t.Errorf("unexpected fold record: %+v", fold)
	}
}

func TestDecodeOrderIsRepeatable(t *testing.T) {
	image := bank0(map[uint16][]byte{
		0x8000: {0xC2, 0x30, 0xB0, 0x0C, 0xE2, 0x30, 0x80, 0x08},
		0x8010: {0xEA, 0x60},
	})
	first := mustDecode(t, image, 0x8000, 1, 1).Order
	for iteration := 0; iteration < 20; iteration++ {
		got := mustDecode(t, image, 0x8000, 1, 1).Order
		if len(got) != len(first) {
			t.Fatalf("iteration %d order length %d, want %d", iteration, len(got), len(first))
		}
		for index := range first {
			if got[index] != first[index] {
				t.Fatalf("iteration %d key %d = %v, want %v", iteration, index, got[index], first[index])
			}
		}
	}
}
