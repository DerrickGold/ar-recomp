package linuxsdk

import (
	"archive/tar"
	"bytes"
	"debug/elf"
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDebianVersionAndDependencySelection(t *testing.T) {
	for _, p := range [][2]string{{"1.2~rc1-1", "1.2-1"}, {"1.2-2", "1.2-10"}, {"9.0", "1:1.0"}, {"2.52.6-1~deb13u1", "2.52.6-1"}} {
		if compareVersion(p[0], p[1]) >= 0 {
			t.Fatal(p)
		}
	}
	if compareVersion("0:1.0-01", "1.0-1") != 0 {
		t.Fatal("zero/epoch ordering")
	}
	pkgs := []Package{{Name: "app", Version: "1", Depends: "libfoo (>= 2), unavailable | api (>= 3)"}, {Name: "libfoo", Version: "1"}, {Name: "libfoo", Version: "2"}, {Name: "provider", Version: "1", Provides: "api (= 3)"}}
	selected, err := selectPackages(pkgs, []string{"app"})
	if err != nil {
		t.Fatal(err)
	}
	if len(selected) != 3 || selected[1].Version != "2" {
		t.Fatal(selected)
	}
	pkgs[0].Depends = "libfoo (= 5)"
	if _, err = selectPackages(pkgs, []string{"app"}); err == nil {
		t.Fatal("ignored unsatisfied version constraint")
	}
	pkgs[0].Depends = "api (>= 4)"
	if _, err = selectPackages(pkgs, []string{"app"}); err == nil {
		t.Fatal("ignored versioned virtual dependency")
	}
	pkgs[0].Depends = "libfoo [arm64]"
	if _, err = selectPackages(pkgs, []string{"app"}); err == nil {
		t.Fatal("silently ignored unsupported syntax")
	}
	pkgs[0].Depends = "libfoo:arm64"
	if _, err = selectPackages(pkgs, []string{"app"}); err == nil {
		t.Fatal("silently ignored explicit multiarch qualifier")
	}
}
func TestIndexContinuationAndArchitecture(t *testing.T) {
	input := "Package: example\nVersion: 1\nArchitecture: arm64\nSize: 16\nFilename: pool/main/e/example/example.deb\nSHA256: " + strings.Repeat("a", 64) + "\nDepends: first,\n second (>= 2)\n\nPackage: ignored\nArchitecture: amd64\n"
	list, err := readIndex(strings.NewReader(input), "https://deb.debian.org/debian", "arm64")
	if err != nil {
		t.Fatal(err)
	}
	if len(list) != 1 || list[0].Depends != "first, second (>= 2)" {
		t.Fatal(list)
	}
	if err = validatePackage(list[0], "arm64"); err != nil {
		t.Fatal(err)
	}
	bad := list[0]
	bad.URL = "https://attacker.invalid/file.deb"
	if validatePackage(bad, "arm64") == nil {
		t.Fatal("untrusted mirror")
	}
	bad = list[0]
	bad.Source = ""
	if validatePackage(bad, "arm64") == nil {
		t.Fatal("missing provenance")
	}
}

func tarBytes(t *testing.T, headers ...*tar.Header) []byte {
	t.Helper()
	var data bytes.Buffer
	w := tar.NewWriter(&data)
	for _, h := range headers {
		h.Mode = 0755
		if h.Typeflag == tar.TypeReg {
			h.Size = 1
		}
		if err := w.WriteHeader(h); err != nil {
			t.Fatal(err)
		}
		if h.Typeflag == tar.TypeReg {
			w.Write([]byte("x"))
		}
	}
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	return data.Bytes()
}
func TestSafeTarAndUsrMerge(t *testing.T) {
	dir := t.TempDir()
	root, err := os.OpenRoot(dir)
	if err != nil {
		t.Fatal(err)
	}
	defer root.Close()
	owners := map[string]string{}
	links := map[string]link{}
	data := tarBytes(t, &tar.Header{Name: "./bin", Typeflag: tar.TypeSymlink, Linkname: "usr/bin"}, &tar.Header{Name: "./usr/lib/libfoo.so", Typeflag: tar.TypeSymlink, Linkname: "/usr/lib/libfoo.so.1"}, &tar.Header{Name: "./usr/lib/libfoo.so.1", Typeflag: tar.TypeReg}, &tar.Header{Name: "./etc/private", Typeflag: tar.TypeReg}, &tar.Header{Name: "./usr/share/doc/foo/index.html", Typeflag: tar.TypeReg}, &tar.Header{Name: "./usr/share/doc/foo/copyright", Typeflag: tar.TypeReg})
	if err = extractTar(bytes.NewReader(data), root, "foo", owners, links); err != nil {
		t.Fatal(err)
	}
	if len(owners) != 3 || len(links) != 1 || links["usr/lib/libfoo.so"].Target != "usr/lib/libfoo.so.1" {
		t.Fatal(owners, links)
	}
	if _, err = root.Stat("etc/private"); !os.IsNotExist(err) {
		t.Fatal("extracted host configuration")
	}
	for _, h := range []*tar.Header{{Name: "../escape", Typeflag: tar.TypeReg}, {Name: "/absolute", Typeflag: tar.TypeReg}, {Name: "usr/lib/evil", Typeflag: tar.TypeSymlink, Linkname: "../../../escape"}, {Name: "usr/lib/device", Typeflag: tar.TypeChar}} {
		if err = extractTar(bytes.NewReader(tarBytes(t, h)), root, "foo", owners, links); err == nil {
			t.Fatal("accepted unsafe tar", h)
		}
	}
}
func testELF(machine elf.Machine) []byte {
	data := make([]byte, 64)
	copy(data, "\x7fELF")
	data[4] = 2
	data[5] = 1
	data[6] = 1
	binary.LittleEndian.PutUint16(data[16:], 3)
	binary.LittleEndian.PutUint16(data[18:], uint16(machine))
	binary.LittleEndian.PutUint32(data[20:], 1)
	binary.LittleEndian.PutUint16(data[52:], 64)
	return data
}
func TestELFArchitectureAndConfinedLookup(t *testing.T) {
	dir := t.TempDir()
	sdk := SDK{Root: dir, Lock: Lock{Arch: "amd64"}}
	lib := sdk.Path("/usr/lib/x86_64-linux-gnu/libfoo.so.1")
	os.MkdirAll(filepath.Dir(lib), 0755)
	os.WriteFile(lib, testELF(elf.EM_X86_64), 0644)
	if _, err := sdk.Library("libfoo.so.1"); err != nil {
		t.Fatal(err)
	}
	if _, err := sdk.Library("../private"); err == nil {
		t.Fatal("dependency traversal")
	}
	os.WriteFile(lib, testELF(elf.EM_AARCH64), 0644)
	if _, err := sdk.Library("libfoo.so.1"); err == nil {
		t.Fatal("wrong target accepted")
	}
	outside := filepath.Join(t.TempDir(), "outside")
	os.WriteFile(outside, testELF(elf.EM_X86_64), 0644)
	os.Remove(lib)
	if err := os.Symlink(outside, lib); err != nil {
		t.Skip(err)
	}
	if _, err := sdk.Library("libfoo.so.1"); err == nil {
		t.Fatal("SDK symlink escaped")
	}
}

func TestSDKPrivateELFPathsAndCheckedInLocks(t *testing.T) {
	for _, arch := range []string{"amd64", "arm64"} {
		data, err := os.ReadFile("../../linux-sdk-" + arch + ".json")
		if err != nil {
			t.Fatal(err)
		}
		var lock Lock
		if err = json.Unmarshal(data, &lock); err != nil {
			t.Fatal(err)
		}
		if err = lock.Validate(); err != nil {
			t.Fatal(err)
		}
		if lock.Arch != arch {
			t.Fatal("wrong lock architecture")
		}
	}
	sdk := SDK{Root: "/sdk", Lock: Lock{Arch: "arm64"}}
	importer := "/sdk/usr/lib/aarch64-linux-gnu/libproxy.so.0"
	for _, value := range []string{"$ORIGIN/libproxy", "${ORIGIN}/libproxy", "/usr/lib/aarch64-linux-gnu/libproxy"} {
		paths, err := sdk.searchPaths([]string{value}, importer)
		if err != nil || len(paths) != 1 || paths[0] != "/sdk/usr/lib/aarch64-linux-gnu/libproxy" {
			t.Fatalf("%s: %v %v", value, paths, err)
		}
	}
	for _, value := range []string{"", "/usr/lib:", "libproxy", "$ORIGIN/../../../../../outside", "$LIB/libproxy"} {
		if _, err := sdk.searchPaths([]string{value}, importer); err == nil {
			t.Fatalf("accepted unsafe search %q", value)
		}
	}
}

// Construct metadata-only ELF sections; no native linker or target execution.
func TestELFRunpathPolicyAlsoCoversLeafLibraries(t *testing.T) {
	sdk := SDK{Root: t.TempDir(), Lock: Lock{Arch: "amd64"}}
	file := sdk.Path("/usr/lib/leaf.so")
	if err := os.MkdirAll(filepath.Dir(file), 0755); err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		tag     elf.DynTag
		value   string
		allowed bool
	}{
		{elf.DT_RUNPATH, "$ORIGIN/private", true},
		{elf.DT_RUNPATH, "/usr/lib/private", true},
		{elf.DT_RUNPATH, "relative", false},
		{elf.DT_RPATH, "/usr/lib/private", false},
	} {
		data := make([]byte, 512)
		copy(data, testELF(elf.EM_X86_64))
		binary.LittleEndian.PutUint64(data[40:], 64) // section table
		binary.LittleEndian.PutUint16(data[58:], 64)
		binary.LittleEndian.PutUint16(data[60:], 3)
		binary.LittleEndian.PutUint16(data[62:], 1)
		// Null section, string table, dynamic section (no DT_NEEDED entries).
		binary.LittleEndian.PutUint32(data[128+4:], uint32(elf.SHT_STRTAB))
		binary.LittleEndian.PutUint64(data[128+24:], 256)
		binary.LittleEndian.PutUint64(data[128+32:], uint64(len(tc.value)+2))
		copy(data[257:], tc.value)
		binary.LittleEndian.PutUint32(data[192+4:], uint32(elf.SHT_DYNAMIC))
		binary.LittleEndian.PutUint64(data[192+24:], 384)
		binary.LittleEndian.PutUint64(data[192+32:], 32)
		binary.LittleEndian.PutUint32(data[192+40:], 1)
		binary.LittleEndian.PutUint64(data[192+56:], 16)
		binary.LittleEndian.PutUint64(data[384:], uint64(tc.tag))
		binary.LittleEndian.PutUint64(data[392:], 1)
		if err := os.WriteFile(file, data, 0644); err != nil {
			t.Fatal(err)
		}
		_, err := sdk.Needed(file)
		if (err == nil) != tc.allowed {
			t.Fatalf("%s %q: %v", tc.tag, tc.value, err)
		}
	}
}
