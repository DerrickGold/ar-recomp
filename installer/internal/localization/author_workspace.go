package localization

import (
	"fmt"
	"slices"
	"strings"
	"unicode/utf8"
)

type TranslationStatus string

const (
	TranslationNotStarted TranslationStatus = "not_started"
	TranslationWIP        TranslationStatus = "wip"
	TranslationDone       TranslationStatus = "done"
)

func validTranslationStatus(status TranslationStatus) bool {
	return status == TranslationNotStarted || status == TranslationWIP || status == TranslationDone
}

type progressRow struct{ start, end int }
type authorProgress struct {
	text     string
	statuses map[string]TranslationStatus
	rows     map[string]progressRow
}

func parseAuthorProgress(text string, ids map[string]int) (authorProgress, error) {
	p := authorProgress{text: text, statuses: make(map[string]TranslationStatus), rows: make(map[string]progressRow)}
	if len(text) > MaxAuthorScriptBytes || !utf8.ValidString(text) || strings.IndexByte(text, 0) >= 0 {
		return authorProgress{}, fmt.Errorf("progress is oversized or not valid UTF-8 text")
	}
	err := authorLines(text, func(line string, number, start, next int) error {
		if line == "" || strings.HasPrefix(line, "#") {
			return nil
		}
		id, value, found := strings.Cut(line, "\t")
		fail := func(message string) error { return authorError("translation-progress.tsv", number, "%s", message) }
		if !found || !authorIdentifier(id) || !validTranslationStatus(TranslationStatus(value)) {
			return fail("expected semantic-id<TAB>not_started|wip|done")
		}
		if _, ok := ids[id]; !ok {
			return fail("unknown progress message " + id)
		}
		if _, exists := p.statuses[id]; exists {
			return fail("duplicate progress message " + id)
		}
		p.statuses[id] = TranslationStatus(value)
		p.rows[id] = progressRow{start + len(id) + 1, start + len(line)}
		return nil
	})
	if err != nil {
		return authorProgress{}, err
	}
	return p, nil
}

func (p authorProgress) withStatus(id string, status TranslationStatus) string {
	if row, found := p.rows[id]; found {
		return p.text[:row.start] + string(status) + p.text[row.end:]
	}
	text := p.text
	if text == "" {
		text = "# format=actraiser-language-progress version=1\n"
	}
	if !strings.HasSuffix(text, "\n") && !strings.HasSuffix(text, "\r") {
		text += "\n"
	}
	return text + id + "\t" + string(status) + "\n"
}

// AuthorWorkspace is an immutable, renderer/filesystem/GUI-independent editor
// transaction. Invalid candidate edits return an error and leave this snapshot
// unchanged. Disk writes and package manifest/font validation are separate.
type AuthorWorkspace struct {
	profile, coverage string
	scripts           []*AuthorScript
	messageScript     map[string]int
	progress          authorProgress
	references        map[string]AuthorReference
	tree              map[string][]AuthorTreeEntry
	stats             AuthorValidationStats
}

// NewAuthorWorkspace opens already-read script/progress files. It never scans
// directories, invokes a decoder, opens a font, or writes anything to disk.
func NewAuthorWorkspace(profile, coverage string, sources map[string]string, progress string) (*AuthorWorkspace, error) {
	return NewAuthorWorkspaceVersion(profile, coverage, sources, progress, 1)
}

func NewAuthorWorkspaceVersion(profile, coverage string, sources map[string]string, progress string, version int) (*AuthorWorkspace, error) {
	if len(sources) == 0 || len(sources) > 64 {
		return nil, fmt.Errorf("workspace requires 1-64 script sources")
	}
	paths := make([]string, 0, len(sources))
	for path := range sources {
		paths = append(paths, path)
	}
	slices.Sort(paths)
	scripts := make([]*AuthorScript, 0, len(paths))
	for _, path := range paths {
		script, err := ParseAuthorScriptVersion(sources[path], path, version)
		if err != nil {
			return nil, err
		}
		scripts = append(scripts, script)
	}
	return newAuthorWorkspace(profile, coverage, scripts, progress)
}

func newAuthorWorkspace(profile, coverage string, scripts []*AuthorScript, progress string) (*AuthorWorkspace, error) {
	stats, err := ValidateAuthorScripts(profile, coverage, scripts...)
	if err != nil {
		return nil, err
	}
	ids := make(map[string]int, stats.MessageCount)
	for i, script := range scripts {
		for _, message := range script.messages {
			ids[message.ID] = i
		}
	}
	parsedProgress, err := parseAuthorProgress(progress, ids)
	if err != nil {
		return nil, err
	}
	refs, err := AuthorReferences(profile)
	if err != nil {
		return nil, err
	}
	w := &AuthorWorkspace{profile: profile, coverage: coverage, scripts: scripts, messageScript: ids,
		progress: parsedProgress, references: make(map[string]AuthorReference, len(refs)), stats: stats}
	if err := validateAuthorPresentationBudgets(w); err != nil {
		return nil, err
	}
	if err := validateAuthorTreatments(w); err != nil {
		return nil, err
	}
	for _, ref := range refs {
		w.references[ref.ID] = ref
	}
	w.buildTree(refs)
	return w, nil
}

func (w *AuthorWorkspace) Sources() map[string]string {
	result := make(map[string]string, len(w.scripts))
	for _, script := range w.scripts {
		result[script.path] = script.text
	}
	return result
}

func (w *AuthorWorkspace) ProgressText() string         { return w.progress.text }
func (w *AuthorWorkspace) Stats() AuthorValidationStats { return w.stats }

// AuthorMessageView supplies only the requested message, its editable raw body,
// and route-specific insertions. No browser needs the entire prose corpus.
type AuthorMessageView struct {
	Reference AuthorReference   `json:"reference"`
	Path      string            `json:"path"`
	Body      string            `json:"body"`
	Status    TranslationStatus `json:"status"`
	Present   bool              `json:"present"`
}

func (w *AuthorWorkspace) Message(id string) (AuthorMessageView, bool) {
	ref, ok := w.references[id]
	if !ok {
		return AuthorMessageView{}, false
	}
	ref.Anchors = append([]string{}, ref.Anchors...)
	ref.Placeholders = append([]AuthorPlaceholder{}, ref.Placeholders...)
	view := AuthorMessageView{Reference: ref, Status: TranslationNotStarted}
	if i, found := w.messageScript[id]; found {
		view.Present = true
		view.Path = w.scripts[i].path
		view.Body, _ = w.scripts[i].Body(id)
		if status, present := w.progress.statuses[id]; present {
			view.Status = status
		}
	}
	return view, true
}

// EditMessage validates the candidate body AND all alias dependents before
// accepting content and progress together. A header injection cannot turn a
// single-message edit into an edit of another message.
func (w *AuthorWorkspace) EditMessage(id, body string, status TranslationStatus) (*AuthorWorkspace, error) {
	if !validTranslationStatus(status) {
		return nil, fmt.Errorf("invalid translation status %q", status)
	}
	i, found := w.messageScript[id]
	if !found {
		return nil, fmt.Errorf("message %q is not included in this workspace", id)
	}
	script, err := w.scripts[i].ReplaceBody(id, body)
	if err != nil {
		return nil, err
	}
	scripts := append([]*AuthorScript{}, w.scripts...)
	scripts[i] = script
	return newAuthorWorkspace(w.profile, w.coverage, scripts, w.progress.withStatus(id, status))
}

// AddMessage fills a missing translation in an existing declared script. It
// cannot change another message or create an undeclared file. Required controls
// are still checked against the route contract, including aliases in other files.
func (w *AuthorWorkspace) AddMessage(id, path, body string, status TranslationStatus) (*AuthorWorkspace, error) {
	if len(body) > MaxAuthorScriptBytes {
		return nil, fmt.Errorf("message body exceeds script size limit")
	}
	if !validTranslationStatus(status) {
		return nil, fmt.Errorf("invalid translation status %q", status)
	}
	if _, ok := w.references[id]; !ok {
		return nil, fmt.Errorf("unknown semantic message %q", id)
	}
	if _, ok := w.messageScript[id]; ok {
		return nil, fmt.Errorf("message %q already exists", id)
	}
	for i, script := range w.scripts {
		if script.path != path {
			continue
		}
		addition := "\n:: " + id + "\n" + body
		if len(addition) > MaxAuthorScriptBytes-len(script.text) {
			return nil, fmt.Errorf("script exceeds size limit")
		}
		next, err := ParseAuthorScriptVersion(script.text+addition, path, script.version)
		if err != nil {
			return nil, err
		}
		if len(next.messages) != len(script.messages)+1 || next.messages[len(next.messages)-1].ID != id {
			return nil, fmt.Errorf("message body cannot introduce additional headers")
		}
		scripts := append([]*AuthorScript{}, w.scripts...)
		scripts[i] = next
		return newAuthorWorkspace(w.profile, w.coverage, scripts, w.progress.withStatus(id, status))
	}
	return nil, fmt.Errorf("script %q is not declared in this workspace", path)
}

// AuthorTreeEntry is one immediate child, with aggregate progress counts.
// A node can be both a message and a group; IDs are not forced into leaf-only
// assumptions. Children() never serializes message bodies.
type AuthorTreeEntry struct {
	ID          string `json:"id"`
	Label       string `json:"label"`
	LabelKey    string `json:"label_key,omitempty"`
	Shared      bool   `json:"shared,omitempty"`
	IsMessage   bool   `json:"is_message"`
	HasChildren bool   `json:"has_children"`
	Total       int    `json:"total"`
	Present     int    `json:"present"`
	NotStarted  int    `json:"not_started"`
	WIP         int    `json:"wip"`
	Done        int    `json:"done"`
}

func (w *AuthorWorkspace) Children(parent string) []AuthorTreeEntry {
	return append([]AuthorTreeEntry{}, w.tree[parent]...)
}

func (w *AuthorWorkspace) buildTree(refs []AuthorReference) {
	nodes := make(map[string]*AuthorTreeEntry)
	parents := make(map[string]string)
	for _, ref := range refs {
		_, present := w.messageScript[ref.ID]
		status := w.progress.statuses[ref.ID]
		parent := ""
		for _, part := range strings.Split(ref.ID, ".") {
			id := part
			if parent != "" {
				id = parent + "." + part
				nodes[parent].HasChildren = true
			}
			entry := nodes[id]
			if entry == nil {
				entry = &AuthorTreeEntry{ID: id, Label: part}
				nodes[id], parents[id] = entry, parent
			}
			entry.Total++
			if present {
				entry.Present++
			}
			switch status {
			case TranslationDone:
				entry.Done++
			case TranslationWIP:
				entry.WIP++
			default:
				entry.NotStarted++
			}
			if id == ref.ID {
				entry.IsMessage = true
			}
			parent = id
		}
	}
	w.tree = make(map[string][]AuthorTreeEntry)
	for id, entry := range nodes {
		parent := parents[id]
		w.tree[parent] = append(w.tree[parent], *entry)
	}
	for _, children := range w.tree {
		slices.SortFunc(children, func(a, b AuthorTreeEntry) int { return strings.Compare(a.ID, b.ID) })
	}
}
