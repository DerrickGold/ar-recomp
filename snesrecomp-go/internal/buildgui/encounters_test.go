package buildgui

import (
	"context"
	"os/exec"
	"testing"
	"time"
)

// Optional developer check of the exact module embedded in the browser.
// Node is never invoked by the builder, extraction, or a player's build.
func TestEncounterTimeline(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("optional JavaScript timeline checks need Node")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	if output, err := exec.CommandContext(ctx, node, "--test", "testdata/encounters.test.mjs").CombinedOutput(); err != nil {
		t.Fatalf("encounter timeline: %v\n%s", err, output)
	}
}
