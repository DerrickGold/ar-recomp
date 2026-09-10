//go:build darwin || linux

package desktop

import (
	"fmt"
	"path/filepath"
	"syscall"
)

func lockInitialization(root string) (func(), error) {
	path := filepath.Join(root, ".actraiser-initialize.lock")
	fd, err := syscall.Open(path, syscall.O_RDWR|syscall.O_CREAT|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0600)
	if err != nil {
		return nil, fmt.Errorf("open initialization lock: %w", err)
	}
	if err := syscall.Flock(fd, syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		syscall.Close(fd)
		return nil, fmt.Errorf("another launcher may be initializing %s: %w", root, err)
	}
	// Keep the inode: unlinking it lets another process lock a different inode.
	// The kernel releases the advisory lock even if the launcher crashes.
	return func() { syscall.Close(fd) }, nil
}
