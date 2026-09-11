package host

import (
	"fmt"
	"os/exec"
	"path/filepath"
	"syscall"

	"golang.org/x/sys/windows"
)

func configureCommand(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: windows.CREATE_NO_WINDOW}
}

func Lock(workspace string) (func(), error) {
	path, err := windows.UTF16PtrFromString(filepath.Join(workspace, ".builder-session.lock"))
	if err != nil {
		return nil, err
	}
	h, err := windows.CreateFile(path, windows.GENERIC_READ|windows.GENERIC_WRITE,
		windows.FILE_SHARE_READ|windows.FILE_SHARE_WRITE, nil, windows.OPEN_ALWAYS,
		windows.FILE_ATTRIBUTE_NORMAL|windows.FILE_FLAG_OPEN_REPARSE_POINT, 0)
	if err != nil {
		return nil, err
	}
	var info windows.ByHandleFileInformation
	if err = windows.GetFileInformationByHandle(h, &info); err != nil || info.FileAttributes&windows.FILE_ATTRIBUTE_REPARSE_POINT != 0 {
		windows.CloseHandle(h)
		return nil, fmt.Errorf("workspace lock must be a regular file: %v", err)
	}
	overlap := &windows.Overlapped{}
	if err = windows.LockFileEx(h, windows.LOCKFILE_EXCLUSIVE_LOCK|windows.LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, overlap); err != nil {
		windows.CloseHandle(h)
		return nil, fmt.Errorf("another Builder is using this workspace: %w", err)
	}
	return func() {
		_ = windows.UnlockFileEx(h, 0, 1, 0, overlap)
		_ = windows.CloseHandle(h)
	}, nil
}
