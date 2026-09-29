//go:build !darwin && !linux && !windows

package project

// Physical memory is not detected on this platform, so the hermetic build
// runs without a memory budget unless one is given explicitly.
func physicalMemoryBytes() (int64, bool) {
	return 0, false
}
