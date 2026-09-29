package winbundle

import (
	"context"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestLaunchCacheSkipsArchiveAndContentsThenRechecksOnlyTouchedFile(t *testing.T) {
	o := fixture(t, "arm64")
	if err := Create(o); err != nil {
		t.Fatal(err)
	}
	cache := t.TempDir()
	var directory string
	for _, mode := range []string{"first", "unchanged", "touched file", "unchanged again", "forced", "touched package", "missing receipt", "bad receipt"} {
		t.Run(mode, func(t *testing.T) {
			var stages []Progress
			report := func(p Progress) { stages = append(stages, p) }
			if mode == "touched file" {
				name := filepath.Join(directory, "payload/utils/snesbuild.ini")
				info, err := os.Stat(name)
				if err != nil {
					t.Fatal(err)
				}
				if err := os.Chtimes(name, info.ModTime(), info.ModTime().Add(time.Hour)); err != nil {
					t.Fatal(err)
				}
			}
			if mode == "touched package" {
				info, err := os.Stat(o.Output)
				if err != nil {
					t.Fatal(err)
				}
				if err := os.Chtimes(o.Output, info.ModTime(), info.ModTime().Add(time.Hour)); err != nil {
					t.Fatal(err)
				}
			}
			if mode == "missing receipt" || mode == "bad receipt" {
				name := directory + ".verification.json"
				var err error
				if mode == "missing receipt" {
					err = os.Remove(name)
				} else {
					err = os.WriteFile(name, []byte("bad JSON"), 0600)
				}
				if err != nil {
					t.Fatal(err)
				}
			}
			a, err := OpenForLaunch(context.Background(), o.Output, cache, mode == "forced", report)
			if err != nil {
				t.Fatal(err)
			}
			defer a.Close()
			directory = filepath.Join(cache, a.ID)
			if _, err := a.PrepareDirectoryContext(context.Background(), directory, nil, report); err != nil {
				t.Fatal(err)
			}
			var archive, contents, changed int
			for _, p := range stages {
				if p.Completed != 0 {
					continue
				}
				switch p.Stage {
				case "Checking bundled package":
					archive++
				case "Verifying prepared files":
					contents++
				case "Verifying changed files":
					changed++
					if p.Total != int64(len("fixture")) {
						t.Fatalf("read more than the touched file: %+v", p)
					}
				}
			}
			full := mode == "first" || mode == "forced" || mode == "touched package" || mode == "missing receipt" || mode == "bad receipt"
			if (archive == 1) != full || (contents == 1) != (full && mode != "first") || (changed == 1) != (mode == "touched file") {
				t.Fatalf("unexpected work: archive=%d contents=%d changed=%d: %+v", archive, contents, changed, stages)
			}
		})
	}
}

func TestLaunchCacheDoesNotAcceptChangedContentsOrReplaceReceiptOnFailure(t *testing.T) {
	for _, mode := range []string{"corrupt file", "extra file", "missing file", "forced same-metadata corruption", "corrupt package", "cancelled"} {
		t.Run(mode, func(t *testing.T) {
			o := fixture(t, "amd64")
			if err := Create(o); err != nil {
				t.Fatal(err)
			}
			cache := t.TempDir()
			a, err := OpenForLaunch(context.Background(), o.Output, cache, false, nil)
			if err != nil {
				t.Fatal(err)
			}
			directory := filepath.Join(cache, a.ID)
			if _, err := a.PrepareDirectoryContext(context.Background(), directory, nil, nil); err != nil {
				t.Fatal(err)
			}
			a.Close()
			prior, err := os.ReadFile(directory + ".verification.json")
			if err != nil {
				t.Fatal(err)
			}
			name := filepath.Join(directory, "payload/utils/snesbuild.ini")
			switch mode {
			case "corrupt file", "forced same-metadata corruption":
				info, err := os.Stat(name)
				if err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(name, []byte("changed"), 0755); err != nil {
					t.Fatal(err)
				}
				mtime := info.ModTime().Add(time.Hour)
				if mode == "forced same-metadata corruption" {
					mtime = info.ModTime()
				}
				if err := os.Chtimes(name, mtime, mtime); err != nil {
					t.Fatal(err)
				}
			case "extra file":
				put(t, directory, "webview/injected.dll", []byte("extra"))
			case "missing file":
				if err := os.Remove(name); err != nil {
					t.Fatal(err)
				}
			case "corrupt package":
				data, err := os.ReadFile(o.Output)
				if err != nil {
					t.Fatal(err)
				}
				data[520] ^= 1
				if err := os.WriteFile(o.Output, data, 0755); err != nil {
					t.Fatal(err)
				}
			}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			a, err = OpenForLaunch(ctx, o.Output, cache, mode == "forced same-metadata corruption", nil)
			if err == nil {
				defer a.Close()
				_, err = a.PrepareDirectoryContext(ctx, directory, nil, func(Progress) {
					if mode == "cancelled" {
						cancel()
					}
				})
			}
			if err == nil {
				t.Fatal("invalid or cancelled launch accepted")
			}
			current, err := os.ReadFile(directory + ".verification.json")
			if err != nil || string(current) != string(prior) {
				t.Fatal("failed launch replaced successful receipt", err)
			}
		})
	}
}

// Optional real-release benchmark; extracts only into a disposable test cache.
func BenchmarkLaunchVerification(b *testing.B) {
	filename := os.Getenv("ACTRAISER_BENCH_WINDOWS_BUNDLE")
	if filename == "" {
		b.Skip("set ACTRAISER_BENCH_WINDOWS_BUNDLE to a packaged Windows Builder")
	}
	cache := b.TempDir()
	run := func(b *testing.B, force bool) {
		a, err := OpenForLaunch(context.Background(), filename, cache, force, nil)
		if err != nil {
			b.Fatal(err)
		}
		defer a.Close()
		if _, err := a.PrepareDirectoryContext(context.Background(), filepath.Join(cache, a.ID), nil, nil); err != nil {
			b.Fatal(err)
		}
	}
	run(b, false)
	for _, force := range []bool{true, false} {
		name := "unchanged"
		if force {
			name = "full"
		}
		b.Run(name, func(b *testing.B) {
			for b.Loop() {
				run(b, force)
			}
		})
	}
}
