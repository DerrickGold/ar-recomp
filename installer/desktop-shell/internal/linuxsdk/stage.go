package linuxsdk

import (
	"archive/tar"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
)

type SDK struct {
	Root   string            `json:"-"`
	Lock   Lock              `json:"lock"`
	Owners map[string]string `json:"owners"`
}

func (s *SDK) Path(p string) string {
	return filepath.Join(s.Root, filepath.FromSlash(strings.TrimPrefix(p, "/")))
}
func (s *SDK) Triple() string {
	if s.Lock.Arch == "amd64" {
		return "x86_64-linux-gnu"
	}
	return "aarch64-linux-gnu"
}
func Load(root string) (*SDK, error) {
	data, err := os.ReadFile(filepath.Join(root, ".linux-sdk.json"))
	if err != nil {
		return nil, err
	}
	var s SDK
	if err = json.Unmarshal(data, &s); err != nil {
		return nil, err
	}
	if err = s.Lock.Validate(); err != nil {
		return nil, err
	}
	s.Root, err = filepath.EvalSymlinks(root)
	if err == nil {
		s.Root, err = filepath.Abs(s.Root)
	}
	return &s, err
}
func (s *SDK) Owner(filename string) (Package, error) {
	rel, err := filepath.Rel(s.Root, filename)
	if err != nil || !filepath.IsLocal(rel) {
		return Package{}, fmt.Errorf("file outside SDK: %s", filename)
	}
	owner := s.Owners[filepath.ToSlash(rel)]
	if owner == "" {
		resolved, e := filepath.EvalSymlinks(filename)
		if e == nil && resolved != filename {
			return s.Owner(resolved)
		}
	}
	for _, p := range s.Lock.Packages {
		if p.Name == owner {
			return p, nil
		}
	}
	return Package{}, fmt.Errorf("missing package provenance: %s", filename)
}

func fileHash(name string) (string, error) {
	f, err := os.Open(name)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err = io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}
func download(p Package, cache string) (string, error) {
	name := filepath.Join(cache, p.SHA256+".deb")
	if info, err := os.Stat(name); err == nil && info.Size() == p.Size {
		if sum, e := fileHash(name); e == nil && sum == p.SHA256 {
			return name, nil
		}
	}
	f, err := os.CreateTemp(cache, ".deb-download-")
	if err != nil {
		return "", err
	}
	defer os.Remove(f.Name())
	defer f.Close()
	response, err := client.Get(p.URL)
	if err != nil {
		return "", err
	}
	defer response.Body.Close()
	if response.StatusCode != 200 {
		return "", fmt.Errorf("%s: HTTP %d; refresh SDK lock if the pinned package left its mirror", p.URL, response.StatusCode)
	}
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(f, h), io.LimitReader(response.Body, p.Size+1))
	if err != nil {
		return "", err
	}
	if n != p.Size || hex.EncodeToString(h.Sum(nil)) != p.SHA256 {
		return "", fmt.Errorf("package checksum/size mismatch: %s", p.Name)
	}
	if err = f.Close(); err != nil {
		return "", err
	}
	if err = os.Rename(f.Name(), name); err != nil {
		return "", err
	}
	return name, nil
}

// Stage uses an immutable lock-keyed cache directory; no package scripts run.
func Stage(lock Lock, cache, destination string) error {
	if err := lock.Validate(); err != nil {
		return err
	}
	data, _ := json.Marshal(lock)
	if existing, err := Load(destination); err == nil {
		old, _ := json.Marshal(existing.Lock)
		if bytes.Equal(data, old) {
			return nil
		}
	}
	if _, err := os.Lstat(destination); !os.IsNotExist(err) {
		return fmt.Errorf("SDK destination exists but has a different or incomplete lock: %s", destination)
	}
	if err := os.MkdirAll(cache, 0755); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(destination), 0755); err != nil {
		return err
	}
	stage, err := os.MkdirTemp(filepath.Dir(destination), ".linux-sdk-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	if err = caseSensitive(stage); err != nil {
		return err
	}
	files := make([]string, len(lock.Packages))
	jobs := make(chan int)
	errors := make(chan error, len(lock.Packages))
	var wg sync.WaitGroup
	for worker := 0; worker < 6; worker++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range jobs {
				name, err := download(lock.Packages[i], cache)
				files[i] = name
				if err != nil {
					errors <- err
				}
			}
		}()
	}
	for i := range lock.Packages {
		jobs <- i
	}
	close(jobs)
	wg.Wait()
	close(errors)
	for err := range errors {
		return err
	}
	root, err := os.OpenRoot(stage)
	if err != nil {
		return err
	}
	defer root.Close()
	s := SDK{Root: stage, Lock: lock, Owners: map[string]string{}}
	links := map[string]link{}
	for i, p := range lock.Packages {
		fmt.Fprintf(os.Stderr, "SDK %s: %s %s\n", lock.Arch, p.Name, p.Version)
		if err := extractDeb(files[i], root, p.Name, s.Owners, links); err != nil {
			return fmt.Errorf("extract %s: %w", p.Name, err)
		}
	}
	for name, l := range links {
		if err = root.MkdirAll(path.Dir(name), 0755); err != nil {
			return err
		}
		if l.Hard {
			if err = root.Link(l.Target, name); err != nil {
				return err
			}
		} else {
			target, err := filepath.Rel(filepath.FromSlash(path.Dir(name)), filepath.FromSlash(l.Target))
			if err != nil {
				return err
			}
			if err = root.Symlink(target, name); err != nil {
				return err
			}
		}
	}
	// Debian usr-merge aliases; link targets remain inside the staged SDK.
	for _, name := range []string{"lib", "lib64", "bin", "sbin"} {
		if _, e := root.Lstat(name); os.IsNotExist(e) {
			if err = root.Symlink("usr/"+name, name); err != nil {
				return err
			}
		}
	}
	receipt, err := json.Marshal(s)
	if err != nil {
		return err
	}
	if err = root.WriteFile(".linux-sdk.json", receipt, 0644); err != nil {
		return err
	}
	root.Close()
	return os.Rename(stage, destination)
}

type link struct {
	Target string
	Hard   bool
}

func caseSensitive(directory string) error {
	name := filepath.Join(directory, ".sdk-case-probe")
	f, err := os.OpenFile(name, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	f.Close()
	defer os.Remove(name)
	if _, err := os.Stat(filepath.Join(directory, ".SDK-CASE-PROBE")); !os.IsNotExist(err) {
		return fmt.Errorf("Linux SDK requires case-sensitive storage; use package-linux-cross.cmake on macOS to provision its native build volume")
	}
	return nil
}

// Linux systemd unit filenames use literal \xHH escapes. Preserve those bytes
// on Unix; never decode them, and never treat arbitrary backslashes as paths.
func safeLinuxCharacters(name string) bool {
	if strings.ContainsRune(name, 0) {
		return false
	}
	for i := 0; i < len(name); i++ {
		if name[i] != '\\' {
			continue
		}
		if runtime.GOOS == "windows" || i+3 >= len(name) || name[i+1] != 'x' {
			return false
		}
		for _, c := range name[i+2 : i+4] {
			if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
				return false
			}
		}
		i += 3
	}
	return true
}

func archivePath(name string) (string, error) {
	name = strings.TrimPrefix(name, "./")
	name = strings.TrimSuffix(name, "/")
	if name == "" || name == "." {
		return "", nil
	}
	if !filepath.IsLocal(name) || path.Clean(name) != name || !safeLinuxCharacters(name) {
		return "", fmt.Errorf("unsafe Debian archive path %q", name)
	}
	for _, prefix := range []string{"lib", "lib64", "bin", "sbin"} {
		if name == prefix || strings.HasPrefix(name, prefix+"/") {
			name = "usr/" + name
			break
		}
	}
	return name, nil
}
func linkTarget(name, target string, hard bool) (string, error) {
	if !safeLinuxCharacters(target) {
		return "", fmt.Errorf("unsafe symlink target")
	}
	if strings.HasPrefix(target, "/") {
		target = strings.TrimPrefix(target, "/")
	} else if !hard {
		target = path.Join(path.Dir(name), target)
	}
	return archivePath(path.Clean(target))
}

func extractDeb(filename string, root *os.Root, pkg string, owners map[string]string, links map[string]link) error {
	f, err := os.Open(filename)
	if err != nil {
		return err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return err
	}
	var magic [8]byte
	if _, err = f.ReadAt(magic[:], 0); err != nil {
		return err
	}
	if string(magic[:]) != "!<arch>\n" {
		return fmt.Errorf("not a Debian ar archive")
	}
	for offset := int64(8); offset < info.Size(); {
		var header [60]byte
		if _, err = f.ReadAt(header[:], offset); err != nil {
			return err
		}
		if string(header[58:]) != "`\n" {
			return fmt.Errorf("invalid ar header")
		}
		size, err := strconv.ParseInt(strings.TrimSpace(string(header[48:58])), 10, 64)
		if err != nil || size < 0 || size > info.Size()-offset-60 {
			return fmt.Errorf("invalid ar member size")
		}
		name := strings.TrimSuffix(strings.TrimSpace(string(header[:16])), "/")
		if strings.HasPrefix(name, "data.tar.") {
			var command *exec.Cmd
			switch name {
			case "data.tar.xz":
				command = exec.Command("xz", "-dc")
			case "data.tar.gz":
				command = exec.Command("gzip", "-dc")
			case "data.tar.zst":
				command = exec.Command("zstd", "-dc")
			default:
				return fmt.Errorf("unsupported data member %s", name)
			}
			command.Stdin = io.NewSectionReader(f, offset+60, size)
			command.Stderr = os.Stderr
			out, err := command.StdoutPipe()
			if err != nil {
				return err
			}
			if err = command.Start(); err != nil {
				return err
			}
			err = extractTar(out, root, pkg, owners, links)
			if err == nil {
				// Drain bounded tar padding so the decompressor cannot block on
				// its stdout pipe after tar.Reader reaches the end marker.
				var n int64
				n, err = io.Copy(io.Discard, io.LimitReader(out, 1<<20))
				if n == 1<<20 {
					err = fmt.Errorf("excessive data after tar end marker")
				}
			}
			if err != nil {
				command.Process.Kill()
			}
			waitErr := command.Wait()
			if err != nil {
				return err
			}
			return waitErr
		}
		offset += 60 + size + size%2
	}
	return fmt.Errorf("Debian package missing data archive")
}
func extractTar(r io.Reader, root *os.Root, pkg string, owners map[string]string, links map[string]link) error {
	t := tar.NewReader(io.LimitReader(r, 8<<30))
	for {
		h, err := t.Next()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
		raw := strings.TrimSuffix(strings.TrimPrefix(h.Name, "./"), "/")
		if h.Typeflag == tar.TypeSymlink && (raw == "bin" || raw == "sbin" || raw == "lib" || raw == "lib64") && strings.TrimPrefix(h.Linkname, "/") == "usr/"+raw {
			continue // Recreated once, after usr-merge normalization below.
		}
		name, err := archivePath(h.Name)
		if err != nil {
			return err
		}
		// Only SDK/resources, not host configuration or installation state.
		if name != "usr" && !strings.HasPrefix(name, "usr/") {
			continue
		}
		// Linux documentation can contain Index.html AND index.html, which
		// collide on default macOS filesystems. Only copyright notices (and
		// package-directory aliases needed to find them) enter this SDK.
		if strings.HasPrefix(name, "usr/share/doc/") {
			if h.Typeflag != tar.TypeDir && path.Base(name) != "copyright" &&
				!(h.Typeflag == tar.TypeSymlink && strings.Count(name, "/") == 3) {
				continue
			}
		}
		if strings.HasPrefix(name, "usr/share/man/") || strings.HasPrefix(name, "usr/share/info/") {
			continue
		}
		switch h.Typeflag {
		case tar.TypeDir:
			if err = root.MkdirAll(name, 0755); err != nil {
				return err
			}
		case tar.TypeSymlink, tar.TypeLink:
			target, e := linkTarget(name, h.Linkname, h.Typeflag == tar.TypeLink)
			if e != nil {
				return e
			}
			value := link{target, h.Typeflag == tar.TypeLink}
			if old, ok := links[name]; ok && old != value {
				return fmt.Errorf("conflicting link %s", name)
			}
			links[name] = value
			owners[name] = pkg
		case tar.TypeReg, tar.TypeRegA:
			if h.Size < 0 || h.Size > 512<<20 {
				return fmt.Errorf("oversized package member %s", name)
			}
			if err = root.MkdirAll(path.Dir(name), 0755); err != nil {
				return err
			}
			f, e := root.OpenFile(name, os.O_WRONLY|os.O_CREATE|os.O_EXCL, os.FileMode(h.Mode)&0755)
			if os.IsExist(e) {
				old, e := root.Open(name)
				if e != nil {
					return e
				}
				a, b := sha256.New(), sha256.New()
				_, e = io.Copy(a, old)
				old.Close()
				if e != nil {
					return e
				}
				if _, e = io.Copy(b, t); e != nil {
					return e
				}
				if !bytes.Equal(a.Sum(nil), b.Sum(nil)) {
					return fmt.Errorf("conflicting SDK file %s", name)
				}
				continue
			}
			if e != nil {
				return e
			}
			_, e = io.Copy(f, t)
			closeErr := f.Close()
			if e != nil {
				return e
			}
			if closeErr != nil {
				return closeErr
			}
			owners[name] = pkg
		default:
			return fmt.Errorf("unsupported tar entry %s", name)
		}
	}
}
