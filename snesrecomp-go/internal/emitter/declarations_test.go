package emitter

import (
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
)

func TestSymbolTableDeclaresUsedSymbolsInFixedOrder(t *testing.T) {
	symbols := NewSymbolTable()
	source := `RecompReturn sr_continuation_00_9000_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry) {
  return RECOMP_RETURN_NORMAL;
}
static inline RecompReturn sr_region_00_8000_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry) {
  return RECOMP_RETURN_NORMAL;
}
`
	if err := symbols.DeclareDefinitions(source); err != nil {
		t.Fatal(err)
	}
	if err := symbols.DeclareBankEntries(0x01, []config.Entry{
		{Name: "Zed", Start: 0x8000, EntryMX: config.MX{M: 1, X: 1}},
		{Start: 0x8100, EntryMX: config.MX{M: 0, X: 0}},
	}); err != nil {
		t.Fatal(err)
	}
	if err := symbols.DeclareVariant("Alpha_M0X0"); err != nil {
		t.Fatal(err)
	}
	uses := `
  _r = Zed_M1X1(cpu); _r = bank_01_8100_M0X0(cpu); _r = Zed_M1X1(cpu);
  RecompReturn (*handler)(CpuState *) = Alpha_M0X0;
  Zed(cpu);
  return sr_continuation_00_9000_M1X1(cpu, _entry_s, _hrv, 0);
  return sr_region_00_8000_M1X1(cpu, _entry_s, _hrv, 1);
`
	want := "RecompReturn Alpha_M0X0(CpuState *cpu);\n" +
		"RecompReturn Zed_M1X1(CpuState *cpu);\n" +
		"RecompReturn bank_01_8100_M0X0(CpuState *cpu);\n" +
		"void Zed(CpuState *cpu);\n" +
		"static inline RecompReturn sr_region_00_8000_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry);\n" +
		"RecompReturn sr_continuation_00_9000_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry);\n"
	if got := symbols.Declarations(uses); got != want {
		t.Fatalf("declarations:\n%s\nwant:\n%s", got, want)
	}
}

func TestSymbolTableIgnoresMentionsThatAreNotUses(t *testing.T) {
	symbols := NewSymbolTable()
	for _, name := range []string{"Named_M0X0", "Commented_M1X1", "Quoted_M0X1", "Char_M1X0"} {
		if err := symbols.DeclareVariant(name); err != nil {
			t.Fatal(err)
		}
	}
	source := `/* tail-call past end: into Commented_M1X1 at $8000 */
// Commented_M1X1(cpu);
sr_missing_mx_variant_warn(cpu, 0x008000u, 0, 1, "Quoted_M0X1(cpu) \" Quoted_M0X1");
char c = '\''; unsigned value = 0x8000u + 1e3;
_r = Named_M0X0(cpu);
`
	if got, want := symbols.Declarations(source), "RecompReturn Named_M0X0(CpuState *cpu);\n"; got != want {
		t.Fatalf("declarations:\n%s\nwant:\n%s", got, want)
	}
}

// A unit needs a declaration only for what it uses: a definition's own name
// is not a use, but a call ahead of the definition or a recursive call is.
func TestSymbolTableDeclaresUsesNotDefinitions(t *testing.T) {
	symbols := NewSymbolTable()
	source := `RecompReturn Quiet_M1X1(CpuState *cpu) {
  return RECOMP_RETURN_NORMAL;
}
RecompReturn Caller_M1X1(CpuState *cpu) {
  return Later_M0X0(cpu);
}
RecompReturn Later_M0X0(CpuState *cpu) {
  return Later_M0X0(cpu);
}
void Quiet(CpuState *cpu) {
  (void)cpu;
}
`
	if err := symbols.DeclareDefinitions(source); err != nil {
		t.Fatal(err)
	}
	if got, want := symbols.Declarations(source), "RecompReturn Later_M0X0(CpuState *cpu);\n"; got != want {
		t.Fatalf("declarations:\n%s\nwant:\n%s", got, want)
	}
}

// An authored symbol is never declared from its name. An HLE predicate
// returns bool and is declared at block scope by the body that calls it; a
// file-scope "RecompReturn" guess for it would be an incompatible redeclaration.
func TestSymbolTableLeavesAuthoredCalleesToTheirBodies(t *testing.T) {
	symbols := NewSymbolTable()
	for _, name := range []string{"Root_M1X1", "Native_M1X1"} {
		if err := symbols.DeclareVariant(name); err != nil {
			t.Fatal(err)
		}
	}
	source := `RecompReturn Root_M1X1(CpuState *cpu) {
  extern RecompReturn HostRoutine_M1X1(CpuState *cpu);
  extern bool HostPredicate_M0X0(CpuState *cpu);
  if (HostPredicate_M0X0(cpu)) {
    return HostRoutine_M1X1(cpu);
  }
  return Native_M1X1(cpu);
}
`
	if got, want := symbols.Declarations(source), "RecompReturn Native_M1X1(CpuState *cpu);\n"; got != want {
		t.Fatalf("declarations:\n%s\nwant:\n%s", got, want)
	}
}

func TestSymbolTableRejectsConflictingDeclarations(t *testing.T) {
	symbols := NewSymbolTable()
	if err := symbols.DeclareBankEntries(0x00, []config.Entry{{Name: "Reset", Start: 0x8000}}); err != nil {
		t.Fatal(err)
	}
	// Redeclaring an identical symbol is fine.
	if err := symbols.DeclareDefinitions("void Reset(CpuState *cpu) {\n}\nRecompReturn Reset_M0X0(CpuState *cpu) {\n}\n"); err != nil {
		t.Fatal(err)
	}
	// A void alias and a compiled variant never share a name.
	if err := symbols.DeclareDefinitions("RecompReturn Reset(CpuState *cpu) {\n}\n"); err == nil ||
		!strings.Contains(err.Error(), "conflicting declarations") {
		t.Fatalf("conflicting alias/variant declaration accepted: %v", err)
	}
	if err := symbols.Declare("sr_region_00_8000_M1X1", "static inline RecompReturn sr_region_00_8000_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry);"); err != nil {
		t.Fatal(err)
	}
	if err := symbols.Declare("sr_region_00_8000_M1X1", "RecompReturn sr_region_00_8000_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry);"); err == nil {
		t.Fatal("a private helper redeclared with external linkage was accepted")
	}
}

func TestDefaultBankHeaderDoesNotIncludeFuncsHeader(t *testing.T) {
	header := defaultBankHeader(0x03)
	if strings.Contains(header, "funcs.h") {
		t.Fatalf("generated units must not depend on the authored funcs.h:\n%s", header)
	}
	for _, include := range []string{
		`#include "snesrecomp/game/cpu.h"`,
		`#include "snesrecomp/game/trace.h"`,
		`#include "snesrecomp/game/generated_support.h"`,
	} {
		if !strings.Contains(header, include) {
			t.Fatalf("bank header lost %s:\n%s", include, header)
		}
	}
}
