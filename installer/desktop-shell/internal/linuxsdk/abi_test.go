package linuxsdk

import (
	"debug/elf"
	"encoding/binary"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestGLIBCVersionRequirements(t *testing.T) {
	for _, tc := range []struct {
		versions []string
		want     string
		bad      bool
	}{
		{[]string{"GLIBC_2.3.4", "GLIBC_2.34", "GLIBC_2.9"}, "2.34", false},
		{[]string{"GLIBC_ABI_DT_RELR", "GLIBC_2.35"}, "2.36", false},
		{[]string{"GLIBCXX_3.4.30", "CXXABI_1.3"}, "", false},
		{[]string{"GLIBC_PRIVATE"}, "", true},
		{[]string{"GLIBC_2.abc"}, "", true},
		{[]string{"GLIBC_ABI_UNKNOWN"}, "", true},
	} {
		need := elf.DynamicVersionNeed{Name: "libc.so.6"}
		for _, version := range tc.versions {
			need.Needs = append(need.Needs, elf.DynamicVersionDep{Dep: version})
		}
		got, err := requiredGLIBC([]elf.DynamicVersionNeed{need})
		if (err != nil) != tc.bad || got != tc.want {
			t.Fatalf("%v: %s %v", tc.versions, got, err)
		}
	}
}

func TestLinuxUnitEscapesAreLiteralNotTraversal(t *testing.T) {
	name := `lib/systemd/system/system-systemd\x2dcryptsetup.slice`
	got, err := archivePath(name)
	if runtime.GOOS == "windows" {
		if err == nil {
			t.Fatal("accepted a literal backslash on Windows")
		}
	} else if err != nil || got != "usr/"+name {
		t.Fatal(got, err)
	}
	for _, name := range []string{`usr/lib/..\escape`, `usr/lib/\x../escape`, `usr/lib/\x2z`, `usr/lib/\x2f/../../../../escape`} {
		if _, err := archivePath(name); err == nil {
			t.Fatalf("unsafe path accepted: %q", name)
		}
	}
}

// Metadata-only ELF with a version need, not an executable fixture. Keeping
// this independent of host compilers lets macOS validate Linux ABI policy.
func versionedELF(version string) []byte {
	data := make([]byte, 768)
	copy(data, "\x7fELF")
	data[4] = 2
	data[5] = 1
	data[6] = 1
	binary.LittleEndian.PutUint16(data[16:], uint16(elf.ET_DYN))
	binary.LittleEndian.PutUint16(data[18:], uint16(elf.EM_X86_64))
	binary.LittleEndian.PutUint32(data[20:], 1)
	binary.LittleEndian.PutUint64(data[40:], 64)
	binary.LittleEndian.PutUint16(data[52:], 64)
	binary.LittleEndian.PutUint16(data[58:], 64)
	binary.LittleEndian.PutUint16(data[60:], 5)
	strtab := []byte("\x00libc.so.6\x00" + version + "\x00")
	binary.LittleEndian.PutUint32(data[128+4:], uint32(elf.SHT_STRTAB))
	binary.LittleEndian.PutUint64(data[128+24:], 384)
	binary.LittleEndian.PutUint64(data[128+32:], uint64(len(strtab)))
	copy(data[384:], strtab)
	binary.LittleEndian.PutUint32(data[192+4:], uint32(elf.SHT_GNU_VERNEED))
	binary.LittleEndian.PutUint64(data[192+24:], 512)
	binary.LittleEndian.PutUint64(data[192+32:], 32)
	binary.LittleEndian.PutUint32(data[192+40:], 1)
	binary.LittleEndian.PutUint16(data[512:], 1)
	binary.LittleEndian.PutUint16(data[514:], 1)
	binary.LittleEndian.PutUint32(data[516:], 1)
	binary.LittleEndian.PutUint32(data[520:], 16)
	binary.LittleEndian.PutUint16(data[534:], 2)
	binary.LittleEndian.PutUint32(data[536:], 11)
	binary.LittleEndian.PutUint32(data[256+4:], uint32(elf.SHT_DYNSYM))
	binary.LittleEndian.PutUint64(data[256+24:], 576)
	binary.LittleEndian.PutUint64(data[256+32:], 24)
	binary.LittleEndian.PutUint32(data[256+40:], 1)
	binary.LittleEndian.PutUint64(data[256+56:], 24)
	binary.LittleEndian.PutUint32(data[320+4:], uint32(elf.SHT_GNU_VERSYM))
	binary.LittleEndian.PutUint64(data[320+24:], 640)
	binary.LittleEndian.PutUint64(data[320+32:], 2)
	binary.LittleEndian.PutUint32(data[320+40:], 3)
	return data
}

func TestAuditABIIncludesHelpersAndRefusesNewerRuntime(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "usr/lib/private/helper")
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatal(err)
	}
	for _, version := range []string{"GLIBC_2.36", "GLIBC_2.41", "GLIBC_ABI_DT_RELR"} {
		if err := os.WriteFile(path, versionedELF(version), 0644); err != nil {
			t.Fatal(err)
		}
		report, err := AuditABI(root, "amd64", "2.36")
		if version == "GLIBC_2.41" {
			if err == nil || !strings.Contains(err.Error(), "above supported") {
				t.Fatal("new dependency accepted", err)
			}
		} else if err != nil || len(report.Files) != 1 || report.MaximumGLIBC != "2.36" {
			t.Fatal(report, err)
		}
	}
	if _, err := AuditABI(root, "arm64", "2.36"); err == nil {
		t.Fatal("mixed architecture accepted")
	}
}
