package desktop

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
)

// Known application-built modules only. Imported data never selects code paths
// or libraries; absence is valid for a game built without optional content.
func nativeModules(binary string) ([]string, error) {
	extension := map[string]string{"darwin": ".dylib", "linux": ".so", "windows": ".dll"}[runtime.GOOS]
	path := filepath.Join(filepath.Dir(binary), "actraiser2-native"+extension)
	info, err := os.Lstat(path)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("native module must be a regular file: %s", path)
	}
	return []string{path}, nil
}
