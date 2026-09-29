//go:build windows

package project

import (
	"math"
	"syscall"
	"unsafe"
)

var globalMemoryStatusEx = syscall.NewLazyDLL("kernel32.dll").NewProc("GlobalMemoryStatusEx")

// memoryStatusEx mirrors the Win32 MEMORYSTATUSEX structure.
type memoryStatusEx struct {
	length               uint32
	memoryLoad           uint32
	totalPhys            uint64
	availPhys            uint64
	totalPageFile        uint64
	availPageFile        uint64
	totalVirtual         uint64
	availVirtual         uint64
	availExtendedVirtual uint64
}

func physicalMemoryBytes() (int64, bool) {
	status := memoryStatusEx{length: uint32(unsafe.Sizeof(memoryStatusEx{}))}
	result, _, _ := globalMemoryStatusEx.Call(uintptr(unsafe.Pointer(&status)))
	if result == 0 || status.totalPhys == 0 || status.totalPhys > math.MaxInt64 {
		return 0, false
	}
	return int64(status.totalPhys), true
}
