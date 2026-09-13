package winbundle

import (
	"context"
	"io"
	"time"
)

// Progress describes one startup stage, not a prediction of total startup time.
// Total == 0 denotes a stage with no measurable byte count.
type Progress struct {
	Stage            string
	Completed, Total int64
}

type ProgressFunc func(Progress)

// A meter is confined to the extraction goroutine. Reading through it keeps
// cancellation responsive even when a single compiler/browser file is large.
type meter struct {
	ctx      context.Context
	progress ProgressFunc
	value    Progress
	last     time.Time
}

func newMeter(ctx context.Context, progress ProgressFunc, stage string, total int64) *meter {
	m := &meter{ctx: ctx, progress: progress, value: Progress{Stage: stage, Total: total}}
	m.report(true)
	return m
}

func (m *meter) report(force bool) {
	if m.progress != nil && (force || time.Since(m.last) >= 100*time.Millisecond) {
		m.progress(m.value)
		m.last = time.Now()
	}
}

func (m *meter) copy(dst io.Writer, src io.Reader) (int64, error) {
	return io.Copy(dst, meteredReader{src, m})
}

type meteredReader struct {
	reader io.Reader
	meter  *meter
}

func (r meteredReader) Read(p []byte) (int, error) {
	if err := r.meter.ctx.Err(); err != nil {
		return 0, err
	}
	n, err := r.reader.Read(p)
	r.meter.value.Completed += int64(n)
	r.meter.report(false)
	return n, err
}

func (a *Archive) expandedSize() int64 {
	var total int64
	for _, entry := range a.Manifest.Files {
		total += entry.Size
	}
	return total
}
