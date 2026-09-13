package desktop

import (
	"path/filepath"
	"runtime"
	"testing"
)

func TestPathWithin(t *testing.T) {
	root := t.TempDir()
	for _, tc := range []struct {
		parent, child string
		want          bool
	}{
		{root, root, true},
		{root, filepath.Join(root, "game"), true},
		{filepath.Join(root, "inputs"), filepath.Join(root, "game"), false},
	} {
		if got, err := pathWithin(tc.parent, tc.child); err != nil || got != tc.want {
			t.Fatalf("pathWithin(%q, %q) = %v, %v", tc.parent, tc.child, got, err)
		}
	}
	if _, err := pathWithin("relative", root); err == nil {
		t.Fatal("accepted relative path")
	}
}

func TestWindowsContainmentAcrossVolumes(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("Windows volume semantics")
	}
	for _, tc := range []struct {
		parent, child string
		want          bool
	}{
		{`C:\Builder\inputs`, `D:\Games\ActRaiser`, false},
		{`D:\Games\ActRaiser`, `C:\Previous game`, false},
		{`C:\Builder`, `c:\builder\inputs`, true},
		{`C:\Builder`, `C:\Builder-other`, false},
		{`\\server\one\Builder`, `\\server\two\Game`, false},
	} {
		if got, err := pathWithin(tc.parent, tc.child); err != nil || got != tc.want {
			t.Errorf("pathWithin(%q, %q) = %v, %v", tc.parent, tc.child, got, err)
		}
	}
}
