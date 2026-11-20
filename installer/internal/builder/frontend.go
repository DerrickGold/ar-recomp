package builder

import (
	"embed"
	"net/http"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/internal/workshopui"
)

// Frontend files are compiled into the existing local executable. Building or
// running a player's game never invokes a package manager or external origin.
//
//go:embed web/index.html
var pageHTML string

//go:embed web/*.css web/*.js web/*.mjs
var frontendFiles embed.FS

func serveFrontend(w http.ResponseWriter, r *http.Request, endpoint string) {
	files := map[string]string{
		"builder/controls.css":            "controls.css",
		"builder/confirm.js":              "web/confirm.js",
		"builder/theme.css":               "web/theme.css",
		"builder/app.js":                  "web/app.js",
		"builder/big-screen.js":           "big-screen.js",
		"builder/big-screen.css":          "web/big-screen.css",
		"builder/mod-packs.js":            "web/mod-packs.js",
		"builder/mod-preview.js":          "web/mod-preview.js",
		"builder/audio-transport.js":      "web/audio-transport.js",
		"builder/workshop-publishing.js":  "web/workshop-publishing.js",
		"builder/mod-migration.js":        "web/mod-migration.js",
		"builder/big-screen-customize.js": "web/big-screen-customize.js",
		"builder/rom-browser.js":          "web/rom-browser.js",
		"builder/rom-checklist.js":        "web/rom-checklist.js",
		"builder/install-import.js":       "web/install-import.js",
		"builder/game-folder.js":          "web/game-folder.js",
		"builder/regional-media.js":       "web/regional-media.js",
		"builder/feedback.js":             "feedback.js",
		"builder/feedback.css":            "feedback.css",
		"builder/file-input.js":           "web/file-input.js",
		"builder/archive-save.js":         "web/archive-save.js",
		"builder/project-links.js":        "web/project-links.js",
		"builder/interface-text.js":       "interface-text.js",
		"builder/i18n.js":                 "web/i18n.js",
		"builder/scene.js":                "web/scene.js",
		"builder/encounters.mjs":          "web/encounters.mjs",
		"builder/scene-assets.mjs":        "web/scene-assets.mjs",
		"builder/ar2-scenes.mjs":          "web/ar2-scenes.mjs",
	}
	path, ok := files[endpoint]
	if !ok || r.Method != http.MethodGet {
		http.NotFound(w, r)
		return
	}
	assets := frontendFiles
	if !strings.HasPrefix(path, "web/") {
		assets = workshopui.Assets
	}
	data, err := assets.ReadFile(path)
	if err != nil {
		http.Error(w, "frontend unavailable", 500)
		return
	}
	typeName := "text/javascript; charset=utf-8"
	if strings.HasSuffix(endpoint, ".css") {
		typeName = "text/css; charset=utf-8"
	}
	serveEmbeddedAsset(w, r, endpoint, typeName, data)
}
