//go:build darwin

package project

import (
	"encoding/binary"
	"math"
	"syscall"
)

func physicalMemoryBytes() (int64, bool) {
	value, err := syscall.Sysctl("hw.memsize")
	if err != nil || len(value) > 8 {
		return 0, false
	}
	// Sysctl returns the raw little-endian uint64 as a string with one
	// trailing NUL byte removed; restore the full width before decoding.
	raw := make([]byte, 8)
	copy(raw, value)
	total := binary.LittleEndian.Uint64(raw)
	if total == 0 || total > math.MaxInt64 {
		return 0, false
	}
	return int64(total), true
}
