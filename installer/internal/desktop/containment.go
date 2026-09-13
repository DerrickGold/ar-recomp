package desktop

import (
	"fmt"
	"path/filepath"
	"strings"
)

// pathWithin compares absolute paths after callers resolve real ancestors.
// Windows cannot make a relative path across volumes; that means disjoint,
// not invalid. Other Rel failures must not bypass the containment check.
func pathWithin(parent, child string) (bool, error) {
	if !filepath.IsAbs(parent) || !filepath.IsAbs(child) {
		return false, fmt.Errorf("containment requires absolute paths: %q, %q", parent, child)
	}
	if !strings.EqualFold(filepath.VolumeName(parent), filepath.VolumeName(child)) {
		return false, nil
	}
	relative, err := filepath.Rel(parent, child)
	if err != nil {
		return false, err
	}
	return localPath(relative, true), nil
}
