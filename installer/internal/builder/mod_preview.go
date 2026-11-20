package builder

import (
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
	"github.com/DerrickGold/ar-recomp/installer/internal/modconfig"
)

var modManualPreviewName = regexp.MustCompile(`^[a-f0-9]{64}-0\.pdf$`)
var languagePreviewRelease = regexp.MustCompile(`^[a-f0-9]{64}$`)

type manualPreview struct {
	name  string
	stamp modFileStamp
	pages []manualPage
}

func (app *application) previewManualPages(file *os.File, info os.FileInfo, name string) ([]manualPage, error) {
	app.manualPreviewMu.Lock()
	defer app.manualPreviewMu.Unlock()
	stamp := readModFileStamp(file, info)
	if cached := app.manualPreview; cached.name == name && cached.stamp == stamp {
		return cached.pages, nil
	}
	data, err := readManualPDF(file)
	if err != nil {
		return nil, err
	}
	pages := carveManualAlbum(data)
	if len(pages) == 0 || !modArchiveUnchanged(file, stamp) {
		return nil, fmt.Errorf("manual preview is unavailable")
	}
	app.manualPreview = manualPreview{name: name, stamp: stamp, pages: pages}
	return pages, nil
}

func (app *application) serveModManualPage(w http.ResponseWriter, r *http.Request, name string) {
	n, err := strconv.Atoi(r.URL.Query().Get("page"))
	if err != nil || n < 1 || n > manualMaxPages || !modManualPreviewName.MatchString(name) {
		http.NotFound(w, r)
		return
	}
	f, info, err := app.openModPreview(name)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	defer f.Close()
	pages, err := app.previewManualPages(f, info, name)
	if err != nil || n > len(pages) {
		http.NotFound(w, r)
		return
	}
	page := pages[n-1]
	w.Header().Set("Content-Type", "image/jpeg")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Cache-Control", "private, max-age=3600")
	http.ServeContent(w, r, "page.jpg", info.ModTime(), io.NewSectionReader(f, int64(page.offset), int64(page.length)))
}

func (app *application) openLanguagePreview(modID, release, key string) (*lk.AuthorPack, string, error) {
	if modID == "" {
		if key == "native-us" {
			pack, err := lk.OpenNativeUSMod(nativeUSModPath(app.options.ProjectRoot))
			return pack, "", err
		}
		pack, err := lk.OpenInstalledLanguage(filepath.Join(app.localizationRoot(), "packs"), key)
		return pack, "", err
	}
	if !modIDPattern.MatchString(modID) {
		return nil, "", fmt.Errorf("invalid language mod ID")
	}
	assets := filepath.Join(app.options.ProjectRoot, "game-assets")
	packs := languagePacksRoot(assets)
	if languagePreviewRelease.MatchString(release) {
		// The catalog just resolved and indexed this archive. Reopen its
		// immutable snapshot without rewriting activation or taking the
		// publication lock while a catalog refresh may be using it.
		settings, err := modconfig.Read(filepath.Join(assets, modOverlayName))
		if err == nil {
			dirs := append([]string{filepath.Join(assets, "mods")}, settings.ResolveDirectories(app.options.ProjectRoot)...)
			for path, entry := range readModIndex(filepath.Join(assets, "mods-cache")) {
				if entry == nil || entry.Type != languageModType || entry.Digest != release ||
					entry.Language == nil || lk.LanguageModID(entry.Language.ID) != modID || !slices.Contains(dirs, filepath.Dir(path)) {
					continue
				}
				file, err := openModFile(path)
				if err != nil {
					continue
				}
				same := modArchiveUnchanged(file, entry.Source)
				_ = file.Close()
				if same {
					pack, err := entry.Language.Open(packs, release)
					return pack, entry.Version, err
				}
			}
		}
	}
	store, unlock, err := openModStore(app.options.ProjectRoot, app.options.ModDirectories)
	if err != nil {
		return nil, "", err
	}
	catalog, err := store.scan()
	store.saveIndex()
	unlock()
	if err != nil {
		return nil, "", err
	}
	group := slices.IndexFunc(catalog.languageGroups, func(group languageModGroup) bool { return group.ModID == modID })
	if group >= 0 {
		releases := catalog.languageGroups[group]
		for _, choice := range slices.Concat(releases.Choices, releases.LocalAlternates) {
			if choice.Key != release {
				continue
			}
			if choice.pack != nil {
				pack, err := choice.pack.language.Open(packs, choice.pack.digest)
				return pack, choice.Version, err
			}
			pack, err := lk.OpenInstalledLanguage(packs, choice.Filename)
			return pack, "", err
		}
	}
	return nil, "", fmt.Errorf("the selected language release is no longer available")
}

type languagePreviewSample struct {
	ID    string `json:"id"`
	Label string `json:"label"`
	Key   string `json:"labelKey,omitempty"`
	Text  string `json:"text"`
}

func previewLanguageSamples(pack *lk.AuthorPack) []languagePreviewSample {
	const maximum = 4
	workspace := pack.Workspace()
	var samples []languagePreviewSample
	var visit func(string)
	visit = func(parent string) {
		for _, item := range workspace.Children(parent) {
			if len(samples) == maximum {
				return
			}
			if item.Present == 0 {
				continue
			}
			if item.IsMessage {
				ops, _ := pack.MessageOperations(item.ID)
				if sample := languagePreviewText(ops); sample != "" {
					samples = append(samples, languagePreviewSample{ID: item.ID, Label: item.Label, Key: item.LabelKey, Text: sample})
				}
			}
			if item.HasChildren {
				visit(item.ID)
			}
		}
	}
	visit("")
	return samples
}

func languagePreviewText(ops []lk.AuthorOperation) string {
	var text strings.Builder
	for _, op := range ops {
		switch op.Op {
		case "text":
			text.WriteString(op.Value)
		case "line", "paragraph", "preferred_line", "page":
			text.WriteByte('\n')
		case "placeholder":
			fmt.Fprintf(&text, "{%s}", op.Name)
		}
		if text.Len() > 4096 {
			break
		}
	}
	runes := []rune(strings.TrimSpace(text.String()))
	if len(runes) > 320 {
		return string(runes[:320]) + "…"
	}
	return string(runes)
}

func (app *application) serveLanguagePreview(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	pack, version, err := app.openLanguagePreview(q.Get("mod"), q.Get("release"), q.Get("key"))
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, struct {
		Metadata lk.PackMetadata         `json:"metadata"`
		Version  string                  `json:"version,omitempty"`
		Coverage lk.RuntimeTextCoverage  `json:"coverage"`
		Samples  []languagePreviewSample `json:"samples"`
	}{pack.Manifest().Metadata(), version, pack.RuntimeTextCoverage(), previewLanguageSamples(pack)})
}
