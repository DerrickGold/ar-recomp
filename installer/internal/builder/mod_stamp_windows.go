package builder

import (
	"os"
	"runtime"
	"syscall"
	"unsafe"
)

var getModFileInfo = syscall.NewLazyDLL("kernel32.dll").NewProc("GetFileInformationByHandleEx")

func modFileIdentity(file *os.File, _ os.FileInfo) (uint64, [2]uint64, int64) {
	const (
		fileBasicInfo = 0
		fileIDInfo    = 18
	)
	var basic struct {
		created, accessed, modified, changed int64
		attributes, padding                  uint32
	}
	var identity struct {
		volume uint64
		id     [2]uint64
	}
	handle := file.Fd()
	basicOK, _, _ := getModFileInfo.Call(handle, fileBasicInfo, uintptr(unsafe.Pointer(&basic)), unsafe.Sizeof(basic))
	idOK, _, _ := getModFileInfo.Call(handle, fileIDInfo, uintptr(unsafe.Pointer(&identity)), unsafe.Sizeof(identity))
	runtime.KeepAlive(file)
	if basicOK == 0 || idOK == 0 {
		return 0, [2]uint64{}, 0
	}
	return identity.volume, identity.id, basic.changed
}
