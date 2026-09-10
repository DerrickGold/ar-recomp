package decoder

import (
	"bytes"
	"reflect"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func returnTableFixture(bank byte) rom.Image {
	image := make(rom.Image, (int(bank)+1)*0x8000)
	base := int(bank) * 0x8000
	copy(image[base:], []byte{0x22, 0, 0x83, bank, 8, 0x80, 0x10, 0x80, 0x6b})
	copy(image[base+0x10:], []byte{0xe8, 0x6b})
	copy(image[base+0x300:], []byte{
		0x08, 0xc2, 0x30, 0xda, 0x5a, 0x29, 0xff, 0, 0x0a, 0xa8, 0xc8,
		0x0b, 0x3b, 0x5b, 0xb7, 8, 0x85, 8, 0xc6, 8, 0x2b, 0x7a, 0xfa, 0x28, 0x6b,
	})
	return image
}

func TestNativeReturnTableContract(t *testing.T) {
	for _, bank := range []byte{0, 2} {
		image := returnTableFixture(bank)
		before := append([]byte(nil), image...)
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				g, err := DecodeFunction(image, bank, 0x8000, m, x, Options{NativeReturnTables: true})
				if err != nil {
					t.Fatal(err)
				}
				call := g.Instructions[g.Entry]
				r := call.Instruction.NativeReturnTable
				if r == nil || r.TablePC != Address24(bank, 0x8004) || r.ReturnPC != Address24(bank, 0x8318) ||
					!reflect.DeepEqual(r.Targets, []uint32{Address24(bank, 0x8008), Address24(bank, 0x8010)}) {
					t.Fatalf("bank %d M%dX%d: %+v", bank, m, x, r)
				}
				if len(g.Instructions) != 1 || len(call.Successors) != 0 {
					t.Fatal("inline data acquired a lexical successor")
				}
			}
		}
		if !bytes.Equal(image, before) {
			t.Fatal("analysis modified ROM")
		}
	}
}

func TestNativeReturnTableRefusals(t *testing.T) {
	for _, tc := range []struct {
		name   string
		offset int
		value  byte
	}{
		{"not JSL", 0, 0x20}, {"missing PHP", 0x300, 0xea},
		{"partial REP", 0x302, 0x20}, {"changed save order", 0x303, 0x5a},
		{"unbounded selector", 0x306, 0xfe}, {"missing scale", 0x308, 0xea},
		{"wrong table origin", 0x30a, 0x88}, {"wrong pointer slot", 0x30f, 7},
		{"wrong destination", 0x311, 9}, {"wrong adjustment", 0x312, 0xe6},
		{"wrong restore", 0x316, 0x7a}, {"not RTL", 0x318, 0x60},
	} {
		t.Run(tc.name, func(t *testing.T) {
			image := returnTableFixture(0)
			image[tc.offset] = tc.value
			g, err := DecodeFunction(image, 0, 0x8000, 0, 0, Options{NativeReturnTables: true})
			if err == nil && g.Instructions[g.Entry].Instruction.NativeReturnTable != nil {
				t.Fatal("near-match accepted as a native contract")
			}
		})
	}
	for _, barrier := range [][2]uint32{{0x8000, 0x8000}, {0x8300, 0x8300}, {0x8311, 0x8312}, {0x8318, 0x8400}} {
		g, err := DecodeFunction(returnTableFixture(0), 0, 0x8000, 0, 0, Options{NativeReturnTables: true, NativeReturnBarriers: [][2]uint32{barrier}})
		if err != nil {
			t.Fatal(err)
		}
		if g.Instructions[g.Entry].Instruction.NativeReturnTable != nil {
			t.Fatalf("override ignored: %x", barrier)
		}
	}
}

func TestNativeReturnTablePrefixIsNotRequiredForContract(t *testing.T) {
	image := returnTableFixture(0)
	image[4], image[5] = 0, 0 // a live unmapped target is still a transfer, never inline code
	g, err := DecodeFunction(image, 0, 0x8000, 1, 0, Options{NativeReturnTables: true})
	if err != nil {
		t.Fatal(err)
	}
	r := g.Instructions[g.Entry].Instruction.NativeReturnTable
	if r == nil || len(r.Targets) != 0 || len(g.Instructions) != 1 {
		t.Fatalf("empty open prefix: %+v", g)
	}
}
