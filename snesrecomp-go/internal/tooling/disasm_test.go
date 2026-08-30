package tooling

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestParseProgramAddressRequiresBank(t *testing.T) {
	for _, value := range []string{"01:9C6F", "$019C6F", "0x01_9C6F"} {
		got, err := ParseProgramAddress(value)
		if err != nil || got != 0x019c6f {
			t.Fatalf("ParseProgramAddress(%q) = %06X, %v", value, got, err)
		}
	}
	if _, err := ParseProgramAddress("9C6F"); err == nil {
		t.Fatal("16-bit program address unexpectedly accepted without a bank")
	}
}

func TestBuildDisassemblyTracksWidthsTargetsAndMetadata(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image, []byte{
		0xc2, 0x20, // REP #$20: M=0
		0xa9, 0x34, 0x12, // LDA #$1234
		0xe2, 0x20, // SEP #$20: M=1
		0xa9, 0x56, // LDA #$56
		0x20, 0x10, 0x80, // JSR $8010
		0x80, 0x02, // BRA $8010
	})
	image[0x10] = 0x60
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	metadataPath := filepath.Join(root, "gen_meta.json")
	metadata := GeneratedMetadata{
		Functions: map[string][]string{"008000": {"_M1X1"}, "008010": {"_M1X1"}},
		Labels:    map[string][]string{},
	}
	encoded, err := json.Marshal(metadata)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(metadataPath, encoded, 0o644); err != nil {
		t.Fatal(err)
	}

	report, err := BuildDisassembly(DisassemblyOptions{
		ROMPath: romPath, MetadataPath: metadataPath, StartPC: 0x008000,
		EntryM: 1, EntryX: 1, Count: 12, UntilFlow: true,
	})
	if err != nil {
		t.Fatal(err)
	}
	if report.StopReason != "flow_end" || len(report.Instructions) != 6 {
		t.Fatalf("report stop=%q instructions=%d", report.StopReason, len(report.Instructions))
	}
	if got := report.Instructions[1]; got.PC != 0x008002 || got.Formatted != "LDA   #$1234" || got.LiveMX.M != 0 {
		t.Fatalf("wide instruction = %+v", got)
	}
	if got := report.Instructions[3]; got.PC != 0x008007 || got.Formatted != "LDA   #$56" || got.LiveMX.M != 1 {
		t.Fatalf("narrow instruction = %+v", got)
	}
	call := report.Instructions[4]
	if call.Target == nil || *call.Target != 0x008010 || !strings.Contains(strings.Join(call.Annotations, " "), "->FUNC $00:8010") {
		t.Fatalf("call target/annotations = %+v", call)
	}
	if got := strings.Join(report.Instructions[0].Annotations, " "); got != "FUNC[M1X1]" {
		t.Fatalf("entry annotation = %q", got)
	}
	branch := report.Instructions[5]
	if branch.Target == nil || *branch.Target != 0x008010 || branch.Formatted != "BRA   $8010" {
		t.Fatalf("branch = %+v", branch)
	}

	var output bytes.Buffer
	if err := WriteDisassemblyReport(&output, report, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, fragment := range []string{"00:8000", "C2 20", "REP   #$20", "m=1 x=1", "->FUNC $00:8010"} {
		if !strings.Contains(output.String(), fragment) {
			t.Fatalf("disassembly output missing %q:\n%s", fragment, output.String())
		}
	}
}

func TestBuildDisassemblyReportsArchitecturalJML(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image, []byte{0x5c, 0x34, 0x92, 0x03})
	copy(image[0x10:], []byte{0xdc, 0x34, 0x12})
	romPath := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	report, err := BuildDisassembly(DisassemblyOptions{ROMPath: romPath, StartPC: 0x008000, EntryM: 1, EntryX: 1, Count: 1})
	if err != nil {
		t.Fatal(err)
	}
	got := report.Instructions[0]
	if got.Mnemonic != "JML" || got.Formatted != "JML   $03:9234" || got.Target == nil || *got.Target != 0x039234 {
		t.Fatalf("long jump = %+v", got)
	}
	indirect, err := BuildDisassembly(DisassemblyOptions{ROMPath: romPath, StartPC: 0x008010, EntryM: 1, EntryX: 1, Count: 1})
	if err != nil {
		t.Fatal(err)
	}
	got = indirect.Instructions[0]
	if got.Mnemonic != "JML" || got.AddressingMode != "[abs]" || got.Formatted != "JML   [$1234]" || got.Target != nil {
		t.Fatalf("indirect long jump = %+v", got)
	}
}
