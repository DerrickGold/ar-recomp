package languagecli

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	lk "github.com/DerrickGold/snesrecomp-go/internal/localizationkit"
)

// A ROM-free executable for exercising the exact desktop process protocol,
// independent of the unrelated recompiler/GUI command packages.
func TestMain(m *testing.M) {
	if len(os.Args) > 1 && os.Args[1] == "language" {
		if err := Run(context.Background(), os.Args[2:], os.Stdout); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		os.Exit(0)
	}
	os.Exit(m.Run())
}

func TestReferenceAndEditorFreeValidation(t *testing.T) {
	var out bytes.Buffer
	if err := Run(context.Background(), []string{"reference"}, &out); err != nil {
		t.Fatal(err)
	}
	var reference struct {
		Format string
		Routes []struct {
			ID      string
			Anchors []string
		}
	}
	if err := json.Unmarshal(out.Bytes(), &reference); err != nil || len(reference.Routes) < 500 {
		t.Fatal(err, len(reference.Routes))
	}
	shipped, err := os.ReadFile("../../../docs/language-authoring-reference.json")
	if err != nil || !bytes.Equal(bytes.TrimSpace(shipped), bytes.TrimSpace(out.Bytes())) {
		t.Fatal("regenerate shipped reference with language reference", err)
	}
	example, err := filepath.Abs("../../../examples/language-pack")
	if err != nil {
		t.Fatal(err)
	}
	out.Reset()
	if err := Run(context.Background(), []string{"validate", "--pack", example}, &out); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "example.fr-ca") {
		t.Fatal(out.String())
	}
	var validation struct {
		TextCoverage lk.AuthorCoverageReport `json:"text_coverage"`
	}
	if err := json.Unmarshal(out.Bytes(), &validation); err != nil {
		t.Fatal(err)
	}
	coverage := validation.TextCoverage
	if coverage.Profile != "us" || !coverage.Runtime || coverage.Required.Total != 495 ||
		coverage.LiveOptional.Total != 26 || len(coverage.Surfaces) != 6 || len(coverage.Dormant) != 2 {
		t.Fatal("CLI omitted authoritative text coverage", out.String())
	}
	path := filepath.Join(t.TempDir(), "reference.json")
	if err := Run(context.Background(), []string{"reference", "--out", path}, &out); err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(path)
	if err := Run(context.Background(), []string{"reference", "--out", path}, &out); err == nil {
		t.Fatal("overwrote an existing artifact")
	}
	if after, _ := os.ReadFile(path); !bytes.Equal(before, after) {
		t.Fatal("changed existing output")
	}
	if err := Run(context.Background(), []string{"package", "--pack", example, "--out", path + ".arlang", "--confirm-rights", "--all-messages"}, &out); err == nil {
		t.Fatal("published without game font gate")
	}
	if _, err := os.Stat(path + ".arlang"); !os.IsNotExist(err) {
		t.Fatal("failed publication left output")
	}
}

func TestCommandHelpAndOptionBoundaries(t *testing.T) {
	var out bytes.Buffer
	if err := Run(context.Background(), []string{"--help"}, &out); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "install") {
		t.Fatal(out.String())
	}
	out.Reset()
	if err := Run(context.Background(), []string{"install", "--help"}, &out); !errors.Is(err, flag.ErrHelp) {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "--replace") || !strings.Contains(out.String(), "publication .arlang file") ||
		strings.Contains(out.String(), "--confirm-rights") || strings.Contains(out.String(), ".arproject") {
		t.Fatal("install help should only describe installation options", out.String())
	}
	if err := Run(context.Background(), []string{"install", "--confirm-rights"}, &out); err == nil || !strings.Contains(err.Error(), "not valid") {
		t.Fatal("publishing rights must not become an installation option", err)
	}
}

func TestDesktopArchiveDiscoveryRelocation(t *testing.T) {
	probe := os.Getenv("AR_LANGUAGE_DESKTOP_PROBE")
	if probe == "" {
		t.Skip("set AR_LANGUAGE_DESKTOP_PROBE to the SDL discovery executable")
	}
	base := t.TempDir()
	bundle := filepath.Join(base, "Install with spaces")
	suffix := ""
	if runtime.GOOS == "windows" {
		suffix = ".exe"
	}
	copyFile := func(from, to string, mode os.FileMode) {
		t.Helper()
		data, err := os.ReadFile(from)
		if err != nil {
			t.Fatal(err)
		}
		if err := os.MkdirAll(filepath.Dir(to), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(to, data, mode); err != nil {
			t.Fatal(err)
		}
	}
	helper, _ := os.Executable()
	copyFile(helper, filepath.Join(bundle, "utils/tools/snesbuild"+suffix), 0755)
	copyFile(probe, filepath.Join(bundle, "game"+suffix), 0755)
	copyFile("../../../examples/example.fr-ca.arlang", filepath.Join(bundle, "utils/game-assets/languages/packs/Any filename.arlang"), 0644)
	run := func() string {
		t.Helper()
		cmd := exec.Command(filepath.Join(bundle, "game"+suffix), filepath.Join(bundle, "utils/game-assets/languages/packs"))
		cmd.Dir = base
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatal(err, string(out))
		}
		return string(out)
	}
	if out := run(); !strings.Contains(out, "example.fr-ca\t") {
		t.Fatal(out)
	}
	moved := filepath.Join(base, "Moved installation")
	if err := os.Rename(bundle, moved); err != nil {
		t.Fatal(err)
	}
	bundle = moved
	if out := run(); !strings.Contains(out, "example.fr-ca\t") {
		t.Fatal("relocated discovery failed", out)
	}
	// A missing helper must not reuse an earlier successful but now stale index.
	helper = filepath.Join(bundle, "utils/tools/snesbuild"+suffix)
	if err := os.Rename(helper, helper+".away"); err != nil {
		t.Fatal(err)
	}
	if out := run(); strings.Contains(out, "example.fr-ca\t") || !strings.Contains(out, "need the installed snesbuild helper") {
		t.Fatal(out)
	}
}
