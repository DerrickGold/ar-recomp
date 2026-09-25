//go:build !windows

package regionalmedia

import "os"

func publishNewDonor(from, to string) error {
	// The caller removes the staging name after publication.
	return os.Link(from, to)
}
