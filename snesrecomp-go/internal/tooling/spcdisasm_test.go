package tooling

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSPCDisassemblyUploadBlockAndReferences(t *testing.T) {
	root := t.TempDir()
	data := make([]byte, 0x40)
	copy(data[0x10:], []byte{0x08, 0x00, 0x00, 0x08, 0xe8, 0x12, 0xc5, 0x4b, 0x00, 0x2f, 0xf9, 0x6f})
	path := filepath.Join(root, "fixture.bin")
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	block := 0x10
	reference := uint16(0x004b)
	report, err := BuildSPCDisassembly(SPCDisassemblyOptions{
		InputPath: path, UploadBlockOffset: &block, StartAddress: 0x0800, EndAddress: 0x0808,
		FindReference: &reference,
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(report.Instructions) != 1 || report.Instructions[0].PC != 0x0802 || report.Instructions[0].Text != "mov !$004B,a" {
		t.Fatalf("SPC report = %+v", report)
	}
	full, err := BuildSPCDisassembly(SPCDisassemblyOptions{InputPath: path, UploadBlockOffset: &block, StartAddress: 0x0800, EndAddress: 0x0808})
	if err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	if err := WriteSPCDisassembly(&output, full, "text"); err != nil {
		t.Fatal(err)
	}
	for _, fragment := range []string{"0800: E8 12", "mov a,#$12", "0805: 2F F9", "bra $0800", "0807: 6F", "ret"} {
		if !strings.Contains(output.String(), fragment) {
			t.Fatalf("SPC output missing %q:\n%s", fragment, output.String())
		}
	}
}

func TestSPCOpcodeTableComplete(t *testing.T) {
	templates := spcOpcodeTemplates()
	for opcode, template := range templates {
		if template == "" {
			t.Fatalf("opcode %02X has no template", opcode)
		}
	}
}
