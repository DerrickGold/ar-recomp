package decoder

import (
	"reflect"
	"testing"
)

func TestStoredRecordFieldFeedsSeparateIndirectConsumer(t *testing.T) {
	for _, indexX := range []bool{false, true} {
		code := []byte{0x8b, 0x4b, 0xab, 0x0a, 0x0a, 0xa8, 0xbe, 0, 0x83, 0xb9, 2, 0x83, 0x8d, 0x40, 0, 0x8d, 0x42, 0, 0xab, 0x60}
		if indexX {
			code[5], code[6], code[9] = 0xaa, 0xbc, 0xbd
		}
		image := bank0(map[uint16][]byte{
			0x8000: code, 0x8100: {0x6c, 0x42, 0},
			0x8300: {0x34, 0x12, 0, 0x84, 0x78, 0x56, 0x10, 0x84},
			0x8400: {0x60, 0xea}, 0x8410: {0x60, 0xea},
		})
		writer := mustDecode(t, image, 0x8000, 0, 0)
		reader, err := DecodeFunction(image, 0x80, 0x8100, 0, 0, Options{})
		if err != nil {
			t.Fatal(err)
		}
		graphs := []*Graph{writer, reader, writer}
		if got := StoredTableDispatchTargets(image, graphs, nil); !reflect.DeepEqual(got, []uint32{0x808400, 0x808410}) {
			t.Fatalf("indexX=%v: got %X", indexX, got)
		}
		if got := StoredTableDispatchTargets(image, graphs, []DataRegion{{Bank: 0x80, Start: 0x8410, End: 0x8411}}); !reflect.DeepEqual(got, []uint32{0x808400}) {
			t.Fatalf("data barrier: %X", got)
		}
		if got := StoredTableDispatchTargets(image, []*Graph{writer}, nil); len(got) != 0 {
			t.Fatalf("table without a decoded slot consumer: %X", got)
		}
		for _, mutation := range []struct {
			offset int
			value  byte
		}{
			{1, 0xea},  // no PHK/PLB bank evidence
			{4, 0x69},  // not a consecutive-ASL stride
			{5, 0xea},  // no index transfer
			{16, 0x44}, // the consumed slot was not written
		} {
			old := image[mutation.offset]
			image[mutation.offset] = mutation.value
			g, err := DecodeFunction(image, 0, 0x8000, 0, 0, Options{})
			if err == nil && len(StoredTableDispatchTargets(image, []*Graph{g, reader}, nil)) != 0 {
				t.Fatalf("accepted clobber at %d", mutation.offset)
			}
			image[mutation.offset] = old
		}
		if got := StoredTableDispatchTargets(image, []*Graph{mustDecode(t, image, 0x8000, 1, 0), reader}, nil); len(got) != 0 {
			t.Fatalf("accepted byte writer: %X", got)
		}
	}
}
