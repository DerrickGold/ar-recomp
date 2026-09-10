package builder

import (
	"context"
	"fmt"
	"path/filepath"

	"github.com/DerrickGold/ar-recomp/installer/internal/fontprobe"
	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

// The application, never an imported pack, selects the trusted game executable.
func (app *application) probeLocalizationFonts(ctx context.Context, fonts []lk.FontCoverageSource, scalars []rune) (lk.FontCoverageProbeResult, error) {
	if app.options.fontCoverageProbe != nil {
		return app.options.fontCoverageProbe(ctx, fonts, scalars)
	}
	app.mu.Lock()
	binary, building := app.result.BinaryPath, app.state == "building"
	app.mu.Unlock()
	if binary == "" || building {
		return lk.FontCoverageProbeResult{}, fmt.Errorf("finish building the game before checking font coverage or installing/exporting language packs")
	}
	builtin := filepath.Join(app.options.ProjectRoot, "game-assets", "fonts", "noto", "NotoSans-SemiCondensedExtraBold.ttf")
	return fontprobe.Run(ctx, binary, builtin, fonts, scalars)
}

// Kept as the GUI host's test seam; CLI and GUI use the same process adapter.
var runLocalizationFontProbe = fontprobe.Run
