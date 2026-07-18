package artifact

import (
	"os"
	"path/filepath"
	"testing"
)

func TestCaptureAndCompare(t *testing.T) {
	source := filepath.Join(t.TempDir(), "gen")
	if err := os.Mkdir(source, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(source, "bank00_v2.c"), []byte("hello\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	archive := filepath.Join(t.TempDir(), "baseline.tar.gz")
	expected, err := Capture(source, archive)
	if err != nil {
		t.Fatal(err)
	}
	archived, err := FromArchive(archive)
	if err != nil {
		t.Fatal(err)
	}
	if differences := Compare(expected, archived); len(differences) != 0 {
		t.Fatalf("archive differs after round trip: %#v", differences)
	}
	if err := os.WriteFile(filepath.Join(source, "bank00_v2.c"), []byte("changed\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	actual, err := FromDir(source)
	if err != nil {
		t.Fatal(err)
	}
	if differences := Compare(archived, actual); len(differences) != 1 {
		t.Fatalf("got %d differences, want 1", len(differences))
	}
}
