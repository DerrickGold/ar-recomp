package regionalmedia

import (
	"os"
	"path/filepath"
	"strings"
	"syscall"
)

func publishNewDonor(from, to string) error {
	source, err := donorWindowsPath(from)
	if err != nil {
		return &os.LinkError{Op: "rename", Old: from, New: to, Err: err}
	}
	destination, err := donorWindowsPath(to)
	if err == nil {
		// MoveFile refuses an existing destination. Unlike a hard link, this
		// same-directory rename also works on exFAT and FAT32 drives.
		err = syscall.MoveFile(&source[0], &destination[0])
	}
	if err != nil {
		return &os.LinkError{Op: "rename", Old: from, New: to, Err: err}
	}
	return nil
}

// Match os file operations: use UTF-16 and support paths beyond MAX_PATH.
// Normalize before adding the prefix, which disables Win32 path normalization.
func donorWindowsPath(path string) ([]uint16, error) {
	path, err := filepath.Abs(path)
	if err != nil {
		return nil, err
	}
	switch {
	case strings.HasPrefix(path, `\\?\`), strings.HasPrefix(path, `\\.\`):
	case strings.HasPrefix(path, `\\`):
		path = `\\?\UNC\` + path[2:]
	default:
		path = `\\?\` + path
	}
	return syscall.UTF16FromString(path)
}
