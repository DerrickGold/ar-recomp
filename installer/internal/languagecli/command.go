// Package languagecli exposes the shared authoring library without a browser.
// No command implements an independent manifest, script or font parser.
package languagecli

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"slices"

	"github.com/DerrickGold/ar-recomp/installer/internal/fontprobe"
	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

type samples []string

func (s *samples) String() string     { return fmt.Sprint([]string(*s)) }
func (s *samples) Set(v string) error { *s = append(*s, v); return nil }

func Run(ctx context.Context, args []string, output io.Writer) error {
	if len(args) > 0 && (args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
		_, err := fmt.Fprintln(output, "Usage: actraiser-builder language <command> [options]\n\n  validate   Check a directory/archive; --game adds font coverage\n  package    Publish reviewed translations as a .arlang\n  reference  Export the ROM-free US authoring contract as JSON\n  install    Copy a publication into this game's language library\n  enable     Enable an installed file/folder\n  disable    Disable without modifying archive contents\n  uninstall  Remove from discovery with a recovery copy\n\nUse <command> --help for options. No command opens the editor.")
		return err
	}
	if len(args) == 0 {
		return fmt.Errorf("usage: actraiser-builder language validate|package|reference|install|enable|disable|uninstall [options]")
	}
	allowed := map[string][]string{
		"validate":  {"pack", "out", "root", "game", "sample"},
		"package":   {"pack", "out", "root", "game", "sample", "confirm-rights", "include-wip", "all-messages"},
		"reference": {"out"}, "install": {"pack", "root", "replace"},
		"enable": {"installed", "root"}, "disable": {"installed", "root"}, "uninstall": {"installed", "root"},
		"prepare": {"root", "packs-root"},
	}
	if _, ok := allowed[args[0]]; !ok {
		return fmt.Errorf("unknown language command %q", args[0])
	}
	f := flag.NewFlagSet("language "+args[0], flag.ContinueOnError)
	f.SetOutput(output)
	packUsage := "author directory, .arlang or private .arproject"
	if args[0] == "install" {
		packUsage = "publication .arlang file"
	}
	packPath := f.String("pack", "", packUsage)
	installed := f.String("installed", "", "installed archive filename or unpacked directory name")
	root := f.String("root", ".", "game working directory (utils in a distribution)")
	packsRoot := f.String("packs-root", "", "internal startup adapter: explicit installation root")
	game := f.String("game", "", "trusted game executable for font coverage; never selected by a pack")
	out := f.String("out", "", "new output file; never overwritten")
	rights := f.Bool("confirm-rights", false, "confirm that you may redistribute the publication's content")
	wip := f.Bool("include-wip", false, "include WIP as well as Done messages in publication")
	all := f.Bool("all-messages", false, "explicitly mark every supplied message reviewed before publication")
	replace := f.Bool("replace", false, "replace an existing same-ID archive installation")
	var sample samples
	f.Var(&sample, "sample", "dynamic text sample for font checking (repeatable)")
	f.Usage = func() {
		fmt.Fprintf(output, "Usage: actraiser-builder language %s [options]\n", args[0])
		f.VisitAll(func(option *flag.Flag) {
			if slices.Contains(allowed[args[0]], option.Name) {
				name, usage := flag.UnquoteUsage(option)
				if name != "" {
					name = " " + name
				}
				fmt.Fprintf(output, "  --%s%s\n      %s\n", option.Name, name, usage)
			}
		})
	}
	if err := f.Parse(args[1:]); err != nil {
		return err
	}
	if f.NArg() != 0 {
		return fmt.Errorf("unexpected positional arguments")
	}
	var optionErr error
	f.Visit(func(flag *flag.Flag) {
		if !slices.Contains(allowed[args[0]], flag.Name) {
			optionErr = fmt.Errorf("--%s is not valid for language %s", flag.Name, args[0])
		}
	})
	if optionErr != nil {
		return optionErr
	}
	absRoot, err := filepath.Abs(*root)
	if err != nil {
		return err
	}
	if args[0] == "reference" {
		refs, err := lk.AuthorReferences("us")
		if err != nil {
			return err
		}
		data := struct {
			Format  string               `json:"format"`
			Version int                  `json:"version"`
			Profile string               `json:"source_profile"`
			Routes  []lk.AuthorReference `json:"routes"`
		}{"actraiser-language-authoring-reference", 1, "us", refs}
		return writeOutput(*out, output, func(w io.Writer) error { e := json.NewEncoder(w); e.SetIndent("", "  "); return e.Encode(data) })
	}
	if args[0] == "prepare" {
		path := filepath.Join(absRoot, "game-assets", "languages", "packs")
		if *packsRoot != "" {
			path = *packsRoot
		}
		return lk.PrepareLanguageArchives(ctx, path, output)
	}
	if args[0] == "enable" || args[0] == "disable" || args[0] == "uninstall" {
		path := filepath.Join(absRoot, "game-assets", "languages", "packs")
		if *installed == "" {
			return fmt.Errorf("--installed is required")
		}
		row, err := lk.InspectInstalledPack(path, *installed)
		if err != nil {
			return err
		}
		if args[0] == "uninstall" {
			backup, err := lk.UninstallLanguagePack(path, row.Key, row.Metadata.ID, row.Revision)
			if err != nil {
				return err
			}
			_, err = fmt.Fprintf(output, "Removed from discovery. Recovery copy: %s. Restart the game.\n", backup)
			return err
		}
		if err := lk.SetLanguagePackEnabled(path, row.Key, row.Metadata.ID, row.Revision, args[0] == "enable"); err != nil {
			return err
		}
		_, err = fmt.Fprintln(output, "Package availability saved. Restart the game.")
		return err
	}
	if args[0] != "validate" && args[0] != "package" && args[0] != "install" {
		return fmt.Errorf("unknown language command %q", args[0])
	}
	if *packPath == "" {
		return fmt.Errorf("--pack is required")
	}
	if args[0] == "install" {
		path, err := lk.InstallLanguageArchive(filepath.Join(absRoot, "game-assets", "languages", "packs"), *packPath, *replace)
		if err != nil {
			return err
		}
		_, err = fmt.Fprintf(output, "Installed %s. Restart the game and select its package name.\n", path)
		return err
	}
	p, err := lk.OpenAuthorInput(*packPath)
	if err != nil {
		return err
	}
	var publication lk.PublicationReport
	if args[0] == "package" {
		if *out == "" || *game == "" {
			return fmt.Errorf("package requires --out and --game (font coverage is required)")
		}
		if *all {
			p, err = p.ReviewedAll()
			if err != nil {
				return err
			}
		}
		p, publication, err = p.Publication(lk.PublicationOptions{ConfirmRights: *rights, IncludeWIP: *wip})
		if err != nil {
			return err
		}
	}
	var coverage *lk.FontCoverageReport
	if *game != "" {
		binary, err := filepath.Abs(*game)
		if err != nil {
			return err
		}
		builtin := filepath.Join(absRoot, "game-assets", "fonts", "noto", "NotoSans-SemiCondensedExtraBold.ttf")
		fallback, err := lk.OpenNativeUSSource(filepath.Join(absRoot, "game-assets", "languages", "native-us"))
		if err != nil {
			return err
		}
		if fallback == nil {
			return fmt.Errorf("native US source is missing; build the game first")
		}
		probe := func(c context.Context, fonts []lk.FontCoverageSource, scalars []rune) (lk.FontCoverageProbeResult, error) {
			return fontprobe.Run(c, binary, builtin, fonts, scalars)
		}
		report, err := p.Pack().CheckFontCoverageWithFallback(ctx, probe, fallback, sample)
		if err != nil {
			return err
		}
		coverage = &report
	}
	if args[0] == "validate" {
		result := struct {
			Metadata     lk.PackMetadata          `json:"metadata"`
			Stats        lk.AuthorValidationStats `json:"semantic_validation"`
			Coverage     *lk.FontCoverageReport   `json:"font_coverage,omitempty"`
			TextCoverage lk.AuthorCoverageReport  `json:"text_coverage"`
		}{p.Pack().Manifest().Metadata(), p.Pack().Workspace().Stats(), coverage, p.Coverage()}
		if err = writeOutput(*out, output, func(w io.Writer) error { e := json.NewEncoder(w); e.SetIndent("", "  "); return e.Encode(result) }); err != nil {
			return err
		}
		if coverage != nil && !coverage.Complete {
			return fmt.Errorf("font coverage failed: %d missing characters", coverage.MissingCount)
		}
		return nil
	}
	if coverage == nil || !coverage.Complete {
		return fmt.Errorf("font coverage failed; run language validate with --game for details")
	}
	if err = writeOutput(*out, output, func(w io.Writer) error { return p.WriteArchive(w, "publication") }); err != nil {
		return err
	}
	_, err = fmt.Fprintf(output, "Published %d messages to %s (%d WIP, %d omitted native-source messages).\n", publication.Included, *out, publication.WIP, publication.UnchangedSource)
	return err
}

// Failed writes remove only the new file created by this invocation.
func writeOutput(path string, stdout io.Writer, write func(io.Writer) error) error {
	if path == "" {
		return write(stdout)
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0644)
	if err != nil {
		return err
	}
	complete := false
	defer func() {
		f.Close()
		if !complete {
			os.Remove(path)
		}
	}()
	if err = write(f); err != nil {
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	complete = true
	return nil
}
