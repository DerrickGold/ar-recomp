package desktop

import (
	"context"
	"debug/elf"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

func linuxArchitecture() string {
	return map[string]string{"amd64": "x86_64", "arm64": "aarch64"}[runtime.GOARCH]
}

func packageLinux(ctx context.Context, options PackageOptions, app, bin, libraries string) error {
	if linuxArchitecture() == "" {
		return fmt.Errorf("unsupported AppImage architecture %s", runtime.GOARCH)
	}
	for _, binary := range []string{options.Binary, options.Builder} {
		imports, err := inspectLinuxBinary(binary)
		if err != nil {
			return err
		}
		if len(imports) == 0 {
			continue
		} // Static Go helper.
		command := exec.CommandContext(ctx, "ldd", binary)
		output, err := command.CombinedOutput()
		if err != nil {
			return fmt.Errorf("inspect Linux dependencies: %w: %s", err, output)
		}
		dependencies, err := parseLdd(string(output))
		if err != nil {
			return err
		}
		for name, source := range dependencies {
			if _, err := inspectLinuxBinary(source); err != nil {
				return err
			}
			target := filepath.Join(libraries, name)
			if existing, err := fileHash(target); err == nil {
				incoming, err := fileHash(source)
				if err != nil {
					return err
				}
				if incoming != existing {
					return fmt.Errorf("conflicting library %s", name)
				}
				continue
			}
			if err := copyFileAtomic(source, target, 0755); err != nil {
				return err
			}
		}
	}
	if err := atomicWrite(filepath.Join(app, "AppRun"), []byte("#!/bin/sh\nAPP_ROOT=\"$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\" || exit 1\nexport LD_LIBRARY_PATH=\"$APP_ROOT/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\nexec \"$APP_ROOT/usr/bin/actraiser-builder\" app-launch \"$@\"\n"), 0755); err != nil {
		return err
	}
	if err := atomicWrite(filepath.Join(app, Name+".desktop"), []byte("[Desktop Entry]\nType=Application\nName=ActRaiser Recomp\nExec=actraiser-builder app-launch\nIcon=ActRaiserRecomp\nCategories=Game;\nTerminal=false\n"), 0644); err != nil {
		return err
	}
	// A source-native icon avoids adding a new raster asset or copied retail art.
	return atomicWrite(filepath.Join(app, Name+".svg"), []byte(`<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256"><rect width="256" height="256" rx="48" fill="#172940"/><path d="M128 32 44 210h38l16-38h60l16 38h38L128 32zm0 78 18 42h-36z" fill="#e2c077"/></svg>`), 0644)
}

func inspectLinuxBinary(path string) ([]string, error) {
	file, err := elf.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	imports, err := file.ImportedLibraries()
	if err != nil {
		return nil, err
	}
	for _, imported := range imports {
		if strings.Contains(imported, "/") {
			return nil, fmt.Errorf("ELF dependency must have a relocatable SONAME in %s: %s", path, imported)
		}
	}
	rpath, err := file.DynString(elf.DT_RPATH)
	if err != nil {
		return nil, err
	}
	runpath, err := file.DynString(elf.DT_RUNPATH)
	if err != nil {
		return nil, err
	}
	// Legacy RPATH takes precedence over LD_LIBRARY_PATH and could silently
	// load libraries from the build tree instead of the packaged dependencies.
	if len(rpath) != 0 && len(runpath) == 0 {
		return nil, fmt.Errorf("%s has legacy DT_RPATH; relink with --enable-new-dtags (DT_RUNPATH) before packaging", path)
	}
	return imports, nil
}

func parseLdd(output string) (map[string]string, error) {
	result := map[string]string{}
	for _, line := range strings.Split(output, "\n") {
		if strings.Contains(line, "not found") {
			return nil, fmt.Errorf("missing Linux dependency: %s", strings.TrimSpace(line))
		}
		name, value, found := strings.Cut(strings.TrimSpace(line), " => ")
		if !found {
			continue
		} // Kernel vDSO and the platform loader.
		name = strings.TrimSpace(name)
		end := strings.LastIndex(value, " (")
		if end >= 0 {
			value = value[:end]
		}
		path := strings.TrimSpace(value)
		if name == "" || filepath.Base(name) != name || !filepath.IsAbs(path) {
			return nil, fmt.Errorf("unrecognized ldd dependency: %s", line)
		}
		if glibcLibrary(name) {
			continue
		}
		result[name] = path
	}
	return result, nil
}

func glibcLibrary(name string) bool {
	for _, value := range []string{"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1", "libresolv.so.2", "libutil.so.1", "libanl.so.1", "ld-linux-x86-64.so.2", "ld-linux-aarch64.so.1"} {
		if name == value {
			return true
		}
	}
	return false
}

func resolveAppImageTools(options PackageOptions) (string, string, error) {
	tool, imageRuntime := options.AppImageTool, options.AppImageRuntime
	toolsDir := filepath.Dir(options.Builder)
	if tool == "" {
		tool = filepath.Join(toolsDir, "appimagetool")
	}
	if imageRuntime == "" {
		imageRuntime = filepath.Join(toolsDir, "appimage-runtime")
	}
	for _, path := range []string{tool, imageRuntime} {
		if _, err := fileHash(path); err != nil {
			return "", "", fmt.Errorf("AppImage toolchain unavailable at %s; supply --appimagetool and --appimage-runtime (or use --format appdir): %w", path, err)
		}
	}
	return tool, imageRuntime, nil
}

// ValidateAppImage checks the executable type-2 envelope without executing it.
func ValidateAppImage(path string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() || info.Mode().Perm()&0111 == 0 {
		return errors.New("AppImage must be a regular executable file")
	}
	header := make([]byte, 11)
	if _, err := io.ReadFull(f, header); err != nil {
		return err
	}
	if string(header[:4]) != "\x7fELF" || string(header[8:11]) != "AI\x02" {
		return errors.New("output is not a type-2 AppImage")
	}
	return nil
}
