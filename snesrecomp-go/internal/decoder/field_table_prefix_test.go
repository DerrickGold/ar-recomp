package decoder

import (
	"reflect"
	"testing"
)

func TestStackedFieldOpenPrefixExtendsOnlyCoherentAnchors(t *testing.T) {
	image := bank0(map[uint16][]byte{
		0x8000: {0xa9, 4, 0, 0x22, 0, 0x81, 0x80, 0xa9, 12, 0, 0x22, 0, 0x81, 0x80, 0x60},
		0x8100: {0x20, 0, 0x82, 0x6b},
		0x8200: {0x8b, 0x4b, 0xab, 0x5a, 0x9b, 0xaa, 0x96, 0, 0xbf, 0, 0x85, 0, 0x99, 2, 0, 0xe2, 0x20, 0xbf, 2, 0x85, 0, 0x99, 4, 0, 0x99, 5, 0, 0xc2, 0x20, 0xbb, 0x7a, 0xab, 0x60},
		0x8400: {0xb9, 4, 0, 0x48, 0xab, 0xb9, 2, 0, 0x48, 0xb9, 6, 0, 0x85, 0x6a, 0xb9, 0x38, 0, 0x29, 0xff, 0, 0x0a, 0xaa, 0x6b},
		0x8500: {0x1f, 0x83, 0, 0, 0x2f, 0x83, 0, 0, 0x3f, 0x83, 0, 0, 0x4f, 0x83, 0, 0, 0x5f, 0x83, 0, 0},
		0x8320: {0x60}, 0x8330: {0x60}, 0x8340: {0x60}, 0x8350: {0x60}, 0x8360: {0x60},
	})
	var graphs []*Graph
	for _, pc := range []uint16{0x8000, 0x8100, 0x8200, 0x8400} {
		graphs = append(graphs, mustDecode(t, image, pc, 0, 0))
	}
	bounded := []uint32{0x8320, 0x8330, 0x8340, 0x8350}
	if got := StackedFieldTableTargets(image, graphs); !reflect.DeepEqual(got, bounded) {
		t.Fatalf("bounded query changed: %X", got)
	}
	if got := StackedFieldTableTargetsWithOpenPrefixes(image, graphs, graphs, nil); !reflect.DeepEqual(got, append(append([]uint32{}, bounded...), 0x8360)) {
		t.Fatalf("open prefix: %X", got)
	}
	if got := StackedFieldTableTargetsWithOpenPrefixes(image, graphs, nil, nil); !reflect.DeepEqual(got, bounded) {
		t.Fatalf("ineligible writer extended: %X", got)
	}
	if got := StackedFieldTableTargetsWithOpenPrefixes(image, graphs, graphs[:3], nil); !reflect.DeepEqual(got, bounded) {
		t.Fatalf("ineligible consumer extended: %X", got)
	}
	if got := StackedFieldTableTargetsWithOpenPrefixes(image, graphs, graphs, []DataRegion{{Bank: 0, Start: 0x8360, End: 0x8361}}); !reflect.DeepEqual(got, bounded) {
		t.Fatalf("data target admitted: %X", got)
	}
	image[0x510], image[0x511] = 0xff, 0x84 // target is the source table itself
	if got := StackedFieldTableTargetsWithOpenPrefixes(image, graphs, graphs, nil); !reflect.DeepEqual(got, bounded) {
		t.Fatalf("scanned into table/code overlap: %X", got)
	}
	image[0x510], image[0x511] = 0x5f, 0x83
	image[8] = 4 // repeated uses of one index do not establish a stride
	duplicateAnchor := append([]*Graph{}, graphs...)
	duplicateAnchor[0] = mustDecode(t, image, 0x8000, 0, 0)
	if got := StackedFieldTableTargetsWithOpenPrefixes(image, duplicateAnchor, duplicateAnchor, nil); !reflect.DeepEqual(got, []uint32{0x8330}) {
		t.Fatalf("single anchor extended: %X", got)
	}
	image[8] = 12
	image[0x509] = 0x10 // invalid interior pointer, not a shorter inferred bound
	if got := StackedFieldTableTargetsWithOpenPrefixes(image, graphs, graphs, nil); !reflect.DeepEqual(got, []uint32{0x8330, 0x8350}) {
		t.Fatalf("bridged a prefix hole: %X", got)
	}
}

func TestStackedFieldOpenPrefixBeyondWindowNeedsCodeEnd(t *testing.T) {
	const records = 258
	build := func(entry uint16) (image []byte, graphs []*Graph) {
		table := make([]byte, records*4)
		for i := 0; i < records; i++ {
			word := uint16(0xa000+2*i) - 1
			table[i*4], table[i*4+1] = byte(word), byte(word>>8)
		}
		targets := make([]byte, 2*records+16)
		for i := range targets {
			targets[i] = 0x60
		}
		img := bank0(map[uint16][]byte{
			0x8000: {0xa9, 4, 0, 0x22, 0, 0x81, 0x80, 0xa9, 12, 0, 0x22, 0, 0x81, 0x80, 0x60},
			0x8100: {0x20, 0, 0x82, 0x6b},
			0x8200: {0x8b, 0x4b, 0xab, 0x5a, 0x9b, 0xaa, 0x96, 0, 0xbf, 0, 0x90, 0, 0x99, 2, 0, 0xe2, 0x20, 0xbf, 2, 0x90, 0, 0x99, 4, 0, 0x99, 5, 0, 0xc2, 0x20, 0xbb, 0x7a, 0xab, 0x60},
			0x8400: {0xb9, 4, 0, 0x48, 0xab, 0xb9, 2, 0, 0x48, 0xb9, 6, 0, 0x85, 0x6a, 0xb9, 0x38, 0, 0x29, 0xff, 0, 0x0a, 0xaa, 0x6b},
			0x9000: table,
			// The record after the table is invalid (its target is padding),
			// and it is also the first byte of a routine: NOP; LDA #0; RTS.
			0x9000 + records*4: {0xea, 0xa9, 0, 0, 0x60},
			0xa000:             targets,
		})
		for _, pc := range []uint16{0x8000, 0x8100, 0x8200, 0x8400} {
			graphs = append(graphs, mustDecode(t, img, pc, 0, 0))
		}
		if entry != 0 {
			graphs = append(graphs, mustDecode(t, img, entry, 0, 0))
		}
		return img, graphs
	}
	want := func(n int) []uint32 {
		var out []uint32
		for i := 0; i < n; i++ {
			out = append(out, uint32(0xa000+2*i))
		}
		return out
	}
	for _, tt := range []struct {
		name  string
		entry uint16
		n     int
	}{
		{"table ends at a decoded routine", 0x9000 + records*4, records},
		{"no routine at the table end", 0, 256},
		{"routine misaligned with the record grid", 0x9000 + records*4 + 1, 256},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, graphs := build(tt.entry)
			if got := StackedFieldTableTargetsWithOpenPrefixes(image, graphs, graphs, nil); !reflect.DeepEqual(got, want(tt.n)) {
				t.Fatalf("got %d targets, want %d: last %X", len(got), tt.n, got[len(got)-1])
			}
			if got := StackedFieldTableTargets(image, graphs); !reflect.DeepEqual(got, want(4)) {
				t.Fatalf("literal-bounded query changed: %X", got)
			}
		})
	}
}
