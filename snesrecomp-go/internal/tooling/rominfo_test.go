package tooling

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestROMInfoSelectsLoROMHeaderAndReportsIdentity(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	header := image[0x7fc0:]
	copy(header, []byte("SYNTHETIC TOOL ROM   "))
	header[0x15], header[0x16], header[0x17], header[0x18], header[0x19], header[0x1b] = 0x30, 0x02, 0x05, 0x03, 0x01, 0x02
	header[0x1c], header[0x1d], header[0x1e], header[0x1f] = 0xcb, 0xed, 0x34, 0x12
	header[0x3a], header[0x3b] = 0xd8, 0x83
	header[0x3c], header[0x3d] = 0x00, 0x80
	path := filepath.Join(root, "fixture.sfc")
	if err := os.WriteFile(path, image, 0o644); err != nil {
		t.Fatal(err)
	}
	report, err := BuildROMInfo(ROMInfoOptions{ROMPath: path})
	if err != nil {
		t.Fatal(err)
	}
	if report.Header == nil || report.Header.Mapper != "lorom" || !report.Header.FastROM || report.Header.Title != "SYNTHETIC TOOL ROM" || !report.Header.ComplementValid {
		t.Fatalf("header = %+v candidates=%+v", report.Header, report.HeaderCandidates)
	}
	if report.SHA256 == "" || report.SHA1 == "" || report.CRC32 == 0 || len(report.Vectors) != 2 {
		t.Fatalf("identity/vectors = %+v", report)
	}
	var output bytes.Buffer
	if err := WriteROMInfo(&output, report, "text"); err != nil {
		t.Fatal(err)
	}
	for _, fragment := range []string{"SYNTHETIC TOOL ROM", "mapper=lorom+fastrom", "emulation_reset", "SHA-256"} {
		if !strings.Contains(output.String(), fragment) {
			t.Fatalf("ROM info missing %q:\n%s", fragment, output.String())
		}
	}
}
