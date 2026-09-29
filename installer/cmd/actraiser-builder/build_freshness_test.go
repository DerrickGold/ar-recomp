package main

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/DerrickGold/ar-recomp/installer/internal/builder"
	"github.com/DerrickGold/ar-recomp/installer/internal/buildworkspace"
)

func buildRecordFixture(t *testing.T) (string, string, string) {
	t.Helper()
	dir := t.TempDir()
	binary := filepath.Join(dir, "ActRaiserRecomp.exe")
	if err := os.WriteFile(binary, []byte("compiled game A"), 0755); err != nil {
		t.Fatal(err)
	}
	return dir, binary, strings.Repeat("a", 64)
}

func TestGameBuildFreshness(t *testing.T) {
	dir, binary, id := buildRecordFixture(t)
	c := &gameBuildChecker{outputDir: dir, inputID: id, version: "same-label-dirty"}
	if got := c.Check(binary); got.State != "unknown" || got.Reason != "missing_record" {
		t.Fatal(got)
	}
	if err := recordGameBuild(context.Background(), dir, binary, id, "same-label-dirty"); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2; i++ {
		if got := c.Check(binary); got.State != "current" || got.BuiltVersion != "same-label-dirty" {
			t.Fatal(got)
		}
	}
	newBuilder := &gameBuildChecker{outputDir: dir, inputID: strings.Repeat("b", 64), version: "same-label-dirty"}
	if got := newBuilder.Check(binary); got.State != "rebuild" {
		t.Fatalf("trusted identical display labels: %+v", got)
	}
	// Player data edits never imply a code update.
	for _, name := range []string{"settings.ini", "config.ini", "user-rom.sfc", "my-save.srm"} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte("player data"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	if got := c.Check(binary); got.State != "current" {
		t.Fatal(got)
	}
	// Same-length executable replacement must not inherit the previous receipt.
	if err := os.WriteFile(binary, []byte("compiled game B"), 0755); err != nil {
		t.Fatal(err)
	}
	future := time.Now().Add(time.Second)
	if err := os.Chtimes(binary, future, future); err != nil {
		t.Fatal(err)
	}
	if got := c.Check(binary); got.State != "unknown" || got.Reason != "game_changed" {
		t.Fatal(got)
	}
	if err := recordGameBuild(context.Background(), dir, binary, newBuilder.inputID, "new release"); err != nil {
		t.Fatal(err)
	}
	if got := newBuilder.Check(binary); got.State != "current" {
		t.Fatal(got)
	}
	if got := c.Check(binary); got.State != "rebuild" {
		t.Fatalf("old Builder falsely claimed to be newer: %+v", got)
	}
}

func TestGameBuildRecordFailureAndCancellation(t *testing.T) {
	dir, binary, id := buildRecordFixture(t)
	if err := recordGameBuild(context.Background(), dir, binary, id, "old"); err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(filepath.Join(dir, buildRecordName))
	if err != nil {
		t.Fatal(err)
	}
	result := builder.Result{BinaryPath: binary}
	failure := errors.New("compiler failed")
	if _, err := completeGameBuild(context.Background(), dir, dir, strings.Repeat("b", 64), result, failure); !errors.Is(err, failure) {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := completeGameBuild(ctx, dir, dir, strings.Repeat("b", 64), result, nil); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	after, _ := os.ReadFile(filepath.Join(dir, buildRecordName))
	if string(before) != string(after) {
		t.Fatal("failed/cancelled build replaced prior receipt")
	}
	if _, err := completeGameBuild(context.Background(), dir, dir, strings.Repeat("b", 64), result, nil); err != nil {
		t.Fatal(err)
	}
	c := &gameBuildChecker{outputDir: dir, inputID: strings.Repeat("b", 64)}
	if got := c.Check(binary); got.State != "current" {
		t.Fatal(got)
	}
	entries, _ := os.ReadDir(dir)
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), ".actraiser-build-") {
			t.Fatal("left receipt staging file")
		}
	}
}

func TestGameBuildReceiptValidationAndRelocation(t *testing.T) {
	for _, artifact := range []string{"ActRaiserRecomp.exe", "ActRaiserRecomp.AppImage", "ActRaiserRecomp.app", "ActRaiserRecomp.AppDir"} {
		t.Run(artifact, func(t *testing.T) {
			parent := t.TempDir()
			dir := filepath.Join(parent, "old game folder")
			binary := gameArtifactBinary(filepath.Join(dir, artifact))
			if err := os.MkdirAll(filepath.Dir(binary), 0755); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(binary, []byte("game"), 0755); err != nil {
				t.Fatal(err)
			}
			id := strings.Repeat("c", 64)
			if err := recordGameBuild(context.Background(), dir, binary, id, "v1"); err != nil {
				t.Fatal(err)
			}
			moved := filepath.Join(parent, "moved 日本語 game")
			if err := os.Rename(dir, moved); err != nil {
				t.Fatal(err)
			}
			c := &gameBuildChecker{outputDir: moved, inputID: id}
			binary = gameArtifactBinary(filepath.Join(moved, artifact))
			if got := c.Check(binary); got.State != "current" {
				t.Fatalf("relocation lost identity: %+v", got)
			}
			canonical, err := filepath.EvalSymlinks(binary)
			if err != nil {
				t.Fatal(err)
			}
			if got := c.Check(canonical); got.State != "current" {
				t.Fatalf("canonical discovery path: %+v", got)
			}
			data, _ := os.ReadFile(filepath.Join(moved, buildRecordName))
			var record gameBuildRecord
			if err := json.Unmarshal(data, &record); err != nil {
				t.Fatal(err)
			}
			if strings.Contains(record.Binary, `\`) || filepath.IsAbs(record.Binary) {
				t.Fatalf("nonportable path %q", record.Binary)
			}
			for _, bad := range []string{"not-json", strings.Repeat("x", 8193), `{"schema":99}`, strings.Replace(string(data), record.Binary, "../outside.exe", 1)} {
				if err := os.WriteFile(filepath.Join(moved, buildRecordName), []byte(bad), 0600); err != nil {
					t.Fatal(err)
				}
				if got := c.Check(binary); got.State != "unknown" {
					t.Fatal(got)
				}
			}
		})
	}
}

func TestGameBuildReceiptRefusesRedirects(t *testing.T) {
	dir, binary, id := buildRecordFixture(t)
	outside := filepath.Join(t.TempDir(), "keep.json")
	if err := os.WriteFile(outside, []byte("untouched"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, filepath.Join(dir, buildRecordName)); err != nil {
		t.Skip("symlink creation unavailable:", err)
	}
	if err := recordGameBuild(context.Background(), dir, binary, id, "v1"); err == nil {
		t.Fatal("accepted redirected receipt")
	}
	c := &gameBuildChecker{outputDir: dir, inputID: id}
	if got := c.Check(binary); got.State != "unknown" {
		t.Fatal(got)
	}
	if data, _ := os.ReadFile(outside); string(data) != "untouched" {
		t.Fatal("modified receipt target")
	}
	if err := recordGameBuild(context.Background(), dir, outside, id, "v1"); err == nil {
		t.Fatal("accepted external artifact")
	}
}

func TestBundledBuildIdentityAndGenericPackaging(t *testing.T) {
	dir := t.TempDir()
	id := strings.Repeat("a", 64)
	if got := bundledBuildID(dir, id); got != id {
		t.Fatal(got)
	}
	if got := bundledBuildID(dir, ""); got != "" {
		t.Fatal("invented a release ID")
	}
	c := &gameBuildChecker{outputDir: dir, version: "dev"}
	if got := c.Check(filepath.Join(dir, "game")); got.State != "unavailable" {
		t.Fatal(got)
	}
	cmake, err := exec.LookPath("cmake")
	if err != nil {
		t.Skip("CMake not installed")
	}
	if err := os.WriteFile(filepath.Join(dir, "snesbuild.ini"), []byte("[project]"), 0600); err != nil {
		t.Fatal(err)
	}
	run := func() string {
		t.Helper()
		cmd := exec.Command(cmake, "-DBUILDER_IDENTITY_ROOT="+dir, "-P", "../../packaging/write_build_identity.cmake")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("stamp generation: %v\n%s", err, out)
		}
		got := bundledBuildID(dir, "")
		if !buildworkspace.ValidID(got) {
			t.Fatal("missing generated release identity")
		}
		return got
	}
	first := run()
	if run() != first {
		t.Fatal("stamp hashed itself or timestamps")
	}
	if err := os.WriteFile(filepath.Join(dir, "source.c"), []byte("new code"), 0600); err != nil {
		t.Fatal(err)
	}
	if run() == first {
		t.Fatal("source update left identity unchanged")
	}
	if bundledBuildID(dir, id) != id {
		t.Fatal("generic stamp overrode verified desktop identity")
	}
	if err := os.WriteFile(filepath.Join(dir, buildInputsName), []byte("not an ID"), 0600); err != nil {
		t.Fatal(err)
	}
	if bundledBuildID(dir, "") != "" {
		t.Fatal("accepted corrupt identity")
	}
}
