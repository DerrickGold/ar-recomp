package decoder

import (
	"reflect"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestOpenRecordTableAndShiftSuffixTargets(t *testing.T) {
	image := bank0(map[uint16][]byte{
		0x8000: {0xa5, 0x40, 0x0a, 0x0a, 0xaa, 0x7c, 0, 0x81},
		0x8100: {0, 0x82, 1, 0, 0x10, 0x82, 0xff, 0xff, 0x20, 0x82, 0, 0, 0, 0x70},
		0x8200: {0x4a, 0x4a, 0x4a, 0x4a, 0x60},
		0x8210: {0x60, 0xea},
		0x8220: {0x60, 0xea},
	})
	graph := mustDecode(t, image, 0x8000, 0, 0)
	want := []uint32{0x8200, 0x8201, 0x8202, 0x8203, 0x8204, 0x8210, 0x8220}
	if got := OpenDispatchColdTargets(image, []*Graph{graph, graph}, nil); !reflect.DeepEqual(got, want) {
		t.Fatalf("got %x want %x", got, want)
	}
	regions := []DataRegion{{Bank: 0, Start: 0x8210, End: 0x8212}, {Bank: 0, Start: 0x8202, End: 0x8203}}
	if got := OpenDispatchColdTargets(image, []*Graph{graph}, regions); !reflect.DeepEqual(got, []uint32{0x8200}) {
		t.Fatalf("crossed a data boundary: %x", got)
	}
	// An intervening INX breaks the record stride, and a non-RTS suffix end
	// does not establish this deliberately narrow unrolled-return pattern.
	copy(image, []byte{0xa5, 0x40, 0x0a, 0x0a, 0xaa, 0xe8, 0x7c, 0, 0x81})
	image[0x204] = 0x6b
	if got := OpenDispatchColdTargets(image, []*Graph{mustDecode(t, image, 0x8000, 0, 0)}, nil); len(got) != 0 {
		t.Fatalf("accepted clobber/unterminated suffix: %x", got)
	}
}

func TestOpenRecordTableDoesNotAssumeADCCarry(t *testing.T) {
	image := bank0(map[uint16][]byte{
		0x8000: {0xa5, 0x40, 0x0a, 0x65, 0x40, 0x0a, 0xaa, 0x7c, 0, 0x81},
		0x8100: {0, 0x82, 1, 0, 2, 0, 0x10, 0x82},
		0x8200: {0x60, 0xea}, 0x8210: {0x60, 0xea},
	})
	if got := OpenDispatchColdTargets(image, []*Graph{mustDecode(t, image, 0x8000, 0, 0)}, nil); len(got) != 0 {
		t.Fatalf("assumed multiply-by-six without carry/decimal evidence: %x", got)
	}
}

func TestOpenTargetsUseMapperBelowLowBankROMWindow(t *testing.T) {
	image := make(rom.Image, 0x400000)
	copy(image[0xffc0:], "SYNTHETIC HIROM TEST  ")
	image[0xffd5], image[0xffd6] = 0x31, 2
	image[0xffdc], image[0xffdd] = 0xff, 0xff
	image[0xfffc], image[0xfffd] = 0x00, 0x81
	copy(image[0x1000:], []byte{0xa5, 0x40, 0x0a, 0x0a, 0xaa, 0x7c, 0, 0x11})
	copy(image[0x1100:], []byte{0, 0x12, 1, 0, 0x10, 0x12, 0, 0})
	copy(image[0x1200:], []byte{0x4a, 0x4a, 0x4a, 0x60})
	image[0x1210] = 0x60
	for _, bank := range []byte{0x40, 0xc0} {
		graph, err := DecodeFunction(image, bank, 0x1000, 0, 0, Options{})
		if err != nil {
			t.Fatal(err)
		}
		want := []uint32{0x1200, 0x1201, 0x1202, 0x1203, 0x1210}
		for i := range want {
			want[i] |= uint32(bank) << 16
		}
		if got := OpenDispatchColdTargets(image, []*Graph{graph}, nil); !reflect.DeepEqual(got, want) {
			t.Fatalf("bank %02X: got %X want %X", bank, got, want)
		}
	}
}

func TestNullableWordTableColdPrefix(t *testing.T) {
	for _, kind := range []string{"nullable", "authored", "no forward boundary", "invalid nonzero", "data boundary", "unscaled index"} {
		t.Run(kind, func(t *testing.T) {
			image := bank0(map[uint16][]byte{
				0x8000: {0xa4, 0x70, 0x0a, 0xaa, 0x7c, 0, 0x81},
				0x8080: {0x60}, 0x8090: {0x60},
				0x8100: {0, 0x82, 0x10, 0x82, 0, 0, 0x20, 0x82},
				0x8200: {0x60}, 0x8210: {0x60}, 0x8220: {0x60},
			})
			want := []uint32{0x8200, 0x8210, 0x8220}
			var regions []DataRegion
			switch kind {
			case "no forward boundary":
				copy(image[0x100:], []byte{0x80, 0x80, 0x90, 0x80})
				want = []uint32{0x8080, 0x8090}
			case "invalid nonzero":
				image[0x105] = 0x70
				want = []uint32{0x8200, 0x8210}
			case "data boundary":
				regions = []DataRegion{{Bank: 0, Start: 0x8220, End: 0x8221}}
				want = []uint32{0x8200, 0x8210}
			case "unscaled index":
				image[2] = 0xea
				want = nil
			case "authored":
				want = nil
			}
			g := mustDecode(t, image, 0x8000, 0, 0)
			if kind == "authored" {
				for _, d := range g.Instructions {
					if d.Instruction.Opcode == 0x7c {
						d.Instruction.DispatchOpen = false
						d.Instruction.DispatchKind = "authored"
						d.Instruction.DispatchEntries = []uint32{0x8200}
					}
				}
			}
			if got := OpenDispatchColdTargets(image, []*Graph{g, g}, regions); !slices.Equal(got, want) {
				t.Fatalf("got %X want %X", got, want)
			}
		})
	}
}
