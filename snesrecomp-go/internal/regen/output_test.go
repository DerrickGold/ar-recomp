package regen

import (
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
)

func TestMonoBankDeclaresEveryReferencedVariant(t *testing.T) {
	source := `/* header */

/* Forward declarations for in-bank entries. */
RecompReturn Caller_M1X1(CpuState *cpu);

RecompReturn Caller_M1X1(CpuState *cpu) {
  RecompReturn first = CrossBank_M0X1(cpu);
  RecompReturn second = Later_M1X0(cpu);
  return first != RECOMP_RETURN_NORMAL ? first : second;
}

RecompReturn Later_M1X0(CpuState *cpu) {
  return RECOMP_RETURN_NORMAL;
}
`
	outputs, err := splitBank(source, 0x01, []config.Entry{
		{Name: "Caller", Start: 0x8000},
		{Name: "Later", Start: 0x8100},
	}, len(source)+1, 0x800)
	if err != nil {
		t.Fatal(err)
	}
	got := outputs["bank01_v2.c"]
	for _, declaration := range []string{
		"RecompReturn CrossBank_M0X1(CpuState *cpu);",
		"RecompReturn Later_M1X0(CpuState *cpu);",
	} {
		if !strings.Contains(got, declaration) {
			t.Errorf("mono bank is missing %q:\n%s", declaration, got)
		}
	}
	declaration := strings.Index(got,
		"RecompReturn CrossBank_M0X1(CpuState *cpu);")
	definition := strings.Index(got,
		"RecompReturn Caller_M1X1(CpuState *cpu) {")
	if declaration < 0 || definition < 0 || declaration > definition {
		t.Fatalf("referenced declaration does not precede first definition:\n%s", got)
	}
}

func TestReferencedVariantDeclarationsAreSortedAndUnique(t *testing.T) {
	got := referencedVariantDeclarations(
		"Zed_M1X1(cpu); Alpha_M0X0(cpu); Zed_M1X1(cpu);")
	want := "RecompReturn Alpha_M0X0(CpuState *cpu);\n" +
		"RecompReturn Zed_M1X1(CpuState *cpu);\n"
	if got != want {
		t.Fatalf("declarations:\n%s\nwant:\n%s", got, want)
	}
}
