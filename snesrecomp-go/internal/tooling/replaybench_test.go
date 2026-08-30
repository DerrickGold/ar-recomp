package tooling

import (
	"io"
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestReplayBenchmarkManifestAndArtifactGate(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("synthetic fixture uses a POSIX script")
	}
	root := t.TempDir()
	binaryPath := filepath.Join(root, "fake-game")
	script := "#!/bin/sh\nmkdir -p saves\nprintf 'frame=3\\n' > saves/state.txt\nprintf 'same-state' > saves/wram.bin\n"
	if err := os.WriteFile(binaryPath, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"game.sfc", "config.ini"} {
		if err := os.WriteFile(filepath.Join(root, name), []byte(name), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	suitePath := filepath.Join(root, "suite.json")
	suite := `{
  "version": 1,
  "command": ["{binary}"],
  "artifacts": ["saves/state.txt", "saves/wram.bin"],
  "final_frame_artifact": "saves/state.txt",
  "final_frame_pattern": "(?m)^frame=(\\d+)",
  "workloads": [{"name":"boot", "frames":3}]
}`
	if err := os.WriteFile(suitePath, []byte(suite), 0o644); err != nil {
		t.Fatal(err)
	}
	result, passed, err := BuildReplayBenchmark(ReplayBenchmarkOptions{
		Root: root, SuitePath: "suite.json", BinaryPath: "fake-game",
		ROMPath: "game.sfc", ConfigPath: "config.ini", Runs: 2,
		MaxRegressionPercent: 5, MaxSuiteRegressionPercent: 3,
	}, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if !passed || len(result.Workloads) != 1 || len(result.Workloads["boot"].FinalArtifactSHA256) != 2 {
		t.Fatalf("passed=%v result=%+v", passed, result)
	}
	if result.Suite.Path != "suite.json" || result.Binary.Path != "fake-game" || result.Inputs["rom"].Path != "game.sfc" {
		t.Fatalf("benchmark evidence paths are not root-relative: %+v", result)
	}
	left := map[string]any{"replay": ReplayBenchmarkFile{Path: "/checkout-a/replay.rec", Bytes: 3, SHA256: "same"}}
	right := map[string]any{"replay": map[string]any{"path": "/checkout-b/replay.rec", "bytes": float64(3), "sha256": "same"}}
	if !equalReplayInputValue(left, right) {
		t.Fatal("relocated but identical benchmark input should compare equal")
	}
}
