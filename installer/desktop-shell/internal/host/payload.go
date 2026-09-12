// Package host is independent of Wails: payload verification and the local-server
// boundary remain testable without a graphical session or a webview SDK.
package host

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/internal/appdata"
	"github.com/DerrickGold/ar-recomp/installer/internal/buildworkspace"
)

const Name = "ActRaiserRecompBuilder"
const manifestName = "builder-payload.json"

type File struct {
	Path   string `json:"path"`
	SHA256 string `json:"sha256"`
	Size   int64  `json:"size"`
	Mode   uint32 `json:"mode"`
}
type Manifest struct {
	Schema int    `json:"schema"`
	OS     string `json:"os"`
	Arch   string `json:"arch"`
	Files  []File `json:"files"`
}

func localPath(p string) bool {
	return p != "." && filepath.IsLocal(p) && filepath.ToSlash(filepath.Clean(p)) == p && !strings.ContainsAny(p, "\\\x00\r\n")
}

// WriteManifest accepts a clean, ROM-free CMake install tree, never a user's
// live installation. Reject mutable/game-derived files instead of copying
// them into an installer by accident.
func WriteManifest(root, goos, arch string) error {
	m := Manifest{Schema: 1, OS: goos, Arch: arch}
	err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(root, path)
		rel = filepath.ToSlash(rel)
		if rel == "." {
			return nil
		}
		if !localPath(rel) || d.Type()&os.ModeSymlink != 0 {
			return fmt.Errorf("unsupported payload path %s", rel)
		}
		if d.Name() == ".DS_Store" || strings.HasPrefix(d.Name(), "._") {
			return fmt.Errorf("macOS transfer metadata in payload: %s; restage without extended attributes", rel)
		}
		lower := strings.ToLower(rel)
		for _, prohibited := range []string{"utils/src/gen", "utils/saves", "utils/config.ini", "utils/settings.ini", "utils/diorama-layers.ini", "utils/game-assets/manifest.ini", "utils/build"} {
			if rel == prohibited || strings.HasPrefix(rel, prohibited+"/") {
				return fmt.Errorf("mutable/private payload input %s", rel)
			}
		}
		if strings.HasSuffix(lower, ".sfc") || strings.HasSuffix(lower, ".smc") {
			return fmt.Errorf("ROM must not enter Builder payload: %s", rel)
		}
		if d.IsDir() || rel == manifestName {
			return nil
		}
		info, err := d.Info()
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("non-regular payload file: %s", rel)
		}
		f, err := os.Open(path)
		if err != nil {
			return err
		}
		h := sha256.New()
		_, err = io.Copy(h, f)
		f.Close()
		if err != nil {
			return err
		}
		m.Files = append(m.Files, File{rel, hex.EncodeToString(h.Sum(nil)), info.Size(), uint32(info.Mode().Perm() & 0755)})
		return nil
	})
	if err != nil {
		return err
	}
	if err := validateManifest(m); err != nil {
		return err
	}
	data, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(root, manifestName), append(data, '\n'), 0644)
}

func validateManifest(m Manifest) error {
	if m.Schema != 1 || (m.OS != "darwin" && m.OS != "linux" && m.OS != "windows") || (m.Arch != "arm64" && m.Arch != "amd64") || len(m.Files) == 0 {
		return errors.New("unsupported Builder payload")
	}
	seen := map[string]bool{}
	windowsSeen := map[string]bool{}
	for _, f := range m.Files {
		hash, err := hex.DecodeString(f.SHA256)
		if !localPath(f.Path) || seen[f.Path] || f.Path == manifestName || f.Size < 0 || err != nil || len(hash) != 32 || f.Mode&^0755 != 0 {
			return fmt.Errorf("invalid payload entry: %s", f.Path)
		}
		seen[f.Path] = true
		if m.OS == "windows" {
			folded := strings.ToLower(f.Path)
			if !WindowsPath(f.Path) || windowsSeen[folded] {
				return fmt.Errorf("invalid or case-colliding Windows payload path: %s", f.Path)
			}
			windowsSeen[folded] = true
		}
	}
	for _, required := range requiredFiles(m.OS) {
		if !seen[required] {
			return fmt.Errorf("incomplete payload: missing %s", required)
		}
	}
	return nil
}

func executableName(name, goos string) string {
	if goos == "windows" {
		return name + ".exe"
	}
	return name
}

func requiredFiles(goos string) []string {
	return []string{"utils/tools/" + executableName("actraiser-builder", goos), "utils/tools/" + executableName("snesbuild", goos), "utils/snesbuild.ini", "utils/defaults/config.ini", "utils/tools/sdl3/include/SDL3/SDL.h"}
}

// ValidateWorkspace checks physical paths before creating any directories.
// Missing path components are resolved through their nearest existing ancestor,
// so a symlink cannot redirect a new workspace into the immutable application.
// This is a one-way containment check; callers requiring fully disjoint inputs,
// scratch and output use buildworkspace.Separate instead.
func ValidateWorkspace(workspace string, immutableRoots ...string) error {
	if !filepath.IsAbs(workspace) {
		return errors.New("workspace must be absolute")
	}
	physical, err := buildworkspace.Physical(workspace)
	if err != nil {
		return err
	}
	for _, root := range immutableRoots {
		// Some callers compare two future writable directories (output and
		// workspace). Both sides need identical ancestor resolution; requiring
		// the second to exist prevented every brand-new desktop installation.
		root, err := buildworkspace.Physical(root)
		if err != nil {
			return err
		}
		if rel, err := filepath.Rel(root, physical); err == nil && (rel == "." || filepath.IsLocal(rel)) {
			return errors.New("workspace cannot be inside the application or its payload")
		}
	}
	return nil
}

func DefaultWorkspace(artifact, override string) (string, error) {
	if override != "" {
		return filepath.Abs(override)
	}
	marker, err := os.ReadFile(artifact + ".portable")
	if err == nil {
		p := strings.TrimSpace(string(marker))
		if !localPath(p) {
			return "", errors.New("Builder .portable must name a dedicated relative workspace directory")
		}
		return filepath.Join(filepath.Dir(artifact), filepath.FromSlash(p)), nil
	}
	if !os.IsNotExist(err) {
		return "", err
	}
	base, err := appdata.Directory(runtime.GOOS, "installer", os.Getenv)
	if err != nil {
		return "", err
	}
	return filepath.Join(base, "workspace"), nil
}

// AuxiliaryDirectory keeps global browser/runtime data within the installer
// namespace without pre-creating the workspace before PrepareSession can own it.
// Explicit and portable workspaces retain their established sibling layout.
func AuxiliaryDirectory(workspace, kind string) string {
	base, err := appdata.Directory(runtime.GOOS, "installer", os.Getenv)
	if err == nil && filepath.Clean(workspace) == filepath.Join(base, "workspace") {
		return filepath.Join(base, kind)
	}
	return workspace + "-" + kind
}

// DefaultOutputDirectory is independent of the workspace storage mode. Desktop
// launchers do not reliably inherit the folder visible in Finder/Explorer, and
// an AppImage changes cwd while mounting. Anchor output to the outer artifact.
func DefaultOutputDirectory(artifact, override string) (string, error) {
	if override != "" {
		return filepath.Abs(override)
	}
	if !filepath.IsAbs(artifact) {
		return "", errors.New("Builder artifact must be absolute")
	}
	if strings.Contains(filepath.ToSlash(artifact), "/AppTranslocation/") {
		return "", errors.New("macOS is running a translocated copy of the Builder; move the app with Finder to your desired writable folder and relaunch, or choose --output-dir")
	}
	return filepath.Join(filepath.Dir(artifact), appdata.Name), nil
}

func Discover(executable, appImage string) (artifact, payload string, err error) {
	executable, err = filepath.EvalSymlinks(executable)
	if err != nil {
		return
	}
	bin := filepath.Dir(executable)
	if filepath.Base(bin) == "MacOS" && filepath.Base(filepath.Dir(bin)) == "Contents" {
		artifact = filepath.Dir(filepath.Dir(bin))
		payload = filepath.Join(artifact, "Contents", "Resources", "payload")
	} else if filepath.Base(bin) == "bin" && filepath.Base(filepath.Dir(bin)) == "usr" {
		artifact = filepath.Dir(filepath.Dir(bin))
		payload = filepath.Join(artifact, "usr", "share", Name, "payload")
		if appImage != "" {
			if !filepath.IsAbs(appImage) {
				err = errors.New("APPIMAGE must be absolute")
				return
			}
			artifact = appImage
		}
	} else {
		err = errors.New("Builder is not packaged; pass --payload for a development run")
	}
	return
}
