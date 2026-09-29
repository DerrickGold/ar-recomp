package regen

import (
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/emitter"
)

// testSymbols builds the table regen would: this bank's entries, every
// definition in source, and the entries of any other banks the test names.
func testSymbols(t *testing.T, bank byte, entries []config.Entry, source string, others map[byte][]config.Entry) *emitter.SymbolTable {
	t.Helper()
	symbols := emitter.NewSymbolTable()
	if err := symbols.DeclareBankEntries(bank, entries); err != nil {
		t.Fatal(err)
	}
	for other, otherEntries := range others {
		if err := symbols.DeclareBankEntries(other, otherEntries); err != nil {
			t.Fatal(err)
		}
	}
	if err := symbols.DeclareDefinitions(source); err != nil {
		t.Fatal(err)
	}
	return symbols
}

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
	entries := []config.Entry{
		{Name: "Caller", Start: 0x8000, EntryMX: config.MX{M: 1, X: 1}},
		{Name: "Later", Start: 0x8100, EntryMX: config.MX{M: 1, X: 0}},
	}
	symbols := testSymbols(t, 0x01, entries, source, map[byte][]config.Entry{
		0x02: {{Name: "CrossBank", Start: 0x8000, EntryMX: config.MX{M: 0, X: 1}}},
	})
	outputs, _, err := splitBank(source, 0x01, entries, len(source)+1, 0x800, 0, symbols)
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
	entries := []config.Entry{
		{Name: "Root", Start: 0x8000, EntryMX: config.MX{M: 1, X: 1}},
		{Name: "Continuation", Start: 0x9000, EntryMX: config.MX{M: 1, X: 1}},
		{Name: "Other", Start: 0x9002, EntryMX: config.MX{M: 1, X: 1}},
	}
	outputs, _, err := splitBank(source, 0x00, entries, 1, 0x800, 0, testSymbols(t, 0x00, entries, source, nil))
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

func TestSplitBankDeclaresMultiOwnerContinuationHelperAcrossChunks(t *testing.T) {
	source := `/* header */

/* Forward declarations for in-bank entries. */
RecompReturn Root_M1X1(CpuState *cpu);
RecompReturn Target_M1X1(CpuState *cpu);

RecompReturn Root_M1X1(CpuState *cpu) {
  return sr_continuation_00_9000_M1X1(cpu, _entry_s, _hrv, 0);
}

RecompReturn Target_M1X1(CpuState *cpu) {
  /* resumable-region owner_pc:$9000 */
  return sr_continuation_00_9000_M1X1(cpu, _entry_s, _hrv, 0);
}
RecompReturn sr_continuation_00_9000_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry) {
  return RECOMP_RETURN_NORMAL;
}
`
	entries := []config.Entry{
		{Name: "Root", Start: 0x8000, EntryMX: config.MX{M: 1, X: 1}},
		{Name: "Target", Start: 0x9000, EntryMX: config.MX{M: 1, X: 1}},
	}
	outputs, _, err := splitBank(source, 0x00, entries, 1, 0x800, 0, testSymbols(t, 0x00, entries, source, nil))
	if err != nil {
		t.Fatal(err)
	}
	declaration := "RecompReturn sr_continuation_00_9000_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry);"
	rootChunk := outputs["bank00_part00_v2.c"]
	if !strings.Contains(rootChunk, declaration) || strings.Contains(rootChunk, declaration[:len(declaration)-1]+" {") {
		t.Fatalf("owner chunk lacks a declaration-only continuation helper:\n%s", rootChunk)
	}
	targetChunk := outputs["bank00_part02_v2.c"]
	if !strings.Contains(targetChunk, declaration) ||
		!strings.Contains(targetChunk, "RecompReturn sr_continuation_00_9000_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry) {") {
		t.Fatalf("target chunk lacks continuation helper declaration/definition:\n%s", targetChunk)
	}
}

// syntheticBank composes a bank of plain functions, one per (name, pc), each
// padded to about size bytes, the way ComposeBank lays them out.
func syntheticBank(t *testing.T, size int, functions ...struct {
	name string
	pc   uint16
}) (string, []config.Entry) {
	t.Helper()
	var entries []config.Entry
	var builder strings.Builder
	builder.WriteString("/* header */\n\n/* Forward declarations for in-bank entries. */\n")
	for _, function := range functions {
		entries = append(entries, config.Entry{Name: function.name, Start: function.pc, EntryMX: config.MX{M: 1, X: 1}})
		builder.WriteString("RecompReturn " + function.name + "_M1X1(CpuState *cpu);\n")
	}
	builder.WriteString("\n")
	for index, function := range functions {
		call := ""
		if index > 0 {
			call = "  (void)" + functions[index-1].name + "_M1X1(cpu);\n"
		}
		body := "RecompReturn " + function.name + "_M1X1(CpuState *cpu) {\n" + call
		padding := size - len(body) - len("  return RECOMP_RETURN_NORMAL;\n}\n")
		body += "  /*" + strings.Repeat(".", max(0, padding-6)) + " */\n  return RECOMP_RETURN_NORMAL;\n}\n"
		builder.WriteString(body + "\n")
	}
	return builder.String(), entries
}

type pcFunction = struct {
	name string
	pc   uint16
}

// unitFunctions lists the functions each unit defines, in order.
func unitFunctions(outputs map[string]string) map[string][]string {
	functions := make(map[string][]string)
	for name, content := range outputs {
		for _, match := range topLevelFunctionRE.FindAllStringSubmatch(content, -1) {
			functions[name] = append(functions[name], match[1])
		}
	}
	return functions
}

func TestSplitBankDividesAnOversizedChunkAtAlignedPCs(t *testing.T) {
	functions := []pcFunction{
		{"A", 0x8000}, {"B", 0x8010}, {"C", 0x8100}, {"D", 0x8110},
		{"E", 0x8400}, {"F", 0x8410}, {"G", 0x8600}, {"H", 0x87f0},
	}
	source, entries := syntheticBank(t, 1000, functions...)
	symbols := testSymbols(t, 0x00, entries, source, nil)
	outputs, oversized, err := splitBank(source, 0x00, entries, 1, 0x800, 2100, symbols)
	if err != nil {
		t.Fatal(err)
	}
	if len(oversized) != 0 {
		t.Fatalf("divisible chunk reported oversized: %v", oversized)
	}
	// $8000-$87FF holds 8 KB: halves of $400 still exceed 2100 bytes, so
	// each is halved again at $8200 and $8600; every piece is <= 2 functions.
	want := map[string][]string{
		"bank00_part00_8000_v2.c": {"A_M1X1", "B_M1X1"},
		"bank00_part00_8100_v2.c": {"C_M1X1", "D_M1X1"},
		"bank00_part00_8400_v2.c": {"E_M1X1", "F_M1X1"},
		"bank00_part00_8600_v2.c": {"G_M1X1", "H_M1X1"},
	}
	got := unitFunctions(outputs)
	if len(got) != len(want) {
		t.Fatalf("units %v, want %v", got, want)
	}
	for name, functions := range want {
		if strings.Join(got[name], ",") != strings.Join(functions, ",") {
			t.Fatalf("%s defines %v, want %v\nall units: %v", name, got[name], functions, got)
		}
	}
	// Bodies move whole and verbatim, and every unit declares what it calls
	// in another unit.
	for name, content := range outputs {
		for _, function := range functions {
			definition := "RecompReturn " + function.name + "_M1X1(CpuState *cpu) {"
			if strings.Contains(content, definition) && !strings.Contains(source, content[strings.Index(content, definition):strings.Index(content, definition)+len(definition)+40]) {
				t.Fatalf("%s altered %s", name, function.name)
			}
		}
		if !strings.HasPrefix(content, "/* header */\n\n/* Split translation unit: bank $00, part 00; entry PCs $") {
			t.Fatalf("%s header:\n%s", name, content[:120])
		}
	}
	if !strings.Contains(outputs["bank00_part00_8100_v2.c"], "RecompReturn B_M1X1(CpuState *cpu);") {
		t.Fatalf("a cross-unit callee is undeclared:\n%s", outputs["bank00_part00_8100_v2.c"])
	}
	// Byte-stable: the same input produces the same units.
	again, _, err := splitBank(source, 0x00, entries, 1, 0x800, 2100, symbols)
	if err != nil {
		t.Fatal(err)
	}
	for name, content := range outputs {
		if again[name] != content {
			t.Fatalf("%s differs between identical runs", name)
		}
	}
}

func TestSplitBankLeavesChunksWithinTheLimitUnchanged(t *testing.T) {
	source, entries := syntheticBank(t, 1000, pcFunction{"A", 0x8000}, pcFunction{"B", 0x8800}, pcFunction{"C", 0x8810})
	symbols := testSymbols(t, 0x00, entries, source, nil)
	unlimited, _, err := splitBank(source, 0x00, entries, 1, 0x800, 0, symbols)
	if err != nil {
		t.Fatal(err)
	}
	limited, oversized, err := splitBank(source, 0x00, entries, 1, 0x800, 4096, symbols)
	if err != nil || len(oversized) != 0 {
		t.Fatalf("%v %v", err, oversized)
	}
	if len(limited) != 2 || limited["bank00_part00_v2.c"] != unlimited["bank00_part00_v2.c"] ||
		limited["bank00_part01_v2.c"] != unlimited["bank00_part01_v2.c"] {
		t.Fatalf("a chunk within the limit changed:\n%v", unitFunctions(limited))
	}
}

func TestSplitBankDivisionIsLocalToTheEditedRange(t *testing.T) {
	base := []pcFunction{{"A", 0x8000}, {"B", 0x8100}, {"C", 0x8400}, {"D", 0x8500}}
	source, entries := syntheticBank(t, 1000, base...)
	before, _, err := splitBank(source, 0x00, entries, 1, 0x800, 2100, testSymbols(t, 0x00, entries, source, nil))
	if err != nil {
		t.Fatal(err)
	}
	// A function added in the upper half leaves the lower half's units as
	// they were: boundaries are fixed PCs, not packing positions.
	edited, editedEntries := syntheticBank(t, 1000, append(append([]pcFunction(nil), base...), pcFunction{"E", 0x8600})...)
	after, _, err := splitBank(edited, 0x00, editedEntries, 1, 0x800, 2100, testSymbols(t, 0x00, editedEntries, edited, nil))
	if err != nil {
		t.Fatal(err)
	}
	if before["bank00_part00_8000_v2.c"] == "" || before["bank00_part00_8000_v2.c"] != after["bank00_part00_8000_v2.c"] {
		t.Fatalf("an edit above $8400 changed the $8000 unit:\nbefore %v\nafter %v", unitFunctions(before), unitFunctions(after))
	}
}

func TestSplitBankPacksOnePCsFunctionsAndReportsWhatCannotBeDivided(t *testing.T) {
	source := `/* header */

/* Forward declarations for in-bank entries. */
RecompReturn Root_M1X1(CpuState *cpu);

RecompReturn Root_M0X0(CpuState *cpu) {
  /*` + strings.Repeat(".", 900) + ` */
  return RECOMP_RETURN_NORMAL;
}

RecompReturn Root_M1X1(CpuState *cpu) {
  /* resumable-region owner_pc:$8000 */
  return sr_region_00_8000_M1X1(cpu, _entry_s, _hrv, 0);
}

static inline RecompReturn sr_region_00_8000_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry) {
  /*` + strings.Repeat(".", 1500) + ` */
  return RECOMP_RETURN_NORMAL;
}

RecompReturn Root_M0X1(CpuState *cpu) {
  /*` + strings.Repeat(".", 900) + ` */
  return RECOMP_RETURN_NORMAL;
}

RecompReturn Continuation_M1X1(CpuState *cpu) {
  /* resumable-region owner_pc:$8000 */
  return sr_region_00_8000_M1X1(cpu, _entry_s, _hrv, 1);
}

void Root(CpuState *cpu) {
  RecompReturn _r = Root_M1X1(cpu);
  (void)_r;
}
`
	entries := []config.Entry{
		{Name: "Root", Start: 0x8000, EntryMX: config.MX{M: 1, X: 1}},
		{Name: "Continuation", Start: 0x8200, EntryMX: config.MX{M: 1, X: 1}},
	}
	outputs, oversized, err := splitBank(source, 0x00, entries, 1, 0x800, 1200, testSymbols(t, 0x00, entries, source, nil))
	if err != nil {
		t.Fatal(err)
	}
	// Everything shares the owner's PC. The region (owner wrapper, private
	// body and the continuation wrapper) is one piece and too large to meet
	// the limit; the other variants and the alias are packed around it.
	got := unitFunctions(outputs)
	want := map[string][]string{
		"bank00_part00_8000_00_v2.c": {"Root_M0X0"},
		"bank00_part00_8000_01_v2.c": {"Root_M1X1", "Continuation_M1X1"},
		"bank00_part00_8000_02_v2.c": {"Root_M0X1", "Root"},
	}
	if len(got) != len(want) {
		t.Fatalf("units %v, want %v", got, want)
	}
	for name, functions := range want {
		if strings.Join(got[name], ",") != strings.Join(functions, ",") {
			t.Fatalf("%s defines %v, want %v", name, got[name], functions)
		}
	}
	region := outputs["bank00_part00_8000_01_v2.c"]
	if !strings.Contains(region, "static inline RecompReturn sr_region_00_8000_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry) {") ||
		!strings.Contains(region, "entry PC $8000, piece 2 of 3") {
		t.Fatalf("region unit:\n%s", region)
	}
	for name, content := range outputs {
		if name != "bank00_part00_8000_01_v2.c" && strings.Contains(content, "sr_region_00_8000_M1X1") {
			t.Fatalf("%s refers to another unit's private region body", name)
		}
	}
	if len(oversized) != 1 || !strings.Contains(oversized[0], "bank00_part00_8000_01_v2.c") ||
		!strings.Contains(oversized[0], "resumable region owned by $8000 (2 functions and its shared body)") ||
		!strings.Contains(oversized[0], "over the 1 KiB unit limit") {
		t.Fatalf("oversized report: %v", oversized)
	}

	// A lone function over the limit keeps its chunk's name and is named in
	// the report.
	single, _ := syntheticBank(t, 3000, pcFunction{"Huge", 0x8000})
	singleEntries := []config.Entry{{Name: "Huge", Start: 0x8000, EntryMX: config.MX{M: 1, X: 1}}}
	outputs, oversized, err = splitBank(single, 0x00, singleEntries, 1, 0x800, 1024, testSymbols(t, 0x00, singleEntries, single, nil))
	if err != nil {
		t.Fatal(err)
	}
	if outputs["bank00_part00_v2.c"] == "" || len(oversized) != 1 ||
		!strings.Contains(oversized[0], "bank00_part00_v2.c holds 2 KiB of function source, over the 1 KiB unit limit: it is the single function Huge_M1X1") {
		t.Fatalf("units %v, report %v", unitFunctions(outputs), oversized)
	}
}

func TestSplitBankDividesAMonoBankOverTheLimit(t *testing.T) {
	source, entries := syntheticBank(t, 1000, pcFunction{"A", 0x8000}, pcFunction{"B", 0x8400})
	symbols := testSymbols(t, 0x00, entries, source, nil)
	// Below the chunk threshold but over a smaller unit limit.
	outputs, _, err := splitBank(source, 0x00, entries, 1<<20, 0x800, 1500, symbols)
	if err != nil {
		t.Fatal(err)
	}
	if _, mono := outputs["bank00_v2.c"]; mono || len(outputs) != 2 {
		t.Fatalf("units %v", unitFunctions(outputs))
	}
	outputs, _, err = splitBank(source, 0x00, entries, 1<<20, 0x800, 0, symbols)
	if err != nil || len(outputs) != 1 || outputs["bank00_v2.c"] == "" {
		t.Fatalf("without a limit the bank stays one unit: %v %v", unitFunctions(outputs), err)
	}
}
