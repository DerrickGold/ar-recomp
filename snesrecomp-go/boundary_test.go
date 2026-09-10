package snesrecomp_test

import (
	"go/parser"
	"go/token"
	"io/fs"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// TestGameAgnosticBoundary makes the module's ownership rule executable. The
// documentation directory may retain clearly historical multi-game validation
// evidence, but production code, data, assets, and imports must stay generic.
func TestGameAgnosticBoundary(t *testing.T) {
	for _, relative := range []string{
		"packaging",
		"internal/buildgui",
		"internal/localizationkit",
		"internal/languagecli",
		"internal/fontprobe",
		"internal/uicatalog",
		"internal/workshopart",
		"internal/spcaudio",
		"internal/quintet",
	} {
		if _, err := os.Stat(relative); !os.IsNotExist(err) {
			t.Errorf("game-owned path remains in generic module: %s", relative)
		}
	}

	forbiddenText := []string{
		"Act" + "Raiser",
		".ar" + "lang",
		".ar" + "project",
		"builtin:" + "actraiser-sans",
		"localization" + "kit",
		"build" + "gui",
	}
	productionExtensions := map[string]bool{
		".go": true, ".c": true, ".h": true, ".json": true, ".ini": true,
		".js": true, ".mjs": true, ".html": true, ".css": true,
	}
	err := filepath.WalkDir(".", func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			if path == "docs" || path == ".git" || strings.HasPrefix(path, "build") {
				return filepath.SkipDir
			}
			return nil
		}
		if filepath.Ext(path) == ".go" {
			parsed, err := parser.ParseFile(token.NewFileSet(), path, nil, parser.ImportsOnly)
			if err != nil {
				return err
			}
			for _, spec := range parsed.Imports {
				importPath, err := strconv.Unquote(spec.Path.Value)
				if err != nil {
					return err
				}
				if strings.Contains(importPath, "/ar-recomp/") || strings.Contains(importPath, "/installer") {
					t.Errorf("%s imports game-owned package %s", path, importPath)
				}
			}
		}
		if strings.HasSuffix(path, "_test.go") || !productionExtensions[filepath.Ext(path)] {
			return nil
		}
		content, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		for _, forbidden := range forbiddenText {
			if strings.Contains(string(content), forbidden) {
				t.Errorf("%s contains game-owned production token %q", path, forbidden)
			}
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
}
