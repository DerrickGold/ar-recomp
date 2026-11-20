package builder

import (
	"os"
	"syscall"
)

func modFileIdentity(_ *os.File, info os.FileInfo) (uint64, [2]uint64, int64) {
	stat := info.Sys().(*syscall.Stat_t)
	return uint64(stat.Dev), [2]uint64{stat.Ino}, stat.Ctim.Nano()
}
