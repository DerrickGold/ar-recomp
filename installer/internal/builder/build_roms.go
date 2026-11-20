package builder

import (
	"bytes"
	"fmt"
	"io"
	"mime/multipart"
	"os"
	"path/filepath"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2rom"
	"github.com/DerrickGold/ar-recomp/installer/internal/romset"
)

func (app *application) prepareBuildROMs(files []*multipart.FileHeader) (string, error) {
	inputs, err := readUploadedROMs(files)
	if err != nil {
		return "", err
	}
	return app.installBuildROMs(inputs, false)
}

func (app *application) resolveBuildROM(form *multipart.Form) (path, errorCode string, err error) {
	files := form.File["rom"]
	if len(form.File) != 0 && (len(form.File) != 1 || len(files) == 0) {
		return "", "", fmt.Errorf("select supported ROM files in the ROM checklist")
	}
	firstRun := firstFormValue(form, "firstRun") == "true"
	reuseSaved := firstFormValue(form, "reuseROM") == "true" && len(files) == 0
	if firstFormValue(form, "useSelectedROMs") == "true" {
		saved, savedErr := reusableBuildROM(app.options.ProjectRoot)
		choices := app.snapshotROMChoices()
		if firstRun {
			app.mu.Lock()
			existing := app.install.CanLaunch || savedErr == nil
			allowed := app.options.BuildOnFirstRun && !app.firstBuildAttempted && !existing
			if allowed && choices["us"].launchPath != "" {
				app.firstBuildAttempted = true
			}
			app.mu.Unlock()
			switch {
			case existing:
				return "", "", fmt.Errorf("an existing game or saved ROM is available; choose Build when ready")
			case !allowed:
				return "", "", fmt.Errorf("automatic first build is unavailable; choose a ROM in the Builder")
			case choices["us"].launchPath == "" || len(files) != 0:
				return "", "builder.build.launch_rom_invalid", fmt.Errorf("select a supported ActRaiser (USA) ROM")
			}
			errorCode = "builder.build.launch_rom_invalid"
		}
		if reuseSaved && savedErr != nil {
			return "", "builder.build.saved_rom_missing", savedErr
		}
		if reuseSaved || choices["us"].preferSavedCore(saved) {
			delete(choices, "us")
		}
		path, err = app.prepareSelectedBuildROMs(files, choices, saved, firstRun)
		return path, errorCode, err
	}
	if len(files) != 0 {
		path, err = app.prepareBuildROMs(files)
		return path, "", err
	}
	if reuseSaved {
		path, err = reusableBuildROM(app.options.ProjectRoot)
		return path, "builder.build.saved_rom_missing", err
	}
	return "", "", fmt.Errorf("select supported ROM files in the ROM checklist")
}

func firstFormValue(form *multipart.Form, key string) string {
	if values := form.Value[key]; len(values) != 0 {
		return values[0]
	}
	return ""
}

func (app *application) prepareSelectedBuildROMs(files []*multipart.FileHeader, choices map[string]romChoice, saved string, firstRun bool) (string, error) {
	uploads, err := readUploadedROMs(files)
	if err != nil {
		return "", err
	}
	seen := make(map[string]bool)
	var inputs []romset.Input
	for _, input := range uploads {
		id, _, err := romset.Identify(input.Data)
		if err != nil {
			return "", fmt.Errorf("%s: unsupported ROM; select a clean supported ActRaiser ROM", input.Name)
		}
		if !seen[id] {
			inputs = append(inputs, input)
			seen[id] = true
		}
		delete(choices, id)
	}
	selected, err := app.readROMChoices(choices)
	if err != nil {
		return "", err
	}
	inputs = append(inputs, selected...)
	if len(inputs) == 0 && saved != "" {
		return saved, nil
	}
	if len(inputs) == 0 {
		return "", fmt.Errorf("select a supported ActRaiser (USA) ROM")
	}
	return app.installBuildROMs(inputs, firstRun)
}

func readUploadedROMs(files []*multipart.FileHeader) ([]romset.Input, error) {
	if len(files) > 6 {
		return nil, fmt.Errorf("select up to six supported ROMs, including ActRaiser (USA) for a new build")
	}
	inputs := make([]romset.Input, 0, len(files))
	for _, header := range files {
		input, err := readUploadedROM(header)
		if err != nil {
			return nil, err
		}
		inputs = append(inputs, input)
	}
	return inputs, nil
}

func readUploadedROM(header *multipart.FileHeader) (romset.Input, error) {
	if !validROMName(header.Filename) {
		return romset.Input{}, fmt.Errorf("%s: select a .sfc or .smc ROM", header.Filename)
	}
	file, err := header.Open()
	if err != nil {
		return romset.Input{}, err
	}
	defer file.Close()
	return readBuildROM(header.Filename, file)
}

func readBuildROM(name string, file io.Reader) (romset.Input, error) {
	data, err := io.ReadAll(io.LimitReader(file, ar2rom.Size+513))
	if err != nil {
		return romset.Input{}, err
	}
	if len(data) > ar2rom.Size+512 {
		return romset.Input{}, fmt.Errorf("%s: ROM exceeds the supported size", name)
	}
	return romset.Input{Name: name, Data: data}, nil
}

func readLaunchROM(path string) (romset.Input, error) {
	if !validROMName(path) {
		return romset.Input{}, fmt.Errorf("select a .sfc or .smc ROM")
	}
	if info, err := os.Stat(path); err != nil || !info.Mode().IsRegular() {
		return romset.Input{}, fmt.Errorf("%s: the selected ROM is missing or unreadable", filepath.Base(path))
	}
	file, err := os.Open(path)
	if err != nil {
		return romset.Input{}, err
	}
	defer file.Close()
	return readRegularROM(filepath.Base(path), file)
}

func readRegularROM(name string, file *os.File) (romset.Input, error) {
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() {
		return romset.Input{}, fmt.Errorf("%s: the selected ROM is missing or unreadable", name)
	}
	return readBuildROM(name, file)
}

func (app *application) installBuildROMs(inputs []romset.Input, requireUSA bool) (string, error) {
	plan, err := romset.Prepare(inputs)
	if err != nil {
		return "", err
	}
	var baseline string
	if plan.Baseline == nil {
		if requireUSA {
			return "", fmt.Errorf("select ActRaiser (USA) for the main build")
		}
		baseline, err = reusableBuildROM(app.options.ProjectRoot)
		if err != nil {
			return "", fmt.Errorf("include an ActRaiser (USA) ROM for the main game; the other ROMs add optional content")
		}
	}
	if err := app.installROMContent(plan, false); err != nil {
		return "", err
	}
	if plan.Baseline != nil {
		baseline, err = storeROM(app.options.ProjectRoot, bytes.NewReader(plan.Baseline))
		if err != nil {
			return "", err
		}
	}
	log := &lockedLogWriter{app: app}
	for _, release := range plan.Releases {
		fmt.Fprintln(log, "Imported "+release)
	}
	return baseline, nil
}

func (app *application) installROMContent(plan *romset.Plan, reimport bool) error {
	// All selected inputs have passed identification/extraction before writes.
	app.gameImportMu.Lock()
	defer app.gameImportMu.Unlock()
	app.regionalMediaMu.Lock()
	defer app.regionalMediaMu.Unlock()
	root := app.options.ProjectRoot
	if err := plan.Install(root); err != nil {
		return err
	}
	for _, source := range plan.LanguageSources() {
		m := source.Pack().Manifest().Metadata()
		if !reimport && regularFileExists(regionalROMModMarker(root, m.ID)) {
			continue
		}
		if err := installRegionalROMMod(root, app.options.ModDirectories, source); err != nil {
			err = fmt.Errorf("install %s language mod: %w", m.Name, err)
			if reimport {
				return err
			}
			fmt.Fprintf(&lockedLogWriter{app: app}, "Optional content: %v\n", err)
		}
	}
	return nil
}
