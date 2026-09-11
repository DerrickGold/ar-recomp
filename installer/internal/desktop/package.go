package desktop

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

type PackageOptions struct {
	Binary, Builder, ROM, Root, Destination string
	// DataRoot contains Workshop runtime assets, separately from build inputs.
	// Empty preserves the existing folder-bundle contract (Root).
	DataRoot string
	Version  string
	// Format is app, appimage, or appdir. Empty selects the host's application.
	Format                        string
	AppImageTool, AppImageRuntime string
	Replace                       bool
	Output                        io.Writer
}

type Artifact struct {
	Path   string
	Backup string
}

// Package assembles a private, locally compiled game. Public release packaging
// must never invoke it with a developer ROM.
func Package(ctx context.Context, options PackageOptions) (Artifact, error) {
	if options.Output == nil {
		options.Output = io.Discard
	}
	if options.Format == "" {
		if runtime.GOOS == "darwin" {
			options.Format = "app"
		} else if runtime.GOOS == "linux" {
			options.Format = "appimage"
		}
	}
	suffix := map[string]string{"app": ".app", "appdir": ".AppDir", "appimage": ".AppImage"}[options.Format]
	if suffix == "" {
		return Artifact{}, fmt.Errorf("unsupported application format %q", options.Format)
	}
	if options.Format == "app" && runtime.GOOS != "darwin" {
		return Artifact{}, errors.New("macOS apps require a macOS packaging host")
	}
	if options.Format != "app" && runtime.GOOS != "linux" {
		return Artifact{}, errors.New("AppImage packaging requires a Linux host for dependency inspection")
	}
	for _, field := range []*string{&options.Binary, &options.Builder, &options.ROM, &options.Root, &options.Destination} {
		if *field == "" {
			return Artifact{}, errors.New("binary, builder, ROM, root, and destination are required")
		}
		absolute, err := filepath.Abs(*field)
		if err != nil {
			return Artifact{}, err
		}
		*field = absolute
	}
	if err := os.MkdirAll(options.Destination, 0755); err != nil {
		return Artifact{}, err
	}
	final := filepath.Join(options.Destination, Name+suffix)
	if _, err := os.Lstat(final); err == nil && !options.Replace {
		return Artifact{}, fmt.Errorf("%s already exists; use --replace to retain it as a backup and publish a new application", final)
	} else if err != nil && !errors.Is(err, os.ErrNotExist) {
		return Artifact{}, err
	}
	stage, err := os.MkdirTemp(options.Destination, ".actraiser-package-*")
	if err != nil {
		return Artifact{}, err
	}
	defer os.RemoveAll(stage)
	app := filepath.Join(stage, Name+suffix)
	if options.Format == "appimage" {
		app = filepath.Join(stage, Name+".AppDir")
	}
	bin, libraries, resources := filepath.Join(app, "usr", "bin"), filepath.Join(app, "usr", "lib"), filepath.Join(app, "usr", "share", Name)
	if options.Format == "app" {
		bin, libraries, resources = filepath.Join(app, "Contents", "MacOS"), filepath.Join(app, "Contents", "Frameworks"), filepath.Join(app, "Contents", "Resources")
	}
	for _, dir := range []string{bin, libraries, resources} {
		if err := os.MkdirAll(dir, 0755); err != nil {
			return Artifact{}, err
		}
	}
	for _, item := range [][2]string{{options.Binary, filepath.Join(bin, Name)}, {options.Builder, filepath.Join(bin, "actraiser-builder")}, {options.ROM, filepath.Join(resources, ROMName)}} {
		mode := fs.FileMode(0755)
		if item[0] == options.ROM {
			mode = 0644
		}
		if err := copyFileAtomic(item[0], item[1], mode); err != nil {
			return Artifact{}, err
		}
	}
	manifest, _ := json.MarshalIndent(Manifest{Format: 1, Product: Name, Version: options.Version}, "", "  ")
	if err := atomicWrite(filepath.Join(resources, markerName), manifest, 0644); err != nil {
		return Artifact{}, err
	}
	dataRoot := options.DataRoot
	if dataRoot == "" {
		dataRoot = options.Root
	}
	if err := stageResources(dataRoot, resources); err != nil {
		return Artifact{}, err
	}
	if dataRoot != options.Root {
		if err := stageNotices(options.Root, resources); err != nil {
			return Artifact{}, err
		}
	}
	if options.Format == "app" {
		if err := packageMacOS(ctx, options, app, bin, libraries); err != nil {
			return Artifact{}, err
		}
	} else {
		if err := packageLinux(ctx, options, app, bin, libraries); err != nil {
			return Artifact{}, err
		}
	}
	if options.Format == "appimage" {
		tool, imageRuntime, err := resolveAppImageTools(options)
		if err != nil {
			return Artifact{}, err
		}
		imagePath := filepath.Join(stage, Name+suffix)
		command := exec.CommandContext(ctx, tool, "--runtime-file", imageRuntime, "--no-appstream", app, imagePath)
		command.Env = append(os.Environ(), "APPIMAGE_EXTRACT_AND_RUN=1", "ARCH="+linuxArchitecture())
		command.Stdout, command.Stderr = options.Output, options.Output
		if err := command.Run(); err != nil {
			return Artifact{}, fmt.Errorf("create AppImage: %w", err)
		}
		if err := os.Chmod(imagePath, 0755); err != nil {
			return Artifact{}, err
		}
		if err := ValidateAppImage(imagePath); err != nil {
			return Artifact{}, err
		}
		app = imagePath
	}
	if err := ctx.Err(); err != nil {
		return Artifact{}, err
	}
	result, err := publishArtifact(app, final, options.Replace)
	if err == nil {
		fmt.Fprintf(options.Output, "Application: %s\n", result.Path)
	}
	return result, err
}

func stageResources(root, resources string) error {
	if err := stageSeed(root, resources); err != nil {
		return err
	}
	seed := filepath.Join(resources, "seed")
	for _, leaf := range []string{"languages/native-us/pack.ini", "fonts/noto/NotoSans-SemiCondensedExtraBold.ttf", "fonts/noto/NotoSansJP-Bold.otf", "fonts/noto/NotoSansArabic-Bold.ttf", "fonts/noto/NotoSansHebrew-Bold.ttf", "fonts/noto/OFL.txt", "fonts/noto/NotoSansJP-OFL.txt"} {
		if _, err := fileHash(filepath.Join(seed, "game-assets", leaf)); err != nil {
			return fmt.Errorf("required runtime content %s: %w", leaf, err)
		}
	}
	return stageNotices(root, resources)
}

// stageSeed also supports first launch, before the ROM-derived language pack
// exists. Package performs the stricter completed-game validation above.
func stageSeed(root, resources string) error {
	seed := filepath.Join(resources, "seed")
	defaults := filepath.Join(root, "defaults")
	if info, err := os.Stat(defaults); err == nil && info.IsDir() {
		if err := copyTree(defaults, filepath.Join(seed, "defaults")); err != nil {
			return err
		}
	} else {
		// Source-checkout defaults have the same provenance as release defaults.
		for _, item := range [][2]string{
			{"installer/packaging/templates/config.ini", "config.ini"},
			{"diorama-layers.ini", "diorama-layers.ini"},
			{"installer/internal/builder/assets/manifest.ini", "game-assets/manifest.ini"},
		} {
			if err := copyFileAtomic(filepath.Join(root, item[0]), filepath.Join(seed, "defaults", item[1]), 0644); err != nil {
				return fmt.Errorf("stage defaults: %w", err)
			}
		}
	}
	for _, leaf := range []string{"fonts", "hd", "audio", "languages/native-us", "languages/packs"} {
		source := filepath.Join(root, "game-assets", leaf)
		if _, err := os.Stat(source); errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err := copyTree(source, filepath.Join(seed, "game-assets", leaf)); err != nil {
			return err
		}
	}
	for _, leaf := range []string{"manual.pdf", "manifest.ini"} {
		if err := copyFileAtomic(filepath.Join(root, "game-assets", leaf), filepath.Join(seed, "game-assets", leaf), 0644); err != nil {
			return err
		}
	}
	return nil
}

func stageNotices(root, resources string) error {
	// Notices survive removal of build machinery and travel with the app.
	for _, leaf := range []string{"licenses", "docs/GAME-LICENSE.txt", "LICENSE", "LICENSE_SCOPE.md", "ATTRIBUTION.md", "THIRD_PARTY_NOTICES.md", "ACTRAISER-THIRD-PARTY-NOTICES.md", "snesrecomp-go/runtime/LICENSE", "snesrecomp-go/runtime/NOTICE.md", "snesrecomp-go/runtime/PROVENANCE.md", "snesrecomp-go/runtime/licenses", "third_party/sheenbidi/LICENSE", "third_party/unicode/NOTICE", "third_party/unicode/utf8proc-LICENSE.md", "installer/THIRD_PARTY_NOTICES.md", "installer/packaging/licenses/sdl-ttf", "installer/packaging/licenses/appimage"} {
		from := filepath.Join(root, leaf)
		info, err := os.Stat(from)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return err
		}
		to := filepath.Join(resources, "notices", leaf)
		if info.IsDir() {
			err = copyTree(from, to)
		} else {
			err = copyFileAtomic(from, to, 0644)
		}
		if err != nil {
			return err
		}
	}
	return nil
}

func copyTree(source, destination string) error {
	return filepath.WalkDir(source, func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if strings.HasPrefix(entry.Name(), ".") {
			if entry.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		relative, err := filepath.Rel(source, path)
		if err != nil {
			return err
		}
		if entry.IsDir() {
			return os.MkdirAll(filepath.Join(destination, relative), 0755)
		}
		if !entry.Type().IsRegular() {
			return fmt.Errorf("runtime content is not a regular file: %s", path)
		}
		return copyFileAtomic(path, filepath.Join(destination, relative), 0644)
	})
}

func publishArtifact(staged, final string, replace bool) (Artifact, error) {
	result := Artifact{Path: final}
	if info, err := os.Lstat(final); err == nil {
		if !replace || info.Mode()&os.ModeSymlink != 0 {
			return Artifact{}, fmt.Errorf("refusing to replace %s", final)
		}
		switch filepath.Ext(final) {
		case ".app":
			err = validateManifest(filepath.Join(final, "Contents", "Resources"))
		case ".AppDir":
			err = validateManifest(filepath.Join(final, "usr", "share", Name))
		case ".AppImage":
			err = ValidateAppImage(final)
		default:
			err = errors.New("unknown artifact type")
		}
		if err != nil {
			return Artifact{}, fmt.Errorf("existing artifact is not recognized: %w", err)
		}
		backup, err := os.MkdirTemp(filepath.Dir(final), Name+"-previous-*")
		if err != nil {
			return Artifact{}, err
		}
		result.Backup = filepath.Join(backup, filepath.Base(final))
		if err := os.Rename(final, result.Backup); err != nil {
			os.Remove(backup)
			return Artifact{}, err
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return Artifact{}, err
	}
	if err := os.Rename(staged, final); err != nil {
		if result.Backup != "" {
			if restoreErr := os.Rename(result.Backup, final); restoreErr != nil {
				return Artifact{}, fmt.Errorf("publish: %v; restore: %v; previous app remains at %s", err, restoreErr, result.Backup)
			}
		}
		return Artifact{}, err
	}
	return result, nil
}

func runTool(ctx context.Context, output io.Writer, name string, args ...string) error {
	command := exec.CommandContext(ctx, name, args...)
	data, err := command.CombinedOutput()
	if len(data) > 0 {
		fmt.Fprint(output, string(data))
	}
	if err != nil {
		return fmt.Errorf("%s: %w: %s", name, err, strings.TrimSpace(string(data)))
	}
	return nil
}
