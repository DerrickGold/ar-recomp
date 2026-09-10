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
		{"direct read stays in bank zero", ir.Read{Seg: ir.SegRef{Kind: ir.Direct, Offset: 0x42}, Width: 1, Out: ir.Value{ID: 1}}, []string{"cpu_read8(cpu, 0x00", "_v1", "cpu->D + 0x0042"}},
		{"indexed direct stays in bank zero", ir.Read{Seg: ir.SegRef{Kind: ir.Direct, Offset: 0x01, Index: &index}, Width: 1, Out: ir.Value{ID: 1}}, []string{"cpu_read8(cpu, 0x00", "cpu->D + 0x0001 + cpu->X"}},
		{"indexed direct write stays in bank zero", ir.Write{Seg: ir.SegRef{Kind: ir.Direct, Offset: 0x03, Index: &index}, Width: 2, Src: ir.Value{ID: 2}}, []string{"cpu_write16(cpu, 0x00", "cpu->D + 0x0003 + cpu->X"}},
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

func TestExactDirectCallMXEmitsSingleCompiledVariant(t *testing.T) {
	context := NewContext()
	context.ExactDirectCallMX = true
	context.ValidVariants[0x008200] = map[[2]uint8]struct{}{{0, 1}: {}}
	source, target := uint32(0x008000), uint32(0x008200)
	lines, err := EmitOperation(context, ir.Call{
		Target: &target, EntryM: 0, EntryX: 1, SourcePC: &source,
	})
	if err != nil {
		t.Fatal(err)
	}
	generated := strings.Join(lines, "\n")
	if !strings.Contains(generated, "bank_00_8200_M0X1(cpu);  /* exact live M/X direct call */") {
		t.Fatalf("exact direct call missing from:\n%s", generated)
	}
	if strings.Contains(generated, "switch (((cpu->m_flag") {
		t.Fatalf("exact direct call retained runtime variant switch:\n%s", generated)
	}
	if len(context.Demands) != 1 {
		t.Fatalf("exact direct call demands = %+v, want one variant", context.Demands)
	}
}

func TestExactDirectCallMXRefusesUnprovedSurvivor(t *testing.T) {
	context := NewContext()
	context.ExactDirectCallMX = true
	context.CurrentSite = 0x008000
	context.ValidVariants[0x008200] = map[[2]uint8]struct{}{{1, 1}: {}}
	source, target := uint32(0x008000), uint32(0x008200)
	_, err := EmitOperation(context, ir.Call{
		Target: &target, EntryM: 0, EntryX: 0, SourcePC: &source,
	})
	if err == nil || !strings.Contains(err.Error(), "requires $008200 M0X0") ||
		!strings.Contains(err.Error(), "without a variant-equivalence proof") {
		t.Fatalf("unproved exact-call route error = %v", err)
	}
}

func TestRuntimeVariantDispatchGuardsUnprovedWidths(t *testing.T) {
	context := NewContext()
	context.ValidVariants[0x008200] = map[[2]uint8]struct{}{{1, 1}: {}}
	guarded := strings.Join(VariantDispatchCases(
		context, 0x008200, "bank_00_8200", "  ", ""), "\n")
	if !strings.Contains(guarded,
		"case 0: _r = sr_missing_mx_variant_warn(cpu, 0x008200u, 0, 0") ||
		strings.Contains(guarded, "case 0: _r = bank_00_8200_M1X1") ||
		strings.Contains(guarded, "nearest survivor") {
		t.Fatalf("unproved runtime M/X route did not fail closed:\n%s", guarded)
	}

	context.ProvenEquivalent[0x008200] = map[[2]uint8]map[[2]uint8]struct{}{
		{0, 0}: {{1, 1}: {}},
	}
	proved := strings.Join(VariantDispatchCases(
		context, 0x008200, "bank_00_8200", "  ", ""), "\n")
	if !strings.Contains(proved,
		"case 0: _r = bank_00_8200_M1X1(cpu); break;  /* M0X0 -> proven-equivalent survivor M1X1 */") {
		t.Fatalf("proved runtime M/X route was not retained:\n%s", proved)
	}
}

func TestForcedDirectCallPreservesHardwareCallEnvelope(t *testing.T) {
	context := NewContext()
	context.CurrentName = "Caller_M0X0"
	context.CurrentSite = 0x00b543
	context.ForceVariantAt[context.CurrentSite] = [2]uint8{1, 0}
	source, target := uint32(0x00b543), uint32(0x0086ef)
	lines, err := EmitOperation(context, ir.Call{
		Target: &target, EntryM: 0, EntryX: 0, SourcePC: &source,
	})
	if err != nil {
		t.Fatal(err)
	}
	generated := strings.Join(lines, "\n")
	for _, wanted := range []string{
		"uint16 _call_s = cpu->S;",
		"JSR return frame -> cpu->S",
		"sr_call_mx_check(cpu, 1, 0",
		"bank_00_86EF_M1X0(cpu);  /* cfg force_variant_at $00B543",
		"cpu->S = _call_s;  /* stack-neutrality restore",
	} {
		if !strings.Contains(generated, wanted) {
			t.Fatalf("forced call missing %q from:\n%s", wanted, generated)
		}
	}
}

func TestDirectCallReturnOwnership(t *testing.T) {
	for _, tc := range []struct {
		name string
		pc   uint32
		long bool
		want string
	}{
		{"JSR live bank", 0x808123, false, "(((uint32)cpu->PB << 16) | 0x8126u), _entry_s, 2u"},
		{"JSR PC wrap", 0x80fffd, false, "(((uint32)cpu->PB << 16) | 0x0000u), _entry_s, 2u"},
		{"JSL full continuation", 0x808123, true, "0x808127u, _entry_s, 3u"},
		{"JSL PC wrap without bank carry", 0x80fffc, true, "0x800000u, _entry_s, 3u"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			ctx := NewContext()
			ctx.CurrentName = "Caller"
			target := uint32(0x008200)
			lines, err := EmitOperation(ctx, ir.Call{Target: &target, SourcePC: &tc.pc, Long: tc.long})
			if err != nil {
				t.Fatal(err)
			}
			s := strings.Join(lines, "\n")
			for _, want := range []string{tc.want, "CpuReturnScope _call_owner;", "cpu_return_scope_end(&_call_owner);", "if (!_call_owner.adjusted_return)"} {
				if !strings.Contains(s, want) {
					t.Fatalf("missing %q:\n%s", want, s)
				}
			}
			if strings.Index(s, "cpu_return_scope_end") > strings.Index(s, "if (_r != RECOMP_RETURN_NORMAL)") {
				t.Fatal("non-local exit bypasses scope teardown")
			}
			if strings.Index(s, "cpu_return_scope_begin") < strings.Index(s, "cpu->host_return_valid = 1") {
				t.Fatal("scope captured S before hardware frame push")
			}
			lines, err = EmitOperation(ctx, ir.Call{Target: &target, Long: tc.long})
			if err != nil {
				t.Fatal(err)
			}
			if strings.Contains(strings.Join(lines, "\n"), "_call_owner") {
				t.Fatal("unknown source PC manufactured an owned continuation")
			}
		})
	}
}

func value(id int) *ir.Value {
	v := ir.Value{ID: id}
	return &v
}
