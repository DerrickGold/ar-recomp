package decoder

import (
	"reflect"
	"testing"
)

func TestStackedFieldTableLiteralArguments(t *testing.T) {
	for _, tt := range []struct {
		name                         string
		unknown, clobber, wrongField bool
		prefix, invalidRecord        bool
	}{
		{name: "literal across mirrored wrapper"},
		{name: "unknown index never scans neighbors", unknown: true},
		{name: "bank index changed", clobber: true},
		{name: "different consumed bank field", wrongField: true},
		{name: "two literals anchor bounded structural prefix", prefix: true},
		{name: "invalid interior rejects entire structural prefix", prefix: true, invalidRecord: true},
	} {
		t.Run(tt.name, func(t *testing.T) {
			caller := []byte{0xa9, 4, 0, 0x22, 0, 0x81, 0x80, 0x60}
			if tt.unknown {
				caller = []byte{0xa5, 0x20, 0x22, 0, 0x81, 0x80, 0x60}
			}
			if tt.prefix {
				caller = []byte{0xa9, 4, 0, 0x22, 0, 0x81, 0x80, 0xa9, 12, 0, 0x22, 0, 0x81, 0x80, 0x60}
			}
			setter := []byte{0x8b, 0x4b, 0xab, 0x5a, 0x9b, 0xaa, 0x96, 0,
				0xbf, 0, 0x85, 0, 0x99, 2, 0, 0xe2, 0x20}
			if tt.clobber {
				setter = append(setter, 0xe8)
			}
			setter = append(setter, 0xbf, 2, 0x85, 0, 0x99, 4, 0, 0x99, 5, 0,
				0xc2, 0x20, 0xbb, 0x7a, 0xab, 0x60)
			consumer := []byte{0xb9, 4, 0, 0x48, 0xab, 0xb9, 2, 0, 0x48,
				0xb9, 6, 0, 0x85, 0x6a, 0xb9, 0x38, 0, 0x29, 0xff, 0, 0x0a, 0xaa, 0x6b}
			if tt.wrongField {
				consumer[1] = 6
			}
			table := []byte{0x1f, 0x83, 0, 0, 0x2f, 0x83, 0, 0, 0x3f, 0x83, 0, 0, 0x4f, 0x83, 0, 0}
			if tt.invalidRecord {
				table[9] = 0x10
			}
			image := bank0(map[uint16][]byte{0x8000: caller, 0x8100: {0x20, 0, 0x82, 0x6b},
				0x8200: setter, 0x8400: consumer,
				0x8500: table})
			var graphs []*Graph
			for _, entry := range []uint16{0x8000, 0x8100, 0x8200, 0x8400} {
				graphs = append(graphs, mustDecode(t, image, entry, 0, 0))
			}
			var want []uint32
			if !tt.unknown && !tt.clobber && !tt.wrongField {
				want = []uint32{0x8330}
				if tt.prefix {
					want = []uint32{0x8320, 0x8330, 0x8340, 0x8350}
				}
				if tt.invalidRecord {
					want = []uint32{0x8330, 0x8350}
				}
			}
			if got := StackedFieldTableTargets(image, graphs); !reflect.DeepEqual(got, want) {
				t.Fatalf("got %X want %X", got, want)
			}
		})
	}
}
