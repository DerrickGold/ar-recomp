package localization

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestUpdateInstalledProjectRejectsStaleOrRemovedCopy(t *testing.T) {
	for _, archive := range []bool{false, true} {
		name := "directory"
		if archive {
			name = "archive"
		}
		t.Run(name, func(t *testing.T) {
			p := authorAdventureProject(t)
			prepared, _, err := p.Installation()
			if err != nil {
				t.Fatal(err)
			}
			root := t.TempDir()
			id := p.Pack().Manifest().Metadata().ID
			if archive {
				var data bytes.Buffer
				if err := prepared.writeArchive(&data, "publication"); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(filepath.Join(root, "Installed.arlang"), data.Bytes(), 0644); err != nil {
					t.Fatal(err)
				}
			} else if _, err := InstallProjectInLibrary(root, prepared, false); err != nil {
				t.Fatal(err)
			}
			row, err := FindInstalledPack(root, id)
			if err != nil || row == nil {
				t.Fatal(row, err)
			}
			if _, err := UpdateInstalledProject(root, p, *row); err == nil {
				t.Fatal("unvalidated project was installed")
			}
			if err := SetLanguagePackEnabled(root, row.Key, id, row.Revision, false); err != nil {
				t.Fatal(err)
			}
			if _, err := UpdateInstalledProject(root, prepared, *row); !errors.Is(err, ErrProjectConflict) {
				t.Fatal("stale update accepted", err)
			}
			row, err = FindInstalledPack(root, id)
			if err != nil || row == nil || row.Enabled {
				t.Fatal(row, err)
			}
			if _, err := UninstallLanguagePack(root, row.Key, id, row.Revision); err != nil {
				t.Fatal(err)
			}
			if _, err := UpdateInstalledProject(root, prepared, *row); !errors.Is(err, ErrProjectConflict) {
				t.Fatal("save reinstalled a removed pack", err)
			}
			if row, err = FindInstalledPack(root, id); err != nil || row != nil {
				t.Fatal("removed pack became discoverable", row, err)
			}
		})
	}
}
