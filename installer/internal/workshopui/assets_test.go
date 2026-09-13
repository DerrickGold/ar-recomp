package workshopui

import (
	"context"
	"os/exec"
	"testing"
	"time"
)

func TestFeedbackComponent(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("Node is only needed for development UI checks")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	if out, err := exec.CommandContext(ctx, node, "--test", "feedback.test.mjs").CombinedOutput(); err != nil {
		t.Fatalf("feedback UI: %v\n%s", err, out)
	}
}
