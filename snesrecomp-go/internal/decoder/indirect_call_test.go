package decoder

import "testing"

func TestUnconfiguredIndirectCallRetainsLiveContinuation(t *testing.T) {
	image := bank0(map[uint16][]byte{0x8000: {0xfc, 6, 0, 0x85, 0x50, 0x86, 0x54, 0x60}})
	for m := uint8(0); m < 2; m++ {
		for x := uint8(0); x < 2; x++ {
			g, err := DecodeFunction(image, 0, 0x8000, m, x, Options{
				// The operand names a pointer, not a callee at $0006.
				CalleeExitMX: map[Variant]MX{{Address: 6, M: m, X: x}: {1, 1}},
			})
			if err != nil {
				t.Fatal(err)
			}
			call := g.Instructions[g.Entry]
			if !call.Instruction.DispatchOpen || len(call.Successors) != 4 || len(g.UnresolvedIndirects) != 1 || len(g.SuppressedIndirectCalls) != 0 {
				t.Fatalf("M%dX%d: call was suppressed or narrowed: %+v", m, x, call)
			}
			for rm := uint8(0); rm < 2; rm++ {
				for rx := uint8(0); rx < 2; rx++ {
					if _, ok := g.Instructions[DecodeKey{PC: 0x8003, M: rm, X: rx}]; !ok {
						t.Fatalf("missing live continuation M%dX%d", rm, rx)
					}
				}
			}
			if got := AnalyzeExitMX(g, nil); got != (MX{-1, -1}) {
				t.Fatalf("unknown indirect callee acquired a definite exit: %v", got)
			}
		}
	}
}
