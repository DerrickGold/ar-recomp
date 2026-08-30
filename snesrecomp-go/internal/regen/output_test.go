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

func TestSplitBankKeepsResumableRegionWrappersWithPrivateBody(t *testing.T) {
	source := `/* header */

/* Forward declarations for in-bank entries. */
RecompReturn Root_M1X1(CpuState *cpu);
RecompReturn Continuation_M1X1(CpuState *cpu);
RecompReturn Other_M1X1(CpuState *cpu);

RecompReturn Root_M1X1(CpuState *cpu) {
  /* resumable-region owner_pc:$8000 */
  return sr_region_00_8000_M1X1(cpu, _entry_s, _hrv, 0);
}
static inline RecompReturn sr_region_00_8000_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry) {
  return RECOMP_RETURN_NORMAL;
}

RecompReturn Continuation_M1X1(CpuState *cpu) {
  /* resumable-region owner_pc:$8000 */
  return sr_region_00_8000_M1X1(cpu, _entry_s, _hrv, 1);
}

RecompReturn Other_M1X1(CpuState *cpu) {
  return RECOMP_RETURN_NORMAL;
}
`
	outputs, err := splitBank(source, 0x00, []config.Entry{
		{Name: "Root", Start: 0x8000},
		{Name: "Continuation", Start: 0x9000},
		{Name: "Other", Start: 0x9002},
	}, 1, 0x800)
	if err != nil {
		t.Fatal(err)
	}
	regionChunk := outputs["bank00_part00_v2.c"]
	if !strings.Contains(regionChunk, "RecompReturn Continuation_M1X1") ||
		!strings.Contains(regionChunk, "static inline RecompReturn sr_region_00_8000_M1X1") {
		t.Fatalf("region wrapper and body were not grouped with their owner:\n%s", regionChunk)
	}
	if strings.Count(regionChunk, "static inline RecompReturn sr_region_00_8000_M1X1(CpuState *cpu") != 2 {
		t.Fatalf("region chunk should contain one prototype and one body:\n%s", regionChunk)
	}
	otherChunk := outputs["bank00_part02_v2.c"]
	if !strings.Contains(otherChunk, "RecompReturn Other_M1X1") ||
		strings.Contains(otherChunk, "Continuation_M1X1") ||
		strings.Contains(otherChunk, "sr_region_00_8000_M1X1") {
		t.Fatalf("unrelated chunk contains resumable-region material:\n%s", otherChunk)
	}
}
