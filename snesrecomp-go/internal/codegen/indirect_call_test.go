package codegen

import (
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/ir"
)

func TestIndirectCallNativeEnvelope(t *testing.T) {
	site, table := uint32(0x81fffd), uint16(0xffff)
	op := ir.Call{Indirect: true, SourcePC: &site, TableBase: &table}
	s := emitted(t, op)
	for _, want := range []string{
		"(uint16)(0xffffu + cpu->X)", "cpu_read16_bank_wrap(cpu, cpu->PB, _indirect_pointer)",
		"(((uint32)cpu->PB << 16) | 0x0000u), _entry_s, 2u", "cpu_dispatch_has_entry",
		"cpu_trace_trapped_dispatch", "return cpu_trace_unresolved_goto_trap", "cpu_dispatch_paired_tail_from",
		"if (_r == RECOMP_RETURN_PARKED_WAIT)", "if (!cpu_finish_owned_unwind(&_call_owner, cpu))",
		"if (!_call_owner.adjusted_return)", "cpu->S = _call_s;",
	} {
		if !strings.Contains(s, want) {
			t.Fatalf("missing %q:\n%s", want, s)
		}
	}
	// W65C816S table 5-7, addressing mode 2b: stack writes precede
	// the pointer reads. This matters when the pointer aliases the stack.
	if strings.Index(s, "cpu_read16_bank_wrap") < strings.LastIndex(s, "cpu_write8") ||
		strings.Contains(s, "SUPPRESSED") || strings.Contains(s, "cpu->DB") {
		t.Fatal("indirect JSR lost native pointer semantics")
	}
	for _, invalid := range []ir.Call{
		{Indirect: true}, {Indirect: true, SourcePC: &site},
		{Indirect: true, TableBase: &table}, {Indirect: true, SourcePC: &site, TableBase: &table, Long: true},
	} {
		if _, err := EmitOperation(NewContext(), invalid); err == nil {
			t.Fatal("incomplete indirect call metadata silently emitted")
		}
	}
}
