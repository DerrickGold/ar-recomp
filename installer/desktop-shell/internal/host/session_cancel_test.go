package host

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSessionCancellationDoesNotPublishOrLeaveStagingFiles(t *testing.T) {
	for _, when := range []string{"before", "during"} {
		t.Run(when, func(t *testing.T) {
			payload := fixture(t)
			parent := t.TempDir()
			work := filepath.Join(parent, "workspace")
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			if when == "before" {
				cancel()
			}
			_, err := PrepareSession(ctx, payload, work, func(string) { cancel() })
			if !errors.Is(err, context.Canceled) {
				t.Fatalf("cancellation: %v", err)
			}
			entries, err := os.ReadDir(parent)
			if err != nil || len(entries) != 0 {
				t.Fatalf("partial workspace/staging files remain: %v, %v", entries, err)
			}
			if _, err := PrepareSession(context.Background(), payload, work, nil); err != nil {
				t.Fatalf("relaunch after cancellation: %v", err)
			}
		})
	}
}

func TestPayloadReaderObservesCancellationBetweenChunks(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	r := contextReader{ctx, strings.NewReader(strings.Repeat("x", 128))}
	chunk := make([]byte, 16)
	if n, err := r.Read(chunk); n != len(chunk) || err != nil {
		t.Fatalf("first chunk: %d, %v", n, err)
	}
	cancel()
	if n, err := io.Copy(io.Discard, r); n != 0 || !errors.Is(err, context.Canceled) {
		t.Fatalf("read after cancellation: %d, %v", n, err)
	}
}
