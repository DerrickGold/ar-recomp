package winbundle

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"debug/pe"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/host"
)

func fakePE(arch string) []byte {
	data := make([]byte, 512)
	copy(data, "MZ")
	binary.LittleEndian.PutUint32(data[0x3c:], 128)
	var b bytes.Buffer
	b.WriteString("PE\x00\x00")
	machine := uint16(pe.IMAGE_FILE_MACHINE_AMD64)
	if arch == "arm64" {
		machine = pe.IMAGE_FILE_MACHINE_ARM64
	}
	h := pe.OptionalHeader64{Magic: 0x20b, Subsystem: pe.IMAGE_SUBSYSTEM_WINDOWS_GUI, SizeOfHeaders: 512, NumberOfRvaAndSizes: 16}
	binary.Write(&b, binary.LittleEndian, pe.FileHeader{Machine: machine, SizeOfOptionalHeader: uint16(binary.Size(h))})
	binary.Write(&b, binary.LittleEndian, h)
	copy(data[128:], b.Bytes())
	return data
}
func pin() Runtime {
	return Runtime{Version: "152.0.4191.62", URL: "https://msedge.sf.dl.delivery.mp.microsoft.com/test.cab", SHA256: strings.Repeat("a", 64)}
}
func put(t *testing.T, root, name string, data []byte) {
	t.Helper()
	file := filepath.Join(root, filepath.FromSlash(name))
	if err := os.MkdirAll(filepath.Dir(file), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(file, data, 0755); err != nil {
		t.Fatal(err)
	}
}
func fixture(t *testing.T, arch string) Options {
	t.Helper()
	root := t.TempDir()
	o := Options{Arch: arch, Shell: filepath.Join(root, "shell.exe"), Payload: filepath.Join(root, "payload"), WebView: filepath.Join(root, "webview"), Output: filepath.Join(root, "Builder.exe"), Runtime: pin()}
	put(t, root, "shell.exe", fakePE(arch))
	for _, name := range []string{"utils/tools/actraiser-builder.exe", "utils/tools/snesbuild.exe"} {
		put(t, o.Payload, name, fakePE(arch))
	}
	for _, name := range []string{"utils/snesbuild.ini", "utils/defaults/config.ini", "utils/tools/sdl3/include/SDL3/SDL.h"} {
		put(t, o.Payload, name, []byte("fixture"))
	}
	put(t, o.WebView, "msedgewebview2.exe", fakePE(arch))
	for _, name := range []string{"msedge.dll", "icudtl.dat", "resources.pak", "Locales/en-US.pak"} {
		put(t, o.WebView, name, []byte("runtime fixture"))
	}
	return o
}
func TestRoundTripAndCachedBytes(t *testing.T) {
	for _, arch := range []string{"amd64", "arm64"} {
		t.Run(arch, func(t *testing.T) {
			o := fixture(t, arch)
			if err := Create(o); err != nil {
				t.Fatal(err)
			}
			a, err := Open(o.Output)
			if err != nil {
				t.Fatal(err)
			}
			defer a.Close()
			if a.Manifest.Arch != arch || len(a.ID) != 64 {
				t.Fatal(a.Manifest)
			}
			dest := filepath.Join(t.TempDir(), "path with spaces", "extracted")
			callback := false
			if err = a.Extract(dest, func(path string) error {
				callback = true
				if filepath.Base(path) != "webview" {
					t.Fatal(path)
				}
				return nil
			}); err != nil {
				t.Fatal(err)
			}
			if !callback {
				t.Fatal("missing permissions callback")
			}
			if err = a.VerifyDirectory(dest); err != nil {
				t.Fatal(err)
			}
			if err = a.Extract(dest, nil); err == nil {
				t.Fatal("overwrote existing extraction")
			}
			put(t, dest, "webview/msedge.dll", []byte("tampered"))
			if err = a.VerifyDirectory(dest); err == nil {
				t.Fatal("trusted stale stamp over changed bytes")
			}
			if err = Create(o); err == nil {
				t.Fatal("overwrote existing exe")
			}
		})
	}
}
func TestPackageRejectsMixedArchitectureAndPrivateInputs(t *testing.T) {
	o := fixture(t, "amd64")
	put(t, o.WebView, "msedgewebview2.exe", fakePE("arm64"))
	if err := Create(o); err == nil {
		t.Fatal("mixed architectures accepted")
	}
	o = fixture(t, "amd64")
	put(t, o.Payload, "utils/private.sfc", []byte("private"))
	if err := Create(o); err == nil {
		t.Fatal("ROM packaged")
	}
	o = fixture(t, "amd64")
	o.Output = filepath.Join(o.Payload, "output.exe")
	if err := Create(o); err == nil {
		t.Fatal("recursive output accepted")
	}
}
func TestCorruptionBoundsAndCertificateTableLayout(t *testing.T) {
	o := fixture(t, "amd64")
	if err := Create(o); err != nil {
		t.Fatal(err)
	}
	original, _ := os.ReadFile(o.Output)
	for _, kind := range []string{"checksum", "bounds", "truncated", "headers"} {
		t.Run(kind, func(t *testing.T) {
			data := bytes.Clone(original)
			switch kind {
			case "checksum":
				data[520] ^= 1
			case "bounds":
				binary.LittleEndian.PutUint64(data[len(data)-footerSize+16:], ^uint64(0))
			case "truncated":
				data = data[:len(data)-10]
			case "headers":
				// Expand SizeOfHeaders to overlap the otherwise valid archive.
				binary.LittleEndian.PutUint32(data[128+4+20+60:], 4096)
			}
			file := filepath.Join(t.TempDir(), "bad.exe")
			os.WriteFile(file, data, 0600)
			if a, err := Open(file); err == nil {
				a.Close()
				t.Fatal("accepted corrupt package")
			}
		})
	}
	// A structural test only: this is deliberately NOT a cryptographic signature.
	signed := bytes.Clone(original)
	for len(signed)%8 != 0 {
		signed = append(signed, 0)
	}
	certOffset := len(signed)
	signed = append(signed, 8, 0, 0, 0, 0, 2, 2, 0)
	certEntry := 128 + 4 + 20 + 112 + 4*8
	binary.LittleEndian.PutUint32(signed[certEntry:], uint32(certOffset))
	binary.LittleEndian.PutUint32(signed[certEntry+4:], 8)
	file := filepath.Join(t.TempDir(), "signed-layout.exe")
	os.WriteFile(file, signed, 0600)
	a, err := Open(file)
	if err != nil {
		t.Fatal(err)
	}
	a.Close()
}

type zipInput struct {
	name string
	data []byte
	mode fs.FileMode
}

func customArchive(t *testing.T, extra []zipInput, badFileHash bool) string {
	t.Helper()
	files := []zipInput{}
	for _, name := range []string{"payload/builder-payload.json", "payload/utils/tools/actraiser-builder.exe", "payload/utils/tools/snesbuild.exe", "webview/msedgewebview2.exe", "webview/msedge.dll", "webview/icudtl.dat", "webview/resources.pak", "webview/locales/en-us.pak"} {
		files = append(files, zipInput{name, []byte("fixture"), 0644})
	}
	files = append(files, extra...)
	m := Manifest{Schema: 1, Arch: "amd64", WebView: pin()}
	for _, item := range files {
		sum := sha256.Sum256(item.data)
		m.Files = append(m.Files, host.File{Path: item.name, SHA256: hex.EncodeToString(sum[:]), Size: int64(len(item.data)), Mode: 0644})
	}
	if badFileHash {
		m.Files[0].SHA256 = strings.Repeat("0", 64)
	}
	var b bytes.Buffer
	z := zip.NewWriter(&b)
	data, _ := json.Marshal(m)
	w, _ := z.Create(manifestName)
	w.Write(data)
	for _, item := range files {
		header := &zip.FileHeader{Name: item.name, Method: zip.Deflate}
		header.SetMode(item.mode)
		w, err := z.CreateHeader(header)
		if err != nil {
			t.Fatal(err)
		}
		w.Write(item.data)
	}
	z.Close()
	base := fakePE("amd64")
	sum := sha256.Sum256(b.Bytes())
	footer := make([]byte, footerSize)
	copy(footer, footerMagic)
	binary.LittleEndian.PutUint64(footer[16:], uint64(len(base)))
	binary.LittleEndian.PutUint64(footer[24:], uint64(b.Len()))
	copy(footer[32:], sum[:])
	base = append(base, b.Bytes()...)
	base = append(base, footer...)
	file := filepath.Join(t.TempDir(), "custom.exe")
	os.WriteFile(file, base, 0600)
	return file
}
func TestUnsafeArchivePathsNeverExtract(t *testing.T) {
	for _, name := range []string{"../escape", "/absolute", "webview/CON.txt", "webview/file:stream", "webview/trailing.", "webview/bad\\path", "webview/MSedge.dll", "webview/locales"} {
		t.Run(name, func(t *testing.T) {
			file := customArchive(t, []zipInput{{name, []byte("bad"), 0644}}, false)
			if a, err := Open(file); err == nil {
				a.Close()
				t.Fatal("unsafe archive accepted")
			}
		})
	}
	file := customArchive(t, []zipInput{{"webview/link", []byte("../outside"), fs.ModeSymlink | 0777}}, false)
	if a, err := Open(file); err == nil {
		a.Close()
		t.Fatal("symlink archive accepted")
	}
}
func TestFailedExtractionDoesNotPublishAndExtraCacheFileRejected(t *testing.T) {
	a, err := Open(customArchive(t, nil, true))
	if err != nil {
		t.Fatal(err)
	}
	defer a.Close()
	dest := filepath.Join(t.TempDir(), "output")
	if err = a.Extract(dest, nil); err == nil {
		t.Fatal("bad per-file checksum accepted")
	}
	if _, err = os.Lstat(dest); !os.IsNotExist(err) {
		t.Fatal("partial extraction published")
	}
	o := fixture(t, "amd64")
	if err = Create(o); err != nil {
		t.Fatal(err)
	}
	good, err := Open(o.Output)
	if err != nil {
		t.Fatal(err)
	}
	defer good.Close()
	permissionsErr := errors.New("synthetic runtime permission failure")
	if err = good.Extract(dest, func(string) error { return permissionsErr }); !errors.Is(err, permissionsErr) {
		t.Fatal("permission failure was not returned", err)
	}
	if _, err = os.Lstat(dest); !os.IsNotExist(err) {
		t.Fatal("runtime with incomplete permissions was published", err)
	}
	if entries, err := os.ReadDir(filepath.Dir(dest)); err != nil || len(entries) != 0 {
		t.Fatal("failed extraction left staging files behind", entries, err)
	}
	if err = good.Extract(dest, nil); err != nil {
		t.Fatal(err)
	}
	put(t, dest, "webview/injected.dll", []byte("unexpected"))
	if err = good.VerifyDirectory(dest); err == nil {
		t.Fatal("extra executable cache content accepted")
	}
}
