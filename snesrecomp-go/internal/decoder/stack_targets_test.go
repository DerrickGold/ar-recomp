package decoder

import (
	"reflect"
	"testing"
)

func TestPushedReturnAddressChains(t *testing.T) {
	for _, tt := range []struct {
		name string
		code []byte
		want []uint32
	}{
		{"immediate", []byte{0xf4, 0xff, 0x81, 0x60}, []uint32{0x8200}},
		{"chained", []byte{0xf4, 0xff, 0x82, 0xf4, 0xff, 0x81, 0x60}, []uint32{0x8200, 0x8300}},
		{"indirect call continuation", []byte{0xf4, 0xff, 0x81, 0x6c, 0x98, 0x00}, []uint32{0x8200}},
		{"consumed data", []byte{0xf4, 0xff, 0x81, 0x68, 0x60}, nil},
		{"unknown call", []byte{0xf4, 0xff, 0x81, 0x20, 0x00, 0x84, 0x60}, nil},
		{"stack alias", []byte{0xf4, 0xff, 0x81, 0x8d, 0xff, 0x01, 0x60}, nil},
		{"initialization loop", []byte{0xf4, 0xff, 0x82, 0xf4, 0xff, 0x81, 0xa2, 0x0a, 0x00, 0x9e, 0x00, 0x43, 0xca, 0x10, 0xfa, 0x60}, []uint32{0x8200, 0x8300}},
	} {
		t.Run(tt.name, func(t *testing.T) {
			g := mustDecode(t, bank0(map[uint16][]byte{0x8000: tt.code}), 0x8000, 0, 0)
			got := PushedReturnTargets(g)
			if len(got) == 0 && len(tt.want) == 0 {
				return
			}
			if !reflect.DeepEqual(got, tt.want) {
				t.Fatalf("got %X want %X", got, tt.want)
			}
		})
	}
}

func TestImmediateContinuationSurvivesUnrelatedWalkBudget(t *testing.T) {
	for _, opcode := range []byte{0x6c, 0x7c} {
		image := bank0(map[uint16][]byte{
			0x8000: {0xb0, 0x0b, // BCS immediate envelope
				0xf4, 0xff, 0x83, 0xa2, 0, 0x40, 0xca, 0xd0, 0xfd, 0x60, 0xea,
				0xf4, 0xff, 0x81, opcode, 0x98, 0},
		})
		g := mustDecode(t, image, 0x8000, 0, 0)
		for range 2 {
			// The 16384-step exact-index loop exhausts the old global walk.
			// Its dependent $8400 result stays unknown, while the separately
			// inventoried immediate envelope is retained in every graph order.
			if got := PushedReturnTargets(g); !reflect.DeepEqual(got, []uint32{0x8200}) {
				t.Fatalf("opcode %02X: got %X", opcode, got)
			}
			for a, b := 0, len(g.Order)-1; a < b; a, b = a+1, b-1 {
				g.Order[a], g.Order[b] = g.Order[b], g.Order[a]
			}
		}
	}
}
