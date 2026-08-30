package decoder

import (
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
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

func TestExplicitDispatchTransferMustMatchInstruction(t *testing.T) {
	tests := []struct {
		name        string
		instruction cpu65816.Instruction
		auth        DispatchAuth
		wantError   string
	}{
		{name: "jmp tail", instruction: cpu65816.Instruction{Address: 0x008498, Mnemonic: "JMP"}, auth: DispatchAuth{Transfer: "tail"}},
		{name: "jmp call", instruction: cpu65816.Instruction{Address: 0x008498, Mnemonic: "JMP"}, auth: DispatchAuth{Transfer: "call"}, wantError: "incompatible with JMP"},
		{name: "jsr call", instruction: cpu65816.Instruction{Address: 0x008498, Mnemonic: "JSR"}, auth: DispatchAuth{Transfer: "call"}},
		{name: "pha call needs return", instruction: cpu65816.Instruction{Address: 0x008498, Mnemonic: "PHA"}, auth: DispatchAuth{Transfer: "call"}, wantError: "needs ret:<pc>"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			err := validateDispatchTransfer(&test.instruction, test.auth)
			if test.wantError == "" && err != nil {
				t.Fatalf("validateDispatchTransfer: %v", err)
			}
			if test.wantError != "" && (err == nil || !strings.Contains(err.Error(), test.wantError)) {
				t.Fatalf("validateDispatchTransfer error = %v, want %q", err, test.wantError)
			}
		})
	}
}

func TestAutoDispatchTableRetainsZeroHolesUntilStructuralBoundary(t *testing.T) {
	table := make([]byte, 72*2)
	for index := 0; index < 72; index++ {
		target := uint16(0x8190)
		if index >= 12 && index <= 15 {
			target = 0
		}
		table[index*2] = byte(target)
		table[index*2+1] = byte(target >> 8)
	}
	image := bank0(map[uint16][]byte{
		0x8000: {0x7C, 0x00, 0x81}, // JMP ($8100,X)
		0x8100: table,
		// The table lands exactly here. A non-padding window also proves this is
		// a plausible handler rather than an all-zero/all-FF data region.
		0x8190: {0x60, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA,
			0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA},
	})

	graph, err := DecodeFunction(image, 0, 0x8000, 1, 0, Options{})
	if err != nil {
		t.Fatal(err)
	}
	decoded := graph.Instructions[DecodeKey{PC: 0x8000, M: 1, X: 0}]
	if decoded == nil {
		t.Fatalf("missing decoded dispatch: keys=%v", graph.Order)
	}
	entries := decoded.Instruction.DispatchEntries
	if len(entries) != 12 {
		t.Fatalf("compiled dispatch entries = %d, want conservative 12", len(entries))
	}
	candidates := decoded.Instruction.DispatchCandidateEntries
	if len(candidates) != 72 {
		t.Fatalf("analysis candidates = %d, want 72", len(candidates))
	}
	for index := 12; index <= 15; index++ {
		if candidates[index] != 0 {
			t.Errorf("candidate %d = %#06x, want zero hole", index, candidates[index])
		}
	}
	if candidates[16] != 0x8190 {
		t.Fatalf("candidate after zero run = %#06x, want $00:8190", candidates[16])
	}
	if decoded.Instruction.DispatchBound != "structural_candidate" {
		t.Fatalf("dispatch bound = %q, want structural_candidate", decoded.Instruction.DispatchBound)
	}
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

func TestInterproceduralPHPPLPRestoreDoesNotLeakCalleeWidth(t *testing.T) {
	image := bank0(map[uint16][]byte{
		// PHP; REP #$30; JSR balanced; JSR neutral; JSR target; PLP; RTS.
		0x8000: {0x08, 0xC2, 0x30, 0x20, 0x00, 0x81, 0x20, 0x00, 0x82, 0x20, 0x00, 0x83, 0x28, 0x60},
		// PHP; SEP #$30; PLP; RTS. The temporary M1X1 state must not escape.
		0x8100: {0x08, 0xE2, 0x30, 0x28, 0x60},
		0x8200: {0xEA, 0x60},
		0x8300: {0xEA, 0x60},
	})

	exits := make(map[Variant]MX)
	for _, address := range []uint16{0x8100, 0x8200, 0x8300} {
		graph, err := DecodeFunction(image, 0, address, 0, 0, Options{})
		if err != nil {
			t.Fatalf("DecodeFunction($%04X): %v", address, err)
		}
		exit := AnalyzeExitMX(graph, nil)
		if exit != (MX{M: 0, X: 0}) {
			t.Fatalf("exit M/X for $%04X = %+v, want M0X0", address, exit)
		}
		exits[Variant{Address: uint32(address), M: 0, X: 0}] = exit
	}

	caller, err := DecodeFunction(image, 0, 0x8000, 1, 1, Options{CalleeExitMX: exits})
	if err != nil {
		t.Fatalf("DecodeFunction(caller): %v", err)
	}
	call := DecodeKey{PC: 0x8009, M: 0, X: 0, PStack: 3, PDepth: 1}
	if caller.Instructions[call] == nil {
		t.Fatalf("third call did not retain M0X0 and outer PHP state: keys=%v", caller.Order)
	}
	plp := DecodeKey{PC: 0x800C, M: 0, X: 0, PStack: 3, PDepth: 1}
	if caller.Instructions[plp] == nil {
		t.Fatalf("PLP did not receive M0X0 after balanced callees: keys=%v", caller.Order)
	}
	rts := DecodeKey{PC: 0x800D, M: 1, X: 1}
	if caller.Instructions[rts] == nil {
		t.Fatalf("outer PLP did not restore entry M1X1: keys=%v", caller.Order)
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

func TestInternalResumePCDecodesThroughSiblingEntryBoundary(t *testing.T) {
	image := bank0(map[uint16][]byte{
		0x8000: {0x80, 0x0E}, // BRA $8010
		0x8010: {0xEA, 0x60}, // NOP; RTS
	})
	options := Options{SiblingEntryPCs: map[uint16]struct{}{0x8010: {}}}
	graph, err := DecodeFunction(image, 0, 0x8000, 1, 1, options)
	if err != nil {
		t.Fatal(err)
	}
	if keys := graph.KeysAtPC(0x8010); len(keys) != 0 {
		t.Fatalf("ordinary sibling entry was decoded into parent region: %v", keys)
	}

	options.InternalResumePCs = map[uint16]struct{}{0x8010: {}}
	graph, err = DecodeFunction(image, 0, 0x8000, 1, 1, options)
	if err != nil {
		t.Fatal(err)
	}
	if keys := graph.KeysAtPC(0x8010); len(keys) != 1 {
		t.Fatalf("internal resume entry keys = %v, want one local block", keys)
	}
}

func TestInternalResumeVariantRequiresExactLiveMX(t *testing.T) {
	image := bank0(map[uint16][]byte{
		0x8000: {0x80, 0x0E}, // BRA $8010
		0x8010: {0xEA, 0x60}, // NOP; RTS
	})
	options := Options{
		SiblingEntryPCs: map[uint16]struct{}{0x8010: {}},
		InternalResumeEdges: map[ResumeEdge]struct{}{
			{
				Source: Variant{Address: 0x008000, M: 0, X: 1},
				Target: Variant{Address: 0x008010, M: 0, X: 1},
			}: {},
		},
	}
	graph, err := DecodeFunction(image, 0, 0x8000, 1, 1, options)
	if err != nil {
		t.Fatal(err)
	}
	if keys := graph.KeysAtPC(0x8010); len(keys) != 0 {
		t.Fatalf("wrong-width continuation entered parent region: %v", keys)
	}

	graph, err = DecodeFunction(image, 0, 0x8000, 0, 1, options)
	if err != nil {
		t.Fatal(err)
	}
	if keys := graph.KeysAtPC(0x8010); len(keys) != 1 || keys[0].M != 0 || keys[0].X != 1 {
		t.Fatalf("exact-width continuation keys = %v, want M0X1 local block", keys)
	}
}
