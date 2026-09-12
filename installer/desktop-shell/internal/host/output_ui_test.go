package host

import (
	"context"
	"os/exec"
	"testing"
	"time"
)

func TestOutputSelectionUI(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("Node is needed for UI regression tests")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	if out, err := exec.CommandContext(ctx, node, "--test", "output_ui.test.mjs").CombinedOutput(); err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
}
