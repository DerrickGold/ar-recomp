package decoder

import (
	"reflect"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
)

func TestStoredAddressReferenceInventory(t *testing.T) {
	for _, tt := range []struct {
		name string
		code []byte
		body []byte
		want []uint32
	}{
		{"literal slot", []byte{0xa9, 0, 0x82, 0x85, 0x42, 0x6c, 0x42, 0}, nil, []uint32{0x8200}},
		{"dynamic scratch is not a global handler", []byte{0xa9, 0, 0x82, 0x85, 0x42, 0xa5, 0x10, 0x85, 0x42, 0x6c, 0x42, 0}, nil, nil},
		{"split long pointer", []byte{0xa9, 0, 0x82, 0x85, 0x4e, 0xa9, 1, 0, 0x85, 0x50, 0xdc, 0x4e, 0}, nil, []uint32{0x018200}},
		{"base addressed shift suffix", []byte{0x18, 0x69, 0, 0x82, 0x85, 0x1e, 0x6c, 0x1e, 0}, []byte{0x4a, 0x4a, 0x4a, 0x4a, 0x60}, []uint32{0x8200, 0x8201, 0x8202, 0x8203, 0x8204}},
		{"short shift sequence not an island", []byte{0x18, 0x69, 0, 0x82, 0x85, 0x1e, 0x6c, 0x1e, 0}, []byte{0x4a, 0x4a, 0x60}, nil},
		{"PEI initialized dense JMP island", []byte{0xa9, 0, 0x82, 0x85, 0x32, 0xd4, 0x32, 0x60}, []byte{0x4c, 0, 0x83, 0xea, 0x4c, 1, 0x83, 0xea, 0x4c, 2, 0x83, 0xea, 0}, []uint32{0x8200, 0x8204, 0x8208}},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image := bank0(map[uint16][]byte{0x8000: tt.code, 0x8200: tt.body})
			g := mustDecode(t, image, 0x8000, 0, 0)
			got := StoredAddressReferences(image, []*Graph{g})
			if len(got) == 0 && len(tt.want) == 0 {
				return
			}
			if !reflect.DeepEqual(got, tt.want) {
				t.Fatalf("got %X want %X", got, tt.want)
			}
		})
	}
}

func TestStoredHandlerJoinedLiteralPaths(t *testing.T) {
	for _, tt := range []struct {
		name string
		arm  []byte
		want []uint32
	}{
		{"two literals", []byte{0xa9, 0x40, 0x82}, []uint32{0x8200, 0x8240}},
		{"same literal", []byte{0xa9, 0, 0x82}, []uint32{0x8200}},
		{"unknown arm rejects union", []byte{0xa5, 0x10, 0xea}, nil},
		{"call clobber", []byte{0x20, 0, 0x83}, nil},
		{"register transfer clobber", []byte{0x8a, 0xea, 0xea}, nil},
		{"separate width keeps only the word path", []byte{0xe2, 0x20, 0xea}, []uint32{0x8200}},
		{"width restoration is a barrier", []byte{0xe2, 0x20, 0xc2, 0x20}, nil},
	} {
		t.Run(tt.name, func(t *testing.T) {
			code := []byte{0xd0, 5, 0xa9, 0, 0x82, 0x80, byte(len(tt.arm))}
			code = append(code, tt.arm...)
			code = append(code, 0x85, 0x42, 0x60)
			image := bank0(map[uint16][]byte{0x8000: code, 0x8100: {0x6c, 0x42, 0}, 0x8300: {0x60}})
			graphs := []*Graph{mustDecode(t, image, 0x8000, 0, 0), mustDecode(t, image, 0x8100, 0, 0)}
			got := StoredAddressReferences(image, graphs)
			if len(got) != len(tt.want) || len(got) != 0 && !reflect.DeepEqual(got, tt.want) {
				t.Fatalf("got %X want %X", got, tt.want)
			}
			slices.Reverse(graphs)
			if !reflect.DeepEqual(got, StoredAddressReferences(image, graphs)) {
				t.Fatal("graph order changes union")
			}
		})
	}
}

func TestShortHandlerSetterLiteralArgument(t *testing.T) {
	image := bank0(map[uint16][]byte{
		0x8000: {0xa9, 0, 0x82, 0x22, 0, 0x81, 0x80, 0x60},
		0x8100: {0x85, 0x42, 0x6b},
		0x8200: {0x6c, 0x42, 0},
	})
	graphs := []*Graph{mustDecode(t, image, 0x8000, 0, 0), mustDecode(t, image, 0x8100, 0, 0), mustDecode(t, image, 0x8200, 0, 0)}
	if got := StoredAddressReferences(image, graphs); !reflect.DeepEqual(got, []uint32{0x8200}) {
		t.Fatalf("short setter: %X", got)
	}
}

func TestStoredJoinedLiteralsFiniteBarriers(t *testing.T) {
	for _, distinct := range []bool{false, true} {
		g := &Graph{Entry: DecodeKey{PC: 0x8000}, Instructions: map[DecodeKey]*DecodedInstruction{}}
		at := DecodeKey{PC: 0x9000}
		pred := map[DecodeKey][]DecodeKey{}
		count := 129
		if distinct {
			count = 65
		}
		for n := 0; n < count; n++ {
			k := DecodeKey{PC: 0x9100 + uint32(n)}
			v := uint32(0x8200)
			if distinct {
				v += uint32(n)
			}
			pred[at] = append(pred[at], k)
			g.Instructions[k] = &DecodedInstruction{Key: k, Instruction: &cpu65816.Instruction{Mnemonic: "LDA", Mode: cpu65816.IMM, Operand: v}}
		}
		if v, ok := storedJoinedLiterals(g, pred, at, "A", 2); ok || len(v) != 0 {
			t.Fatalf("unbounded union distinct=%v: %X", distinct, v)
		}
	}
	// Original synthetic graph: two predecessor chains merging at a store.
	for _, tt := range []struct {
		name  string
		n     int
		cycle bool
	}{{"depth", 34, false}, {"cycle", 4, true}} {
		t.Run(tt.name, func(t *testing.T) {
			g := &Graph{Entry: DecodeKey{PC: 0x8000}, Instructions: map[DecodeKey]*DecodedInstruction{}}
			pred := map[DecodeKey][]DecodeKey{}
			at := DecodeKey{PC: 0x9000}
			for n := 0; n < tt.n; n++ {
				k := DecodeKey{PC: 0x9000 + uint32(n)}
				p := DecodeKey{PC: k.PC + 1}
				pred[k] = []DecodeKey{p}
				g.Instructions[p] = &DecodedInstruction{Key: p, Instruction: &cpu65816.Instruction{Mnemonic: "NOP"}}
			}
			last := DecodeKey{PC: 0x9000 + uint32(tt.n)}
			if tt.cycle {
				pred[last] = []DecodeKey{{PC: 0x9001}}
			} else {
				g.Instructions[last].Instruction = &cpu65816.Instruction{Mnemonic: "LDA", Mode: cpu65816.IMM, Operand: 0x8200}
			}
			if v, ok := storedJoinedLiterals(g, pred, at, "A", 2); ok || len(v) != 0 {
				t.Fatalf("accepted %s: %X", tt.name, v)
			}
		})
	}
}

func TestHandlerSetterLiteralArgumentsAcrossMirror(t *testing.T) {
	for _, clobber := range []bool{false, true} {
		caller := []byte{0xa9, 0, 0x82, 0xa2, 1, 0}
		if clobber {
			caller = append(caller, 0x8a)
		} // TXA is not a transparent A producer.
		caller = append(caller, 0x22, 0, 0x81, 0x80, 0x60)
		image := bank0(map[uint16][]byte{
			0x8000: caller,
			0x8100: {0x4c, 0x10, 0x81},
			0x8110: {0x4b, 0xab, 0x85, 0x4e, 0x86, 0x50, 0x60},
			0x8200: {0xdc, 0x4e, 0},
		})
		graphs := []*Graph{mustDecode(t, image, 0x8000, 0, 0), mustDecode(t, image, 0x8100, 0, 0), mustDecode(t, image, 0x8200, 0, 0)}
		got := StoredAddressReferences(image, graphs)
		if clobber {
			if len(got) != 0 {
				t.Fatalf("clobbered caller invented %X", got)
			}
		} else if !reflect.DeepEqual(got, []uint32{0x018200}) {
			t.Fatalf("literal setter: %X", got)
		}
	}
}
