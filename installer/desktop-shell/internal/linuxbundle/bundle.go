// Package linuxbundle assembles an offline GTK3/WebKitGTK 4.1 AppDir on a
// native Debian-family maintainer host, or from a pinned Linux SDK on macOS.
// It never installs packages; copied runtime files carry package provenance.
package linuxbundle

import (
	"bytes"
	"crypto/sha256"
	"debug/elf"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/linuxsdk"
)

const name = "ActRaiserRecompBuilder"

type Entry struct {
	Path          string `json:"path"`
	Source        string `json:"source"`
	SHA256        string `json:"sha256"`
	Package       string `json:"package,omitempty"`
	Version       string `json:"version,omitempty"`
	SourcePackage string `json:"sourcePackage,omitempty"`
	SourceVersion string `json:"sourceVersion,omitempty"`
}
type Relocation struct {
	Path         string         `json:"path"`
	BeforeSHA256 string         `json:"beforeSha256"`
	AfterSHA256  string         `json:"afterSha256"`
	Replacements map[string]int `json:"replacements"`
}
type Receipt struct {
	Schema       int                 `json:"schema"`
	Architecture string              `json:"architecture"`
	BuildOS      string              `json:"buildOS"`
	Files        []Entry             `json:"files"`
	Relocations  []Relocation        `json:"relocations"`
	ABI          *linuxsdk.ABIReport `json:"abi,omitempty"`
}
type builder struct {
	root     string
	receipt  Receipt
	seen     map[string]string
	packages map[string]Entry
	owners   map[string]string
	sdk      *linuxsdk.SDK
}

func command(program string, args ...string) (string, error) {
	c := exec.Command(program, args...)
	// Dependency discovery must not inherit a calling AppImage's search path.
	for _, env := range os.Environ() {
		key, _, _ := strings.Cut(env, "=")
		if key != "LD_LIBRARY_PATH" && key != "LD_PRELOAD" && key != "LC_ALL" {
			c.Env = append(c.Env, env)
		}
	}
	c.Env = append(c.Env, "LC_ALL=C")
	out, err := c.CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("%s %v: %w: %s", program, args, err, out)
	}
	return strings.TrimSpace(string(out)), nil
}

// HostLibrary is deliberately small. The OS still supplies its libc, display
// drivers and GPU interfaces. Do not bundle a maintainer's Mesa/NVIDIA driver.
// Keep the OS C++/unwinder runtime too: shipping an older libstdc++ can break
// the host's newer Mesa/LLVM even when WebKit's own glibc baseline is satisfied.
// Mesa also imports newer Wayland client symbols, even for some X11 launches.
// Do not shadow its client with our older GUI SDK's copy. See
// https://github.com/AppImageCommunity/pkg2appimage/pull/559.
func HostLibrary(soname string) bool {
	for _, exact := range []string{"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1", "libresolv.so.2", "libutil.so.1", "libanl.so.1", "libstdc++.so.6", "libgcc_s.so.1", "libwayland-client.so.0"} {
		if soname == exact {
			return true
		}
	}
	for _, prefix := range []string{"ld-linux-", "libEGL.so", "libGL.so", "libGLX.so", "libGLdispatch.so", "libOpenGL.so", "libGLES", "libgbm.so", "libdrm", "libvulkan.so"} {
		if strings.HasPrefix(soname, prefix) {
			return true
		}
	}
	return false
}

// Check the finished GUI tree, not just dependency traversal: a future resource
// copy must not silently reintroduce a host-coupled library. Run before adding
// the isolated compiler/SDL payload, whose libraries do not enter GUI lookup.
func validateHostLibraries(root string) error {
	return filepath.WalkDir(filepath.Join(root, "usr/lib"), func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !entry.IsDir() && HostLibrary(entry.Name()) {
			return fmt.Errorf("GUI bundle shadows a host graphics/system library: %s", path)
		}
		return nil
	})
}

// Dependencies parses ldd output from trusted locally built/distro ELFs only.
func Dependencies(output string) ([]string, error) {
	var paths []string
	for _, line := range strings.Split(output, "\n") {
		fields := strings.Fields(line)
		if len(fields) >= 3 && fields[1] == "=>" {
			if fields[2] == "not" {
				return nil, fmt.Errorf("unresolved ELF dependency: %s", line)
			}
			if !filepath.IsAbs(fields[2]) {
				return nil, fmt.Errorf("invalid ldd path: %s", line)
			}
			if !HostLibrary(fields[0]) {
				paths = append(paths, fields[2])
			}
		}
	}
	return paths, nil
}

func hash(data []byte) string { h := sha256.Sum256(data); return hex.EncodeToString(h[:]) }

// Relocate changes only known absolute path prefixes at string boundaries,
// preserving byte length and therefore ELF offsets. No broad /usr replacement.
// AppRun must keep cwd at AppDir/usr for the lifetime of WebKit processes.
func Relocate(data []byte, paths []string) ([]byte, map[string]int, error) {
	result := bytes.Clone(data)
	counts := map[string]int{}
	for _, old := range paths {
		if !strings.HasPrefix(old, "/usr/") {
			return nil, nil, fmt.Errorf("unsupported relocation %s", old)
		}
		newPath := "././" + old[4:]
		for offset := 0; offset < len(result); {
			i := bytes.Index(result[offset:], []byte(old))
			if i < 0 {
				break
			}
			i += offset
			end := i + len(old)
			if (i == 0 || result[i-1] == 0) && end < len(result) && (result[end] == 0 || result[end] == '/') {
				copy(result[i:end], newPath)
				counts[old]++
			}
			offset = end
		}
	}
	return result, counts, nil
}

func (b *builder) copy(from, rel string, provenance bool) error {
	if !filepath.IsLocal(rel) {
		return fmt.Errorf("unsafe destination %s", rel)
	}
	resolved, err := filepath.EvalSymlinks(from)
	if err != nil {
		return err
	}
	if b.sdk != nil {
		rel, err := filepath.Rel(b.sdk.Root, resolved)
		if err != nil || !filepath.IsLocal(rel) {
			return fmt.Errorf("runtime resource escapes SDK: %s", from)
		}
	}
	info, err := os.Stat(resolved)
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("not a regular input: %s", from)
	}
	data, err := os.ReadFile(resolved)
	if err != nil {
		return err
	}
	digest := hash(data)
	if previous, exists := b.seen[rel]; exists {
		if previous != digest {
			return fmt.Errorf("conflicting library/resource: %s", rel)
		}
		return nil
	}
	to := filepath.Join(b.root, rel)
	if err = os.MkdirAll(filepath.Dir(to), 0755); err != nil {
		return err
	}
	f, err := os.OpenFile(to, os.O_WRONLY|os.O_CREATE|os.O_EXCL, info.Mode().Perm()&0755)
	if err != nil {
		return err
	}
	_, writeErr := f.Write(data)
	closeErr := f.Close()
	if writeErr != nil {
		return writeErr
	}
	if closeErr != nil {
		return closeErr
	}
	b.seen[rel] = digest
	entry := Entry{Path: filepath.ToSlash(rel), Source: from, SHA256: digest}
	if b.sdk != nil {
		relative, _ := filepath.Rel(b.sdk.Root, from)
		entry.Source = "/" + filepath.ToSlash(relative)
	}
	if provenance && b.sdk != nil {
		p, err := b.sdk.Owner(from)
		if err != nil {
			return err
		}
		entry.Package, entry.Version = p.Name, p.Version
		parts := strings.Fields(p.Source)
		entry.SourcePackage, entry.SourceVersion = parts[0], p.Version
		if len(parts) > 1 {
			entry.SourceVersion = strings.Trim(parts[1], "()")
		}
		if _, exists := b.packages[p.Name]; !exists {
			b.packages[p.Name] = entry
			if err := b.copy(b.sdk.Path("/usr/share/doc/"+p.Name+"/copyright"), filepath.Join("usr/share/doc", name, "runtime-licenses", p.Name, "copyright"), false); err != nil {
				return err
			}
		}
	} else if provenance {
		pkg := b.owners[from]
		if pkg == "" {
			owner, err := command("dpkg-query", "-S", from)
			if err != nil && resolved != from {
				owner, err = command("dpkg-query", "-S", resolved)
			}
			if err != nil {
				return err
			}
			var ok bool
			pkg, _, ok = strings.Cut(strings.Split(owner, "\n")[0], ": ")
			if !ok {
				return fmt.Errorf("cannot identify package owner of %s", from)
			}
		}
		meta, exists := b.packages[pkg]
		if !exists {
			text, err := command("dpkg-query", "-W", "-f=${Version}\t${source:Package}\t${source:Version}", pkg)
			if err != nil {
				return err
			}
			fields := strings.Split(text, "\t")
			if len(fields) != 3 {
				return fmt.Errorf("invalid package provenance for %s", pkg)
			}
			meta = Entry{Package: pkg, Version: fields[0], SourcePackage: fields[1], SourceVersion: fields[2]}
			b.packages[pkg] = meta
			base, _, _ := strings.Cut(pkg, ":")
			if err := b.copy(filepath.Join("/usr/share/doc", base, "copyright"), filepath.Join("usr/share/doc", name, "runtime-licenses", pkg, "copyright"), false); err != nil {
				return err
			}
		}
		entry.Package, entry.Version, entry.SourcePackage, entry.SourceVersion = meta.Package, meta.Version, meta.SourcePackage, meta.SourceVersion
	}
	b.receipt.Files = append(b.receipt.Files, entry)
	return nil
}

func (b *builder) elf(from, rel string) error {
	already := b.seen[rel] != ""
	if err := b.copy(from, rel, true); err != nil {
		return err
	}
	if b.sdk != nil {
		if already {
			return nil
		}
		dependencies, err := b.sdk.Needed(from)
		if err != nil {
			return err
		}
		for _, soname := range dependencies {
			if HostLibrary(soname) {
				continue
			}
			file, err := b.sdk.LibraryFrom(soname, from)
			if err != nil {
				return err
			}
			if err := b.elf(file, filepath.Join("usr/lib", soname)); err != nil {
				return err
			}
		}
		return nil
	}
	output, err := command("ldd", from)
	if err != nil {
		return err
	}
	paths, err := Dependencies(output)
	if err != nil {
		return err
	}
	for _, path := range paths {
		if err := b.copy(path, filepath.Join("usr/lib", filepath.Base(path)), true); err != nil {
			return err
		}
	}
	return nil
}

func (b *builder) tree(source, destination string, elfs bool) error {
	if b.sdk == nil {
		owners, err := command("dpkg-query", "-S", source+"/*")
		if err != nil {
			return err
		}
		for _, line := range strings.Split(owners, "\n") {
			pkg, path, ok := strings.Cut(line, ": ")
			if ok {
				b.owners[path] = pkg
			}
		}
	}
	return filepath.WalkDir(source, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		switch d.Name() {
		case "gschemas.compiled", "icon-theme.cache", "giomodule.cache", "immodules.cache", "loaders.cache":
			return nil
		}
		rel, err := filepath.Rel(source, path)
		if err != nil {
			return err
		}
		to := filepath.Join(destination, rel)
		if elfs {
			if f, err := elf.Open(path); err == nil {
				f.Close()
				return b.elf(path, to)
			}
		}
		return b.copy(path, to, true)
	})
}

// Stage requires a new empty AppDir prepared by the package command, with only
// the shell already installed. The project payload is added after this pass so
// the GUI deployment never scans or rewrites the compiler/SDL toolchain.
func Stage(root, shell string) error {
	if runtime.GOOS != "linux" {
		return fmt.Errorf("Linux runtime deployment must run on Linux")
	}
	return stage(root, shell, nil)
}

// StageSDK is host-independent. ELF and package metadata replace ldd/dpkg;
// cache compilers are native HOST tools, never programs from the Linux SDK.
func StageSDK(root, shell string, sdk *linuxsdk.SDK) error { return stage(root, shell, sdk) }

func stage(root, shell string, sdk *linuxsdk.SDK) error {
	var libdir string
	var err error
	if sdk != nil {
		libdir = "/usr/lib/" + sdk.Triple()
	} else {
		libdir, err = command("pkg-config", "--variable=libdir", "webkit2gtk-4.1")
	}
	if err != nil {
		return err
	}
	if !strings.HasPrefix(libdir, "/usr/lib/") || strings.ContainsAny(libdir, "\n\r ") {
		return fmt.Errorf("unsupported native library directory: %s", libdir)
	}
	b := builder{root: root, sdk: sdk, seen: map[string]string{}, packages: map[string]Entry{}, owners: map[string]string{}, receipt: Receipt{Schema: 1, Architecture: runtime.GOARCH}}
	source := func(p string) string {
		if sdk != nil {
			return sdk.Path(p)
		}
		return p
	}
	if sdk != nil {
		b.receipt.Architecture = sdk.Lock.Arch
		b.receipt.BuildOS = "Linux SDK: " + sdk.Lock.Distribution + "; packaging host: " + runtime.GOOS + "/" + runtime.GOARCH
		deps, err := sdk.Needed(shell)
		if err != nil {
			return err
		}
		for _, soname := range deps {
			if HostLibrary(soname) {
				continue
			}
			file, err := sdk.Library(soname)
			if err != nil {
				return err
			}
			if err = b.elf(file, filepath.Join("usr/lib", soname)); err != nil {
				return err
			}
		}
	} else {
		osRelease, err := os.ReadFile("/etc/os-release")
		if err != nil {
			return err
		}
		b.receipt.BuildOS = string(osRelease)
		output, err := command("ldd", shell)
		if err != nil {
			return err
		}
		deps, err := Dependencies(output)
		if err != nil {
			return err
		}
		for _, path := range deps {
			if err = b.copy(path, filepath.Join("usr/lib", filepath.Base(path)), true); err != nil {
				return err
			}
		}
	}
	webkitRoot := filepath.Join(libdir, "webkit2gtk-4.1")
	for _, leaf := range []string{"WebKitWebProcess", "WebKitNetworkProcess", "WebKitGPUProcess", "injected-bundle/libwebkit2gtkinjectedbundle.so"} {
		if err = b.elf(source(filepath.Join(webkitRoot, leaf)), filepath.Join(strings.TrimPrefix(webkitRoot, "/"), leaf)); err != nil {
			return err
		}
	}
	for _, path := range []string{"/usr/bin/bwrap", "/usr/bin/xdg-dbus-proxy"} {
		if err = b.elf(source(path), strings.TrimPrefix(path, "/")); err != nil {
			return err
		}
	}
	for _, pair := range [][2]string{
		{filepath.Join(libdir, "gio/modules"), "usr/lib/gio/modules"},
		{filepath.Join(libdir, "gstreamer-1.0"), "usr/lib/gstreamer-1.0"},
		{filepath.Join(libdir, "gstreamer1.0/gstreamer-1.0/gst-plugin-scanner"), "usr/libexec/gst-plugin-scanner"},
		{filepath.Join(libdir, "gdk-pixbuf-2.0/2.10.0"), "usr/lib/gdk-pixbuf-2.0/2.10.0"},
		{filepath.Join(libdir, "gtk-3.0/3.0.0"), "usr/lib/gtk-3.0/3.0.0"},
		{"/usr/share/glib-2.0/schemas", "usr/share/glib-2.0/schemas"},
		{"/usr/share/mime/packages", "usr/share/mime/packages"},
		{"/usr/share/icons/Adwaita", "usr/share/icons/Adwaita"},
		{"/usr/share/icons/hicolor", "usr/share/icons/hicolor"},
	} {
		pair[0] = source(pair[0])
		if info, err := os.Stat(pair[0]); err != nil {
			return err
		} else if info.IsDir() {
			if err = b.tree(pair[0], pair[1], true); err != nil {
				return err
			}
		} else if err = b.elf(pair[0], pair[1]); err != nil {
			return err
		}
	}
	if _, err = command("glib-compile-schemas", filepath.Join(root, "usr/share/glib-2.0/schemas")); err != nil {
		return err
	}
	if _, err = command("update-mime-database", filepath.Join(root, "usr/share/mime")); err != nil {
		return err
	}
	// Caches contain absolute maintainer paths. Regenerate at launch into a
	// private temporary directory, never in the mounted AppImage.
	for _, pair := range [][2]string{
		{filepath.Join(libdir, "gdk-pixbuf-2.0/gdk-pixbuf-query-loaders"), "usr/bin/gdk-pixbuf-query-loaders"},
		{filepath.Join(libdir, "libgtk-3-0/gtk-query-immodules-3.0"), "usr/bin/gtk-query-immodules-3.0"},
	} {
		if err = b.elf(source(pair[0]), pair[1]); err != nil {
			return err
		}
	}
	webkitLib := filepath.Join(root, "usr/lib/libwebkit2gtk-4.1.so.0")
	data, err := os.ReadFile(webkitLib)
	if err != nil {
		return err
	}
	relocated, counts, err := Relocate(data, []string{webkitRoot, "/usr/bin/bwrap", "/usr/bin/xdg-dbus-proxy"})
	if err != nil {
		return err
	}
	if counts[webkitRoot] == 0 {
		return fmt.Errorf("WebKit helper prefix not found; audit this WebKit build before packaging")
	}
	if err = os.WriteFile(webkitLib, relocated, 0644); err != nil {
		return err
	}
	b.receipt.Relocations = append(b.receipt.Relocations, Relocation{Path: "usr/lib/libwebkit2gtk-4.1.so.0", BeforeSHA256: hash(data), AfterSHA256: hash(relocated), Replacements: counts})
	for i := range b.receipt.Files {
		if b.receipt.Files[i].Path == "usr/lib/libwebkit2gtk-4.1.so.0" {
			b.receipt.Files[i].SHA256 = hash(relocated)
		}
	}
	if err = validateHostLibraries(root); err != nil {
		return err
	}
	sort.Slice(b.receipt.Files, func(i, j int) bool { return b.receipt.Files[i].Path < b.receipt.Files[j].Path })
	if sdk != nil {
		audit, err := linuxsdk.AuditABI(root, sdk.Lock.Arch, linuxsdk.GLIBCBaseline)
		if err != nil {
			return err
		}
		b.receipt.ABI = &audit
	}
	manifest, err := json.MarshalIndent(b.receipt, "", "  ")
	if err != nil {
		return err
	}
	if err = os.WriteFile(filepath.Join(root, "usr/share/doc", name, "linux-runtime.json"), append(manifest, '\n'), 0644); err != nil {
		return err
	}
	if sdk != nil {
		lock, err := json.MarshalIndent(sdk.Lock, "", "  ")
		if err != nil {
			return err
		}
		if err = os.WriteFile(filepath.Join(root, "usr/share/doc", name, "linux-sdk-lock.json"), append(lock, '\n'), 0644); err != nil {
			return err
		}
	}
	return writeLauncher(root, strings.TrimPrefix(webkitRoot, "/usr/"))
}

func writeLauncher(root, webkitRel string) error {
	launcher := `#!/bin/sh
set -eu
APPDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
export APPDIR
# Keep only the GUI's loader search paths here. The Go host removes them from
# the child builder's environment before starting any compilation or game.
export LD_LIBRARY_PATH="$APPDIR/usr/lib"
unset LD_PRELOAD
export GIO_MODULE_DIR="$APPDIR/usr/lib/gio/modules"
export GIO_EXTRA_MODULES="$GIO_MODULE_DIR"
export GSETTINGS_SCHEMA_DIR="$APPDIR/usr/share/glib-2.0/schemas"
export GSETTINGS_BACKEND=memory
export GTK_PATH="$APPDIR/usr/lib/gtk-3.0"
export AR_BUILDER_HOST_XDG_DATA_DIRS="${XDG_DATA_DIRS-}"
if [ "${XDG_DATA_DIRS+x}" = x ]; then export AR_BUILDER_HOST_XDG_DATA_DIRS_MODE=set; else export AR_BUILDER_HOST_XDG_DATA_DIRS_MODE=unset; fi
export XDG_DATA_DIRS="$APPDIR/usr/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}:/usr/local/share:/usr/share"
export GST_PLUGIN_SYSTEM_PATH_1_0="$APPDIR/usr/lib/gstreamer-1.0"
export GST_PLUGIN_PATH_1_0="$GST_PLUGIN_SYSTEM_PATH_1_0"
export GST_PLUGIN_SCANNER="$APPDIR/usr/libexec/gst-plugin-scanner"
export WEBKIT_INJECTED_BUNDLE_PATH="$APPDIR/usr/@WEBKIT@/injected-bundle"
export GDK_PIXBUF_MODULEDIR="$APPDIR/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders"
cache=$(mktemp -d "${TMPDIR:-/tmp}/actraiser-builder-webview.XXXXXXXX")
trap 'rm -f "$cache/pixbuf.cache" "$cache/immodules.cache" "$cache/gstreamer.bin"; rmdir "$cache" 2>/dev/null || true' EXIT
export GDK_PIXBUF_MODULE_FILE="$cache/pixbuf.cache"
export GTK_IM_MODULE_FILE="$cache/immodules.cache"
export GST_REGISTRY_1_0="$cache/gstreamer.bin"
"$APPDIR/usr/bin/gdk-pixbuf-query-loaders" "$GDK_PIXBUF_MODULEDIR/"*.so > "$GDK_PIXBUF_MODULE_FILE"
"$APPDIR/usr/bin/gtk-query-immodules-3.0" "$APPDIR/usr/lib/gtk-3.0/3.0.0/immodules/"*.so > "$GTK_IM_MODULE_FILE"
cd "$APPDIR/usr"
"$APPDIR/usr/bin/ActRaiserRecompBuilder" "$@"
`
	launcher = strings.ReplaceAll(launcher, "@WEBKIT@", webkitRel)
	if err := os.WriteFile(filepath.Join(root, "AppRun"), []byte(launcher), 0755); err != nil {
		return err
	}
	desktop := "[Desktop Entry]\nType=Application\nName=ActRaiser Recomp Builder\nExec=ActRaiserRecompBuilder\nIcon=ActRaiserRecompBuilder\nStartupWMClass=ActRaiserRecompBuilderWebView\nCategories=Game;Development;\nTerminal=false\n"
	if err := os.WriteFile(filepath.Join(root, name+".desktop"), []byte(desktop), 0644); err != nil {
		return err
	}
	icon := `<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256"><rect width="256" height="256" rx="48" fill="#181b24"/><path d="M48 184 108 56h40l60 128h-40l-12-28H100l-12 28zm64-60h32l-16-40z" fill="#edcd83"/></svg>`
	return os.WriteFile(filepath.Join(root, name+".svg"), []byte(icon), 0644)
}

// CopyPayload copies only a prevalidated clean staged payload, rejecting links.
func CopyPayload(source, target string) error {
	return filepath.WalkDir(source, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(source, path)
		if err != nil {
			return err
		}
		to := filepath.Join(target, rel)
		if d.IsDir() {
			return os.MkdirAll(to, 0755)
		}
		info, err := d.Info()
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("non-regular payload input: %s", path)
		}
		in, err := os.Open(path)
		if err != nil {
			return err
		}
		defer in.Close()
		out, err := os.OpenFile(to, os.O_CREATE|os.O_EXCL|os.O_WRONLY, info.Mode().Perm()&0755)
		if err != nil {
			return err
		}
		_, copyErr := io.Copy(out, in)
		closeErr := out.Close()
		if copyErr != nil {
			return copyErr
		}
		return closeErr
	})
}
