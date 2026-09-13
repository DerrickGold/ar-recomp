package winbundle

import (
	"bytes"
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"testing"
)

func TestStartupProgressAndCancellation(t *testing.T) {
	o := fixture(t, "amd64")
	if err := Create(o); err != nil {
		t.Fatal(err)
	}
	var updates []Progress
	report := func(p Progress) {
		if p.Completed < 0 || p.Completed > p.Total {
			t.Fatalf("invalid progress: %+v", p)
		}
		updates = append(updates, p)
	}
	a, err := OpenContext(context.Background(), o.Output, report)
	if err != nil {
		t.Fatal(err)
	}
	defer a.Close()
	dest := filepath.Join(t.TempDir(), "runtime")
	if err := a.ExtractContext(context.Background(), dest, func(string) error { return nil }, report); err != nil {
		t.Fatal(err)
	}
	if err := a.VerifyDirectoryContext(context.Background(), dest, report); err != nil {
		t.Fatal(err)
	}
	for _, stage := range []string{"Checking bundled package", "Extracting bundled tools and browser", "Applying browser permissions", "Verifying prepared files"} {
		started, finished := false, false
		for _, p := range updates {
			if p.Stage == stage {
				started = started || p.Completed == 0
				finished = finished || p.Completed == p.Total
			}
		}
		if !started || !finished {
			t.Fatalf("stage %q did not report its bounds: %+v", stage, updates)
		}
	}
	t.Run("cancel before open", func(t *testing.T) {
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		if _, err := OpenContext(ctx, o.Output, report); !errors.Is(err, context.Canceled) {
			t.Fatal(err)
		}
	})
	t.Run("cancel extraction before publish", func(t *testing.T) {
		parent := t.TempDir()
		ctx, cancel := context.WithCancel(context.Background())
		defer cancel()
		err := a.ExtractContext(ctx, filepath.Join(parent, "runtime"), func(string) error { cancel(); return nil }, report)
		if !errors.Is(err, context.Canceled) {
			t.Fatal(err)
		}
		files, err := os.ReadDir(parent)
		if err != nil || len(files) != 0 {
			t.Fatalf("left partial extraction: %v %v", files, err)
		}
	})
	t.Run("cancel warm verification preserves cache", func(t *testing.T) {
		ctx, cancel := context.WithCancel(context.Background())
		defer cancel()
		err := a.VerifyDirectoryContext(ctx, dest, func(Progress) { cancel() })
		if !errors.Is(err, context.Canceled) {
			t.Fatal(err)
		}
		if err := a.VerifyDirectory(dest); err != nil {
			t.Fatal(err)
		}
	})
}

type cancelReader struct{ cancel context.CancelFunc }

func (r cancelReader) Read(p []byte) (int, error) { r.cancel(); p[0] = 'x'; return 1, nil }

func TestStartupCopyCancelsWithinLargeFile(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	m := newMeter(ctx, nil, "test", 1<<30)
	n, err := m.copy(io.Discard, cancelReader{cancel})
	if n != 1 || !errors.Is(err, context.Canceled) {
		t.Fatalf("copy ignored cancellation: %d %v", n, err)
	}
	var out bytes.Buffer
	n, err = m.copy(&out, bytes.NewReader([]byte("must not be copied")))
	if n != 0 || !errors.Is(err, context.Canceled) {
		t.Fatalf("started cancelled copy: %d %v", n, err)
	}
}
