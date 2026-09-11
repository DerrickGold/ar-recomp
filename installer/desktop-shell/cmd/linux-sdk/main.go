package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/linuxsdk"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
func run() error {
	mode := flag.String("mode", "stage", "resolve (explicit pin update), stage, build, or audit")
	arch := flag.String("arch", "", "amd64 or arm64 (resolve)")
	lockfile := flag.String("lock", "", "reviewed Linux SDK lock (stage)")
	cache := flag.String("cache", "", "download cache (stage)")
	out := flag.String("output", "", "new lock file or SDK directory")
	sdkRoot := flag.String("sdk", "", "staged SDK root (build)")
	zig := flag.String("zig", "", "native host Zig executable (build)")
	source := flag.String("source", ".", "desktop shell module (build)")
	smoke := flag.Bool("smoke", false, "test-only renderer probe (build)")
	glibcMax := flag.String("glibc-max", linuxsdk.GLIBCBaseline, "maximum supported glibc (audit)")
	flag.Parse()
	if *out == "" {
		return fmt.Errorf("--output is required")
	}
	if *mode == "audit" {
		report, err := linuxsdk.AuditABI(*source, *arch, *glibcMax)
		if err != nil {
			return err
		}
		data, err := json.MarshalIndent(report, "", "  ")
		if err != nil {
			return err
		}
		file, err := os.OpenFile(*out, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0644)
		if err != nil {
			return err
		}
		_, err = file.Write(append(data, '\n'))
		closeErr := file.Close()
		if err != nil {
			return err
		}
		return closeErr
	}
	if *mode == "build" {
		if *sdkRoot == "" || *zig == "" {
			return fmt.Errorf("build requires --sdk and --zig")
		}
		sdk, err := linuxsdk.Load(*sdkRoot)
		if err != nil {
			return err
		}
		return sdk.BuildShell(*source, *out, *zig, *smoke)
	}
	if *mode == "resolve" {
		l, err := linuxsdk.Resolve(*arch)
		if err != nil {
			return err
		}
		if err = l.Validate(); err != nil {
			return err
		}
		data, err := json.MarshalIndent(l, "", "  ")
		if err != nil {
			return err
		}
		f, err := os.OpenFile(*out, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0644)
		if err != nil {
			return err
		}
		_, err = f.Write(append(data, '\n'))
		closeErr := f.Close()
		if err != nil {
			return err
		}
		return closeErr
	}
	if *mode != "stage" || *lockfile == "" || *cache == "" {
		return fmt.Errorf("stage requires --lock, --cache and --output")
	}
	data, err := os.ReadFile(*lockfile)
	if err != nil {
		return err
	}
	var l linuxsdk.Lock
	if err = json.Unmarshal(data, &l); err != nil {
		return err
	}
	return linuxsdk.Stage(l, *cache, *out)
}
