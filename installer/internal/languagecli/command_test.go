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

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
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
	refs, err := lk.AuthorReferences("us")
	if err != nil {
		t.Fatal(err)
	}
	required, optional := 0, 0
	for _, ref := range refs {
		if !ref.NativeInProfile {
			continue
		}
		if ref.RequiredForComplete {
			required++
		}
		if ref.USRuntimeUsage == "live_optional" {
			optional++
		}
	}
	if coverage.Profile != "us" || !coverage.Runtime || coverage.Required.Total != required ||
		coverage.LiveOptional.Total != optional || len(coverage.Surfaces) != 6 || len(coverage.Dormant) != 2 {
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

func TestUpgradeWritesNewProjectOnly(t *testing.T) {
	example, err := filepath.Abs("../../../tests/fixtures/language-pack-v1")
	if err != nil {
		t.Fatal(err)
	}
	manifest, err := os.ReadFile(filepath.Join(example, "pack.ini"))
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "upgraded.arproject")
	args := []string{"upgrade", "--pack", example, "--new-id", "example.upgraded", "--out", path}
	var out bytes.Buffer
	if err := Run(context.Background(), args, &out); err != nil {
		t.Fatal(err)
	}
	project, err := lk.OpenAuthorInput(path)
	if err != nil || project.Pack().Manifest().Version() != 2 || project.Pack().Manifest().Metadata().ID != "example.upgraded" {
		t.Fatal(project, err)
	}
	after, _ := os.ReadFile(filepath.Join(example, "pack.ini"))
	if !bytes.Equal(manifest, after) {
		t.Fatal("upgraded input in place")
	}
	before, _ := os.ReadFile(path)
	if err := Run(context.Background(), args, &out); err == nil {
		t.Fatal("overwrote upgrade output")
	}
	after, _ = os.ReadFile(path)
	if !bytes.Equal(before, after) {
		t.Fatal("failed upgrade changed existing output")
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
	copyFile(helper, filepath.Join(bundle, "utils/tools/actraiser-builder"+suffix), 0755)
	copyFile(probe, filepath.Join(bundle, "game"+suffix), 0755)
	packs := filepath.Join(bundle, "utils/game-assets/languages/packs")
	copyFile("../../../examples/example.fr-ca.arlang", filepath.Join(packs, "Any filename.arlang"), 0644)
	second, err := lk.OpenAuthorProjectDirectory("../../../examples/language-pack")
	if err != nil {
		t.Fatal(err)
	}
	metadata := second.Pack().Manifest().Metadata()
	metadata.ID = "example.fr-ca.alternate"
	metadata.Name = "Alternate French Canadian"
	second, err = second.WithMetadata(metadata)
	if err != nil {
		t.Fatal(err)
	}
	second, err = second.ReviewedAll()
	if err != nil {
		t.Fatal(err)
	}
	second, _, err = second.Publication(lk.PublicationOptions{ConfirmRights: true})
	if err != nil {
		t.Fatal(err)
	}
	var secondArchive bytes.Buffer
	if err := second.WriteArchive(&secondArchive, "publication"); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(packs, "Same locale, second package.arlang"), secondArchive.Bytes(), 0644); err != nil {
		t.Fatal(err)
	}
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
	if out := run(); !strings.Contains(out, "example.fr-ca\t") || !strings.Contains(out, "example.fr-ca.alternate\t") {
		t.Fatal(out)
	}
	moved := filepath.Join(base, "Moved installation")
	if err := os.Rename(bundle, moved); err != nil {
		t.Fatal(err)
	}
	bundle = moved
	if out := run(); !strings.Contains(out, "example.fr-ca\t") || !strings.Contains(out, "example.fr-ca.alternate\t") {
		t.Fatal("relocated discovery failed", out)
	}
	// A missing helper must not reuse an earlier successful but now stale index.
	helper = filepath.Join(bundle, "utils/tools/actraiser-builder"+suffix)
	if err := os.Rename(helper, helper+".away"); err != nil {
		t.Fatal(err)
	}
	if out := run(); strings.Contains(out, "example.fr-ca\t") || !strings.Contains(out, "need the installed ActRaiser Builder helper") {
		t.Fatal(out)
	}
}

func TestInstallUpgradesV1CopyAndReportsIt(t *testing.T) {
	project, err := lk.OpenAuthorInput("../../../tests/fixtures/language-pack-v1")
	if err != nil {
		t.Fatal(err)
	}
	publication, _, err := project.Publication(lk.PublicationOptions{ConfirmRights: true, IncludeWIP: true})
	if err != nil {
		t.Fatal(err)
	}
	var archive bytes.Buffer
	if err := publication.WriteArchive(&archive, "publication"); err != nil {
		t.Fatal(err)
	}
	source := filepath.Join(t.TempDir(), "original.arlang")
	if err := os.WriteFile(source, archive.Bytes(), 0644); err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	var out bytes.Buffer
	if err := Run(context.Background(), []string{"install", "--root", root, "--pack", source}, &out); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "Automatically upgraded the installed copy from v1 to v2") {
		t.Fatal("missing CLI upgrade notice", out.String())
	}
	installedPath := filepath.Join(root, "game-assets", "languages", "packs", project.Pack().Manifest().Metadata().ID+".arlang")
	installed, err := lk.OpenAuthorInput(installedPath)
	if err != nil || installed.Pack().Manifest().Version() != 2 || installed.Pack().Manifest().Metadata() != project.Pack().Manifest().Metadata() {
		t.Fatal("installed archive was not upgraded with its identity intact", err)
	}
	before, err := os.ReadFile(source)
	if err != nil || !bytes.Equal(before, archive.Bytes()) {
		t.Fatal("original archive changed", err)
	}
	// Reinstalling v2 copies its bytes as-is and does not report another upgrade.
	out.Reset()
	secondRoot := t.TempDir()
	if err := Run(context.Background(), []string{"install", "--root", secondRoot, "--pack", installedPath}, &out); err != nil {
		t.Fatal(err)
	}
	if strings.Contains(out.String(), "upgraded") {
		t.Fatal("v2 was reported as upgraded", out.String())
	}
	first, _ := os.ReadFile(installedPath)
	second, err := os.ReadFile(filepath.Join(secondRoot, "game-assets", "languages", "packs", filepath.Base(installedPath)))
	if err != nil || !bytes.Equal(first, second) {
		t.Fatal("v2 archive was rewritten", err)
	}
}
