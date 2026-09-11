package linuxsdk

import (
	"debug/elf"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

func ELFArchitecture(filename string) (string, error) {
	f, err := elf.Open(filename)
	if err != nil {
		return "", err
	}
	defer f.Close()
	if f.Class != elf.ELFCLASS64 || f.Data != elf.ELFDATA2LSB {
		return "", fmt.Errorf("expected little-endian ELF64: %s", filename)
	}
	switch f.Machine {
	case elf.EM_X86_64:
		return "amd64", nil
	case elf.EM_AARCH64:
		return "arm64", nil
	}
	return "", fmt.Errorf("unsupported ELF machine: %s", f.Machine)
}
func (s *SDK) Needed(filename string) ([]string, error) {
	arch, err := ELFArchitecture(filename)
	if err != nil {
		return nil, err
	}
	if arch != s.Lock.Arch {
		return nil, fmt.Errorf("wrong ELF architecture in %s", filename)
	}
	f, err := elf.Open(filename)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	paths, err := runpaths(f, filename)
	if err != nil {
		return nil, err
	}
	if _, err = s.searchPaths(paths, filename); err != nil {
		return nil, err
	}
	return f.ImportedLibraries()
}
func (s *SDK) Library(soname string) (string, error) {
	return s.library(soname, nil)
}

// LibraryFrom also honors an ELF's private dependency directories. Paths are
// translated into the SDK, never resolved against the maintainer's /usr.
func (s *SDK) LibraryFrom(soname, importer string) (string, error) {
	f, err := elf.Open(importer)
	if err != nil {
		return "", err
	}
	defer f.Close()
	paths, err := runpaths(f, importer)
	if err != nil {
		return "", err
	}
	directories, err := s.searchPaths(paths, importer)
	if err != nil {
		return "", err
	}
	return s.library(soname, directories)
}

func runpaths(f *elf.File, importer string) ([]string, error) {
	paths, err := f.DynString(elf.DT_RUNPATH)
	if err != nil {
		return nil, err
	}
	if len(paths) == 0 {
		paths, err = f.DynString(elf.DT_RPATH)
		if err != nil {
			return nil, err
		}
		if len(paths) != 0 {
			// Legacy RPATH wins over AppRun's LD_LIBRARY_PATH and could
			// select incompatible HOST libraries. Current SDKs use RUNPATH.
			return nil, fmt.Errorf("legacy ELF RPATH requires relocation review: %s", importer)
		}
	}
	return paths, nil
}

func (s *SDK) searchPaths(paths []string, importer string) ([]string, error) {
	var directories []string
	for _, value := range paths {
		for _, dir := range strings.Split(value, ":") {
			if dir == "" {
				return nil, fmt.Errorf("empty ELF search path: %s", importer)
			}
			if strings.Contains(dir, "$ORIGIN") || strings.Contains(dir, "${ORIGIN}") {
				dir = strings.ReplaceAll(strings.ReplaceAll(dir, "${ORIGIN}", filepath.Dir(importer)), "$ORIGIN", filepath.Dir(importer))
			} else if filepath.IsAbs(dir) {
				dir = s.Path(dir)
			} else {
				return nil, fmt.Errorf("relative ELF search path: %s", dir)
			}
			rel, err := filepath.Rel(s.Root, filepath.Clean(dir))
			if err != nil || !filepath.IsLocal(rel) || strings.Contains(dir, "$") {
				return nil, fmt.Errorf("unsupported/escaping ELF search path: %s", dir)
			}
			directories = append(directories, dir)
		}
	}
	return directories, nil
}

func (s *SDK) library(soname string, extra []string) (string, error) {
	if filepath.Base(soname) != soname || strings.ContainsAny(soname, "\\\x00") {
		return "", fmt.Errorf("unsafe ELF dependency %q", soname)
	}
	directories := append([]string{s.Path("/usr/lib/" + s.Triple()), s.Path("/usr/lib")}, extra...)
	for _, dir := range directories {
		name := filepath.Join(dir, soname)
		if info, err := os.Stat(name); err == nil && info.Mode().IsRegular() {
			resolved, err := filepath.EvalSymlinks(name)
			if err != nil {
				return "", err
			}
			physicalRoot, err := filepath.EvalSymlinks(s.Root)
			if err != nil {
				return "", err
			}
			rel, err := filepath.Rel(physicalRoot, resolved)
			if err != nil || !filepath.IsLocal(rel) {
				return "", fmt.Errorf("library escapes SDK: %s", name)
			}
			arch, err := ELFArchitecture(name)
			if err != nil {
				return "", err
			}
			if arch != s.Lock.Arch {
				return "", fmt.Errorf("wrong architecture in dependency %s", name)
			}
			return name, nil
		}
	}
	return "", fmt.Errorf("SDK is missing ELF dependency %s", soname)
}
