package gamerom

import (
	"encoding/hex"
	"slices"
	"testing"
)

func TestIdentities(t *testing.T) {
	var order []string
	ids, hashes := map[string]bool{}, map[string]bool{}
	for _, release := range Releases() {
		order = append(order, release.ID)
		hash, err := hex.DecodeString(release.SHA256)
		if err != nil || len(hash) != 32 || ids[release.ID] || hashes[release.SHA256] {
			t.Fatal(release)
		}
		ids[release.ID], hashes[release.SHA256] = true, true
	}
	// Persisted .armedia release numbers are one-based indices in this list.
	if !slices.Equal(order, []string{"us", "jp", "eu-en", "de", "fr"}) {
		t.Fatal("persisted release numbers changed", order)
	}
	copy := Releases()
	copy[0].ID = "changed"
	if len(ids) != 5 || Releases()[0].ID != "us" {
		t.Fatal("retail identity ownership")
	}
	for _, size := range []int{0, Size - 1, Size, Size + 512} {
		if _, err := Identify(make([]byte, size)); err == nil {
			t.Fatal("unrecognized ROM accepted", size)
		}
	}
}
