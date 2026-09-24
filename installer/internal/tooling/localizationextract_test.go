package tooling

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

func TestLocalizationExtractCommandRejects(t *testing.T) {
	for _, args := range [][]string{nil, {"--rom", "missing"}, {"--out", "x.zip"}, {"--rom", "missing", "--out", "x.zip"}, {"--rom", "missing", "--out", "x.zip", "--format", "unknown"}} {
		dir := t.TempDir()
		if err := RunLocalizationExtractCommand(args, dir, &bytes.Buffer{}); err == nil {
			t.Fatal("invalid request accepted")
		}
		entries, err := os.ReadDir(dir)
		if err != nil || len(entries) != 0 {
			t.Fatal("failed extraction left output", err)
		}
	}
}

func TestNewLocalizationFileCleanup(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "source.zip")
	errWrite := errors.New("injected writer failure")
	if err := writeNewToolFile(path, func(w io.Writer) error { _, _ = w.Write([]byte("partial")); return errWrite }); !errors.Is(err, errWrite) {
		t.Fatal(err)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatal("partial output left behind")
	}
	if err := os.WriteFile(path, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	for _, target := range []string{path, filepath.Join(dir, "symlink")} {
		if target != path {
			if err := os.Symlink(path, target); err != nil {
				t.Skip("symlink not available", err)
			}
		}
		if err := writeNewToolFile(target, func(io.Writer) error { t.Fatal("opened existing destination"); return nil }); err == nil {
			t.Fatal("existing output accepted")
		}
		data, err := os.ReadFile(path)
		if err != nil || string(data) != "keep" {
			t.Fatal("existing output modified")
		}
	}
}

func TestLocalizationExtractCommandROM(t *testing.T) {
	root := os.Getenv("AR_LOCALIZATION_GUI_ROM_ROOT")
	if root == "" {
		t.Skip("regional ROM fixtures not configured")
	}
	for _, name := range []string{"ar.sfc", "ar-eu.sfc", "ar-fra.sfc", "ar-ger.sfc", "ar-jp.sfc"} {
		t.Run(name, func(t *testing.T) {
			d, err := readLocalizationROM(filepath.Join(root, name))
			if err != nil {
				t.Fatal(err)
			}
			pack, err := d.BuildNativeAuthorPack(d.NativeSourceMetadata())
			if err != nil {
				t.Fatal(err)
			}
			for _, format := range []string{"pack", "catalog", "runtime-routes", "compose-routes"} {
				dir := t.TempDir()
				out := filepath.Join(dir, "output")
				args := []string{"--rom", filepath.Join(root, name), "--out", "output", "--format", format}
				err := RunLocalizationExtractCommand(args, dir, &bytes.Buffer{})
				if (format == "runtime-routes" || format == "compose-routes") && d.ReleaseID() != "us" {
					if err == nil {
						t.Fatal("non-US runtime addresses accepted")
					}
					if _, err := os.Stat(out); !os.IsNotExist(err) {
						t.Fatal("rejected runtime extraction left output")
					}
					continue
				}
				if err != nil {
					t.Fatal(err)
				}
				data, err := os.ReadFile(out)
				if err != nil {
					t.Fatal(err)
				}
				if format == "pack" {
					project, err := localization.ReadAuthorArchive(bytes.NewReader(data), int64(len(data)))
					if err != nil {
						t.Fatal("builder archive importer rejected source", err)
					}
					if project.Origin() != "native-source" || !reflect.DeepEqual(project.Pack().Files(), pack.Files()) {
						t.Fatal("CLI/GUI source pack drift")
					}
				} else {
					var actual map[string]any
					if err := json.Unmarshal(data, &actual); err != nil {
						t.Fatal(err)
					}
					if format == "catalog" {
						source, ok := actual["source"].(map[string]any)
						if !ok || source["release_id"] != d.ReleaseID() || actual["format"] != "actraiser-native-language-evidence" || actual["consumer_census"] == nil || actual["coverage"] == nil {
							t.Fatal("unidentified or incomplete catalogue evidence")
						}
					}
				}
				if err := RunLocalizationExtractCommand(args, dir, &bytes.Buffer{}); err == nil {
					t.Fatal("existing output overwritten")
				}
				after, err := os.ReadFile(out)
				if err != nil || !bytes.Equal(data, after) {
					t.Fatal("output changed after rejected overwrite")
				}
			}
		})
	}
}
