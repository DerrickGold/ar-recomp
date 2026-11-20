package localization

import (
	"fmt"
	"io"
)

const romLanguageRelease = "1.0.0"

// WriteNativeUSMod packages the generated baseline. Only the Builder-owned
// native slot can read it back. Ordinary language installs still reserve its ID.
func WriteNativeUSMod(output io.Writer, pack *AuthorPack) error {
	if pack == nil {
		return fmt.Errorf("native US language source is missing")
	}
	m := pack.Manifest().Metadata()
	if m.ID != "native-us" || m.SourceProfile != "us" || m.Target != "us-runtime" || m.Coverage != "complete" {
		return fmt.Errorf("invalid native US language source")
	}
	project, err := NewSourceProject(pack)
	if err != nil {
		return err
	}
	return project.writeLanguageMod(output, romLanguageRelease)
}

// OpenNativeUSMod only reads the generated baseline archive. It never adds it
// to the optional runtime language catalogue.
func OpenNativeUSMod(filename string) (*AuthorPack, error) {
	file, err := openRegularAuthorFile(filename)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return nil, err
	}
	index, err := inspectLanguageArchive(file, info.Size(), true)
	if err != nil {
		return nil, err
	}
	project, err := readAuthorIndex(index)
	if err != nil {
		return nil, err
	}
	return project.Pack(), nil
}

// WriteRegionalROMMod projects a read-only ROM reference onto the US semantic
// contract. Unsupported routes stay absent so the game uses its native fallback.
func WriteRegionalROMMod(output io.Writer, source *AuthorProject, japaneseFont, fontNotice []byte) error {
	pack := source.Pack()
	m := pack.Manifest().Metadata()
	if source.Origin() != "native-source" || m.SourceProfile == "us" || m.ID != "source."+m.SourceProfile ||
		m.Target != "reference-only" || m.Coverage != "complete" {
		return fmt.Errorf("expected a complete regional ROM source")
	}
	var messages []AuthorMessage
	for _, route := range authorContracts.ordered {
		if _, supported := route.Anchors["us"]; !supported {
			continue
		}
		if _, present := pack.workspace.messageScript[route.ID]; !present {
			continue
		}
		message, err := pack.ResolvedMessage(route.ID)
		if err != nil {
			return err
		}
		script := &AuthorScript{path: "text/source.artext", messages: []AuthorMessage{message}}
		if _, err := ValidateAuthorScripts("us", "partial", script); err == nil {
			messages = append(messages, message)
		}
	}
	const scriptPath = "text/source.artext"
	script, err := EmitAuthorScriptVersion(messages, scriptPath, pack.manifest.Version(), pack.Treatments()...)
	if err != nil {
		return err
	}
	m.SourceProfile, m.Target, m.Coverage = "us", "us-runtime", "partial"
	manifest, err := NewPackManifestVersion(m, pack.manifest.Fonts(), []string{scriptPath}, pack.manifest.Version())
	if err != nil {
		return err
	}
	w, err := newAuthorWorkspace("us", "partial", []*AuthorScript{script}, "")
	if err != nil {
		return err
	}
	project := &AuthorProject{
		pack: &AuthorPack{manifest: manifest, workspace: w, fonts: pack.fonts},
		info: projectInfo{Version: 1, Origin: "publication"}, notices: source.notices,
	}
	if m.Locale == "ja-JP" {
		const japanese = "fonts/NotoSansJP-Bold.otf"
		fonts := manifest.Fonts()
		fonts.Fallback = append(fonts.Fallback, japanese)
		for i := range fonts.Roles {
			fonts.Roles[i].Fallback = append(fonts.Roles[i].Fallback, japanese)
		}
		project, err = project.WithFonts(fonts, map[string][]byte{japanese: japaneseFont})
		if err != nil {
			return err
		}
		project, err = project.WithNotice("NotoSansJP-OFL.txt", string(fontNotice))
		if err != nil {
			return err
		}
	}
	prepared, _, err := project.Installation()
	if err != nil {
		return err
	}
	return prepared.WriteInstalledLanguageMod(output, romLanguageRelease)
}
