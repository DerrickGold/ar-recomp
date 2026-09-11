// package assembles macOS and Linux desktop artifacts from a clean CMake tree.
package main

import (
	"debug/macho"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/host"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
func run() error {
	source := flag.String("source", "", "fresh CMake install tree (not a live player installation)")
	shell := flag.String("shell", "", "built desktop shell")
	out := flag.String("output", "", "new .app, .AppDir, or .AppImage output path")
	sdk := flag.String("linux-sdk", "", "Linux SDK root for host-independent AppImage packaging")
	glibcMax := flag.String("glibc-max", "", "maximum glibc for the complete Linux artifact, including offline build tools/SDL")
	flag.Parse()
	if *source == "" || *shell == "" || *out == "" {
		return fmt.Errorf("--source, --shell and --output are required")
	}
	if runtime.GOOS == "linux" || *sdk != "" {
		sourcePath, err := filepath.Abs(*source)
		if err != nil {
			return err
		}
		shellPath, err := filepath.Abs(*shell)
		if err != nil {
			return err
		}
		outputPath, err := filepath.Abs(*out)
		if err != nil {
			return err
		}
		return packageLinux(sourcePath, shellPath, outputPath, *sdk, *glibcMax)
	}
	if runtime.GOOS != "darwin" {
		return fmt.Errorf("use cmd/windows-package for Windows, or --linux-sdk for cross-host Linux packaging")
	}
	if *source == "" || *shell == "" || *out == "" || filepath.Ext(*out) != ".app" {
		return fmt.Errorf("--source, --shell and --output ending in .app are required")
	}
	output, err := filepath.Abs(*out)
	if err != nil {
		return err
	}
	if _, err := os.Lstat(output); !os.IsNotExist(err) {
		return fmt.Errorf("output already exists or cannot be checked; choose a new path: %s", output)
	}
	if err := host.ValidateWorkspace(output, *source); err != nil {
		return fmt.Errorf("output must be outside the source payload: %w", err)
	}
	// Validate and hash the clean staging input before creating an app.
	mach, err := macho.Open(*shell)
	if err != nil {
		return err
	}
	arch := map[macho.Cpu]string{macho.CpuAmd64: "amd64", macho.CpuArm64: "arm64"}[mach.Cpu]
	mach.Close()
	if arch == "" {
		return fmt.Errorf("unsupported macOS shell architecture")
	}
	for _, name := range []string{"actraiser-builder", "snesbuild"} {
		file := filepath.Join(*source, "utils/tools", name)
		f, err := macho.Open(file)
		if err != nil {
			return err
		}
		actual := map[macho.Cpu]string{macho.CpuAmd64: "amd64", macho.CpuArm64: "arm64"}[f.Cpu]
		f.Close()
		if actual != arch {
			return fmt.Errorf("mixed macOS payload: %s is %s, expected %s", file, actual, arch)
		}
	}
	if err = host.WriteManifest(*source, "darwin", arch); err != nil {
		return err
	}
	if err = os.MkdirAll(filepath.Dir(output), 0755); err != nil {
		return err
	}
	tmp, err := os.MkdirTemp(filepath.Dir(output), ".builder-package-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)
	app := filepath.Join(tmp, host.Name+".app")
	bin := filepath.Join(app, "Contents", "MacOS")
	payload := filepath.Join(app, "Contents", "Resources", "payload")
	if err = os.MkdirAll(bin, 0755); err != nil {
		return err
	}
	if err = copyFile(*shell, filepath.Join(bin, host.Name), 0755); err != nil {
		return err
	}
	err = filepath.WalkDir(*source, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(*source, path)
		if err != nil {
			return err
		}
		to := filepath.Join(payload, rel)
		if d.IsDir() {
			return os.MkdirAll(to, 0755)
		}
		info, err := d.Info()
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("non-regular package file: %s", path)
		}
		return copyFile(path, to, info.Mode().Perm())
	})
	if err != nil {
		return err
	}
	plist := `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>ActRaiserRecompBuilder</string>
<key>CFBundleIdentifier</key><string>org.actraiserrecomp.builder</string>
<key>CFBundleName</key><string>ActRaiser Recomp Builder</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>12.0</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
`
	if err = os.WriteFile(filepath.Join(app, "Contents", "Info.plist"), []byte(plist), 0644); err != nil {
		return err
	}
	cmd := exec.Command("/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", app)
	if out, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("sign Builder: %w: %s", err, out)
	}
	cmd = exec.Command("/usr/bin/codesign", "--verify", "--strict", app)
	if out, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("verify Builder: %w: %s", err, out)
	}
	if err = os.Rename(app, output); err != nil {
		return err
	}
	fmt.Println(output)
	return nil
}

func copyFile(from, to string, mode fs.FileMode) error {
	in, err := os.Open(from)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(to, os.O_CREATE|os.O_EXCL|os.O_WRONLY, mode)
	if err != nil {
		return err
	}
	_, err = io.Copy(out, in)
	closeErr := out.Close()
	if err != nil {
		return err
	}
	return closeErr
}
