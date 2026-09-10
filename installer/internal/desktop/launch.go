package desktop

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

type LaunchOptions struct {
	StorageOptions
	PrintPaths  bool
	PrepareOnly bool
	GameArgs    []string
}

func ParseLaunchOptions(args []string) (LaunchOptions, error) {
	var options LaunchOptions
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--data-dir":
			i++
			if i == len(args) || args[i] == "" {
				return options, errors.New("--data-dir requires a directory")
			}
			options.DataDir = args[i]
		case "--portable":
			options.Portable = true
		case "--global":
			options.Global = true
		case "--print-paths":
			options.PrintPaths = true
		case "--prepare-only":
			options.PrepareOnly = true
		case "--":
			options.GameArgs = append(options.GameArgs, args[i+1:]...)
			return options, nil
		default:
			if strings.HasPrefix(args[i], "--data-dir=") {
				options.DataDir = strings.TrimPrefix(args[i], "--data-dir=")
				if options.DataDir == "" {
					return options, errors.New("--data-dir requires a directory")
				}
			} else {
				options.GameArgs = append(options.GameArgs, args[i])
			}
		}
	}
	return options, nil
}

// Launch establishes the working directory once. No writability heuristic and
// no game-side bundle anchor is involved: game-assets lives in the data tree,
// while the code and ROM stay in the application.
func Launch(options LaunchOptions) error {
	executable, err := os.Executable()
	if err != nil {
		return err
	}
	layout, err := Discover(executable, os.Getenv("APPIMAGE"))
	if err != nil {
		return err
	}
	cwd, err := os.Getwd()
	if err != nil {
		return err
	}
	// The AppImage runtime may have moved the working directory during mount.
	if runtime.GOOS == "linux" && os.Getenv("APPIMAGE") != "" && filepath.IsAbs(os.Getenv("OWD")) {
		cwd = os.Getenv("OWD")
	}
	root, err := ResolveDataDirectory(layout, options.StorageOptions, cwd, runtime.GOOS, os.Getenv)
	if err != nil {
		return err
	}
	root, err = rejectPackageData(layout, root)
	if err != nil {
		return err
	}
	if options.PrintPaths {
		fmt.Printf("Application: %s\nData: %s\nResources: %s\n", layout.Artifact, root, layout.Resources)
		return nil
	}
	arguments, err := gameArguments(options.GameArgs, cwd, root, layout.Resources)
	if err != nil {
		return err
	}
	if err := InitializeData(layout.Resources, root); err != nil {
		return err
	}
	if options.PrepareOnly {
		fmt.Println(root)
		return nil
	}
	logs := filepath.Join(root, "logs")
	if err := os.MkdirAll(logs, 0755); err != nil {
		return err
	}
	log, err := os.CreateTemp(logs, "launch-"+time.Now().Format("20060102-150405")+"-*.log")
	if err != nil {
		return err
	}
	defer log.Close()
	fmt.Fprintf(log, "Application: %s\nData: %s\n", layout.Artifact, root)
	command := exec.Command(layout.Binary, arguments...)
	command.Dir = root
	command.Env = append(os.Environ(), "AR_USER_DATA_DIR="+root)
	if runtime.GOOS == "linux" {
		// The bundle's private dependencies lead; user driver configuration
		// remains available through the inherited path.
		path := layout.Libraries
		if inherited := os.Getenv("LD_LIBRARY_PATH"); inherited != "" {
			path += ":" + inherited
		}
		command.Env = append(command.Env, "LD_LIBRARY_PATH="+path)
	}
	command.Stdout, command.Stderr = log, log
	if err := command.Run(); err != nil {
		return fmt.Errorf("the game could not run: %w\nDetails: %s", err, log.Name())
	}
	return nil
}

func gameArguments(args []string, cwd, root, resources string) ([]string, error) {
	result := append([]string{}, args...)
	rom, config := false, false
	absolute := func(path string) string {
		if filepath.IsAbs(path) {
			return path
		}
		return filepath.Join(cwd, path)
	}
	for i := 0; i < len(result); i++ {
		if result[i] == "--config" {
			i++
			if i == len(result) {
				return nil, errors.New("--config requires a file")
			}
			result[i] = absolute(result[i])
			config = true
		} else if !strings.HasPrefix(result[i], "-") {
			result[i] = absolute(result[i])
			rom = true
		}
	}
	if !rom {
		result = append(result, filepath.Join(resources, ROMName))
	}
	if !config {
		result = append(result, "--config", filepath.Join(root, "config.ini"))
	}
	return result, nil
}

func rejectPackageData(layout Layout, root string) (string, error) {
	// Resolve existing ancestors as well as the leaf so a symlink cannot send
	// portable data into the signed app or a mounted AppImage.
	resolve := func(path string) (string, error) {
		var tail []string
		for {
			resolved, err := filepath.EvalSymlinks(path)
			if err == nil {
				for i := len(tail) - 1; i >= 0; i-- {
					resolved = filepath.Join(resolved, tail[i])
				}
				return resolved, nil
			}
			if !errors.Is(err, os.ErrNotExist) {
				return "", err
			}
			parent := filepath.Dir(path)
			if parent == path {
				return "", err
			}
			tail = append(tail, filepath.Base(path))
			path = parent
		}
	}
	resolved, err := resolve(root)
	if err != nil {
		return "", err
	}
	// Resources is always inside the physical package, including AppImage mounts.
	packageRoot := filepath.Dir(filepath.Dir(layout.Resources))
	if filepath.Base(filepath.Dir(layout.Resources)) == "share" {
		packageRoot = filepath.Dir(packageRoot)
	}
	packageRoot, err = resolve(packageRoot)
	if err != nil {
		return "", err
	}
	relative, err := filepath.Rel(packageRoot, resolved)
	if err != nil {
		return "", err
	}
	if localPath(relative, true) {
		return "", errors.New("application data must be outside the application package; choose --global or an external --data-dir")
	}
	return resolved, nil
}

// ReportLaunchError makes desktop startup failures visible without a terminal.
// Message text is a process argument, never interpolated into a shell script.
func ReportLaunchError(err error) {
	message := err.Error()
	switch runtime.GOOS {
	case "darwin":
		exec.Command("/usr/bin/osascript", "-e", `on run argv
display alert "ActRaiser Recomp" message (item 1 of argv) as critical
end run`, message).Run()
	case "linux":
		if tool, err := exec.LookPath("zenity"); err == nil {
			exec.Command(tool, "--error", "--title=ActRaiser Recomp", "--text="+message).Run()
		} else if tool, err := exec.LookPath("kdialog"); err == nil {
			exec.Command(tool, "--error", message, "--title", "ActRaiser Recomp").Run()
		}
	}
}
