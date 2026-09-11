//go:build !darwin && !linux && !windows

package desktop

import "errors"

func lockInitialization(root string) (func(), error) {
	return nil, errors.New("desktop application launch requires macOS or Linux")
}
