package tooling

import (
	"io"
	"os"
	"path/filepath"

	"github.com/DerrickGold/ar-recomp/installer/internal/gamerom"
)

func resolveToolPath(cwd, path string) string {
	if filepath.IsAbs(path) {
		return path
	}
	return filepath.Join(cwd, path)
}

func readGameROMFile(path string) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	data, err := io.ReadAll(io.LimitReader(file, gamerom.Size+1))
	closeErr := file.Close()
	if err != nil {
		return nil, err
	}
	if closeErr != nil {
		return nil, closeErr
	}
	return data, nil
}

// An output is never opened until extraction/validation succeeds. O_EXCL also
// rejects symlinks/existing files; a failed writer removes only its new file.
func writeNewToolFile(path string, write func(io.Writer) error) error {
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	complete := false
	defer func() {
		_ = file.Close()
		if !complete {
			_ = os.Remove(path)
		}
	}()
	if err := write(file); err != nil {
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	complete = true
	return nil
}
