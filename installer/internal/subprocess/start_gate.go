package subprocess

import (
	"fmt"
	"io"
)

// WaitForDesktopGate prevents a desktop backend from spawning helpers before
// its host has assigned it to the Windows lifetime job. EOF is cancellation.
func WaitForDesktopGate(input io.Reader) error {
	var token [1]byte
	if _, err := io.ReadFull(input, token[:]); err != nil {
		return fmt.Errorf("desktop host closed before process supervision was ready: %w", err)
	}
	if token[0] != 1 {
		return fmt.Errorf("invalid desktop startup gate")
	}
	return nil
}
