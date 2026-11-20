package localization

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"maps"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
)

const LanguageModType = "actraiser-language"
const MaxLanguageModMembers = maxArchiveFiles + 2
const languageModPrefix = "language/"

type languageModHeader struct {
	Format int    `json:"format"`
	Type   string `json:"type"`
	Game   string `json:"game"`
	ID     string `json:"id"`
}

type languageModManifest struct {
	Name        string            `json:"name"`
	Version     string            `json:"version"`
	Author      string            `json:"author"`
	Description string            `json:"description,omitempty"`
	License     string            `json:"license,omitempty"`
	Main        string            `json:"main,omitempty"`
	Binaries    map[string]string `json:"binaries,omitempty"`
}

// LanguageModID groups releases that share the same runtime pack.ini ID.
// Older O2R archives also used this value as the archive ID.
func LanguageModID(id string) string {
	id = strings.ToLower(id)
	if len(id) <= 55 {
		return "language-" + id
	}
	digest := sha256.Sum256([]byte(id))
	return fmt.Sprintf("language--%x", digest[:16])
}

type LanguageRelease [3]uint32

func ParseLanguageRelease(version string) (LanguageRelease, error) {
	parts := strings.Split(version, ".")
	var result LanguageRelease
	if len(parts) != len(result) {
		return result, fmt.Errorf("release version must use MAJOR.MINOR.PATCH")
	}
	for i, part := range parts {
		value, err := strconv.ParseUint(part, 10, 32)
		if err != nil || strconv.FormatUint(value, 10) != part {
			return result, fmt.Errorf("release version must use MAJOR.MINOR.PATCH")
		}
		result[i] = uint32(value)
	}
	return result, nil
}

func CompareLanguageReleases(a, b string) int {
	left, _ := ParseLanguageRelease(a)
	right, _ := ParseLanguageRelease(b)
	return slices.Compare(left[:], right[:])
}

// WriteLanguageMod publishes a reviewed translation in the shared asset-only
// transport. The contents are ordinary language files, with their own schema.
func (p *AuthorProject) WriteLanguageMod(output io.Writer, version string) error {
	if !p.publicationReady {
		return fmt.Errorf("prepare publication before exporting")
	}
	if _, err := ParseLanguageRelease(version); err != nil {
		return err
	}
	if p.pack.manifest.Version() == 1 {
		upgraded, _, err := p.Installation()
		if err != nil {
			return err
		}
		p = upgraded
	}
	return p.writeLanguageMod(output, version)
}

// WriteInstalledLanguageMod packages a reviewed local installation. The
// authoring backup and the separately reviewed publication stay independent.
func (p *AuthorProject) WriteInstalledLanguageMod(output io.Writer, version string) error {
	if !p.installationReady {
		return fmt.Errorf("prepare the translation for installation first")
	}
	if _, err := ParseLanguageRelease(version); err != nil {
		return err
	}
	return p.writeLanguageMod(output, version)
}

const languageContentPrefix = "language-content-"

var languageContentIDPattern = regexp.MustCompile(`^language-content-[0-9a-f]{40}$`)

// ValidLanguageModID accepts the original launcher identity and the current
// content-addressed form. Archive reads separately verify the content hash.
func ValidLanguageModID(id, runtimeID string) bool {
	return id == LanguageModID(runtimeID) || languageContentIDPattern.MatchString(id)
}

func languageContentID(manifest []byte, files map[string][]byte) string {
	digest := sha256.New()
	fmt.Fprintf(digest, "%d:", len(manifest))
	digest.Write(manifest)
	for _, name := range slices.Sorted(maps.Keys(files)) {
		data := files[name]
		fmt.Fprintf(digest, "%d:%s:%d:", len(name), name, len(data))
		digest.Write(data)
	}
	return languageContentPrefix + fmt.Sprintf("%x", digest.Sum(nil)[:20])
}

func (p *AuthorProject) writeLanguageMod(output io.Writer, version string) error {
	files := p.projectFiles(false)
	if err := validateProjectFiles(files); err != nil {
		return err
	}
	m := p.pack.manifest.Metadata()
	manifest, _ := json.Marshal(languageModManifest{
		Name: limitModText(m.Name, 160), Version: version, Author: m.Author,
		Description: limitModText(m.Description, 4096), License: m.License,
	})
	header, _ := json.Marshal(languageModHeader{1, LanguageModType, "actraiser-recomp", languageContentID(manifest, files)})
	members := map[string][]byte{"manifest.json": manifest, "pack.json": header}
	for name, data := range files {
		members[languageModPrefix+name] = data
	}
	return writeLanguageZIP(output, members)
}

// ConvertLegacyLanguageMod wraps a published legacy pack, preserving the
// runtime ID and upgrading v1 text for the current reader.
func ConvertLegacyLanguageMod(output io.Writer, input io.ReaderAt, size int64) error {
	index, err := inspectAuthorArchive(input, size)
	if err != nil {
		return err
	}
	if index.mod {
		return fmt.Errorf("select a legacy .arlang publication")
	}
	if err := requirePublication(index); err != nil {
		return err
	}
	project, err := readAuthorIndex(index)
	if err != nil {
		return err
	}
	prepared, _, err := project.Installation()
	if err != nil {
		return err
	}
	return prepared.WriteInstalledLanguageMod(output, "1.0.0")
}

func limitModText(value string, limit int) string {
	if len(value) <= limit {
		return value
	}
	for value[limit]&0xc0 == 0x80 {
		limit--
	}
	return value[:limit]
}

func readLanguageModHeader(files map[string]*zip.File) (languageModHeader, error) {
	var h languageModHeader
	raw, err := readArchiveMember(files["pack.json"], 64<<10)
	if err != nil {
		return h, err
	}
	if err := strictAuthorJSON(raw, &h); err != nil || h.Format != 1 ||
		h.Type != LanguageModType || h.Game != "actraiser-recomp" {
		return h, fmt.Errorf("invalid language mod descriptor")
	}
	var m languageModManifest
	raw, err = readArchiveMember(files["manifest.json"], 64<<10)
	if err != nil {
		return h, err
	}
	if err := json.Unmarshal(raw, &m); err != nil || m.Main != "" || len(m.Binaries) != 0 ||
		strings.TrimSpace(m.Name) == "" || len(m.Name) > 160 || len(m.Author) > 256 ||
		len(m.Description) > 4096 || len(m.License) > 256 {
		return h, fmt.Errorf("invalid language mod manifest")
	}
	_, err = ParseLanguageRelease(m.Version)
	return h, err
}

// ReadLanguageMod applies the same publication validation as the legacy
// archive reader, including semantic contracts, paths, fonts and notices.
func ReadLanguageMod(input io.ReaderAt, size int64) (*AuthorProject, error) {
	index, err := inspectAuthorArchive(input, size)
	if err != nil {
		return nil, err
	}
	if !index.mod {
		return nil, fmt.Errorf("expected an .o2r language pack")
	}
	project, err := readAuthorIndex(index)
	if err != nil {
		return nil, err
	}
	return project, project.checkInstallable()
}

var languageDigestPattern = regexp.MustCompile(`^[0-9a-f]{64}$`)

// PreparedLanguage is the immutable snapshot behind one validated mod. Its
// token fits the existing Go-to-C language catalog protocol.
type PreparedLanguage struct {
	ID      string           `json:"id"`
	Locale  string           `json:"locale"`
	Content string           `json:"content"`
	Token   string           `json:"token"`
	Sizes   map[string]int64 `json:"sizes"`
}

// Open reads the immutable installation snapshot for this release.
func (p *PreparedLanguage) Open(root, digest string) (*AuthorPack, error) {
	if !p.Cached(root, digest) {
		return nil, fmt.Errorf("the language pack snapshot is unavailable")
	}
	cache, err := os.OpenRoot(filepath.Join(root, archiveCacheDirectory))
	if err != nil {
		return nil, err
	}
	defer cache.Close()
	snapshot, err := cache.OpenRoot(filepath.FromSlash(p.Token))
	if err != nil {
		return nil, err
	}
	defer snapshot.Close()
	return LoadAuthorPack(rootedPackFS{snapshot})
}

func PrepareLanguageMod(root, digest string, input io.ReaderAt, size int64) (*PreparedLanguage, error) {
	if !languageDigestPattern.MatchString(digest) {
		return nil, fmt.Errorf("invalid language mod digest")
	}
	index, err := inspectAuthorArchive(input, size)
	if err != nil {
		return nil, err
	}
	if !index.mod {
		return nil, fmt.Errorf("expected an .o2r language pack")
	}
	p, err := readAuthorIndex(index)
	if err != nil {
		return nil, err
	}
	p, _, err = p.Installation()
	if err != nil {
		return nil, err
	}
	files := p.projectFiles(false)
	m := p.pack.manifest.Metadata()
	result := &PreparedLanguage{ID: m.ID, Locale: m.Locale, Content: index.payloadRevision(), Sizes: make(map[string]int64, len(files))}
	for name, data := range files {
		result.Sizes[name] = int64(len(data))
	}
	object := filepath.Join(root, archiveCacheDirectory, digest)
	if err := makeAuthorDirectory(object); err != nil {
		return nil, err
	}
	unlock, err := authorLock(object)
	if err != nil {
		return nil, err
	}
	defer unlock()
	token, err := writeArchiveSnapshot(object, files)
	if err != nil {
		return nil, err
	}
	result.Token = digest + "/" + token
	return result, nil
}

func (p *PreparedLanguage) Cached(root, digest string) bool {
	if p == nil || !authorIdentifier(p.ID) || len(p.ID) > 96 || !packLocale(p.Locale) ||
		!languageDigestPattern.MatchString(p.Content) || p.Sizes["pack.ini"] <= 0 || len(p.Sizes) > maxArchiveFiles {
		return false
	}
	hash, version, ok := strings.Cut(p.Token, "/")
	if !ok || hash != digest || !languageDigestPattern.MatchString(hash) || !cacheVersionValid(version) {
		return false
	}
	r, err := os.OpenRoot(filepath.Join(root, archiveCacheDirectory))
	if err != nil {
		return false
	}
	defer r.Close()
	for name, size := range p.Sizes {
		if !PortablePackPath(name) || size < 0 {
			return false
		}
		info, err := r.Lstat(filepath.FromSlash(p.Token + "/" + name))
		if err != nil || !info.Mode().IsRegular() || info.Size() != size {
			return false
		}
	}
	return true
}
