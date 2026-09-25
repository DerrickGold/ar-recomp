package regionalmedia

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestDonorPublicationPreservesExisting(t *testing.T) {
	testDonorPublication(t, filepath.Join(t.TempDir(), "Regional media 日本 🎮"))
}

func testDonorPublication(t *testing.T, root string) {
	t.Helper()
	if err := os.MkdirAll(root, 0700); err != nil {
		t.Fatal(err)
	}
	dir, err := openInstallRoot(root, true)
	if err != nil {
		t.Fatal(err)
	}
	defer dir.Close()
	for _, step := range []struct {
		content, want string
		replace       bool
		err           error
	}{
		{"original", "original", false, nil},
		{"unconfirmed", "original", false, ErrReplaceRequired},
		{"replacement", "replacement", true, nil},
	} {
		err := publishDonor(dir, "jp.armedia", []byte(step.content), step.replace)
		if !errors.Is(err, step.err) {
			t.Fatalf("publish %s: %v, want %v", step.content, err, step.err)
		}
		data, err := readInstalled(dir, "jp.armedia")
		if err != nil || string(data) != step.want {
			t.Fatalf("published data = %q, %v; want %q", data, err, step.want)
		}
		entries, err := os.ReadDir(dir.Name())
		if err != nil || len(entries) != 1 {
			t.Fatalf("staging files remain: %v, %v", entries, err)
		}
	}
}

func TestDonorPublicationConcurrent(t *testing.T) {
	dir, err := openInstallRoot(t.TempDir(), true)
	if err != nil {
		t.Fatal(err)
	}
	defer dir.Close()
	type result struct {
		content byte
		err     error
	}
	start := make(chan struct{})
	results := make(chan result, 8)
	for i := byte(0); i < 8; i++ {
		go func() {
			<-start
			results <- result{i, publishDonor(dir, "jp.armedia", []byte{i}, false)}
		}()
	}
	close(start)
	wins, winner := 0, byte(0)
	for i := 0; i < 8; i++ {
		r := <-results
		if r.err == nil {
			wins++
			winner = r.content
		} else if !errors.Is(r.err, ErrReplaceRequired) {
			t.Errorf("unexpected publication failure: %v", r.err)
		}
	}
	data, err := readInstalled(dir, "jp.armedia")
	if wins != 1 || err != nil || len(data) != 1 || data[0] != winner {
		t.Fatalf("concurrent publication: wins=%d, winner=%d, data=%v, error=%v", wins, winner, data, err)
	}
}
