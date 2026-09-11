package winbundle

import (
	"archive/zip"
	"crypto/sha256"
	"debug/pe"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/host"
)

type Options struct {
	Shell, Payload, WebView, Output, Arch string
	Runtime                               Runtime
}

// Create appends a ZIP and checked footer to an unsigned GUI PE. It can run on
// macOS/Linux/Windows, but every embedded executable must match the target.
func Create(o Options) error {
	if err := validateRuntime(o.Runtime); err != nil {
		return err
	}
	if o.Arch != "amd64" && o.Arch != "arm64" {
		return fmt.Errorf("unsupported Windows architecture %s", o.Arch)
	}
	if !strings.EqualFold(filepath.Ext(o.Output), ".exe") {
		return fmt.Errorf("output must end in .exe")
	}
	var err error
	o.Output, err = filepath.Abs(o.Output)
	if err != nil {
		return err
	}
	if _, err = os.Lstat(o.Output); !os.IsNotExist(err) {
		return fmt.Errorf("output exists or cannot be checked: %s", o.Output)
	}
	if err = host.ValidateWorkspace(o.Output, o.Payload, o.WebView); err != nil {
		return err
	}
	shell, err := os.Open(o.Shell)
	if err != nil {
		return err
	}
	defer shell.Close()
	info, err := shell.Stat()
	if err != nil {
		return err
	}
	arch, subsystem, end, err := PEInfo(shell, info.Size())
	if err != nil {
		return err
	}
	if arch != o.Arch || subsystem != pe.IMAGE_SUBSYSTEM_WINDOWS_GUI || end != info.Size() {
		return fmt.Errorf("shell must be an unsigned %s GUI PE; sign only after packaging", o.Arch)
	}
	if old, err := Open(o.Shell); err == nil {
		old.Close()
		return fmt.Errorf("shell is already packaged")
	} else if err != ErrNoBundle {
		return err
	}
	for _, executable := range []string{filepath.Join(o.Payload, "utils/tools/actraiser-builder.exe"), filepath.Join(o.Payload, "utils/tools/snesbuild.exe"), filepath.Join(o.WebView, "msedgewebview2.exe")} {
		f, err := os.Open(executable)
		if err != nil {
			return err
		}
		stat, err := f.Stat()
		if err != nil {
			f.Close()
			return err
		}
		arch, _, _, err := PEInfo(f, stat.Size())
		f.Close()
		if err != nil {
			return err
		}
		if arch != o.Arch {
			return fmt.Errorf("architecture mismatch in %s: %s, expected %s", executable, arch, o.Arch)
		}
	}
	if err = host.WriteManifest(o.Payload, "windows", o.Arch); err != nil {
		return err
	}
	m := Manifest{Schema: 1, Arch: o.Arch, WebView: o.Runtime}
	inputs := map[string]string{}
	for _, tree := range []struct{ root, prefix string }{{o.Payload, "payload"}, {o.WebView, "webview"}} {
		err = filepath.WalkDir(tree.root, func(name string, d fs.DirEntry, err error) error {
			if err != nil {
				return err
			}
			rel, err := filepath.Rel(tree.root, name)
			if err != nil {
				return err
			}
			if rel == "." {
				return nil
			}
			entryPath := tree.prefix + "/" + filepath.ToSlash(rel)
			if !host.WindowsPath(entryPath) || d.Type()&os.ModeSymlink != 0 || d.Name() == ".DS_Store" || strings.HasPrefix(d.Name(), "._") {
				return fmt.Errorf("unsafe Windows package input: %s", name)
			}
			if d.IsDir() {
				return nil
			}
			stat, err := d.Info()
			if err != nil {
				return err
			}
			if !stat.Mode().IsRegular() {
				return fmt.Errorf("non-regular input: %s", name)
			}
			f, err := os.Open(name)
			if err != nil {
				return err
			}
			h := sha256.New()
			_, err = io.Copy(h, f)
			f.Close()
			if err != nil {
				return err
			}
			m.Files = append(m.Files, host.File{Path: entryPath, SHA256: hex.EncodeToString(h.Sum(nil)), Size: stat.Size(), Mode: uint32(stat.Mode().Perm() & 0755)})
			inputs[entryPath] = name
			return nil
		})
		if err != nil {
			return err
		}
	}
	if err = os.MkdirAll(filepath.Dir(o.Output), 0755); err != nil {
		return err
	}
	stage, err := os.MkdirTemp(filepath.Dir(o.Output), ".builder-windows-package-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	filename := filepath.Join(stage, host.Name+".exe")
	f, err := os.OpenFile(filename, os.O_CREATE|os.O_EXCL|os.O_RDWR, 0755)
	if err != nil {
		return err
	}
	defer f.Close()
	offset, err := io.Copy(f, shell)
	if err != nil {
		return err
	}
	hash := sha256.New()
	z := zip.NewWriter(io.MultiWriter(f, hash))
	writeEntry := func(name string, mode fs.FileMode, source io.Reader) error {
		header := &zip.FileHeader{Name: name, Method: zip.Deflate, Modified: time.Date(1980, 1, 1, 0, 0, 0, 0, time.UTC)}
		header.SetMode(mode)
		w, err := z.CreateHeader(header)
		if err != nil {
			return err
		}
		_, err = io.Copy(w, source)
		return err
	}
	data, err := json.Marshal(m)
	if err != nil {
		return err
	}
	if err = writeEntry(manifestName, 0644, strings.NewReader(string(data))); err != nil {
		return err
	}
	for _, entry := range m.Files {
		in, err := os.Open(inputs[entry.Path])
		if err != nil {
			return err
		}
		check := sha256.New()
		err = writeEntry(entry.Path, fs.FileMode(entry.Mode), io.TeeReader(io.LimitReader(in, entry.Size+1), check))
		in.Close()
		if err != nil {
			return err
		}
		if hex.EncodeToString(check.Sum(nil)) != entry.SHA256 {
			return fmt.Errorf("input changed while packaging: %s", entry.Path)
		}
	}
	if err = z.Close(); err != nil {
		return err
	}
	archiveEnd, err := f.Seek(0, io.SeekCurrent)
	if err != nil {
		return err
	}
	footer := make([]byte, footerSize)
	copy(footer, footerMagic)
	binary.LittleEndian.PutUint64(footer[16:24], uint64(offset))
	binary.LittleEndian.PutUint64(footer[24:32], uint64(archiveEnd-offset))
	copy(footer[32:], hash.Sum(nil))
	if _, err = f.Write(footer); err != nil {
		return err
	}
	if err = f.Sync(); err != nil {
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	// Exercise the same reader used by player startup before publishing.
	a, err := Open(filename)
	if err != nil {
		return err
	}
	a.Close()
	return os.Rename(filename, o.Output)
}
