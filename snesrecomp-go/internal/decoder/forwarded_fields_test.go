package decoder

import (
	"encoding/json"
	"reflect"
	"testing"
)

func TestForwardedIndirectFieldPaths(t *testing.T) {
	for _, tt := range []struct {
		name string
		code []byte
		want bool
	}{
		{"direct", []byte{0xb5, 0x28, 0x85, 0x62, 0x6c, 0x62, 0}, true},
		{"bank stack and native return envelope", []byte{0xb5, 0x28, 0xd0, 1, 0x60, 0x8b, 0x4b, 0xab, 0xda, 0x85, 0x62, 0xf4, 0x55, 0x88, 0x6c, 0x62, 0}, true},
		{"absolute field distinct from DP", []byte{0xbd, 0x28, 0x12, 0x85, 0x62, 0x6c, 0x72, 0}, true},
		{"immediate is not a field", []byte{0xa9, 0, 0x82, 0x85, 0x62, 0x6c, 0x62, 0}, false},
		{"stream Y load is not an object X field", []byte{0xb9, 0, 0, 0x85, 0x62, 0x6c, 0x62, 0}, false},
		{"A clobber", []byte{0xb5, 0x28, 0x8a, 0x85, 0x62, 0x6c, 0x62, 0}, false},
		{"scratch overwrite", []byte{0xb5, 0x28, 0x85, 0x62, 0x64, 0x62, 0x6c, 0x62, 0}, false},
		{"scratch reload", []byte{0xb5, 0x28, 0x85, 0x62, 0xa5, 0x80, 0x85, 0x62, 0x6c, 0x62, 0}, false},
		{"call barrier", []byte{0xb5, 0x28, 0x20, 0, 0x81, 0x85, 0x62, 0x6c, 0x62, 0}, false},
		{"width barrier", []byte{0xb5, 0x28, 0xe2, 0x20, 0x85, 0x62, 0x6c, 0x62, 0}, false},
		{"unknown write aliases scratch", []byte{0xb5, 0x28, 0x85, 0x62, 0x99, 0, 0, 0x6c, 0x62, 0}, false},
		{"unknown pull", []byte{0xb5, 0x28, 0x85, 0x62, 0x68, 0x6c, 0x62, 0}, false},
		{"long jump unsupported", []byte{0xb5, 0x28, 0x85, 0x62, 0xdc, 0x62, 0}, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			g := mustDecode(t, bank0(map[uint16][]byte{0x8000: tt.code, 0x8100: {0x60}}), 0x8000, 0, 0)
			snapshot := func() string {
				var instructions []*DecodedInstruction
				for _, key := range g.Order {
					instructions = append(instructions, g.Instructions[key])
				}
				data, err := json.Marshal(instructions)
				if err != nil {
					t.Fatal(err)
				}
				return string(data)
			}
			before := snapshot()
			fields := ForwardedIndirectFields(g)
			if (len(fields) == 1) != tt.want {
				t.Fatalf("fields=%+v", fields)
			}
			if tt.want {
				f := fields[0]
				if f.LoadPC != 0x8000 || f.Path[0] != f.LoadPC || f.Path[len(f.Path)-1] != f.DispatchPC || f.RequiredD != f.PointerAddress-f.ScratchOperand {
					t.Fatalf("bad evidence %+v", f)
				}
			}
			if before != snapshot() {
				t.Fatal("query changed graph")
			}
		})
	}
}

func TestForwardedFieldLiteralInventory(t *testing.T) {
	for _, tt := range []struct {
		name   string
		writer []byte
		want   bool
	}{
		{"literal", []byte{0xa9, 0, 0x82, 0x95, 0x28, 0x60}, true},
		{"X setup", []byte{0xa9, 0, 0x82, 0xa2, 0, 0x10, 0x95, 0x28, 0x60}, true},
		{"unrelated field", []byte{0xa9, 0, 0x82, 0x95, 0x2a, 0x60}, false},
		{"not same addressing mode", []byte{0xa9, 0, 0x82, 0x9d, 0x28, 0, 0x60}, false},
		{"data derived remains unknown", []byte{0xb9, 0, 0, 0x95, 0x28, 0x60}, false},
		{"callee result unknown", []byte{0xa9, 0, 0x82, 0x20, 0, 0x83, 0x95, 0x28, 0x60}, false},
		{"arithmetic unknown", []byte{0xa9, 0, 0x82, 0x69, 1, 0, 0x95, 0x28, 0x60}, false},
		{"register clobber", []byte{0xa9, 0, 0x82, 0x98, 0x95, 0x28, 0x60}, false},
		{"unmapped target", []byte{0xa9, 0, 0x12, 0x95, 0x28, 0x60}, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image := bank0(map[uint16][]byte{0x8000: tt.writer, 0x8100: {0xb5, 0x28, 0x85, 0x62, 0x6c, 0x62, 0}, 0x8200: {0x60}, 0x8300: {0x60}})
			a, b := mustDecode(t, image, 0x8000, 0, 0), mustDecode(t, image, 0x8100, 0, 0)
			targets := ForwardedFieldLiteralTargets(image, []*Graph{a, b, a})
			if tt.want {
				if !reflect.DeepEqual(targets, []uint32{0x8200}) {
					t.Fatalf("targets %X", targets)
				}
			} else if len(targets) != 0 {
				t.Fatalf("unproven value %X", targets)
			}
			if !reflect.DeepEqual(targets, ForwardedFieldLiteralTargets(image, []*Graph{b, a})) {
				t.Fatal("order/duplicates changed inventory")
			}
		})
	}
}

func TestForwardedFieldsEntryAndBudgetBarriers(t *testing.T) {
	image := bank0(map[uint16][]byte{0x8000: {0xb5, 0x28, 0x85, 0x62, 0x6c, 0x62, 0}})
	g := mustDecode(t, image, 0x8000, 0, 0)
	g.Entry = DecodeKey{PC: 0x8002}
	if len(ForwardedIndirectFields(g)) != 0 {
		t.Fatal("invented incoming A at external entry")
	}
	g = mustDecode(t, image, 0x8000, 0, 0)
	g.Instructions[DecodeKey{PC: 0x8000}].Successors = append(g.Instructions[DecodeKey{PC: 0x8000}].Successors, DecodeKey{PC: 0x8004})
	if len(ForwardedIndirectFields(g)) != 0 {
		t.Fatal("crossed a bypass of the scratch definition")
	}
	writer := []byte{}
	for i := 0; i < 65; i++ {
		writer = append(writer, 0xa9, byte(i), 0x82, 0x95, 0x28)
	}
	writer = append(writer, 0x60)
	image = bank0(map[uint16][]byte{0x8000: writer, 0x8400: {0xb5, 0x28, 0x85, 0x62, 0x6c, 0x62, 0}})
	if len(ForwardedFieldLiteralTargets(image, []*Graph{mustDecode(t, image, 0x8000, 0, 0), mustDecode(t, image, 0x8400, 0, 0)})) != 0 {
		t.Fatal("overflow retained arbitrary literal prefix")
	}
}
