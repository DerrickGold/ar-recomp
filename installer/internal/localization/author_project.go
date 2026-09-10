package localization

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"slices"
	"strings"
	"sync"
	"unicode/utf8"
)

// AuthorProject contains private author metadata separately from the runtime
// pack. Publication is an explicit, validated transformation, never Files().
type AuthorProject struct {
	pack              *AuthorPack
	info              projectInfo
	notices           map[string][]byte
	revisionOnce      sync.Once
	revision          string
	publicationReady  bool
	installationReady bool
	nativeReady       bool
}

func (p *AuthorProject) clone() *AuthorProject {
	next := &AuthorProject{pack: p.pack, info: p.info, notices: p.notices}
	return next
}

type projectInfo struct {
	Version  int               `json:"version"`
	Origin   string            `json:"origin"`
	Baseline map[string]string `json:"baseline,omitempty"`
	Notes    string            `json:"notes,omitempty"`
}

func (p *AuthorProject) Pack() *AuthorPack { return p.pack }
func (p *AuthorProject) Notes() string     { return p.info.Notes }
func (p *AuthorProject) Origin() string    { return p.info.Origin }

func NewSourceProject(pack *AuthorPack) (*AuthorProject, error) {
	if pack == nil {
		return nil, fmt.Errorf("source pack is required")
	}
	return &AuthorProject{pack: pack, info: projectInfo{Version: 1, Origin: "native-source"}, notices: map[string][]byte{}, nativeReady: true}, nil
}

// NewTranslationProject keeps a local source template for editing, but records
// its resolved presentation hashes. Unchanged retail prose cannot be published
// simply by marking it done. Missing translations use native runtime fallback.
func NewTranslationProject(source *AuthorPack, metadata PackMetadata) (*AuthorProject, error) {
	if strings.EqualFold(metadata.ID, "native-us") {
		return nil, fmt.Errorf("native-us is reserved for the local baseline; choose a community package ID")
	}
	if source == nil || source.manifest.metadata.SourceProfile != "us" || metadata.SourceProfile != "us" || metadata.Target != "us-runtime" {
		return nil, fmt.Errorf("translations must start from the US semantic source")
	}
	metadata.Coverage = "partial"
	pack, err := source.WithMetadata(metadata)
	if err != nil {
		return nil, err
	}
	w, err := newAuthorWorkspace("us", "partial", pack.workspace.scripts, "")
	if err != nil {
		return nil, err
	}
	pack, err = pack.withWorkspace(w)
	if err != nil {
		return nil, err
	}
	baseline := map[string]string{}
	for id := range w.messageScript {
		ops, err := resolvedAuthorOperations(w, id)
		if err != nil {
			return nil, err
		}
		baseline[id] = presentationDigest(ops)
	}
	return &AuthorProject{pack: pack, info: projectInfo{Version: 1, Origin: "translation", Baseline: baseline}, notices: map[string][]byte{}}, nil
}

func (p *AuthorProject) WithMetadata(metadata PackMetadata) (*AuthorProject, error) {
	pack, err := p.pack.WithMetadata(metadata)
	if err != nil {
		return nil, err
	}
	next := p.clone()
	next.pack = pack
	return next, nil
}
func (p *AuthorProject) WithNotes(notes string) (*AuthorProject, error) {
	if len(notes) > 1<<20 || !utf8.ValidString(notes) || strings.ContainsRune(notes, 0) {
		return nil, fmt.Errorf("notes must be UTF-8, at most 1 MiB")
	}
	next := p.clone()
	next.info.Notes = notes
	return next, nil
}
func (p *AuthorProject) WithNotice(name, text string) (*AuthorProject, error) {
	path := "notices/" + name
	if !PortablePackPath(path) || strings.Contains(name, "/") || !(strings.HasSuffix(name, ".txt") || strings.HasSuffix(name, ".md")) || len(text) > 1<<20 || !utf8.ValidString(text) || strings.ContainsRune(text, 0) {
		return nil, fmt.Errorf("notice must be a portable .txt/.md filename and UTF-8 text up to 1 MiB")
	}
	notices := make(map[string][]byte, len(p.notices)+1)
	for key, data := range p.notices {
		if strings.EqualFold(key, path) && key != path {
			return nil, fmt.Errorf("notice filename conflicts")
		}
		notices[key] = data
	}
	if text == "" {
		delete(notices, path)
	} else {
		notices[path] = []byte(text)
	}
	if len(notices) > 32 {
		return nil, fmt.Errorf("at most 32 notices")
	}
	next := p.clone()
	next.notices = notices
	return next, nil
}
func (p *AuthorProject) Notices() map[string]string {
	out := map[string]string{}
	for path, data := range p.notices {
		out[path] = string(data)
	}
	return out
}
func (p *AuthorProject) EditMessage(id, body string, status TranslationStatus) (*AuthorProject, error) {
	if p.info.Origin == "native-source" {
		return nil, fmt.Errorf("native sources are read-only; create a translation to edit")
	}
	var pack *AuthorPack
	var err error
	if _, exists := p.pack.workspace.messageScript[id]; exists {
		pack, err = p.pack.EditMessage(id, body, status)
	} else {
		pack, err = p.pack.AddMessage(id, p.pack.manifest.sources[0], body, status)
	}
	if err != nil {
		return nil, err
	}
	next := p.clone()
	next.pack = pack
	return next, nil
}

func resolvedAuthorOperations(w *AuthorWorkspace, id string) ([]AuthorOperation, error) {
	seen := map[string]bool{}
	for {
		if seen[id] {
			return nil, fmt.Errorf("cyclic alias %s", id)
		}
		seen[id] = true
		i, ok := w.messageScript[id]
		if !ok {
			return nil, fmt.Errorf("missing alias target %s", id)
		}
		s := w.scripts[i]
		m := s.messages[s.byID[id]]
		if m.Alias == "" {
			return authorPresentation(m.Operations), nil
		}
		id = m.Alias
	}
}
func presentationDigest(ops []AuthorOperation) string {
	data, _ := json.Marshal(authorPresentation(ops))
	return fmt.Sprintf("%x", sha256.Sum256(data))
}

type PublicationOptions struct {
	ConfirmRights bool
	IncludeWIP    bool
}
type PublicationReport struct {
	Coverage        AuthorCoverageReport `json:"coverage"`
	Included        int                  `json:"included"`
	WIP             int                  `json:"wip"`
	UnchangedSource int                  `json:"unchangedSource"`
	Fallback        int                  `json:"fallback"`
}

type InstallationReport struct {
	Coverage AuthorCoverageReport `json:"coverage"`
	Messages int                  `json:"messages"`
	Fallback int                  `json:"fallback"`
}

// Installation is local use, not a publication. Preserve every supplied
// message (including WIP/unreviewed text), font and notice. The installer omits
// author metadata/progress; export still requires its separate publication gate.
func (p *AuthorProject) Installation() (*AuthorProject, InstallationReport, error) {
	var report InstallationReport
	if p.info.Origin == "native-source" || strings.EqualFold(p.pack.manifest.metadata.ID, "native-us") {
		return nil, report, fmt.Errorf("native sources are references; create a translation to install a separate language pack")
	}
	if p.pack.manifest.metadata.SourceProfile != "us" || p.pack.manifest.metadata.Target != "us-runtime" {
		return nil, report, fmt.Errorf("reference-only regional sources cannot be installed as game translations")
	}
	for _, font := range append([]string{p.pack.manifest.fonts.Primary}, p.pack.manifest.fonts.Fallback...) {
		if strings.HasPrefix(font, "builtin:") && font != "builtin:actraiser-sans" {
			return nil, report, fmt.Errorf("unsupported built-in font dependency: %s", font)
		}
	}
	report.Messages = len(p.pack.workspace.messageScript)
	report.Coverage = p.Coverage()
	refs, _ := AuthorReferences("us")
	for _, ref := range refs {
		if _, present := p.pack.workspace.messageScript[ref.ID]; ref.NativeInProfile && !present {
			report.Fallback++
		}
	}
	next := p.clone()
	next.installationReady = true
	return next, report, nil
}

// Publication strips comments/private metadata, resolves aliases independently
// and selects only reviewed translations. Copyright permission is still the
// author's responsibility; a license string is not treated as proof of rights.
func (p *AuthorProject) Publication(options PublicationOptions) (*AuthorProject, PublicationReport, error) {
	var report PublicationReport
	fail := func(text string) (*AuthorProject, PublicationReport, error) {
		return nil, report, fmt.Errorf("%s", text)
	}
	if !options.ConfirmRights {
		return fail("confirm redistribution rights before publication")
	}
	if p.info.Origin == "native-source" {
		return fail("ROM-derived source packs are local-only; create a translation first")
	}
	if strings.EqualFold(p.pack.manifest.metadata.ID, "native-us") {
		return fail("native-us is reserved for the local baseline")
	}
	if p.pack.manifest.metadata.Target != "us-runtime" {
		return fail("reference-only regional sources cannot be published as game translations")
	}
	if len(p.pack.fonts) > 0 && len(p.notices) == 0 {
		return fail("include redistribution/license notices for local fonts")
	}
	for _, font := range append([]string{p.pack.manifest.fonts.Primary}, p.pack.manifest.fonts.Fallback...) {
		if strings.HasPrefix(font, "builtin:") && font != "builtin:actraiser-sans" {
			return fail("unsupported built-in font dependency: " + font)
		}
	}
	ids := make([]string, 0, len(p.pack.workspace.messageScript))
	for id := range p.pack.workspace.messageScript {
		ids = append(ids, id)
	}
	slices.Sort(ids)
	messages := []AuthorMessage{}
	for _, id := range ids {
		view, _ := p.pack.workspace.Message(id)
		if view.Status == TranslationNotStarted || view.Status == TranslationWIP && !options.IncludeWIP {
			continue
		}
		ops, err := resolvedAuthorOperations(p.pack.workspace, id)
		if err != nil {
			return nil, report, err
		}
		if p.info.Baseline[id] != "" && p.info.Baseline[id] == presentationDigest(ops) {
			report.UnchangedSource++
			continue
		}
		messages = append(messages, AuthorMessage{ID: id, Operations: ops})
		report.Included++
		if view.Status == TranslationWIP {
			report.WIP++
		}
	}
	included := map[string]bool{}
	for _, m := range messages {
		included[m.ID] = true
	}
	report.Coverage = p.coverageFor(included)
	refs, _ := AuthorReferences("us")
	for _, ref := range refs {
		if ref.NativeInProfile && !included[ref.ID] {
			report.Fallback++
		}
	}
	if len(messages) == 0 {
		return fail("no reviewed translations to publish; save an edited message as WIP or Done")
	}
	script, err := EmitAuthorScript(messages, "text/translation.artext")
	if err != nil {
		return nil, report, err
	}
	metadata := p.pack.manifest.Metadata()
	metadata.Description = ""
	metadata.Coverage = "partial"
	if _, err := ValidateAuthorScripts("us", "complete", script); err == nil {
		metadata.Coverage = "complete"
	}
	manifest, err := NewPackManifest(metadata, p.pack.manifest.Fonts(), []string{script.path})
	if err != nil {
		return nil, report, err
	}
	var progress strings.Builder
	for _, m := range messages {
		fmt.Fprintf(&progress, "%s\tdone\n", m.ID)
	}
	w, err := newAuthorWorkspace("us", metadata.Coverage, []*AuthorScript{script}, progress.String())
	if err != nil {
		return nil, report, err
	}
	pack := &AuthorPack{manifest: manifest, workspace: w, fonts: p.pack.fonts}
	return &AuthorProject{pack: pack, info: projectInfo{Version: 1, Origin: "publication"}, notices: p.notices, publicationReady: true}, report, nil
}

// ProjectRevision includes author progress/notes and notices, unlike the
// runtime revision. Use it for optimistic concurrency of save/edit requests.
func (p *AuthorProject) ProjectRevision() string {
	p.revisionOnce.Do(func() {
		h := sha256.New()
		files := map[string][]byte{"progress": []byte(p.pack.workspace.progress.text)}
		files["author"], _ = json.Marshal(p.info)
		for path, data := range p.notices {
			files[path] = data
		}
		fmt.Fprintf(h, "runtime:%016x", p.pack.RuntimeRevision())
		paths := make([]string, 0, len(files))
		for path := range files {
			paths = append(paths, path)
		}
		slices.Sort(paths)
		for _, path := range paths {
			fmt.Fprintf(h, "%d:%s:%d:", len(path), path, len(files[path]))
			h.Write(files[path])
		}
		p.revision = fmt.Sprintf("%x", h.Sum(nil))
	})
	return p.revision
}
func (p *AuthorProject) projectFiles(backup bool) map[string][]byte {
	files := p.pack.Files()
	if backup {
		files["author-project.json"], _ = json.Marshal(p.info)
	} else {
		delete(files, "translation-progress.tsv")
	}
	for path, data := range p.notices {
		files[path] = append([]byte{}, data...)
	}
	return files
}
