package builder

import "syscall"

var getLogicalDrives = syscall.NewLazyDLL("kernel32.dll").NewProc("GetLogicalDrives")

func romDriveLocations() []romLocation {
	mask, _, _ := getLogicalDrives.Call()
	var locations []romLocation
	for letter := 'A'; letter <= 'Z'; letter++ {
		if mask&(1<<(letter-'A')) != 0 {
			drive := string(letter) + ":"
			locations = append(locations, romLocation{ID: drive, Label: drive, path: drive + `\`})
		}
	}
	return locations
}
