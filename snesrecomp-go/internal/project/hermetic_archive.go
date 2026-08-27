package project

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// createObjectArchive packages the exact object list for one hermetic link
// into a temporary static archive. Zig's linkers may keep every direct object
// input open at once; that exceeds the 256-descriptor limit inherited by a
// macOS GUI process once a project grows large enough. An archive is one
// linker input regardless of how many members it contains.
//
// The response file also avoids Windows' command-line length limit. It uses
// LLVM's explicitly selected POSIX response-file grammar on every host, so
// path quoting does not change when the build targets another platform.
func createObjectArchive(zigPath, targetOS, outputDir string, objects []string) (string, func(), error) {
	if len(objects) == 0 {
		return "", nil, fmt.Errorf("cannot create an empty object archive")
	}
	temporaryDir, err := os.MkdirTemp(outputDir, ".hermetic-link-")
	if err != nil {
		return "", nil, err
	}
	extension := ".a"
	if targetOS == "windows" {
		extension = ".lib"
	}
	archivePath := filepath.Join(temporaryDir, "objects"+extension)
	cleanup := func() {
		_ = os.Remove(archivePath)
		_ = os.Remove(temporaryDir)
	}
	if err := writeObjectArchive(zigPath, targetOS, archivePath, objects); err != nil {
		cleanup()
		return "", nil, err
	}
	return archivePath, cleanup, nil
}

// writeObjectArchive creates one deterministic static-library input from an
// exact object list. A fresh archive is built beside the destination and then
// renamed into place, so removed manifest entries cannot survive as stale
// members and a failed `ar` invocation cannot destroy the last good archive.
// The response file is transient; the result is suitable for retaining as the
// separately vendable runner build artifact.
func writeObjectArchive(zigPath, targetOS, archivePath string, objects []string) error {
	if len(objects) == 0 {
		return fmt.Errorf("cannot create an empty object archive")
	}
	responsePath := archivePath + ".rsp"
	stagingPath := archivePath + ".tmp"
	response, err := archiveResponseFile(objects)
	if err != nil {
		return err
	}
	if err := os.WriteFile(responsePath, response, 0o600); err != nil {
		return err
	}
	defer os.Remove(responsePath)
	if err := os.Remove(stagingPath); err != nil && !os.IsNotExist(err) {
		return err
	}
	defer os.Remove(stagingPath)

	args := []string{
		"ar",
		"--format=" + objectArchiveFormat(targetOS),
		"--rsp-quoting=posix",
		"rcsD",
		stagingPath,
		"@" + responsePath,
	}
	output, err := exec.Command(zigPath, args...).CombinedOutput()
	if err != nil {
		return fmt.Errorf("archive %d objects: %w\n%s", len(objects), err, strings.TrimSpace(string(output)))
	}
	if err := os.Remove(archivePath); err != nil && !os.IsNotExist(err) {
		return err
	}
	return os.Rename(stagingPath, archivePath)
}

func runtimeArchiveName(targetOS string) string {
	if targetOS == "windows" {
		return "snesrecomp_runtime.lib"
	}
	return "libsnesrecomp_runtime.a"
}

func objectArchiveFormat(targetOS string) string {
	switch targetOS {
	case "darwin":
		return "darwin"
	case "windows":
		return "coff"
	default:
		return "gnu"
	}
}

// forceLoadArchiveArgs makes an archive behave like the former direct object
// list. Without force-loading, a linker extracts only members needed to
// resolve symbols seen so far and can silently omit registration-only or
// otherwise unreferenced translation units.
func forceLoadArchiveArgs(targetOS, archivePath string) []string {
	if targetOS == "darwin" {
		// Zig's Mach-O linker supports -force_load for one named archive. Its
		// broader -all_load spelling is deliberately not used because Zig
		// 0.16 rejects it and it would affect third-party archives as well.
		return []string{"-Wl,-force_load," + archivePath}
	}
	// Zig uses the GNU spelling for ELF and for its MinGW/COFF driver.
	// Close the scope immediately so SDL and compiler runtime archives keep
	// their normal extraction semantics.
	return []string{"-Wl,--whole-archive", archivePath, "-Wl,--no-whole-archive"}
}

func archiveResponseFile(objects []string) ([]byte, error) {
	var response strings.Builder
	for _, object := range objects {
		if strings.ContainsAny(object, "\r\n") {
			return nil, fmt.Errorf("object path contains a newline and cannot be represented in a response file: %q", object)
		}
		response.WriteByte('"')
		for _, character := range object {
			switch character {
			case '\\', '"':
				response.WriteByte('\\')
			}
			response.WriteRune(character)
		}
		response.WriteString("\"\n")
	}
	return []byte(response.String()), nil
}
