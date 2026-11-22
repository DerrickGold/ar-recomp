package builder

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/internal/desktop"
	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

type modDownload struct {
	token string
	name  string
	file  *os.File
}

func (download *modDownload) close() {
	if download != nil {
		_ = download.file.Close()
		_ = os.Remove(download.file.Name())
	}
}

var (
	modAudioPreviewName = regexp.MustCompile(`^[a-f0-9]{64}-[0-9]+\.ogg$`)
	modTitlePreviewName = regexp.MustCompile(`^[a-f0-9]{64}-0\.png$`)
)

func validBrowsedModName(name string) bool {
	return strings.EqualFold(filepath.Ext(name), ".o2r")
}

func validModImportName(name string) bool {
	switch strings.ToLower(filepath.Ext(name)) {
	case ".o2r", ".arlang", ".pdf":
		return true
	}
	return false
}

func (app *application) openBrowsedMod(location, selected string) (*os.File, error) {
	path, err := filepath.Localize(selected)
	if err != nil || !validModImportName(path) || strings.HasPrefix(filepath.Base(path), ".") {
		return nil, fmt.Errorf("select an .o2r mod pack, legacy .arlang publication or PDF manual")
	}
	root, err := app.openROMRoot(location)
	if err != nil {
		return nil, err
	}
	defer root.Close()
	info, err := root.Lstat(path)
	if err != nil || !info.Mode().IsRegular() {
		return nil, fmt.Errorf("the selected mod is missing or unreadable")
	}
	return root.Open(path)
}

func (app *application) serveMods(response http.ResponseWriter, request *http.Request, endpoint string) {
	root := app.options.ProjectRoot
	switch {
	case strings.HasPrefix(endpoint, "archive/") && (request.Method == http.MethodGet || request.Method == http.MethodHead):
		app.serveLocalModArchive(response, request, strings.TrimPrefix(endpoint, "archive/"))
		return
	case strings.HasPrefix(endpoint, "manual-page/") && request.Method == http.MethodGet:
		app.serveModManualPage(response, request, strings.TrimPrefix(endpoint, "manual-page/"))
		return
	case endpoint == "import-draft" && request.Method == http.MethodPost:
		app.serveWorkshopImport(response, request)
		return
	case endpoint == "language-preview" && request.Method == http.MethodGet:
		app.serveLanguagePreview(response, request)
		return
	case endpoint == "migration" && (request.Method == http.MethodGet || request.Method == http.MethodPost):
		app.serveLegacyModMigration(response, request)
		return
	case endpoint == "browse" && request.Method == http.MethodGet:
		app.browseLocalFiles(response, request, "mod", validModImportName)
		return
	case strings.HasPrefix(endpoint, "audio/") && request.Method == http.MethodGet:
		name := strings.TrimPrefix(endpoint, "audio/")
		if modAudioPreviewName.MatchString(name) {
			app.serveModPreview(response, request, name, "audio/ogg")
		} else {
			http.NotFound(response, request)
		}
		return
	case strings.HasPrefix(endpoint, "image/") && request.Method == http.MethodGet:
		name := strings.TrimPrefix(endpoint, "image/")
		if modTitlePreviewName.MatchString(name) {
			app.serveModPreview(response, request, name, "image/png")
		} else {
			http.NotFound(response, request)
		}
		return
	case (endpoint == "export" || endpoint == "export-draft") && request.Method == http.MethodGet:
		if endpoint == "export-draft" {
			root = musicDraftRoot(root)
		}
		app.assetMu.Lock()
		tracks, err := modExportTracks(root)
		app.assetMu.Unlock()
		if err != nil {
			writeJSONError(response, http.StatusInternalServerError, err.Error())
		} else {
			writeJSON(response, http.StatusOK, tracks)
		}
		return
	case strings.HasPrefix(endpoint, "download/") && (request.Method == http.MethodGet || request.Method == http.MethodHead):
		app.serveModDownload(response, request, strings.TrimPrefix(endpoint, "download/"))
		return
	case (endpoint == "export" || endpoint == "export-draft") && request.Method == http.MethodPost:
		if endpoint == "export-draft" {
			root = musicDraftRoot(root)
		}
		app.serveModExport(response, request, root)
		return
	case endpoint == "validate-title" && request.Method == http.MethodPost:
		request.Body = http.MaxBytesReader(response, request.Body, maxTitlePNGBytes+1)
		if err := writeModTitle(io.Discard, request.Body, true); err != nil {
			writeJSONError(response, http.StatusBadRequest, err.Error())
		} else {
			writeJSON(response, http.StatusOK, map[string]bool{"valid": true})
		}
		return
	case endpoint == "validate-manual" && request.Method == http.MethodPost:
		request.Body = http.MaxBytesReader(response, request.Body, maxManualBytes+1)
		data, err := readManualPDF(request.Body)
		if err != nil {
			writeJSONError(response, http.StatusBadRequest, err.Error())
			return
		}
		verdict := judgeManualAlbum(data)
		writeJSON(response, http.StatusOK, map[string]any{"valid": verdict.OK, "pages": verdict.Pages})
		return
	case endpoint == "export-title" && request.Method == http.MethodPost:
		app.serveFileModExport(response, request, titleModType)
		return
	case endpoint == "export-manual" && request.Method == http.MethodPost:
		app.serveFileModExport(response, request, manualModType)
		return
	}
	var source io.Reader
	var format string
	var selection browserFolder
	var change modChange
	switch {
	case endpoint == "catalog" && request.Method == http.MethodGet:
	case endpoint == "install-browsed" && request.Method == http.MethodPost:
		request.Body = http.MaxBytesReader(response, request.Body, maxModMetadata)
		if err := json.NewDecoder(request.Body).Decode(&selection); err != nil {
			writeJSONError(response, http.StatusBadRequest, "invalid mod selection")
			return
		}
		file, err := app.openBrowsedMod(selection.Location, selection.Path)
		if err != nil {
			writeJSONError(response, http.StatusBadRequest, err.Error())
			return
		}
		defer file.Close()
		source = file
		format = strings.ToLower(filepath.Ext(selection.Path))
	case endpoint == "install" && request.Method == http.MethodPost:
		request.Body = http.MaxBytesReader(response, request.Body, maxModBytes)
		source = request.Body
		format = request.URL.Query().Get("format")
		switch kind := request.URL.Query().Get("type"); kind {
		case languageModType, manualModType:
			change.InstallType = kind
		}
	case endpoint == "activation" && request.Method == http.MethodPost:
		request.Body = http.MaxBytesReader(response, request.Body, maxModMetadata)
		if err := json.NewDecoder(request.Body).Decode(&change); err != nil {
			writeJSONError(response, http.StatusBadRequest, "invalid mod action")
			return
		}
	default:
		http.NotFound(response, request)
		return
	}
	var installedLegacyManual bool
	if source != nil && (format == ".arlang" || format == ".pdf") {
		converted, fallback, err := app.convertModInput(source, format)
		if err != nil {
			writeJSONError(response, http.StatusBadRequest, err.Error())
			return
		}
		source, installedLegacyManual = converted, fallback
	}
	catalog, err := updateModCatalog(root, app.options.ModDirectories, source, &change)
	if err != nil {
		writeJSONError(response, http.StatusBadRequest, err.Error())
		return
	}
	if installedLegacyManual {
		catalog.InstalledType = manualModType
		catalog.InstalledLegacyManual = true
	}
	if selection.Location != "" {
		app.rememberBrowserFolder("mod", selection.Location, selection.Path)
	}
	writeJSON(response, http.StatusOK, catalog.view(root))
}

type modCatalogView struct {
	modCatalog
	Languages     []localizationCatalogEntry `json:"languages"`
	LanguageError string                     `json:"languageError,omitempty"`
}

func (catalog modCatalog) view(root string) modCatalogView {
	if info, err := os.Lstat(manualPath(root)); err == nil && info.Mode().IsRegular() {
		catalog.LegacyManual = true
	}
	for i := range catalog.Packs {
		pack := &catalog.Packs[i]
		pack.setPreviews()
		pack.ArchiveURL = localModArchiveURL(pack)
		pack.UninstallRevision = localModUninstallRevision(pack)
	}
	catalog.readLegacyLanguages(languagePacksRoot(filepath.Join(root, "game-assets")))
	reply := modCatalogView{modCatalog: catalog, Languages: catalog.installedLanguageCatalog()}
	if regularFileExists(nativeUSModPath(root)) {
		reply.Languages = append([]localizationCatalogEntry{{
			ID: "native-us", Key: "native-us", Name: "Native US text", Locale: "en-US", Installed: true,
			Enabled: true, Required: true, ReadOnly: true, Archive: true,
		}}, reply.Languages...)
	}
	if catalog.legacyLanguageError != nil {
		reply.LanguageError = catalog.legacyLanguageError.Error()
	}
	return reply
}

// The mod picker accepts legacy inputs. Formats that can be represented as
// packs take the same install path as an uploaded O2R.
func (app *application) convertModInput(source io.Reader, format string) (io.Reader, bool, error) {
	var output bytes.Buffer
	if format == ".arlang" {
		data, err := io.ReadAll(io.LimitReader(source, lk.MaxAuthorArchiveBytes+1))
		if err != nil {
			return nil, false, err
		}
		if err := lk.ConvertLegacyLanguageMod(&output, bytes.NewReader(data), int64(len(data))); err != nil {
			return nil, false, err
		}
		return &output, false, nil
	}
	data, err := readManualPDF(source)
	if err != nil {
		return nil, false, err
	}
	if judgeManualAlbum(data).OK {
		metadata := modMetadata{Name: "Instruction manual", Version: "1.0.0", Author: "Imported PDF"}
		if err := exportManualMod(&output, metadata, "", bytes.NewReader(data)); err != nil {
			return nil, false, err
		}
		return &output, false, nil
	}
	if err := app.installLegacyPDF(data); err != nil {
		return nil, false, err
	}
	return nil, true, nil
}

func (app *application) installLegacyPDF(data []byte) error {
	app.assetMu.Lock()
	defer app.assetMu.Unlock()
	root := app.options.ProjectRoot
	unlock, err := desktop.LockGameData(root)
	if err != nil {
		return err
	}
	defer unlock()
	current, absent, err := readBoundedRegularFile(manualPath(root), maxManualBytes)
	if err != nil {
		return err
	}
	if !absent {
		if bytes.Equal(current, data) {
			return nil
		}
		return fmt.Errorf("a fallback PDF is already installed. Remove it from Manage Mods before importing another")
	}
	staged, err := stageBytes(root, manualRelativePath, data)
	if err != nil {
		return err
	}
	defer os.Remove(staged.Temporary)
	return replaceStagedFile(staged)
}

func localModArchiveURL(pack *installedMod) string {
	if pack.Error == "" && !pack.External && !pack.Bundled && pack.path != "" && localModFilename(pack.Filename) {
		return "mods/archive/" + url.PathEscape(pack.Filename)
	}
	return ""
}

func localModFilename(name string) bool {
	return filepath.IsLocal(name) && !strings.ContainsAny(name, "/\\") &&
		!strings.HasPrefix(name, ".") && validBrowsedModName(name)
}

func (app *application) serveLocalModArchive(w http.ResponseWriter, r *http.Request, name string) {
	directory := filepath.Join(app.options.ProjectRoot, "game-assets", "mods")
	info, err := os.Lstat(directory)
	if !localModFilename(name) || err != nil || !info.IsDir() {
		http.NotFound(w, r)
		return
	}
	file, err := openModFile(filepath.Join(directory, name))
	if err != nil {
		http.NotFound(w, r)
		return
	}
	defer file.Close()
	serveModArchiveFile(w, r, file, name)
}

func (app *application) serveLegacyModMigration(w http.ResponseWriter, r *http.Request) {
	app.assetMu.Lock()
	defer app.assetMu.Unlock()
	app.mu.Lock()
	busy := app.playing || app.state == "building"
	install := app.install
	app.mu.Unlock()
	rebuild := install.CanLaunch && (install.Build.State == "rebuild" ||
		install.Build.State == "unknown" && install.Build.BuilderVersion != "")
	if r.Method == http.MethodGet {
		summary := r.URL.Query().Get("summary") == "true"
		review, err := reviewLegacyMods(app.options.ProjectRoot, summary)
		if err == nil && !summary &&
			slices.ContainsFunc(review.Items, func(item legacyModItem) bool { return item.Reason == "" }) &&
			hasModSources(app.options.ProjectRoot, app.options.ModDirectories) {
			err = app.reviewMigrationSelections(&review)
		}
		if err != nil {
			writeJSONError(w, http.StatusBadRequest, err.Error())
			return
		}
		review.Rebuild = rebuild
		writeJSON(w, http.StatusOK, review)
		return
	}
	if busy || rebuild {
		writeJSONError(w, http.StatusConflict, "finish rebuilding and close the game before converting legacy customizations")
		return
	}
	var selection struct {
		Revision string `json:"revision"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, maxModMetadata)).Decode(&selection); err != nil {
		writeJSONError(w, http.StatusBadRequest, "invalid migration review")
		return
	}
	review, err := applyLegacyMods(app.options.ProjectRoot, app.options.ModDirectories, selection.Revision)
	switch {
	case errors.Is(err, errLegacyReviewChanged):
		writeJSONError(w, http.StatusConflict, err.Error())
	case err != nil:
		writeJSONError(w, http.StatusBadRequest, err.Error())
	default:
		writeJSON(w, http.StatusOK, review)
	}
}

func (app *application) reviewMigrationSelections(review *legacyModReview) error {
	store, unlock, err := openModStore(app.options.ProjectRoot, app.options.ModDirectories)
	if err != nil {
		return err
	}
	defer unlock()
	defer store.saveIndex()
	catalog, err := store.scan()
	if err != nil {
		return err
	}
	if _, err := store.render(&catalog); err != nil {
		return err
	}
	review.existingSelections(catalog)
	return nil
}

func (pack *installedMod) setPreviews() {
	if pack.Error != "" {
		return
	}
	base := "mods/audio/"
	switch pack.Type {
	case titleModType:
		base = "mods/image/"
	case manualModType:
		base = "mods/manual-page/"
		pack.ManualPages = pack.manualBook.Pages
	}
	pack.Previews = make([]string, len(pack.assetPaths()))
	for i := range pack.Previews {
		pack.Previews[i] = base + pack.cacheFilename(pack.digest, i)
	}
}

func (app *application) openModPreview(name string) (*os.File, os.FileInfo, error) {
	cache, err := os.OpenRoot(filepath.Join(app.options.ProjectRoot, "game-assets", "mods-cache"))
	if err != nil {
		return nil, nil, err
	}
	defer cache.Close()
	info, err := cache.Lstat(name)
	if err != nil || !info.Mode().IsRegular() {
		return nil, nil, fmt.Errorf("preview is unavailable")
	}
	file, err := cache.Open(name)
	if err != nil {
		return nil, nil, err
	}
	actual, err := file.Stat()
	if err != nil || !os.SameFile(info, actual) {
		_ = file.Close()
		return nil, nil, fmt.Errorf("preview changed while opening")
	}
	return file, actual, nil
}

func (app *application) serveModPreview(response http.ResponseWriter, request *http.Request, name, contentType string) {
	file, info, err := app.openModPreview(name)
	if err != nil {
		http.NotFound(response, request)
		return
	}
	defer file.Close()
	response.Header().Set("Content-Type", contentType)
	response.Header().Set("X-Content-Type-Options", "nosniff")
	http.ServeContent(response, request, name, info.ModTime(), file)
}

func (app *application) serveModExport(response http.ResponseWriter, request *http.Request, root string) {
	var selection modExportRequest
	request.Body = http.MaxBytesReader(response, request.Body, maxModMetadata)
	if err := json.NewDecoder(request.Body).Decode(&selection); err != nil {
		writeJSONError(response, http.StatusBadRequest, "invalid music pack export")
		return
	}
	selection.ID = ""
	app.prepareModDownload(response, request.URL.Query().Get("install") == "true", func(output io.Writer) error {
		app.assetMu.Lock()
		defer app.assetMu.Unlock()
		return exportMusicMod(root, output, selection)
	})
}

func (app *application) serveFileModExport(response http.ResponseWriter, request *http.Request, packType string) {
	field, maximum, export := "artwork", int64(maxTitlePNGBytes), exportTitleMod
	readError, missing := "could not read title artwork", "select a PNG for the title pack"
	if packType == manualModType {
		field, maximum, export = "manual", maxManualBytes, exportManualMod
		readError, missing = "could not read the selected manual", "select a PDF for the manual pack"
	}
	request.Body = http.MaxBytesReader(response, request.Body, maxModBytes+maxModMetadata)
	if err := request.ParseMultipartForm(maxModMetadata); err != nil {
		writeJSONError(response, http.StatusBadRequest, readError)
		return
	}
	defer request.MultipartForm.RemoveAll()
	file, header, err := request.FormFile(field)
	if err != nil {
		writeJSONError(response, http.StatusBadRequest, missing)
		return
	}
	defer file.Close()
	if header.Size > maximum {
		writeJSONError(response, http.StatusBadRequest, "the selected asset exceeds its size limit")
		return
	}
	var extras []modExtra
	total := header.Size
	for _, name := range modExtraNames {
		if files := request.MultipartForm.File["attachment:"+name]; len(files) > 0 {
			header := files[0]
			total += header.Size
			if len(files) != 1 || header.Size > maxTrackBytes || total > maxModBytes-2*maxModMetadata {
				writeJSONError(response, http.StatusBadRequest, "invalid or oversized pack attachment")
				return
			}
			extras = append(extras, modExtra{name: name, open: func() (io.ReadCloser, error) { return header.Open() }})
		}
	}
	metadata := modMetadata{
		Name: request.FormValue("name"), Version: request.FormValue("version"),
		Author: request.FormValue("author"), License: request.FormValue("license"), Description: request.FormValue("description"),
	}
	app.prepareModDownload(response, request.URL.Query().Get("install") == "true", func(output io.Writer) error {
		return export(output, metadata, "", file, extras...)
	})
}

func (app *application) prepareModDownload(response http.ResponseWriter, install bool, write func(io.Writer) error) {
	file, err := os.CreateTemp("", "actraiser-mod-export-*")
	if err != nil {
		writeJSONError(response, http.StatusInternalServerError, err.Error())
		return
	}
	download := &modDownload{file: file}
	published := false
	defer func() {
		if !published {
			download.close()
		}
	}()
	var pack modArchive
	err = write(file)
	if err == nil {
		var info os.FileInfo
		info, err = file.Stat()
		if err == nil {
			pack, err = readModArchive(file, info.Size())
		}
	}
	if err != nil {
		writeJSONError(response, http.StatusBadRequest, err.Error())
		return
	}
	if install {
		var catalog modCatalog
		if _, err = file.Seek(0, io.SeekStart); err == nil {
			catalog, err = updateModCatalog(app.options.ProjectRoot, app.options.ModDirectories, file, &modChange{InstallType: pack.Type})
		}
		if err != nil {
			writeJSONError(response, http.StatusBadRequest, err.Error())
			return
		}
		writeJSON(response, http.StatusOK, catalog.view(app.options.ProjectRoot))
		return
	}
	download.name = pack.ID + ".o2r"
	download.token, err = randomToken()
	if err != nil {
		writeJSONError(response, http.StatusInternalServerError, err.Error())
		return
	}
	app.replaceModDownload(download)
	published = true
	writeJSON(response, http.StatusOK, map[string]string{
		"url": "mods/download/" + download.token, "name": download.name,
	})
}

func (app *application) replaceModDownload(download *modDownload) {
	app.modExportMu.Lock()
	old := app.modExport
	app.modExport = download
	app.modExportMu.Unlock()
	old.close()
}

func (app *application) serveModDownload(response http.ResponseWriter, request *http.Request, token string) {
	app.modExportMu.Lock()
	download := app.modExport
	if download == nil || download.token != token {
		app.modExportMu.Unlock()
		http.NotFound(response, request)
		return
	}
	file, err := os.Open(download.file.Name())
	app.modExportMu.Unlock()
	if err != nil {
		writeJSONError(response, http.StatusInternalServerError, err.Error())
		return
	}
	defer func() {
		_ = file.Close()
		app.modExportMu.Lock()
		defer app.modExportMu.Unlock()
		if app.modExport != download {
			_ = os.Remove(download.file.Name())
		}
	}()
	serveModArchiveFile(response, request, file, download.name)
}

func serveModArchiveFile(w http.ResponseWriter, r *http.Request, file *os.File, name string) {
	info, err := file.Stat()
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}
	w.Header().Set("Content-Disposition", mime.FormatMediaType("attachment", map[string]string{"filename": name}))
	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("Cache-Control", "private, no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	http.ServeContent(w, r, name, info.ModTime(), file)
}
