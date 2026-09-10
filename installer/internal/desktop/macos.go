package desktop

import (
	"context"
	"debug/macho"
	"encoding/xml"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

type machoInfo struct{ imports, rpaths []string }

func inspectMachO(path string) (machoInfo, error) {
	var result machoInfo
	read := func(file *macho.File) error {
		for _, load := range file.Loads {
			raw := load.Raw()
			if isDylibImport(file.ByteOrder.Uint32(raw[:4])) {
				name, err := machoCommandString(raw, file.ByteOrder)
				if err != nil {
					return err
				}
				result.imports = append(result.imports, name)
			}
			if rpath, ok := load.(*macho.Rpath); ok {
				result.rpaths = append(result.rpaths, rpath.Path)
			}
		}
		return nil
	}
	if fat, err := macho.OpenFat(path); err == nil {
		defer fat.Close()
		for _, arch := range fat.Arches {
			if err := read(arch.File); err != nil {
				return result, err
			}
		}
	} else {
		file, err := macho.Open(path)
		if err != nil {
			return result, fmt.Errorf("inspect Mach-O %s: %w", path, err)
		}
		defer file.Close()
		if err := read(file); err != nil {
			return result, err
		}
	}
	result.imports, result.rpaths = unique(result.imports), unique(result.rpaths)
	return result, nil
}

func unique(values []string) []string {
	seen := map[string]bool{}
	var out []string
	for _, value := range values {
		if !seen[value] {
			seen[value] = true
			out = append(out, value)
		}
	}
	return out
}

func systemMachO(path string) bool {
	return strings.HasPrefix(path, "/System/Library/") || strings.HasPrefix(path, "/usr/lib/")
}

func packageMacOS(ctx context.Context, options PackageOptions, app, bin, libraries string) error {
	rootInfo, err := inspectMachO(options.Binary)
	if err != nil {
		return err
	}
	installed := map[string]string{} // destination leaf -> source identity
	var rewrite func(string, string, bool, []string) error
	rewrite = func(source, target string, library bool, inherited []string) error {
		info, err := inspectMachO(source)
		if err != nil {
			return err
		}
		expand := func(path string) string {
			path = strings.ReplaceAll(path, "@loader_path", filepath.Dir(source))
			return strings.ReplaceAll(path, "@executable_path", filepath.Dir(options.Binary))
		}
		paths := append([]string{}, inherited...)
		for _, path := range info.rpaths {
			paths = append(paths, expand(path))
		}
		changes := map[string]string{}
		for _, dependency := range info.imports {
			if systemMachO(dependency) {
				continue
			}
			var candidates []string
			if strings.HasPrefix(dependency, "@rpath/") {
				for _, path := range paths {
					candidates = append(candidates, filepath.Join(path, strings.TrimPrefix(dependency, "@rpath/")))
				}
				candidates = append(candidates, filepath.Join(filepath.Dir(source), filepath.Base(dependency)))
			} else {
				candidates = []string{expand(dependency)}
			}
			resolved := ""
			for _, candidate := range candidates {
				if !filepath.IsAbs(candidate) {
					continue
				}
				if info, err := os.Stat(candidate); err == nil && info.Mode().IsRegular() {
					resolved = candidate
					break
				}
			}
			if resolved == "" {
				return fmt.Errorf("cannot bundle dependency %s of %s", dependency, source)
			}
			leaf := filepath.Base(resolved)
			if !strings.HasSuffix(leaf, ".dylib") {
				leaf += ".dylib"
			}
			identity, err := filepath.EvalSymlinks(resolved)
			if err != nil {
				return err
			}
			if prior, exists := installed[leaf]; exists {
				if prior != identity {
					return fmt.Errorf("different libraries share the name %s: %s and %s", leaf, prior, identity)
				}
			} else {
				installed[leaf] = identity
				if err := copyMacLibraryNotices(identity, filepath.Join(app, "Contents", "Resources", "notices", "libraries", leaf)); err != nil {
					return err
				}
				to := filepath.Join(libraries, leaf)
				if err := copyFileAtomic(resolved, to, 0755); err != nil {
					return err
				}
				if err := rewrite(resolved, to, true, paths); err != nil {
					return err
				}
			}
			newPath := "@executable_path/../Frameworks/" + leaf
			if library {
				newPath = "@loader_path/" + leaf
			}
			changes[dependency] = newPath
		}
		// Once imports are explicit relative paths, no build-machine rpath is
		// needed. Removing it also prevents accidental system fallback later.
		id := ""
		if library {
			id = "@rpath/" + filepath.Base(target)
		}
		return relocateMachO(target, changes, id)
	}
	paths := []string{}
	for _, path := range rootInfo.rpaths {
		paths = append(paths, strings.ReplaceAll(strings.ReplaceAll(path, "@executable_path", filepath.Dir(options.Binary)), "@loader_path", filepath.Dir(options.Binary)))
	}
	for _, item := range [][2]string{{options.Binary, filepath.Join(bin, Name)}, {options.Builder, filepath.Join(bin, "actraiser-builder")}} {
		if err := rewrite(item[0], item[1], false, paths); err != nil {
			return err
		}
	}
	if err := atomicWrite(filepath.Join(app, "Contents", "Info.plist"), []byte(macOSPlist(options.Version)), 0644); err != nil {
		return err
	}
	var code []string
	for leaf := range installed {
		code = append(code, filepath.Join(libraries, leaf))
	}
	sort.Strings(code)
	code = append(code, filepath.Join(bin, Name), filepath.Join(bin, "actraiser-builder"), app)
	for _, path := range code {
		if err := runTool(ctx, options.Output, "/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", path); err != nil {
			return err
		}
	}
	return runTool(ctx, options.Output, "/usr/bin/codesign", "--verify", "--deep", "--strict", app)
}

// Source builds may use Homebrew's dependency tree instead of the release
// SDK. Carry those installed notices too, alongside the vended SDK notices.
func copyMacLibraryNotices(source, destination string) error {
	index := strings.Index(source, "/Cellar/")
	if index < 0 {
		return nil
	}
	parts := strings.Split(source[index+len("/Cellar/"):], "/")
	if len(parts) < 3 {
		return nil
	}
	root := filepath.Join(source[:index], "Cellar", parts[0], parts[1])
	for _, pattern := range []string{"LICENSE*", "COPYING*", "COPYRIGHT*", "share/doc/*/COPYING*", "share/licenses/*"} {
		matches, err := filepath.Glob(filepath.Join(root, pattern))
		if err != nil {
			return err
		}
		for _, path := range matches {
			info, err := os.Stat(path)
			if err != nil {
				return err
			}
			relative, _ := filepath.Rel(root, path)
			if info.IsDir() {
				err = copyTree(path, filepath.Join(destination, relative))
			} else {
				err = copyFileAtomic(path, filepath.Join(destination, relative), 0644)
			}
			if err != nil {
				return err
			}
		}
	}
	return nil
}

func macOSPlist(version string) string {
	// Git-describe strings are useful metadata but not valid bundle versions.
	var versionXML strings.Builder
	xml.EscapeText(&versionXML, []byte(version))
	return `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>actraiser-builder</string>
<key>CFBundleIdentifier</key><string>org.actraiser-recomp.ActRaiserRecomp</string>
<key>CFBundleName</key><string>ActRaiserRecomp</string>
<key>CFBundleDisplayName</key><string>ActRaiser Recomp</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>ActRaiserBuildVersion</key><string>` + versionXML.String() + `</string>
<key>NSHighResolutionCapable</key><true/>
<key>LSApplicationCategoryType</key><string>public.app-category.games</string>
<key>LSMinimumSystemVersion</key><string>11.0</string>
</dict></plist>
`
}
