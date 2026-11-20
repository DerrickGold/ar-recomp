//go:build !darwin && !linux && !windows

package builder

import "os"

func modFileIdentity(_ *os.File, _ os.FileInfo) (uint64, [2]uint64, int64) {
	return 0, [2]uint64{}, 0
}
