// Package winbundle stores a checked ZIP overlay in an ordinary Windows GUI
// PE executable. It is stdlib-only apart from the shared host package, and all
// archive/extraction tests can run without Windows.
package winbundle

import (
	"archive/zip"
	"bytes"
	"context"
	"crypto/sha256"
	"debug/pe"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
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

// Schema 2 carries only the Builder payload. Schema 1 also bundled a Fixed
// Version WebView2 runtime; the Builder now uses the system Evergreen runtime.
const manifestSchema = 2

var ErrNoBundle = errors.New("no embedded Windows Builder payload")

type Manifest struct {
	Schema int         `json:"schema"`
	Arch   string      `json:"arch"`
	Files  []host.File `json:"files"`
}
type Archive struct {
	Manifest       Manifest
	ID             string
	file           *os.File
	zip            *zip.Reader
	receiptPath    string
	receipt        *verificationReceipt
	packageStamp   fileStamp
	manifestSHA256 string
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
	return OpenContext(context.Background(), filename, nil)
}

func OpenContext(ctx context.Context, filename string, progress ProgressFunc) (_ *Archive, err error) {
	return openContext(ctx, filename, "", true, progress)
}

// OpenForLaunch can reuse a successful package check when its metadata and
// manifest still match. Inspection/packaging callers use OpenContext for a full
// checksum check. force also bypasses the extracted-file verification cache.
func OpenForLaunch(ctx context.Context, filename, cache string, force bool, progress ProgressFunc) (*Archive, error) {
	return openContext(ctx, filename, cache, force, progress)
}

func openContext(ctx context.Context, filename, cache string, force bool, progress ProgressFunc) (_ *Archive, err error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
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
	id := hex.EncodeToString(footer[32:])
	var receipt *verificationReceipt
	var receiptPath string
	if cache != "" {
		receiptPath = filepath.Join(cache, id+".verification.json")
		if !force {
			receipt = readVerificationReceipt(receiptPath, id, stampFile(info))
		}
	}
	checkPackage := func() error {
		h := sha256.New()
		m := newMeter(ctx, progress, "Checking bundled package", int64(length))
		if _, err := m.copy(h, io.NewSectionReader(f, int64(offset), int64(length))); err != nil {
			return err
		}
		m.report(true)
		if !bytes.Equal(h.Sum(nil), footer[32:]) {
			return errors.New("embedded archive checksum mismatch")
		}
		return nil
	}
	if receipt == nil {
		if err := checkPackage(); err != nil {
			return nil, err
		}
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
		if err := ctx.Err(); err != nil {
			return nil, err
		}
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
	manifestDigest := fmt.Sprintf("%x", sha256.Sum256(data))
	if receipt != nil && receipt.ManifestSHA256 != manifestDigest {
		// Metadata alone is never enough to accept a changed manifest.
		if err := checkPackage(); err != nil {
			return nil, err
		}
		receipt = nil
	}
	if err = json.Unmarshal(data, &m); err != nil {
		return nil, err
	}
	if m.Schema != manifestSchema || m.Arch != arch || len(m.Files)+1 != len(z.File) {
		return nil, errors.New("inconsistent Windows bundle manifest")
	}
	seen := map[string]bool{}
	var total int64
	for _, entry := range m.Files {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		key := strings.ToLower(entry.Path)
		item := entries[key]
		digest, hashErr := hex.DecodeString(entry.SHA256)
		if !host.WindowsPath(entry.Path) || !strings.HasPrefix(entry.Path, "payload/") || seen[key] || item == nil || item.Name != entry.Path || entry.Size < 0 || uint64(entry.Size) != item.UncompressedSize64 || hashErr != nil || len(digest) != 32 || entry.Mode&^0755 != 0 {
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
	for _, name := range []string{"payload/builder-payload.json", "payload/utils/tools/actraiser-builder.exe", "payload/utils/tools/snesbuild.exe"} {
		if !seen[name] {
			return nil, fmt.Errorf("missing bundled component %s", name)
		}
	}
	return &Archive{Manifest: m, ID: id, file: f, zip: z,
		receiptPath: receiptPath, receipt: receipt, packageStamp: stampFile(info), manifestSHA256: manifestDigest}, nil
}

// Extract publishes only a complete verified directory, never overwriting a
// prior destination.
func (a *Archive) Extract(destination string) error {
	return a.ExtractContext(context.Background(), destination, nil)
}

func (a *Archive) ExtractContext(ctx context.Context, destination string, progress ProgressFunc) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	m := newMeter(ctx, progress, "Extracting bundled tools", a.expandedSize())
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
		if err := ctx.Err(); err != nil {
			return err
		}
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
		n, copyErr := m.copy(io.MultiWriter(out, h), io.LimitReader(in, entry.Size+1))
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
	m.report(true)
	if err := ctx.Err(); err != nil {
		return err
	}
	if err = root.WriteFile(".bundle-id", []byte(a.ID+"\n"), 0600); err != nil {
		return err
	}
	root.Close()
	if err := ctx.Err(); err != nil {
		return err
	}
	return os.Rename(stage, destination)
}

// VerifyDirectory performs an explicit full integrity check, regardless of any
// saved metadata. It never repairs or deletes edits.
func (a *Archive) VerifyDirectory(directory string) error {
	return a.VerifyDirectoryContext(context.Background(), directory, nil)
}

func (a *Archive) VerifyDirectoryContext(ctx context.Context, directory string, progress ProgressFunc) error {
	_, err := a.checkDirectory(ctx, directory, nil, false, progress)
	return err
}
