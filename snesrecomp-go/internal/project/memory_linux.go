//go:build linux

package project

import (
	"os"
	"strconv"
	"strings"
	"syscall"
)

func physicalMemoryBytes() (int64, bool) {
	var info syscall.Sysinfo_t
	if err := syscall.Sysinfo(&info); err != nil {
		return 0, false
	}
	total := int64(info.Totalram) * int64(info.Unit)
	// Inside a container the cgroup limit, not the host's memory, is what
	// the compiles can actually use.
	if limit, ok := cgroupMemoryLimit(); ok && limit < total {
		total = limit
	}
	return total, total > 0
}

func cgroupMemoryLimit() (int64, bool) {
	for _, path := range []string{
		"/sys/fs/cgroup/memory.max",                   // cgroup v2
		"/sys/fs/cgroup/memory/memory.limit_in_bytes", // cgroup v1
	} {
		data, err := os.ReadFile(path)
		if err != nil {
			continue
		}
		value, err := strconv.ParseInt(strings.TrimSpace(string(data)), 10, 64)
		if err != nil || value <= 0 {
			// "max" (v2) means no limit.
			return 0, false
		}
		return value, true
	}
	return 0, false
}
