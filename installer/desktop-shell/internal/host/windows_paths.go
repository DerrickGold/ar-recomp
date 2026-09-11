package host

import "strings"

// WindowsPath is checked on the maintainer's OS too. filepath.IsLocal on macOS
// cannot catch Windows device names, alternate data streams or trailing dots.
func WindowsPath(path string) bool {
	if !localPath(path) || strings.ContainsAny(path, `<>:"|?*`) {
		return false
	}
	for _, part := range strings.Split(path, "/") {
		if strings.TrimRight(part, ". ") != part {
			return false
		}
		for _, r := range part {
			if r < 32 {
				return false
			}
		}
		base, _, _ := strings.Cut(strings.ToUpper(part), ".")
		switch base {
		case "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$":
			return false
		}
		if len(base) == 4 && (strings.HasPrefix(base, "COM") || strings.HasPrefix(base, "LPT")) && base[3] >= '0' && base[3] <= '9' {
			return false
		}
		if strings.HasPrefix(base, "COM") || strings.HasPrefix(base, "LPT") {
			if strings.ContainsAny(strings.TrimPrefix(strings.TrimPrefix(base, "COM"), "LPT"), "¹²³") {
				return false
			}
		}
	}
	return true
}
