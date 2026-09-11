// Package winbundle stores a checked ZIP overlay in an ordinary Windows GUI
// PE executable. It is stdlib-only apart from the shared host package, and all
// archive/extraction tests can run without Windows or WebView2.
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
	"fmt"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/host"
)

const footerMagic = "ARBUILDERZIP0001"
const footerSize = 16 + 8 + 8 + 32
const manifestName = "windows-bundle.json"
const maxExpanded = int64(6 << 30)

var ErrNoBundle = errors.New("no embedded Windows Builder payload")

type Runtime struct {
	Version string `json:"version"`
	URL     string `json:"url"`
	SHA256  string `json:"sha256"`
}
type Manifest struct {
	Schema  int         `json:"schema"`
	Arch    string      `json:"arch"`
	WebView Runtime     `json:"webview"`
	Files   []host.File `json:"files"`
}
type Archive struct {
	Manifest Manifest
	ID       string
	file     *os.File
	zip      *zip.Reader
}

func (a *Archive) Close() error { return a.file.Close() }

// PEInfo also locates an optional Authenticode certificate table. Signing is
// done AFTER packaging; a certificate can follow the overlay with <=7 padding
// bytes. This reads layout only, and does not verify a publisher's signature.
func PEInfo(reader io.ReaderAt, size int64) (arch string, subsystem uint16, contentEnd int64, err error) {
	p, err := pe.NewFile(reader)
	if err != nil {
		return "", 0, 0, err
	}
	defer p.Close()
	switch p.Machine {
	case pe.IMAGE_FILE_MACHINE_AMD64:
		arch = "amd64"
	case pe.IMAGE_FILE_MACHINE_ARM64:
		arch = "arm64"
	default:
		return "", 0, 0, fmt.Errorf("unsupported PE machine %#x", p.Machine)
	}
	h, ok := p.OptionalHeader.(*pe.OptionalHeader64)
	if !ok {
		return "", 0, 0, errors.New("expected a 64-bit PE executable")
	}
	contentEnd = size
	cert := h.DataDirectory[4]
	if cert.VirtualAddress != 0 || cert.Size != 0 {
		if cert.VirtualAddress == 0 || cert.Size < 8 || int64(cert.VirtualAddress)+int64(cert.Size) != size || cert.VirtualAddress%8 != 0 {
			return "", 0, 0, errors.New("invalid PE certificate table")
		}
		contentEnd = int64(cert.VirtualAddress)
	}
	return arch, h.Subsystem, contentEnd, nil
}

func Open(filename string) (_ *Archive, err error) {
	f, err := os.Open(filename)
	if err != nil {
		return nil, err
	}
	defer func() {
		if err != nil {
			f.Close()
		}
	}()
	info, err := f.Stat()
	if err != nil {
		return nil, err
	}
	arch, subsystem, end, err := PEInfo(f, info.Size())
	if err != nil {
		return nil, err
	}
	if subsystem != pe.IMAGE_SUBSYSTEM_WINDOWS_GUI {
		return nil, errors.New("Builder must use the Windows GUI subsystem")
	}
	var footer []byte
	var footerStart int64
	for padding := int64(0); padding <= 7; padding++ {
		if end-padding < footerSize {
			break
		}
		candidate := make([]byte, footerSize+padding)
		start := end - int64(len(candidate))
		if _, err = f.ReadAt(candidate, start); err != nil {
			return nil, err
		}
		if string(candidate[:16]) == footerMagic && bytes.Equal(candidate[footerSize:], make([]byte, padding)) {
			footer = candidate[:footerSize]
			footerStart = start
			break
		}
		if end == info.Size() {
			break
		} // padding is permitted only before a signature
	}
	if footer == nil {
		return nil, ErrNoBundle
	}
	offset, length := binary.LittleEndian.Uint64(footer[16:24]), binary.LittleEndian.Uint64(footer[24:32])
	if offset > uint64(footerStart) || length != uint64(footerStart)-offset || length == 0 || length > uint64(maxExpanded) {
		return nil, errors.New("invalid embedded archive bounds")
	}
	// Never accept an overlay that overlaps PE headers or a PE section.
	p, err := pe.NewFile(f)
	if err != nil {
		return nil, err
	}
	if uint64(p.OptionalHeader.(*pe.OptionalHeader64).SizeOfHeaders) > offset {
		p.Close()
		return nil, errors.New("archive overlaps PE headers")
	}
	for _, s := range p.Sections {
		if uint64(s.Offset)+uint64(s.Size) > offset {
			p.Close()
			return nil, errors.New("archive overlaps PE sections")
		}
	}
	p.Close()
	section := io.NewSectionReader(f, int64(offset), int64(length))
	h := sha256.New()
	if _, err = io.Copy(h, section); err != nil {
		return nil, err
	}
	if !bytes.Equal(h.Sum(nil), footer[32:]) {
		return nil, errors.New("embedded archive checksum mismatch")
	}
	z, err := zip.NewReader(section, int64(length))
	if err != nil {
		return nil, err
	}
	if len(z.File) > 100000 {
		return nil, errors.New("too many archive entries")
	}
	var m Manifest
	entries := map[string]*zip.File{}
	for _, item := range z.File {
		if !host.WindowsPath(item.Name) || !item.Mode().IsRegular() || item.UncompressedSize64 > uint64(maxExpanded) {
			return nil, fmt.Errorf("unsafe archive entry %s", item.Name)
		}
		key := strings.ToLower(item.Name)
		if entries[key] != nil {
			return nil, fmt.Errorf("duplicate archive entry %s", item.Name)
		}
		entries[key] = item
	}
	mf := entries[manifestName]
	if mf == nil || mf.UncompressedSize64 > 32<<20 {
		return nil, errors.New("missing or oversized bundle manifest")
	}
	r, err := mf.Open()
	if err != nil {
		return nil, err
	}
	data, err := io.ReadAll(io.LimitReader(r, (32<<20)+1))
	r.Close()
	if err != nil {
		return nil, err
	}
	if err = json.Unmarshal(data, &m); err != nil {
		return nil, err
	}
	if m.Schema != 1 || m.Arch != arch || len(m.Files)+1 != len(z.File) {
		return nil, errors.New("inconsistent Windows bundle manifest")
	}
	if err = validateRuntime(m.WebView); err != nil {
		return nil, err
	}
	seen := map[string]bool{}
	var total int64
	for _, entry := range m.Files {
		key := strings.ToLower(entry.Path)
		item := entries[key]
		digest, hashErr := hex.DecodeString(entry.SHA256)
		if !host.WindowsPath(entry.Path) || (!strings.HasPrefix(entry.Path, "payload/") && !strings.HasPrefix(entry.Path, "webview/")) || seen[key] || item == nil || item.Name != entry.Path || entry.Size < 0 || uint64(entry.Size) != item.UncompressedSize64 || hashErr != nil || len(digest) != 32 || entry.Mode&^0755 != 0 {
			return nil, fmt.Errorf("invalid manifest entry %s", entry.Path)
		}
		if entry.Size > maxExpanded-total {
			return nil, errors.New("expanded bundle exceeds limit")
		}
		total += entry.Size
		seen[key] = true
	}
	for name := range seen {
		for parent := path.Dir(name); parent != "."; parent = path.Dir(parent) {
			if seen[parent] {
				return nil, errors.New("file/directory archive collision")
			}
		}
	}
	for _, name := range []string{"payload/builder-payload.json", "payload/utils/tools/actraiser-builder.exe", "payload/utils/tools/snesbuild.exe", "webview/msedgewebview2.exe", "webview/msedge.dll", "webview/icudtl.dat", "webview/resources.pak", "webview/locales/en-us.pak"} {
		if !seen[name] {
			return nil, fmt.Errorf("missing bundled component %s", name)
		}
	}
	return &Archive{Manifest: m, ID: hex.EncodeToString(footer[32:]), file: f, zip: z}, nil
}

func validateRuntime(r Runtime) error {
	h, err := hex.DecodeString(r.SHA256)
	if err != nil || len(h) != 32 || !strings.HasPrefix(r.URL, "https://msedge.sf.dl.delivery.mp.microsoft.com/") || !strings.HasSuffix(r.URL, ".cab") {
		return errors.New("runtime provenance requires an official HTTPS CAB URL and SHA-256")
	}
	parts := strings.Split(r.Version, ".")
	if len(parts) != 4 {
		return errors.New("runtime version must be exact")
	}
	for _, part := range parts {
		if part == "" || strings.Trim(part, "0123456789") != "" {
			return errors.New("invalid runtime version")
		}
	}
	return nil
}

// Extract publishes only a complete verified directory, never overwriting a
// prior destination. The optional callback grants Windows runtime permissions
// on the new staging tree, not on any pre-existing user directory.
func (a *Archive) Extract(destination string, prepare func(string) error) error {
	if _, err := os.Lstat(destination); !os.IsNotExist(err) {
		return fmt.Errorf("extraction destination exists or cannot be checked: %s", destination)
	}
	parent := filepath.Dir(destination)
	if err := os.MkdirAll(parent, 0700); err != nil {
		return err
	}
	stage, err := os.MkdirTemp(parent, ".builder-extract-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	root, err := os.OpenRoot(stage)
	if err != nil {
		return err
	}
	defer root.Close()
	byName := map[string]*zip.File{}
	for _, f := range a.zip.File {
		byName[f.Name] = f
	}
	for _, entry := range a.Manifest.Files {
		if err = root.MkdirAll(filepath.FromSlash(path.Dir(entry.Path)), 0700); err != nil {
			return err
		}
		out, err := root.OpenFile(filepath.FromSlash(entry.Path), os.O_CREATE|os.O_EXCL|os.O_WRONLY, os.FileMode(entry.Mode))
		if err != nil {
			return err
		}
		in, err := byName[entry.Path].Open()
		if err != nil {
			out.Close()
			return err
		}
		h := sha256.New()
		n, copyErr := io.Copy(io.MultiWriter(out, h), io.LimitReader(in, entry.Size+1))
		closeErr := out.Close()
		in.Close()
		if copyErr != nil {
			return copyErr
		}
		if closeErr != nil {
			return closeErr
		}
		if n != entry.Size || hex.EncodeToString(h.Sum(nil)) != entry.SHA256 {
			return fmt.Errorf("checksum mismatch: %s", entry.Path)
		}
	}
	if prepare != nil {
		if err = prepare(filepath.Join(stage, "webview")); err != nil {
			return err
		}
	}
	if err = root.WriteFile(".bundle-id", []byte(a.ID+"\n"), 0600); err != nil {
		return err
	}
	root.Close()
	return os.Rename(stage, destination)
}

// VerifyDirectory checks cached bytes on every launch; a stamp alone is not
// sufficient for executable runtime files. It never repairs or deletes edits.
func (a *Archive) VerifyDirectory(directory string) error {
	info, err := os.Lstat(directory)
	if err != nil {
		return err
	}
	if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return errors.New("runtime cache must be a real directory")
	}
	root, err := os.OpenRoot(directory)
	if err != nil {
		return err
	}
	defer root.Close()
	id, err := root.ReadFile(".bundle-id")
	if err != nil || string(id) != a.ID+"\n" {
		return errors.New("unmanaged or mismatched runtime cache")
	}
	expected := map[string]bool{".bundle-id": true}
	for _, entry := range a.Manifest.Files {
		expected[entry.Path] = true
		info, err := root.Lstat(filepath.FromSlash(entry.Path))
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() || info.Size() != entry.Size {
			return fmt.Errorf("invalid cached file %s", entry.Path)
		}
		f, err := root.Open(filepath.FromSlash(entry.Path))
		if err != nil {
			return err
		}
		h := sha256.New()
		n, err := io.Copy(h, io.LimitReader(f, entry.Size+1))
		f.Close()
		if err != nil {
			return err
		}
		if n != entry.Size || hex.EncodeToString(h.Sum(nil)) != entry.SHA256 {
			return fmt.Errorf("modified runtime cache file: %s", entry.Path)
		}
	}
	return fs.WalkDir(root.FS(), ".", func(name string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.Type()&os.ModeSymlink != 0 || (!d.IsDir() && !expected[name]) {
			return fmt.Errorf("unexpected cache entry %s", name)
		}
		return nil
	})
}
