package fsutil

import "os"

// DirectoryExists reports whether path names an accessible directory.
func DirectoryExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}
