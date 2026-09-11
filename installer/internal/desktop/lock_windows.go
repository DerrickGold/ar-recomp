package desktop

import (
	"fmt"
	"path/filepath"
	"syscall"
)

// An exclusive, non-inherited Windows file handle serializes initialization.
// Keep the file after closing; no delete/recreate race and no stale PID lock.
func lockInitialization(root string) (func(), error) {
	path, err := syscall.UTF16PtrFromString(filepath.Join(root, ".actraiser-initialize.lock"))
	if err != nil {
		return nil, err
	}
	h, err := syscall.CreateFile(path, syscall.GENERIC_READ|syscall.GENERIC_WRITE,
		0, nil, syscall.OPEN_ALWAYS, syscall.FILE_ATTRIBUTE_NORMAL|syscall.FILE_FLAG_OPEN_REPARSE_POINT, 0)
	if err != nil {
		return nil, fmt.Errorf("open game-data initialization lock (another process may be initializing): %w", err)
	}
	var info syscall.ByHandleFileInformation
	if err = syscall.GetFileInformationByHandle(h, &info); err != nil {
		syscall.CloseHandle(h)
		return nil, err
	}
	if info.FileAttributes&(syscall.FILE_ATTRIBUTE_REPARSE_POINT|syscall.FILE_ATTRIBUTE_DIRECTORY) != 0 {
		syscall.CloseHandle(h)
		return nil, fmt.Errorf("initialization lock must be a regular file")
	}
	return func() { syscall.CloseHandle(h) }, nil
}
