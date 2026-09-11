package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestGUIReadyFileIsPrivateAndNeverReplaced(t *testing.T) {
	path := filepath.Join(t.TempDir(), "ready.json")
	address := "http://127.0.0.1:12345/private-token/"
	if err := writeGUIReadyFile(path, address); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var descriptor struct {
		Schema int    `json:"schema"`
		URL    string `json:"url"`
	}
	if err = json.Unmarshal(data, &descriptor); err != nil || descriptor.Schema != 1 || descriptor.URL != address {
		t.Fatalf("invalid descriptor: %+v, %v", descriptor, err)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if runtime.GOOS != "windows" && info.Mode().Perm() != 0600 {
		t.Fatalf("not private: %v", info.Mode())
	}
	if err = writeGUIReadyFile(path, "replacement"); err == nil {
		t.Fatal("replaced existing session descriptor")
	}
	after, err := os.ReadFile(path)
	if err != nil || string(after) != string(data) {
		t.Fatal("descriptor changed")
	}
}
