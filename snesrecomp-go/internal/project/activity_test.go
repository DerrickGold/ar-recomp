package project

import (
	"io"
	"strings"
	"testing"
	"time"
)

type activityWriter chan string

func (w activityWriter) Write(p []byte) (int, error) { w <- string(p); return len(p), nil }

func TestBuildActivityReportsLivenessAndStops(t *testing.T) {
	w := make(activityWriter, 100)
	stop := buildActivity(&toolLog{writer: w}, true, time.Millisecond, func() string { return "compiling: 1/2 complete, 1 active" })
	select {
	case line := <-w:
		if !strings.Contains(line, "1/2 complete, 1 active") {
			t.Fatalf("activity: %s", line)
		}
	case <-time.After(time.Second):
		t.Fatal("no build activity")
	}
	stop()
	before := len(w)
	time.Sleep(5 * time.Millisecond)
	if len(w) != before {
		t.Fatal("activity writer outlived build phase")
	}
	buildActivity(&toolLog{writer: io.Discard}, false, time.Millisecond, func() string { t.Error("disabled activity ran"); return "" })()
}
