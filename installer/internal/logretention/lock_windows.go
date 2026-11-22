package logretention

import (
	"errors"
	"os"
	"syscall"
)

func holdOpen(_ *os.File) error {
	// Go opens logs without FILE_SHARE_DELETE. An inherited handle
	// continues to prevent deletion after the parent launcher closes.
	return nil
}

func lockRotation(path string) (*os.File, error) {
	name, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return nil, err
	}
	handle, err := syscall.CreateFile(name, syscall.GENERIC_READ|syscall.GENERIC_WRITE, 0,
		nil, syscall.OPEN_ALWAYS, syscall.FILE_FLAG_OPEN_REPARSE_POINT, 0)
	if errors.Is(err, syscall.Errno(32)) { // ERROR_SHARING_VIOLATION
		return nil, errBusy
	}
	if err != nil {
		return nil, err
	}
	return os.NewFile(uintptr(handle), path), nil
}

func removeInactive(path string) bool { return os.Remove(path) == nil }
