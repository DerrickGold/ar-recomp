package host

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/buildworkspace"
)

func TestOutputBesideOuterArtifactRegardlessOfWorkspace(t *testing.T) {
	base := t.TempDir()
	base, err := filepath.EvalSymlinks(base)
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{Name + ".app", Name + ".AppImage", Name + ".exe", "Renamed Builder.AppImage"} {
		artifact := filepath.Join(base, name)
		want := filepath.Join(base, "ActRaiserRecomp")
		for _, sidecar := range []string{"", "BuilderData\n"} {
			if sidecar != "" {
				if err := os.WriteFile(artifact+".portable", []byte(sidecar), 0600); err != nil {
					t.Fatal(err)
				}
			}
			got, err := DefaultOutputDirectory(artifact, "")
			if err != nil || got != want {
				t.Fatalf("%s: %s %v", name, got, err)
			}
		}
	}
	app := filepath.Join(base, Name+".app")
	exe := filepath.Join(app, "Contents", "MacOS", Name)
	if err := os.MkdirAll(filepath.Dir(exe), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(exe, nil, 0755); err != nil {
		t.Fatal(err)
	}
	artifact, _, err := Discover(exe, "")
	if err != nil || artifact != app {
		t.Fatalf("discover outer bundle: %s %v", artifact, err)
	}
	got, err := DefaultOutputDirectory(artifact, "")
	if err != nil || got != filepath.Join(base, "ActRaiserRecomp") {
		t.Fatalf("nested executable output: %s %v", got, err)
	}
	if _, err := DefaultOutputDirectory("relative.app", ""); err == nil {
		t.Fatal("accepted relative artifact")
	}
	translocated := filepath.Join(base, "AppTranslocation", "id", "d", Name+".app")
	if _, err := DefaultOutputDirectory(translocated, ""); err == nil {
		t.Fatal("silently selected translocated output")
	}
	explicit := filepath.Join(base, "chosen")
	if got, err := DefaultOutputDirectory(translocated, explicit); err != nil || got != explicit {
		t.Fatalf("explicit output: %s %v", got, err)
	}
}

func TestFreshWorkspaceAndOutputContainmentUsesExistingAncestors(t *testing.T) {
	base := t.TempDir()
	workspace := filepath.Join(base, "new-installer", "workspace")
	output := filepath.Join(base, "new-game")
	for _, pair := range [][2]string{{workspace, output}, {output, workspace}} {
		if err := ValidateWorkspace(pair[0], pair[1]); err != nil {
			t.Fatalf("fresh siblings rejected: %v", err)
		}
	}
	for _, pair := range [][2]string{{workspace, workspace}, {filepath.Join(workspace, "game"), workspace}, {filepath.Join(output, "tools"), output}} {
		if err := ValidateWorkspace(pair[0], pair[1]); err == nil {
			t.Fatal("allowed nested future directories", pair)
		}
	}
	alias := filepath.Join(base, "alias")
	if err := os.Symlink(base, alias); err != nil {
		t.Skip(err)
	}
	if err := ValidateWorkspace(filepath.Join(alias, "new-game", "tools"), output); err == nil {
		t.Fatal("future path escaped containment through an ancestor symlink")
	}
	if _, err := os.Stat(workspace); !os.IsNotExist(err) {
		t.Fatal("validation created workspace")
	}
	if _, err := os.Stat(output); !os.IsNotExist(err) {
		t.Fatal("validation created output")
	}
}

func TestWorkspaceContainmentRemainsOneWay(t *testing.T) {
	parent := t.TempDir()
	child := filepath.Join(parent, "future-input")
	if err := ValidateWorkspace(parent, child); err != nil {
		t.Fatalf("one-way output check rejected an ancestor: %v", err)
	}
	if err := ValidateWorkspace(child, parent); err == nil {
		t.Fatal("one-way output check accepted a nested destination")
	}
	if err := buildworkspace.Separate(parent, child); err == nil {
		t.Fatal("session separation accepted overlapping input/output")
	}
}
