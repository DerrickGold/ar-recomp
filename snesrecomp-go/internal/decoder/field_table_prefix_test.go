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
