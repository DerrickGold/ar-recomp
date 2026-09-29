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
		`uint16 _call_s = sr_call_enter(cpu, &_call_owner, 0xb545u, 0xb546u, _entry_s, 1, 0, "Caller_M0X0", 0x00b543u);`,
		"bank_00_86EF_M1X0(cpu);  /* cfg force_variant_at $00B543",
		"int _k = sr_call_leave(cpu, &_call_owner, _r, _call_s);",
	} {
		if !strings.Contains(generated, wanted) {
			t.Fatalf("forced call missing %q from:\n%s", wanted, generated)
		}
	}
}

// A direct call with a known source runs its hardware frame, ownership scope
// and result handling in the runtime (call_boundary.c, whose ordering the
// runtime's call_boundary test checks against the inline envelope). The site
// must pass that envelope the exact frame, continuation and bank, keep the
// compiled callee selection between enter and leave, and pop its activation
// only on the path that returns.
func TestDirectCallReturnOwnership(t *testing.T) {
	for _, tc := range []struct {
		name  string
		pc    uint32
		long  bool
		enter string
	}{
		{"JSR live bank", 0x808123, false, `sr_call_enter(cpu, &_call_owner, 0x8125u, 0x8126u, _entry_s, 0, 0, "Caller", 0x000000u);`},
		{"JSR PC wrap", 0x80fffd, false, `sr_call_enter(cpu, &_call_owner, 0xffffu, 0x0000u, _entry_s, 0, 0, "Caller", 0x000000u);`},
		{"JSL live mirrored bank", 0x808123, true, `sr_call_enter_long(cpu, &_call_owner, &_saved_pb, 0x00, 0x8126u, 0x8127u, _entry_s, 0, 0, "Caller", 0x000000u);`},
		{"JSL PC wrap without bank carry", 0x80fffc, true, `sr_call_enter_long(cpu, &_call_owner, &_saved_pb, 0x00, 0xffffu, 0x0000u, _entry_s, 0, 0, "Caller", 0x000000u);`},
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
			leave := "int _k = sr_call_leave(cpu, &_call_owner, _r, _call_s);"
			if tc.long {
				leave = "int _k = sr_call_leave_long(cpu, &_call_owner, _r, _call_s, _saved_pb);"
			}
			pop := "if (_k >= 0) { RecompStackPop(); return (RecompReturn)_k; }"
			order := []string{"CpuReturnScope _call_owner;", tc.enter, "case 3: _r = bank_00_8200_M1X1(cpu); break;", leave, pop}
			at := -1
			for _, want := range order {
				next := strings.Index(s, want)
				if next < 0 {
					t.Fatalf("missing %q:\n%s", want, s)
				}
				if next < at {
					t.Fatalf("%q is out of order:\n%s", want, s)
				}
				at = next
			}
			for _, inline := range []string{"cpu_return_scope_begin", "cpu_write8", "cpu_finish_owned_unwind", "cpu->S = _call_s", "cpu->PB ="} {
				if strings.Contains(s, inline) {
					t.Fatalf("outlined call still carries %q inline:\n%s", inline, s)
				}
			}
			if strings.Count(s, "RecompStackPop") != 1 || strings.Count(s, "return ") != 1 {
				t.Fatalf("the site must pop and return on exactly one path:\n%s", s)
			}
			lines, err = EmitOperation(ctx, ir.Call{Target: &target, Long: tc.long})
			if err != nil {
				t.Fatal(err)
			}
			synthetic := strings.Join(lines, "\n")
			if strings.Contains(synthetic, "_call_owner") || strings.Contains(synthetic, "sr_call_enter") {
				t.Fatal("unknown source PC manufactured an owned continuation")
			}
		})
	}
}

func value(id int) *ir.Value {
	v := ir.Value{ID: id}
	return &v
}
