package decoder

import "testing"

func TestPushedReturnDoesNotSummarizeDispatchWidthAsExit(t *testing.T) {
	for _, push := range [][]byte{{0xd4, 0x40}, {0xf4, 0xff, 0x81}, {0xc2, 0x20, 0x48}, {0xda}} {
		code := append([]byte{0xe2, 0x20}, push...)
		code = append(code, 0x60)
		g := mustDecode(t, bank0(map[uint16][]byte{0x8000: code}), 0x8000, 0, 0)
		if modes, ok := AnalyzeExitModes(g, nil); !ok || len(modes) != 4 {
			t.Fatalf("push %x modes=%v complete=%v", push, modes, ok)
		}
		if got := AnalyzeExitMX(g, nil); got != (MX{-1, -1}) {
			t.Fatalf("push %x exit=%v", push, got)
		}
	}
	g := mustDecode(t, bank0(map[uint16][]byte{0x8000: {0xe2, 0x20, 0x48, 0x68, 0x60}}), 0x8000, 0, 0)
	if got := AnalyzeExitMX(g, nil); got != (MX{1, 0}) {
		t.Fatalf("balanced data push/pull lost exact exit: %v", got)
	}
	// An unrelated unresolved tail must not make the result depend on Go map
	// iteration order: the computed return's full uncertainty dominates.
	g = mustDecode(t, bank0(map[uint16][]byte{0x8000: {0xd0, 5, 0xe2, 0x20, 0xd4, 0x40, 0x60, 0x6c, 0x42, 0}}), 0x8000, 0, 0)
	for n := 0; n < 100; n++ {
		if modes, ok := AnalyzeExitModes(g, nil); !ok || len(modes) != 4 {
			t.Fatalf("mixed exits modes=%v complete=%v", modes, ok)
		}
	}
}

func TestUnknownCalleeExitRetainsLiveContinuationModes(t *testing.T) {
	for _, exit := range []MX{{-1, -1}, {-1, 0}, {1, -1}, {0, 0}} {
		image := bank0(map[uint16][]byte{0x8000: {0x22, 0, 0x81, 0x80, 0x85, 0x50, 0x86, 0x54, 0xc2, 0x30, 0x60}})
		g, err := DecodeFunction(image, 0, 0x8000, 0, 0, Options{CalleeExitMX: map[Variant]MX{{Address: 0x8100}: exit}})
		if err != nil {
			t.Fatal(err)
		}
		count := 0
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				want := (exit.M < 0 || m == uint8(exit.M)) && (exit.X < 0 || x == uint8(exit.X))
				_, got := g.Instructions[DecodeKey{PC: 0x8004, M: m, X: x}]
				if got != want {
					t.Fatalf("exit=%v continuation M%dX%d present=%v want=%v", exit, m, x, got, want)
				}
				if want {
					count++
				}
			}
		}
		if len(g.Instructions[g.Entry].Successors) != count {
			t.Fatal("lost live post-call edge")
		}
	}
}
