package builder

import (
	"bytes"
	"fmt"
	"io"
	"mime/multipart"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2rom"
	"github.com/DerrickGold/ar-recomp/installer/internal/romset"
)

func (app *application) prepareBuildROMs(files []*multipart.FileHeader) (string, error) {
	if len(files) == 0 || len(files) > 6 {
		return "", fmt.Errorf("select up to six supported ROMs, including ActRaiser (USA) for a new build")
	}
	inputs := make([]romset.Input, 0, len(files))
	for _, header := range files {
		if !validROMName(header.Filename) {
			return "", fmt.Errorf("%s: select a .sfc or .smc ROM", header.Filename)
		}
		file, err := header.Open()
		if err != nil {
			return "", err
		}
		data, err := io.ReadAll(io.LimitReader(file, ar2rom.Size+513))
		file.Close()
		if err != nil {
			return "", err
		}
		if len(data) > ar2rom.Size+512 {
			return "", fmt.Errorf("%s: ROM exceeds the supported size", header.Filename)
		}
		inputs = append(inputs, romset.Input{Name: header.Filename, Data: data})
	}
	plan, err := romset.Prepare(inputs)
	if err != nil {
		return "", err
	}
	var baseline string
	if plan.Baseline == nil {
		baseline, err = reusableBuildROM(app.options.ProjectRoot)
		if err != nil {
			return "", fmt.Errorf("include an ActRaiser (USA) ROM for the main game; the other ROMs add optional content")
		}
	}
	// All selected inputs have passed identification/extraction before writes.
	app.gameImportMu.Lock()
	defer app.gameImportMu.Unlock()
	app.regionalMediaMu.Lock()
	defer app.regionalMediaMu.Unlock()
	if err := plan.Install(app.options.ProjectRoot); err != nil {
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
