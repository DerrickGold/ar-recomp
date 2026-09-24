// Package gamerom owns the exact retail identities accepted by ActRaiser's
// content tools. This is game-specific; it does not belong in snesbuild.
package gamerom

import (
	"crypto/sha256"
	"fmt"
)

const Size = 1 << 20
const USSHA256 = "b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0"

type Release struct {
	ID, SHA256 string
}

var releases = [...]Release{
	{"us", USSHA256},
	{"jp", "3655833fd0fb4985c3dbbf28141f65564f7a228ee92d81a12982ef7cb952a51b"},
	{"eu-en", "146a68436fa9dbe728ddc7355821384765325e356cb8b9b193a4f22333ed52a0"},
	{"de", "01923db83e0e8b19d476483649d956e3e24cfc918ca04a6aa04faa29ba8e4c41"},
	{"fr", "6cc2cadfcb4fba4c1abb2a1d06b49b840bec65d75acaa0ac8831576442e7e96a"},
}

// Releases returns a copy, never a mutable alias of the trusted identities.
func Releases() []Release { return append([]Release(nil), releases[:]...) }

func Identify(rom []byte) (Release, error) {
	if len(rom) != Size {
		return Release{}, fmt.Errorf("expected 1 MiB headerless ROM, got %d bytes", len(rom))
	}
	digest := fmt.Sprintf("%x", sha256.Sum256(rom))
	for _, release := range releases {
		if release.SHA256 == digest {
			return release, nil
		}
	}
	return Release{}, fmt.Errorf("unsupported ROM SHA-256 %s; an exact supported clean ROM is required", digest)
}
