package builder

import "net/http"

// Open only the project's fixed public pages. Desktop webviews do not all
// handle target="_blank" links, so the local host launches the system browser.
func (app *application) openProjectPage(w http.ResponseWriter, r *http.Request, page string) {
	if r.Method != http.MethodPost {
		http.NotFound(w, r)
		return
	}
	var address string
	switch page {
	case "repository":
		address = "https://github.com/DerrickGold/ar-recomp"
	case "releases":
		address = "https://github.com/DerrickGold/ar-recomp/releases"
	default:
		http.NotFound(w, r)
		return
	}
	open := app.options.openURL
	if open == nil {
		open = openBrowser
	}
	if err := open(address); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{
			"error": err.Error(), "errorCode": "builder.links.open_failed",
		})
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"opened": true})
}
