package tooling

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
)

func TestParseXrefQueryPreservesAddressWidth(t *testing.T) {
	tests := []struct {
		text    string
		address uint32
		bits    uint8
	}{
		{"$1C", 0x1c, 8},
		{"001C", 0x1c, 16},
		{"$00:C210", 0x00c210, 24},
		{"0x7E001C", 0x7e001c, 24},
	}
	for _, test := range tests {
		query, err := ParseXrefQuery(test.text)
		if err != nil {
			t.Fatalf("ParseXrefQuery(%q): %v", test.text, err)
		}
		if query.Address != test.address || query.Bits != test.bits {
			t.Fatalf("ParseXrefQuery(%q) = %+v, want address=%06X bits=%d", test.text, query, test.address, test.bits)
		}
	}
}

func TestBuildXrefUsesDecodedBoundariesAndDistinguishesWidths(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{
		0xA5, 0x1C, // LDA $1C
		0xD6, 0x1C, // DEC $1C,X
		0x85, 0x1C, // STA $1C
		0xAD, 0x1C, 0x00, // LDA $001C
		0x7C, 0x0C, 0x80, // JMP ($800C,X), inline table follows
		0x20, 0x80, 0x30, 0x80, // $8020, $8030
	})
	image[0x0020] = 0x60
	image[0x0030] = 0x60
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), "bank = 00\nfunc Root 8000 entry_mx:1,1\n")

	dp, err := BuildXref(XrefOptions{
		ROMPath: romPath, CFGDir: cfgDir, Jobs: 2,
		Query: XrefQuery{Address: 0x1c, Bits: 8},
	})
	if err != nil {
		t.Fatal(err)
	}
	if dp.Summary.References != 3 || dp.Summary.UniqueSourcePCs != 3 {
		t.Fatalf("direct-page summary = %+v, references=%+v", dp.Summary, dp.References)
	}
	want := []struct {
		pc                 uint32
		access, resolution string
	}{
		{0x008000, "read", "direct_page_offset"},
		{0x008002, "read_write", "indexed_direct_page_base"},
		{0x008004, "write", "direct_page_offset"},
	}
	for index, expected := range want {
		got := dp.References[index]
		if got.PC != expected.pc || got.Access != expected.access || got.Resolution != expected.resolution {
			t.Fatalf("reference[%d] = %+v, want pc=%06X access=%s resolution=%s", index, got, expected.pc, expected.access, expected.resolution)
		}
		if len(got.FunctionEntries) != 1 || got.FunctionEntries[0] != 0x008000 {
			t.Fatalf("reference[%d] owners = %v, want $00:8000", index, got.FunctionEntries)
		}
	}

	abs, err := BuildXref(XrefOptions{
		ROMPath: romPath, CFGDir: cfgDir, Jobs: 1,
		Query: XrefQuery{Address: 0x001c, Bits: 16},
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(abs.References) != 1 || abs.References[0].PC != 0x008006 || abs.References[0].Resolution != "absolute_db_operand" {
		t.Fatalf("absolute references = %+v", abs.References)
	}

	table, err := BuildXref(XrefOptions{
		ROMPath: romPath, CFGDir: cfgDir, Jobs: 1,
		Query: XrefQuery{Address: 0x00800c, Bits: 24},
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(table.References) != 1 || table.References[0].PC != 0x008009 || table.References[0].Access != "pointer_read" || table.References[0].Resolution != "program_bank_indexed_pointer_base" {
		t.Fatalf("table references = %+v", table.References)
	}
	var output bytes.Buffer
	if err := WriteXrefReport(&output, table, "text"); err != nil {
		t.Fatal(err)
	}
	for _, fragment := range []string{"xref v1", "$00:8009", "pointer_read", "owners=$00:8000", "bytes=7C 0C 80"} {
		if !strings.Contains(output.String(), fragment) {
			t.Fatalf("xref output missing %q:\n%s", fragment, output.String())
		}
	}

	shadow, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{
		ROMPath: romPath, CFGDir: cfgDir, Jobs: 2,
	})
	if err != nil {
		t.Fatal(err)
	}
	if shadow.Summary.CandidateTableSpans != 1 || shadow.Summary.ConfirmedTableSpans != 0 || len(shadow.TableSpans) != 1 {
		t.Fatalf("table ownership summary = %+v spans=%+v", shadow.Summary, shadow.TableSpans)
	}
	span := shadow.TableSpans[0]
	if span.SitePC != 0x008009 || span.StartPC != 0x00800c || span.EndExclusive != 0x008010 || span.EntryBytes != 2 || span.EntryCount != 2 || span.Ownership != shadowTableOwnershipCandidate {
		t.Fatalf("inline table span = %+v", span)
	}

	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), `bank = 00
func Root 8000 entry_mx:1,1
indirect_dispatch 8009 2 idx:X tables:800C transfer:tail
`)
	confirmed, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{
		ROMPath: romPath, CFGDir: cfgDir, Jobs: 2,
	})
	if err != nil {
		t.Fatal(err)
	}
	if confirmed.Summary.ConfirmedTableSpans != 1 || confirmed.Summary.CandidateTableSpans != 0 || len(confirmed.TableSpans) != 1 {
		t.Fatalf("confirmed table ownership summary = %+v spans=%+v", confirmed.Summary, confirmed.TableSpans)
	}
	if confirmed.TableSpans[0].Ownership != shadowTableOwnershipConfirmed || confirmed.TableSpans[0].Confidence != analysis.ConfidenceAuthored {
		t.Fatalf("confirmed inline table span = %+v", confirmed.TableSpans[0])
	}
}
