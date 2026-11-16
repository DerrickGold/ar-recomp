package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestLinuxAppImageProfileUsesSharedLauncherOptions(t *testing.T) {
	app := t.TempDir()
	source := filepath.Join("..", "..", "recomplauncher.json")
	if err := stageLinuxProfile(app, source); err != nil {
		t.Fatal(err)
	}
	readProfile := func(path string) map[string]any {
		t.Helper()
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		var profile map[string]any
		if err := json.Unmarshal(data, &profile); err != nil {
			t.Fatal(err)
		}
		return profile
	}
	want := readProfile(source)
	want["start"].(map[string]any)["program"] = []any{"ActRaiserRecompBuilder.AppImage"}
	if got := readProfile(filepath.Join(app, "recomplauncher.json")); !reflect.DeepEqual(got, want) {
		t.Fatalf("Linux launcher profile differs from the shared profile: %v", got)
	}
}
