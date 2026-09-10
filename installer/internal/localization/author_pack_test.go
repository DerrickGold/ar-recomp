package localization

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"hash/fnv"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"slices"
	"strings"
	"sync"
	"testing"
	"testing/fstest"
	"time"
)

const authorPackManifest = `[pack]
format = actraiser-language-pack
version = 1
id = example.excellent
locale = en-CA
name = Most Excellent English
autonym = English
author = Example Author
license = MIT
direction = auto
target = us-runtime
source_profile = us
fallback = native-us
coverage = partial

[fonts]
primary = builtin:actraiser-sans

[scripts]
source = text/sky.artext
`

func TestAuthorPackManifestEditPreservesIdentityAndNotes(t *testing.T) {
	text := "\ufeff# private note\r\n" + strings.ReplaceAll(authorPackManifest, "\n", "\r\n")
	m, err := ParsePackManifest(text, "pack.ini")
	if err != nil {
		t.Fatal(err)
	}
	metadata := m.Metadata()
	if metadata.ID != "example.excellent" || metadata.Locale != "en-CA" || metadata.Name != "Most Excellent English" {
		t.Fatal(metadata)
	}
	metadata.ID, metadata.Name, metadata.Locale = "example.other", "Other English", "en-US"
	metadata.Description = "A second pack for the same locale."
	next, err := m.WithMetadata(metadata)
	if err != nil {
		t.Fatal(err)
	}
	if m.Text() != text || next.Metadata() != metadata {
		t.Fatal("metadata edit changed the original or lost fields")
	}
	if !strings.HasPrefix(next.Text(), "\ufeff# private note\r\n") || !strings.Contains(next.Text(), "source = text/sky.artext\r\n") {
		t.Fatal("unrelated text was reformatted")
	}
	without, err := next.WithMetadata(m.Metadata())
	if err != nil {
		t.Fatal(err)
	}
	if without.Text() != text {
		t.Fatalf("metadata roundtrip lost original bytes:\n%q\n%q", text, without.Text())
	}
	metadata.Description = "line\n[other]"
	if _, err := m.WithMetadata(metadata); err == nil {
		t.Fatal("metadata newline injection accepted")
	}
	metadata = m.Metadata()
	metadata.Locale = "en_CA"
	if _, err := m.WithMetadata(metadata); err == nil {
		t.Fatal("invalid locale accepted")
	}
	if m.Fonts().Primary != "builtin:actraiser-sans" {
		t.Fatal("font changed")
	}
	sources := m.Sources()
	sources[0] = "poison"
	if m.Sources()[0] != "text/sky.artext" {
		t.Fatal("mutable source slice escaped")
	}
	fonts := PackFonts{"builtin:actraiser-sans", []string{"builtin:other", "fonts/École.ttf"}}
	created, err := NewPackManifest(m.Metadata(), fonts, m.Sources())
	if err != nil {
		t.Fatal(err)
	}
	created2, err := NewPackManifest(created.Metadata(), created.Fonts(), created.Sources())
	if err != nil || created2.Text() != created.Text() {
		t.Fatal("nondeterministic manifest", err)
	}
	for _, bad := range []string{"../font.ttf", "x.ttf\nsource = evil.artext", "builtin:bad id", " font.ttf"} {
		if _, err := NewPackManifest(m.Metadata(), PackFonts{Primary: bad}, m.Sources()); err == nil {
			t.Fatalf("unsafe font emitted: %q", bad)
		}
	}
	// Arbitrary section order and [pack] at EOF must retain all unchanged bytes.
	parts := strings.Split(authorPackManifest, "\n[fonts]")
	reordered := "[fonts]" + parts[1] + "\n" + parts[0]
	m, err = ParsePackManifest(strings.TrimSuffix(reordered, "\n"), "pack.ini")
	if err != nil {
		t.Fatal(err)
	}
	metadata = m.Metadata()
	metadata.Description = "An EOF description."
	next, err = m.WithMetadata(metadata)
	if err != nil || next.Metadata() != metadata {
		t.Fatal("EOF insertion failed", err)
	}
}

func authorManifestVectors() []string {
	cases := []string{authorPackManifest, "\ufeff" + authorPackManifest, strings.ReplaceAll(authorPackManifest, "\n", "\r"), strings.ReplaceAll(authorPackManifest, "\n", "\r\n")}
	for _, key := range append([]string{"format", "version"}, packMetadataKeys...) {
		if key == "description" {
			continue
		}
		for _, line := range strings.Split(authorPackManifest, "\n") {
			if strings.HasPrefix(line, key+" = ") {
				cases = append(cases, strings.Replace(authorPackManifest, line+"\n", "", 1), strings.Replace(authorPackManifest, line, line+"\n"+line, 1))
			}
		}
	}
	for _, section := range []string{"pack", "fonts", "scripts", "unknown"} {
		cases = append(cases, authorPackManifest+"["+section+"]\n")
	}
	for _, value := range []string{"00", "01", "2", "1 ; note"} {
		cases = append(cases, strings.Replace(authorPackManifest, "version = 1", "version = "+value, 1))
	}
	for _, value := range []string{"en-US", "en-CA", "fr-FR", "ja", "zh-Hant-TW", "en-us-x-test", "en_CA", "e", "en-", "en--US", "123", "englishhh", strings.Repeat("e", 33)} {
		cases = append(cases, strings.Replace(authorPackManifest, "locale = en-CA", "locale = "+value, 1))
	}
	for _, field := range []struct {
		key, value string
		limit      int
	}{{"id", "example.excellent", 96}, {"name", "Most Excellent English", 192}, {"autonym", "English", 192}, {"author", "Example Author", 192}, {"license", "MIT", 128}} {
		for _, size := range []int{field.limit, field.limit + 1} {
			cases = append(cases, strings.Replace(authorPackManifest, field.key+" = "+field.value, field.key+" = "+strings.Repeat("a", size), 1))
		}
	}
	for _, pair := range [][2]string{{"direction = auto", "direction = rtl"}, {"direction = auto", "direction = diagonal"}, {"coverage = partial", "coverage = complete"}, {"coverage = partial", "coverage = unknown"}, {"source_profile = us", "source_profile = fr"}, {"fallback = native-us", "fallback = unknown"}} {
		cases = append(cases, strings.Replace(authorPackManifest, pair[0], pair[1], 1))
	}
	for _, profile := range []string{"us", "eu-en", "de", "fr", "jp"} {
		cases = append(cases, strings.Replace(strings.Replace(authorPackManifest, "target = us-runtime", "target = reference-only", 1), "source_profile = us", "source_profile = "+profile, 1))
	}
	for _, value := range []string{"fonts/Font.ttf", "fonts/École.ttf", "font file.ttf", "builtin:other", strings.Repeat("f", 511), strings.Repeat("f", 512), "builtin:", "builtin:bad id", "../font.ttf", "/font.ttf", "fonts/./a.ttf", "fonts/../a.ttf", "fonts//a.ttf", "C:font.ttf", "C:/font.ttf", "fonts/a.ttf:stream", "fonts\\a.ttf", "NUL", "nul.ttf", "COM1.ttf", "LPT³.ttf", "CONIN$.txt", "fonts/end./a.ttf", "fonts/end /a.ttf", "fonts/a?.ttf", "fonts/a|b.ttf", "fonts/\x01.ttf"} {
		cases = append(cases, strings.Replace(authorPackManifest, "primary = builtin:actraiser-sans", "primary = "+value, 1))
	}
	for _, value := range []string{"text/second.artext", "../second.artext", "x/./second.artext", "NUL.artext", "name:stream.artext", "text/not-a-script.txt"} {
		cases = append(cases, strings.Replace(authorPackManifest, "source = text/sky.artext", "source = "+value, 1))
	}
	for _, count := range []int{8, 9} {
		var extra strings.Builder
		for i := 0; i < count; i++ {
			fmt.Fprintf(&extra, "fallback = builtin:font%d\n", i)
		}
		cases = append(cases, strings.Replace(authorPackManifest, "\n[scripts]", "\n"+extra.String()+"\n[scripts]", 1))
	}
	for _, count := range []int{64, 65} {
		var sources strings.Builder
		for i := 0; i < count; i++ {
			fmt.Fprintf(&sources, "source = text/file%d.artext\n", i)
		}
		cases = append(cases, strings.Replace(authorPackManifest, "source = text/sky.artext\n", sources.String(), 1))
	}
	cases = append(cases, strings.Replace(authorPackManifest, "\n[scripts]", "\nfallback = builtin:a\nfallback = builtin:a\n[scripts]", 1), strings.Replace(authorPackManifest, "[fonts]", "description = Some notes\n[fonts]", 1))
	for value := 0; value < 256; value++ {
		cases = append(cases, strings.Replace(authorPackManifest, "name = Most Excellent English", "name = before"+string([]byte{byte(value)})+"after", 1))
	}
	return cases
}

func TestAuthorManifestRuntimeParity(t *testing.T) {
	probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE")
	if probe == "" {
		t.Skip("CTest supplies the C metadata probe")
	}
	for i, text := range authorManifestVectors() {
		t.Run(fmt.Sprintf("case-%d", i), func(t *testing.T) {
			m, goErr := ParsePackManifest(text, "pack.ini")
			path := filepath.Join(t.TempDir(), "pack.ini")
			if err := os.WriteFile(path, []byte(text), 0600); err != nil {
				t.Fatal(err)
			}
			output, cErr := exec.Command(probe, "--metadata", path).CombinedOutput()
			if (goErr == nil) != (cErr == nil) {
				t.Fatalf("Go/C metadata acceptance differs: Go=%v C=%v %s\n%q", goErr, cErr, output, text)
			}
			if goErr != nil {
				return
			}
			var actual struct {
				Metadata PackMetadata
				Fonts    PackFonts
				Revision string
			}
			if err := json.Unmarshal(output, &actual); err != nil {
				t.Fatal(err, string(output))
			}
			expected := m.Metadata()
			expected.Description = "" // Runtime intentionally ignores author description.
			h := fnv.New64a()
			hashPackPart(h, "manifest", "pack.ini", []byte(text))
			if expected != actual.Metadata || !reflect.DeepEqual(m.Fonts(), actual.Fonts) || actual.Revision != fmt.Sprintf("%016x", h.Sum64()) {
				t.Fatalf("metadata/font/revision mismatch: %+v", actual)
			}
		})
	}
}

func authorPackFS() fstest.MapFS {
	manifest := strings.Replace(authorPackManifest, "\n[scripts]", "\nfallback = fonts/Test.ttf\nfallback = builtin:other\n[scripts]", 1) + "source = text/other.artext\n"
	return fstest.MapFS{
		"pack.ini":                 {Data: []byte(manifest)},
		"text/sky.artext":          {Data: []byte(authorConfirm)},
		"text/other.artext":        {Data: []byte("# note\n:: action.hud.act_1\nMost excellent!\n@end\n")},
		"fonts/Test.ttf":           {Data: []byte{0, 1, 2, 3, 4}}, // Format gate only; not a font-readiness claim.
		"translation-progress.tsv": {Data: []byte("# private translator note\nsky.action_mode.confirm\twip\n")},
		"unused-secret.txt":        {Data: []byte("This is not part of the declared snapshot.")},
	}
}

func packSnapshotFS(files map[string][]byte) fstest.MapFS {
	result := fstest.MapFS{}
	for path, data := range files {
		result[path] = &fstest.MapFile{Data: data}
	}
	return result
}

func writeAuthorTestFiles(t *testing.T, root string, files map[string][]byte) {
	t.Helper()
	for path, data := range files {
		if !PortablePackPath(path) {
			t.Fatalf("unsafe test output path %q", path)
		}
		full := filepath.Join(root, filepath.FromSlash(path))
		if err := os.MkdirAll(filepath.Dir(full), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
}

func TestAuthorPackLoadEditExportReopen(t *testing.T) {
	input := authorPackFS()
	p, err := LoadAuthorPack(input)
	if err != nil {
		t.Fatal(err)
	}
	exposedManifest, exposedWorkspace := p.Manifest(), p.Workspace()
	*exposedManifest = PackManifest{}
	*exposedWorkspace = AuthorWorkspace{}
	if p.Manifest().Metadata().ID != "example.excellent" || p.Workspace().Stats().MessageCount != 2 {
		t.Fatal("mutable pack state escaped")
	}
	files := p.Files()
	if _, ok := files["unused-secret.txt"]; ok {
		t.Fatal("undeclared files exported")
	}
	for path, data := range files {
		if !bytes.Equal(data, input[path].Data) {
			t.Fatal("load changed bytes", path)
		}
	}
	input["fonts/Test.ttf"].Data[0] = 99
	files["fonts/Test.ttf"][1] = 99
	if !bytes.Equal(p.Files()["fonts/Test.ttf"], []byte{0, 1, 2, 3, 4}) {
		t.Fatal("mutable font buffer escaped")
	}
	originalRevision := p.RuntimeRevision()
	view, _ := p.Workspace().Message("sky.action_mode.confirm")
	statusOnly, err := p.EditMessage(view.Reference.ID, view.Body, TranslationDone)
	if err != nil {
		t.Fatal(err)
	}
	if statusOnly.RuntimeRevision() != originalRevision {
		t.Fatal("progress changed runtime revision")
	}
	metadata := p.Manifest().Metadata()
	metadata.ID = "example.excellent-2"
	metadata.Name = "Another Excellent English"
	metadata.Locale = "en-CA"
	alt, err := statusOnly.WithMetadata(metadata)
	if err != nil {
		t.Fatal(err)
	}
	if p.Manifest().Metadata() == alt.Manifest().Metadata() || alt.RuntimeRevision() == originalRevision {
		t.Fatal("metadata identity/revision failed")
	}
	alt, err = alt.AddMessage("action.hud.act_2", "text/other.artext", "@alias action.hud.act_1\n", TranslationWIP)
	if err != nil {
		t.Fatal(err)
	}
	for _, invalid := range []struct{ id, path, body string }{{"action.hud.act_2", "text/other.artext", "@empty\n"}, {"unknown", "text/other.artext", "@empty\n"}, {"action.hud.act_3", "undeclared.artext", "@empty\n"}, {"action.hud.act_3", "text/other.artext", "@empty\n:: action.hud.act_4\n@empty\n"}} {
		if next, err := alt.AddMessage(invalid.id, invalid.path, invalid.body, TranslationWIP); err == nil || next != nil {
			t.Fatal("unsafe add accepted", invalid)
		}
	}
	metadata = alt.Manifest().Metadata()
	metadata.Coverage = "complete"
	if next, err := alt.WithMetadata(metadata); err == nil || next != nil {
		t.Fatal("invalid completeness committed")
	}
	if originalRevision != p.RuntimeRevision() {
		t.Fatal("old snapshot changed")
	}
	root := filepath.Join(t.TempDir(), "a pack with spaces")
	writeAuthorTestFiles(t, root, alt.Files())
	reopened, err := OpenAuthorPack(root)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(reopened.Files(), alt.Files()) || reopened.RuntimeRevision() != alt.RuntimeRevision() {
		t.Fatal("disk roundtrip drift")
	}
	probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE")
	if probe != "" {
		assertAuthorPackRuntime(t, probe, root, alt)
	}
	var group sync.WaitGroup
	for i := 0; i < 16; i++ {
		group.Add(1)
		go func() {
			defer group.Done()
			if reopened.RuntimeRevision() != alt.RuntimeRevision() {
				t.Error("concurrent revision drift")
			}
		}()
	}
	group.Wait()
}

func assertAuthorPackRuntime(t *testing.T, probe, root string, p *AuthorPack) {
	t.Helper()
	output, err := exec.Command(probe, "--inspect", filepath.Join(root, "pack.ini")).CombinedOutput()
	if err != nil {
		t.Fatal(err, string(output))
	}
	var actual struct {
		Metadata PackMetadata
		Fonts    PackFonts
		Revision string
		Messages []AuthorMessage
	}
	if err := json.Unmarshal(output, &actual); err != nil {
		t.Fatal(err, string(output))
	}
	metadata := p.Manifest().Metadata()
	metadata.Description = ""
	var messages []AuthorMessage
	for _, script := range p.workspace.scripts {
		messages = append(messages, script.Messages()...)
	}
	if actual.Metadata != metadata || !reflect.DeepEqual(actual.Fonts, p.Manifest().Fonts()) || actual.Revision != fmt.Sprintf("%016x", p.RuntimeRevision()) || !reflect.DeepEqual(messages, actual.Messages) {
		t.Fatal("whole pack Go/C drift", actual.Revision, fmt.Sprintf("%016x", p.RuntimeRevision()))
	}
}

func TestAuthorPackRejectsMissingUnsafeAndConflictingFiles(t *testing.T) {
	for _, path := range []string{"pack.ini", "text/sky.artext", "fonts/Test.ttf"} {
		files := authorPackFS()
		delete(files, path)
		if p, err := LoadAuthorPack(files); err == nil || p != nil || !errors.Is(err, fs.ErrNotExist) {
			t.Fatal("missing file accepted", path, err)
		}
	}
	files := authorPackFS()
	delete(files, "translation-progress.tsv")
	p, err := LoadAuthorPack(files)
	if err != nil {
		t.Fatal("optional progress rejected", err)
	}
	if _, ok := p.Files()["translation-progress.tsv"]; ok {
		t.Fatal("invented absent progress")
	}
	for _, change := range []func(fstest.MapFS){
		func(f fstest.MapFS) { f["fonts/Test.ttf"].Data = nil },
		func(f fstest.MapFS) { f["text/sky.artext"].Mode = fs.ModeDir },
		func(f fstest.MapFS) { f["translation-progress.tsv"].Data = []byte("unknown\tdone\n") },
		func(f fstest.MapFS) {
			f["pack.ini"].Data = append(f["pack.ini"].Data, []byte("source = text/SKY.artext\n")...)
		},
		func(f fstest.MapFS) {
			f["pack.ini"].Data = append(f["pack.ini"].Data, []byte("source = text/sky.artext/child.artext\n")...)
		},
		func(f fstest.MapFS) {
			f["pack.ini"].Data = bytes.Replace(f["pack.ini"].Data, []byte("fonts/Test.ttf"), []byte("text/sky.artext"), 1)
		},
	} {
		f := authorPackFS()
		change(f)
		if p, err := LoadAuthorPack(f); err == nil || p != nil {
			t.Fatal("invalid pack accepted")
		}
	}
	if p, err := loadAuthorPack(authorPackFS(), len(authorPackManifest)); err == nil || p != nil {
		t.Fatal("aggregate limit bypassed")
	}
	if _, err := LoadAuthorPack(nil); err == nil {
		t.Fatal("nil filesystem accepted")
	}
	// Runtime hashing follows manifest declaration order, not map/tree sort.
	one, err := LoadAuthorPack(authorPackFS())
	if err != nil {
		t.Fatal(err)
	}
	f := authorPackFS()
	f["pack.ini"].Data = bytes.Replace(f["pack.ini"].Data, []byte("source = text/sky.artext\nsource = text/other.artext"), []byte("source = text/other.artext\nsource = text/sky.artext"), 1)
	two, err := LoadAuthorPack(f)
	if err != nil {
		t.Fatal(err)
	}
	if one.RuntimeRevision() == two.RuntimeRevision() || two.workspace.scripts[0].path != "text/other.artext" {
		t.Fatal("source order lost")
	}
	if probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE"); probe != "" {
		root := t.TempDir()
		writeAuthorTestFiles(t, root, two.Files())
		assertAuthorPackRuntime(t, probe, root, two)
	}
}

func TestAuthorPackDiskContainment(t *testing.T) {
	p, err := LoadAuthorPack(authorPackFS())
	if err != nil {
		t.Fatal(err)
	}
	for _, kind := range []string{"outside-file", "inside-file", "directory", "dangling"} {
		t.Run(kind, func(t *testing.T) {
			root := t.TempDir()
			files := p.Files()
			delete(files, "fonts/Test.ttf")
			writeAuthorTestFiles(t, root, files)
			if err := os.MkdirAll(filepath.Join(root, "fonts"), 0700); err != nil {
				t.Fatal(err)
			}
			target := filepath.Join(t.TempDir(), "outside.ttf")
			if kind == "inside-file" {
				target = filepath.Join(root, "inside.ttf")
			}
			if kind != "dangling" {
				if err := os.WriteFile(target, []byte("font"), 0600); err != nil {
					t.Fatal(err)
				}
			}
			link := filepath.Join(root, "fonts", "Test.ttf")
			if kind == "directory" {
				if err := os.Remove(filepath.Join(root, "fonts")); err != nil {
					t.Fatal(err)
				}
				link = filepath.Join(root, "fonts")
				target = t.TempDir()
			}
			if err := os.Symlink(target, link); err != nil {
				t.Skip("symlinks unavailable", err)
			}
			if opened, err := OpenAuthorPack(root); err == nil || opened != nil {
				t.Fatal("symlink pack member accepted")
			}
		})
	}
}

type authorTestFile struct {
	reader     io.Reader
	size       int64
	mode       fs.FileMode
	closeError error
}

func (f *authorTestFile) Read(p []byte) (int, error) { return f.reader.Read(p) }
func (f *authorTestFile) Close() error               { return f.closeError }
func (f *authorTestFile) Stat() (fs.FileInfo, error) { return f, nil }
func (f *authorTestFile) Name() string               { return "test" }
func (f *authorTestFile) Size() int64                { return f.size }
func (f *authorTestFile) Mode() fs.FileMode          { return f.mode }
func (f *authorTestFile) ModTime() time.Time         { return time.Time{} }
func (f *authorTestFile) IsDir() bool                { return f.mode.IsDir() }
func (f *authorTestFile) Sys() any                   { return nil }

type authorTestFS struct{ file *authorTestFile }

func (f authorTestFS) Open(string) (fs.File, error) { return f.file, nil }

func TestAuthorPackBoundedReads(t *testing.T) {
	for _, f := range []*authorTestFile{
		{reader: strings.NewReader("longer"), size: 1},
		{reader: strings.NewReader("x"), size: 2},
		{reader: strings.NewReader("x"), size: -1},
		{reader: strings.NewReader("x"), size: 999},
		{reader: strings.NewReader("x"), size: 1, mode: fs.ModeNamedPipe},
		{reader: strings.NewReader("x"), size: 1, closeError: errors.New("close failed")},
	} {
		if data, err := readPackFile(authorTestFS{f}, "test", 3); err == nil || data != nil {
			t.Fatal("invalid read accepted")
		}
	}
}

func FuzzAuthorManifest(f *testing.F) {
	for _, seed := range []string{authorPackManifest, "\ufeff" + authorPackManifest, "[pack]\nid = broken\n"} {
		f.Add(seed)
	}
	f.Fuzz(func(t *testing.T, text string) {
		if len(text) > MaxPackManifestBytes+1 {
			return
		}
		m, err := ParsePackManifest(text, "pack.ini")
		if err != nil {
			if m != nil {
				t.Fatal("partial manifest escaped")
			}
			return
		}
		next, err := m.WithMetadata(m.Metadata())
		if err != nil || next.Text() != text {
			t.Fatal("self edit not lossless", err)
		}
		canonical, err := NewPackManifest(m.Metadata(), m.Fonts(), m.Sources())
		if err != nil {
			t.Fatal("accepted manifest cannot be emitted", err)
		}
		if canonical.Metadata() != m.Metadata() || !reflect.DeepEqual(canonical.Fonts(), m.Fonts()) || !slices.Equal(canonical.Sources(), m.Sources()) {
			t.Fatal("canonical metadata drift")
		}
	})
}
