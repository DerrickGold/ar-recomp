package toolchain

import (
	"encoding/hex"
	"strings"
	"testing"
)

func TestSDL3TtfPins(t *testing.T) {
	for key := range pinnedSDL3 {
		platform := strings.Split(key, "/")
		url, sha, archive, kind, err := SDL3TtfPin(platform[0], platform[1])
		if err != nil {
			t.Fatal(err)
		}
		if raw, err := hex.DecodeString(sha); err != nil || len(raw) != 32 {
			t.Fatal("invalid hash", sha)
		}
		if !strings.Contains(url, "/SDL_ttf/releases/download/release-"+PinnedSDL3TtfVersion+"/") || !strings.HasSuffix(url, archive) || kind != pinnedSDL3[key].Kind {
			t.Fatal(url, archive, kind)
		}
	}
	if _, _, _, _, err := SDL3TtfPin("linux", "amd64"); err == nil {
		t.Fatal("invented official Linux binary")
	}
	hURL, hSHA, hArchive, rURL, rSHA, rArchive := SteamDeckSDL3TtfPins()
	for _, sha := range []string{hSHA, rSHA} {
		if raw, err := hex.DecodeString(sha); err != nil || len(raw) != 32 {
			t.Fatal("invalid Deck hash")
		}
	}
	if !strings.HasSuffix(hURL, hArchive) || !strings.HasSuffix(rURL, rArchive) || !strings.HasSuffix(rArchive, "_amd64.deb") {
		t.Fatal("invalid Deck identity")
	}
}
