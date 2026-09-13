package host

import (
	"fmt"
	"os"
	"os/exec"
	"sync"
	"unsafe"

	"golang.org/x/sys/windows"
)

// The host owns the job, so host crashes also reap the backend and compilers.
// Only deliberately detached games request BREAKAWAY; ordinary helpers inherit
// membership. A stdin gate closes the Start/Assign race before any helper runs.
func startBackendCommand(command *exec.Cmd) (func(), error) {
	job, err := windows.CreateJobObject(nil, nil)
	if err != nil {
		return nil, fmt.Errorf("create Builder process job: %w", err)
	}
	var once sync.Once
	closeJob := func() { once.Do(func() { _ = windows.CloseHandle(job) }) }
	limits := windows.JOBOBJECT_EXTENDED_LIMIT_INFORMATION{}
	limits.BasicLimitInformation.LimitFlags = windows.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | windows.JOB_OBJECT_LIMIT_BREAKAWAY_OK
	if _, err = windows.SetInformationJobObject(job, windows.JobObjectExtendedLimitInformation, uintptr(unsafe.Pointer(&limits)), uint32(unsafe.Sizeof(limits))); err != nil {
		closeJob()
		return nil, fmt.Errorf("configure Builder process job: %w", err)
	}
	reader, writer, err := os.Pipe()
	if err != nil {
		closeJob()
		return nil, err
	}
	defer reader.Close()
	defer writer.Close()
	command.Stdin = reader
	command.Args = append(command.Args, "--desktop-start-gate")
	if err = command.Start(); err != nil {
		closeJob()
		return nil, err
	}
	process, err := windows.OpenProcess(windows.PROCESS_SET_QUOTA|windows.PROCESS_TERMINATE, false, uint32(command.Process.Pid))
	if err == nil {
		err = windows.AssignProcessToJobObject(job, process)
		_ = windows.CloseHandle(process)
	}
	if err == nil {
		_, err = writer.Write([]byte{1})
	}
	if err != nil {
		_ = command.Process.Kill()
		_ = command.Wait()
		closeJob()
		return nil, fmt.Errorf("supervise Builder process tree: %w", err)
	}
	return closeJob, nil
}
