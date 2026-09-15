package decoder

import "testing"

func TestAutomaticJumpTargetsAreOpen(t *testing.T) {
	for _, opcode := range []byte{0x6c, 0x7c} {
		image := bank0(map[uint16][]byte{
			0x8000: {opcode, 0, 0x81},
			0x8100: {0, 0x82},
			0x8200: {0x60, 0xea},
		})
		key := DecodeKey{PC: 0x8000, M: 0, X: 0}
		for _, authored := range []bool{false, true} {
			options := Options{}
			if authored {
				options.IndirectDispatch = map[uint32]DispatchAuth{0x8000: {Count: 1, IndexReg: "X", TableBases: []uint16{0x8100}}}
			}
			graph, err := DecodeFunction(image, 0, 0x8000, 0, 0, options)
			if err != nil {
				t.Fatal(err)
			}
			i := graph.Instructions[key].Instruction
			if len(i.DispatchEntries) != 1 || i.DispatchEntries[0] != 0x8200 || i.DispatchOpen == authored {
				t.Fatalf("opcode=%02x authored=%v entries=%v open=%v", opcode, authored, i.DispatchEntries, i.DispatchOpen)
			}
			if !authored {
				if modes, complete := AnalyzeExitModes(graph, nil); !complete || len(modes) != 4 {
					t.Fatal("an open prefix cannot prove all possible handler exit widths")
				}
			}
		}
		graph, err := DecodeFunction(image, 0, 0x8000, 0, 0, Options{HLEDispatch: map[uint16]string{0x8000: "AuthoredDispatch"}})
		if err != nil {
			t.Fatal(err)
		}
		if i := graph.Instructions[key].Instruction; len(i.DispatchEntries) != 0 || i.DispatchOpen || len(graph.UnresolvedIndirects) != 0 {
			t.Fatal("automatic target inventory replaced authored HLE dispatch")
		}
	}
}
