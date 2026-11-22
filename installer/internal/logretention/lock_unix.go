//go:build !windows

package logretention

import (
	"errors"
	"os"
	"syscall"
)

func holdOpen(file *os.File) error {
	return syscall.Flock(int(file.Fd()), syscall.LOCK_EX|syscall.LOCK_NB)
}

func lockRotation(path string) (*os.File, error) {
	const flags = syscall.O_CREAT | syscall.O_RDWR | syscall.O_NOFOLLOW | syscall.O_CLOEXEC
	fd, err := syscall.Open(path, flags, 0600)
	if err != nil {
		return nil, err
	}
	file := os.NewFile(uintptr(fd), path)
	if err := holdOpen(file); err != nil {
		file.Close()
		if errors.Is(err, syscall.EWOULDBLOCK) {
			return nil, errBusy
		}
		return nil, err
	}
	return file, nil
}

func removeInactive(path string) bool {
	fd, err := syscall.Open(path, syscall.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_CLOEXEC|syscall.O_NONBLOCK, 0)
	if err != nil {
		return false
	}
	file := os.NewFile(uintptr(fd), path)
	defer file.Close()
	return holdOpen(file) == nil && os.Remove(path) == nil
}
