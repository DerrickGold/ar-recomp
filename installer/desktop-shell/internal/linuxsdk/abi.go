package linuxsdk

import (
	"debug/elf"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

type ELFRequirement struct {
	Path         string                   `json:"path"`
	GLIBC        string                   `json:"glibc,omitempty"`
	Needed       []string                 `json:"needed,omitempty"`
	VersionNeeds []elf.DynamicVersionNeed `json:"versionNeeds,omitempty"`
}
type ABIReport struct {
	Schema          int              `json:"schema"`
	Architecture    string           `json:"architecture"`
	MaximumGLIBC    string           `json:"maximumGLIBC"`
	EnforcedMaximum string           `json:"enforcedMaximum,omitempty"`
	Files           []ELFRequirement `json:"files"`
}

// Version-needs, not a strings/grep approximation: this includes ABI tags such
// as DT_RELR even if no ordinary imported symbol carries that requirement.
func requiredGLIBC(needs []elf.DynamicVersionNeed) (string, error) {
	maximum := ""
	for _, library := range needs {
		for _, need := range library.Needs {
			if !strings.HasPrefix(need.Dep, "GLIBC_") {
				continue
			}
			version := strings.TrimPrefix(need.Dep, "GLIBC_")
			if version == "ABI_DT_RELR" {
				version = "2.36"
			}
			parts := strings.Split(version, ".")
			if len(parts) < 2 || len(parts) > 3 {
				return "", fmt.Errorf("unsupported glibc ABI requirement %s from %s", need.Dep, library.Name)
			}
			for _, part := range parts {
				if n, err := strconv.Atoi(part); err != nil || n < 0 {
					return "", fmt.Errorf("invalid glibc ABI version %s", need.Dep)
				}
			}
			if maximum == "" || compareVersion(version, maximum) > 0 {
				maximum = version
			}
		}
	}
	return maximum, nil
}

// AuditABI examines every ELF executable/shared object, including helpers,
// plugins and (when present) the offline compiler/SDL payload. It runs on macOS
// without executing Linux code. A successful report is NOT a GPU/Deck test.
func AuditABI(root, arch, maximum string) (ABIReport, error) {
	report := ABIReport{Schema: 1, Architecture: arch, EnforcedMaximum: maximum, Files: []ELFRequirement{}}
	err := filepath.WalkDir(root, func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			return nil
		}
		if !entry.Type().IsRegular() {
			return fmt.Errorf("ABI audit refuses non-regular file %s", path)
		}
		input, err := os.Open(path)
		if err != nil {
			return err
		}
		var magic [4]byte
		_, readErr := io.ReadFull(input, magic[:])
		input.Close()
		if errors.Is(readErr, io.EOF) || errors.Is(readErr, io.ErrUnexpectedEOF) {
			return nil
		}
		if readErr != nil {
			return readErr
		}
		if string(magic[:]) != "\x7fELF" {
			return nil
		}
		f, err := elf.Open(path)
		if err != nil {
			return err
		}
		defer f.Close()
		// Relocatable objects in a toolchain are inputs, not loadable programs.
		if f.Type != elf.ET_EXEC && f.Type != elf.ET_DYN {
			return nil
		}
		actual, err := ELFArchitecture(path)
		if err != nil {
			return err
		}
		if actual != arch {
			return fmt.Errorf("ABI audit: %s is %s, expected %s", path, actual, arch)
		}
		var versions []elf.DynamicVersionNeed
		if f.SectionByType(elf.SHT_GNU_VERNEED) != nil {
			versions, err = f.DynamicVersionNeeds()
			if err != nil {
				return fmt.Errorf("ABI audit %s: %w", path, err)
			}
		}
		glibc, err := requiredGLIBC(versions)
		if err != nil {
			return fmt.Errorf("ABI audit %s: %w", path, err)
		}
		if maximum != "" && glibc != "" && compareVersion(glibc, maximum) > 0 {
			return fmt.Errorf("%s requires GLIBC_%s, above supported GLIBC_%s", path, glibc, maximum)
		}
		needed, err := f.ImportedLibraries()
		if err != nil {
			return err
		}
		relative, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		report.Files = append(report.Files, ELFRequirement{Path: filepath.ToSlash(relative), GLIBC: glibc, Needed: needed, VersionNeeds: versions})
		if glibc != "" && (report.MaximumGLIBC == "" || compareVersion(glibc, report.MaximumGLIBC) > 0) {
			report.MaximumGLIBC = glibc
		}
		return nil
	})
	sort.Slice(report.Files, func(i, j int) bool { return report.Files[i].Path < report.Files[j].Path })
	return report, err
}
