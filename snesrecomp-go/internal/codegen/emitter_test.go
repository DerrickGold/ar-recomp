package codegen

import (
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/ir"
)

func emitted(t *testing.T, operation ir.Op) string {
	t.Helper()
	lines, err := EmitOperation(NewContext(), operation)
	if err != nil {
		t.Fatal(err)
	}
	return strings.Join(lines, "\n")
}

func TestCoreOperations(t *testing.T) {
	index := ir.X
	bank := byte(0x7e)
	cases := []struct {
		name      string
		operation ir.Op
		contains  []string
	}{
		{"read", ir.Read{Seg: ir.SegRef{Kind: ir.Direct, Offset: 0x42}, Width: 1, Out: ir.Value{ID: 1}}, []string{"cpu_read8", "_v1", "0x0042"}},
		{"indexed long", ir.Write{Seg: ir.SegRef{Kind: ir.Long, Offset: 0x1234, Bank: &bank, Index: &index}, Width: 2, Src: ir.Value{ID: 2}}, []string{"cpu_write16", "0x7e1234", "cpu->X"}},
		{"add", ir.Alu{Kind: ir.Add, LHS: ir.Value{ID: 1}, RHS: ir.Value{ID: 2}, Width: 1, Out: value(3)}, []string{"cpu->_flag_C", "_v3"}},
		{"shift", ir.Shift{Kind: ir.ASL, Src: ir.Value{ID: 1}, Width: 1, Out: ir.Value{ID: 2}}, []string{"<< 1", "_flag_C", "_flag_Z"}},
		{"xba", ir.ExchangeAccumulatorBytes{}, []string{"cpu->A =", "<< 8", ">> 8", "_flag_N"}},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			text := emitted(t, test.operation)
			for _, fragment := range test.contains {
				if !strings.Contains(text, fragment) {
					t.Errorf("missing %q in:\n%s", fragment, text)
				}
			}
		})
	}
}

func TestStaticStackWidthsAvoidRuntimeBranch(t *testing.T) {
	zero, one := uint8(0), uint8(1)
	word := emitted(t, ir.PushReg{Reg: ir.X, StaticX: &zero})
	if strings.Contains(word, "x_flag") || !strings.Contains(word, "cpu_write16") || strings.Contains(word, "cpu_write8") {
		t.Errorf("static X0 push is not a fixed word:\n%s", word)
	}
	bytePull := emitted(t, ir.PullReg{Reg: ir.A, StaticM: &one})
	if strings.Contains(bytePull, "m_flag") || !strings.Contains(bytePull, "cpu_read8") || strings.Contains(bytePull, "cpu_read16") {
		t.Errorf("static M1 pull is not a fixed byte:\n%s", bytePull)
	}
}

func value(id int) *ir.Value {
	v := ir.Value{ID: id}
	return &v
}
