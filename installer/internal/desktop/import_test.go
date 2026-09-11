package desktop

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func importFixture(t *testing.T) (string, string) {
	t.Helper()
	source, output := t.TempDir(), t.TempDir()
	put(t, filepath.Join(source, "game-assets/manifest.ini"), "old manifest")
	put(t, filepath.Join(source, "settings.ini"), "old preferences")
	put(t, filepath.Join(source, "saves/save.srm"), "old save")
	return source, output
}

func TestInstallImportRestoresSeededOutputAndPreservesUserData(t *testing.T) {
	source, output := importFixture(t)
	put(t, filepath.Join(source, "config.ini"), "old config")
	put(t, filepath.Join(source, "game-assets/workshop-settings.json"), `{"language":"ja"}`)
	put(t, filepath.Join(source, "game-assets/hd/deleted.png"), "old image")
	put(t, filepath.Join(source, "game-assets/languages/projects/mine/project.json"), "author work")
	for _, leaf := range []string{"game.sfc", "tools/old-builder", "defaults/config.ini", "build/compiler-cache"} {
		put(t, filepath.Join(source, leaf), "do not copy")
	}
	put(t, filepath.Join(output, "game-assets/manifest.ini"), "fresh shipped manifest")
	hash, _ := fileHash(filepath.Join(output, "game-assets/manifest.ini"))
	seed, _ := json.Marshal(map[string]string{"game-assets/manifest.ini": hash, "game-assets/hd/deleted.png": "previously seeded"})
	put(t, filepath.Join(output, seedStateName), string(seed))
	put(t, filepath.Join(output, "settings.ini"), "new preferences")
	put(t, filepath.Join(output, "saves/save.srm"), "new save")
	p, err := PreviewInstallImport(context.Background(), source, output)
	if err != nil {
		t.Fatal(err)
	}
	if p.Copy != 4 || p.Conflicts != 3 {
		t.Fatalf("unexpected preview %+v", p)
	}
	if _, err = ApplyInstallImport(context.Background(), p); err != nil {
		t.Fatal(err)
	}
	for leaf, want := range map[string]string{"config.ini": "old config", "settings.ini": "new preferences", "saves/save.srm": "new save", "game-assets/manifest.ini": "old manifest", "game-assets/workshop-settings.json": `{"language":"ja"}`} {
		if got := read(t, filepath.Join(output, leaf)); got != want {
			t.Fatalf("%s: %s", leaf, got)
		}
	}
	for _, leaf := range []string{"game.sfc", "tools/old-builder", "defaults/config.ini", "build/compiler-cache", "game-assets/hd/deleted.png"} {
		if _, err := os.Stat(filepath.Join(output, leaf)); !os.IsNotExist(err) {
			t.Fatalf("copied excluded/deleted %s", leaf)
		}
	}
	if read(t, filepath.Join(source, "saves/save.srm")) != "old save" {
		t.Fatal("source mutated")
	}
	if err := os.Remove(filepath.Join(output, "config.ini")); err != nil {
		t.Fatal(err)
	}
	p, err = PreviewInstallImport(context.Background(), source, output)
	if err != nil || !p.AlreadyImported {
		t.Fatal(p, err)
	}
	if _, err = ApplyInstallImport(context.Background(), p); err == nil {
		t.Fatal("reimport resurrected deleted data")
	}
}

func TestInstallImportDecisionIndependentOfSeedAndSkip(t *testing.T) {
	source, output := importFixture(t)
	put(t, filepath.Join(output, seedStateName), "{}")
	state, err := ReadImportDecision(output)
	if err != nil || state.Decided {
		t.Fatal(state, err)
	}
	if err = SkipInstallImport(output); err != nil {
		t.Fatal(err)
	}
	state, err = ReadImportDecision(output)
	if err != nil || !state.Decided {
		t.Fatal(state, err)
	}
	p, err := PreviewInstallImport(context.Background(), source, output)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = ApplyInstallImport(context.Background(), p); err != nil {
		t.Fatal("skip prevented manual import", err)
	}
}

func TestInstallImportPreviewRejectsChangedInputs(t *testing.T) {
	for _, side := range []string{"source", "destination"} {
		t.Run(side, func(t *testing.T) {
			source, output := importFixture(t)
			p, err := PreviewInstallImport(context.Background(), source, output)
			if err != nil {
				t.Fatal(err)
			}
			root := source
			if side == "destination" {
				root = output
			}
			put(t, filepath.Join(root, "saves/save.srm"), "changed after preview")
			if _, err = ApplyInstallImport(context.Background(), p); err == nil {
				t.Fatal("accepted stale preview")
			}
			state, err := ReadImportDecision(output)
			if err != nil || state.Decided {
				t.Fatal(state, err)
			}
		})
	}
}

func TestInstallImportCancellationAndVerifiedCopy(t *testing.T) {
	source, output := importFixture(t)
	p, err := PreviewInstallImport(context.Background(), source, output)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err = ApplyInstallImport(ctx, p); err == nil {
		t.Fatal("ignored cancellation")
	}
	path := filepath.Join(output, "settings.ini")
	put(t, path, "keep")
	if err = writeVerifiedImport(path, strings.NewReader("changed source"), "wrong hash"); err == nil {
		t.Fatal("unverified copy")
	}
	if read(t, path) != "keep" {
		t.Fatal("partial copy replaced destination")
	}
	if state, _ := ReadImportDecision(output); state.Decided {
		t.Fatal("recorded failed import")
	}
}

func TestInstallImportRefusesLinksAndOverlaps(t *testing.T) {
	for _, side := range []string{"source", "destination"} {
		t.Run(side, func(t *testing.T) {
			source, output := importFixture(t)
			root := source
			if side == "destination" {
				root = output
			}
			if err := os.Symlink(t.TempDir(), filepath.Join(root, "game-assets/linked")); err != nil {
				if side == "destination" {
					if err = os.MkdirAll(filepath.Join(root, "game-assets"), 0755); err != nil {
						t.Fatal(err)
					}
					if err = os.Symlink(t.TempDir(), filepath.Join(root, "game-assets/linked")); err != nil {
						t.Skip(err)
					}
				} else {
					t.Skip(err)
				}
			}
			if side == "destination" {
				put(t, filepath.Join(source, "game-assets/linked/asset"), "asset")
			}
			if _, err := PreviewInstallImport(context.Background(), source, output); err == nil {
				t.Fatal("accepted symlink")
			}
		})
	}
	source, _ := importFixture(t)
	if _, err := PreviewInstallImport(context.Background(), source, source); err == nil {
		t.Fatal("self import")
	}
	if _, err := PreviewInstallImport(context.Background(), source, filepath.Join(source, "game-assets")); err == nil {
		t.Fatal("overlapping import")
	}
}

func TestDiscoverLegacyInstallAndSidecarWithoutScanning(t *testing.T) {
	base := t.TempDir()
	output := filepath.Join(base, "ActRaiserRecomp")
	put(t, filepath.Join(base, "utils/game-assets/manifest.ini"), "legacy")
	put(t, filepath.Join(output, "game-assets/manifest.ini"), "new output")
	put(t, filepath.Join(base, "unrelated/deeper/game-assets/manifest.ini"), "not searched")
	put(t, filepath.Join(base, "ActRaiserRecomp.app.portable"), "utils\n")
	put(t, filepath.Join(base, "ActRaiserRecomp.AppImage.portable"), "../escape\n")
	found, err := DiscoverInstallData(base, output)
	want, _ := filepath.EvalSymlinks(filepath.Join(base, "utils"))
	if err != nil || len(found) != 1 || found[0] != want {
		t.Fatal(found, err)
	}
	state, _ := ReadImportDecision(output)
	if state.Decided {
		t.Fatal("discovery wrote state")
	}
}
