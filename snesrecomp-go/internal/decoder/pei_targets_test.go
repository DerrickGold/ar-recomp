package decoder

import (
	"reflect"
	"testing"
)

func TestPEIReturnTargets(t *testing.T) {
	// A 16-bit initializer supplies the high byte, an unrelated stream byte
	// supplies the low byte through a mask/shift. Values are return-minus-one.
	prefix := []byte{0xa9, 0x01, 0x82, 0x85, 0x36, 0xe2, 0x20}
	suffix := []byte{0xa5, 0x22, 0x29, 0xf0, 0x4a, 0x4a, 0x85, 0x36, 0xd4, 0x36, 0x60}
	for _, tt := range []struct {
		name   string
		middle []byte
		want   bool
	}{
		{"bounded partial store", nil, true},
		{"bank setup retains open inventory", []byte{0x48, 0xab}, true},
		{"unknown call invalidates", []byte{0x20, 0x00, 0x90}, false},
		{"direct page relocation invalidates", []byte{0x5b}, false},
		{"absolute alias invalidates", []byte{0x8d, 0x37, 0x00}, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			code := append(append(append([]byte{}, prefix...), tt.middle...), suffix...)
			g := mustDecode(t, bank0(map[uint16][]byte{0x8000: code, 0x9000: {0x60}}), 0x8000, 0, 0)
			var want []uint32
			if tt.want {
				for n := uint32(0); n < 16; n++ {
					want = append(want, 0x8201+4*n)
				}
			}
			if got := PEIReturnTargets(g); !reflect.DeepEqual(got, want) {
				t.Fatalf("got %X want %X", got, want)
			}
		})
	}
}

func TestPEIByteLoadDoesNotInventAccumulatorHighByte(t *testing.T) {
	image := bank0(map[uint16][]byte{0x8000: {0xe2, 0x20, 0xa9, 0x42,
		0xc2, 0x20, 0x85, 0x36, 0xd4, 0x36, 0x60}})
	if got := PEIReturnTargets(mustDecode(t, image, 0x8000, 0, 0)); len(got) != 0 {
		t.Fatalf("M8 immediate invented a known high byte: %X", got)
	}
}
