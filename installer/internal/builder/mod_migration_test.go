package builder

import (
	"bytes"
	"crypto/sha256"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

func TestBundledTitlePacksPreserveExternalIDsAndOriginalTitle(t *testing.T) {
	root, external := t.TempDir(), t.TempDir()
	foreign := filepath.Join(external, "launcher-title.o2r")
	if err := prepareBundledTitles(root, nil); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(root, "game-assets", modOverlayName)); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("seeding occupied the previous-installation import's activation path: %v", err)
	}
	catalog, err := updateModCatalog(root, nil, nil, nil)
	if err != nil || len(catalog.Packs) != 2 || len(catalog.Slots) != 0 || catalog.Packs[0].Enabled || !catalog.Packs[0].Bundled {
		t.Fatalf("startup title packs = %+v, %v", catalog, err)
	}
	english := bundledTitles[0].id
	if _, err := updateModCatalog(root, nil, nil, &modChange{Action: "enable", Pack: english}); err != nil {
		t.Fatal(err)
	}
	var archive bytes.Buffer
	if err := exportTitleMod(&archive, modMetadata{Name: "Launcher title", Version: "1.0.0"}, english, bytes.NewReader(testTitlePNG(t, 90))); err != nil {
		t.Fatal(err)
	}
	writeAssetTestFile(t, foreign, archive.Bytes())
	catalog, err = updateModCatalog(root, []string{external}, nil, nil)
	if err != nil || !slices.Equal(catalog.TitleBundleConflicts, []string{english}) ||
		!slices.ContainsFunc(catalog.Packs, func(pack installedMod) bool {
			return pack.ID == english && pack.External && pack.Enabled && pack.Error == ""
		}) {
		t.Fatalf("external title was displaced: %+v %v", catalog, err)
	}
	if _, err := updateModCatalog(root, nil, nil, &modChange{Action: "disable", Pack: english}); err != nil {
		t.Fatal(err)
	}
	if got, err := os.ReadFile(foreign); err != nil || !bytes.Equal(got, archive.Bytes()) {
		t.Fatal("launcher archive changed", err)
	}
}

func TestBundledTitleUpgradeKeepsActivation(t *testing.T) {
	root := t.TempDir()
	title := bundledTitles[0]
	var old bytes.Buffer
	if err := exportTitleMod(&old, modMetadata{Name: title.name, Version: "0.9.0"}, title.id, bytes.NewReader(testTitlePNG(t, 70))); err != nil {
		t.Fatal(err)
	}
	store, unlock, err := openModStore(root, nil)
	if err != nil {
		t.Fatal(err)
	}
	writeAssetTestFile(t, store.bundledTitlePath(title.id), old.Bytes())
	store.bundled[title.id] = bundledTitleRecord{Digest: fmt.Sprintf("%x", sha256.Sum256(old.Bytes())), Version: "0.9.0"}
	err = store.saveBundledTitles()
	unlock()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := updateModCatalog(root, nil, nil, &modChange{Action: "enable", Pack: title.id}); err != nil {
		t.Fatal(err)
	}
	if err := prepareBundledTitles(root, nil); err != nil {
		t.Fatal(err)
	}
	catalog, err := updateModCatalog(root, nil, nil, nil)
	if err != nil || !slices.ContainsFunc(catalog.Packs, func(pack installedMod) bool {
		return pack.ID == title.id && pack.Version == bundledTitleVersion && pack.Enabled && pack.Bundled
	}) {
		t.Fatalf("title release failed to advance: %+v %v", catalog, err)
	}
}

func TestBundledTitleMigrationAndSwitching(t *testing.T) {
	root := t.TempDir()
	path := liveAssetManifestPath(root)
	english, japanese := bundledTitles[0].id, bundledTitles[1].id
	art, _ := bundledTitleArtwork("ja")
	relative := bundledTitleRelativePath(art)
	var legacy strings.Builder
	legacy.WriteString("; Player's music settings\n[music:custom]\ngain = 85\n")
	writeModTitleSections(&legacy, relative)
	writeAssetTestFile(t, filepath.Join(filepath.Dir(path), filepath.FromSlash(relative)), art)
	writeAssetTestFile(t, path, []byte(legacy.String()))

	if err := prepareBundledTitles(root, nil); err != nil {
		t.Fatal(err)
	}
	check := func(change *modChange, winner string) {
		t.Helper()
		catalog, err := updateModCatalog(root, nil, nil, change)
		if err != nil {
			t.Fatal(err)
		}
		if len(catalog.Packs) != 2 {
			t.Fatalf("expected both bundled packs: %+v", catalog.Packs)
		}
		for _, pack := range catalog.Packs {
			if !pack.Bundled || pack.Enabled != (pack.ID == winner) {
				t.Fatalf("unexpected title pack state: %+v", pack)
			}
		}
		if winner == "" && len(catalog.Slots) != 0 ||
			winner != "" && (len(catalog.Slots) != 1 || catalog.Slots[0].Winner != winner) {
			t.Fatalf("unexpected title priority for %q: %+v", winner, catalog.Slots)
		}
	}
	check(nil, japanese)
	if updated, err := os.ReadFile(path); err != nil || strings.Contains(string(updated), "[replace:title-") ||
		!strings.Contains(string(updated), "[music:custom]\ngain = 85") {
		t.Fatalf("title conversion changed unrelated settings: %s (%v)", updated, err)
	}
	backupPath := filepath.Join(filepath.Dir(path), legacyModBackupDir,
		fmt.Sprintf("manifest-%x.ini", sha256.Sum256([]byte(legacy.String()))))
	if backup, err := os.ReadFile(backupPath); err != nil || string(backup) != legacy.String() {
		t.Fatalf("original manifest not backed up: %v", err)
	}
	check(&modChange{Action: "enable", Pack: english}, english)
	check(&modChange{Action: "disable", Pack: english}, "")
	if err := prepareBundledTitles(root, nil); err != nil {
		t.Fatal(err)
	}
	check(nil, "")
}

func TestBundledTitleMigrationLeavesCustomMappings(t *testing.T) {
	root := t.TempDir()
	path := liveAssetManifestPath(root)
	art, _ := bundledTitleArtwork("en")
	relative := bundledTitleRelativePath(art)
	var legacy strings.Builder
	writeModTitleSections(&legacy, relative)
	manifest := strings.Replace(legacy.String(), "rect = 11,27,248,122", "rect = 12,27,248,122", 1)
	if manifest == legacy.String() {
		t.Fatal("fixture has no title rectangle to change")
	}
	writeAssetTestFile(t, filepath.Join(filepath.Dir(path), filepath.FromSlash(relative)), art)
	writeAssetTestFile(t, path, []byte(manifest))
	if err := prepareBundledTitles(root, nil); err != nil {
		t.Fatal(err)
	}
	if unchanged, err := os.ReadFile(path); err != nil || string(unchanged) != manifest {
		t.Fatalf("custom title mapping changed: %v", err)
	}
	catalog, err := updateModCatalog(root, nil, nil, nil)
	if err != nil || len(catalog.Packs) != 2 || len(catalog.Slots) != 0 {
		t.Fatalf("custom title was superseded by an included pack: %+v (%v)", catalog, err)
	}
}

func TestBundledTitleMigrationKeepsAnExistingTitleChoice(t *testing.T) {
	root := t.TempDir()
	path := liveAssetManifestPath(root)
	english, japanese := bundledTitles[0].id, bundledTitles[1].id
	if err := prepareBundledTitles(root, nil); err != nil {
		t.Fatal(err)
	}
	if _, err := updateModCatalog(root, nil, nil, &modChange{Action: "enable", Pack: english}); err != nil {
		t.Fatal(err)
	}
	art, _ := bundledTitleArtwork("ja")
	relative := bundledTitleRelativePath(art)
	var legacy strings.Builder
	writeModTitleSections(&legacy, relative)
	writeAssetTestFile(t, filepath.Join(filepath.Dir(path), filepath.FromSlash(relative)), art)
	writeAssetTestFile(t, path, []byte(legacy.String()))
	if err := prepareBundledTitles(root, nil); err != nil {
		t.Fatal(err)
	}
	if manifest, err := os.ReadFile(path); err != nil || string(manifest) != legacy.String() {
		t.Fatalf("automatic migration changed a title fallback under an active pack: %v", err)
	}
	review, err := reviewLegacyMods(root, false)
	if err != nil || len(review.Items) != 1 || review.Items[0].Pack != japanese {
		t.Fatalf("legacy title missing from explicit review: %+v (%v)", review.Items, err)
	}
	if _, err := applyLegacyMods(root, nil, review.Revision); err != nil {
		t.Fatal(err)
	}
	catalog, err := updateModCatalog(root, nil, nil, nil)
	if err != nil || len(catalog.Slots) != 1 || catalog.Slots[0].Winner != english ||
		!slices.Contains(catalog.Slots[0].Candidates, japanese) {
		t.Fatalf("explicit migration did not preserve the selected mod and fallback: %+v (%v)", catalog.Slots, err)
	}
}

func TestLegacyModMigrationResumesAndPreservesExistingSelections(t *testing.T) {
	root, external := t.TempDir(), t.TempDir()
	if err := prepareBundledTitles(root, []string{external}); err != nil {
		t.Fatal(err)
	}
	path := liveAssetManifestPath(root)
	assets := filepath.Dir(path)
	base, split := "song-09", "song-09-fillmore"
	var legacy strings.Builder
	gain := 85
	for _, target := range []string{base, split} {
		file := "audio/" + target + ".ogg"
		writeModMusicSection(&legacy, target, musicModTargets[target], modTrack{Gain: &gain}, file)
		writeAssetTestFile(t, filepath.Join(assets, file), testModAudio(t))
	}
	custom := "\n[music:private-rule]\nsrc = 0E:F69F\nwhen = wram[0018]==0x42\nfile = audio/private.ogg\n"
	writeAssetTestFile(t, filepath.Join(assets, "audio/private.ogg"), testModAudio(t))
	legacy.WriteString(custom)
	art, _ := bundledTitleArtwork("ja")
	relative := bundledTitleRelativePath(art)
	writeModTitleSections(&legacy, relative)
	writeAssetTestFile(t, filepath.Join(assets, filepath.FromSlash(relative)), art)
	writeAssetTestFile(t, path, []byte(legacy.String()))
	manual := albumPDF(t, 3)
	installManual(t, root, manual)
	foreign := filepath.Join(external, "launcher-song.o2r")
	foreignData := testMusicMod(t, "launcher-music", []modTrack{{Track: base, File: "audio/track.ogg"}}, "")
	writeAssetTestFile(t, foreign, foreignData)
	review, err := reviewLegacyMods(root, false)
	if err != nil || len(review.Items) != 5 || review.Items[0].Reason != "" ||
		review.Items[1].Reason != "music_rules" || review.Items[3].Pack != bundledTitles[1].id {
		t.Fatalf("unexpected review: %+v %v", review, err)
	}
	// Resume after interruption between publishing the first pack and retiring
	// the matching manifest section.
	store, unlock, err := openModStore(root, []string{external})
	if err != nil {
		t.Fatal(err)
	}
	catalog, err := store.scan()
	if err == nil {
		_, err = store.render(&catalog)
	}
	if err == nil {
		err = store.convertLegacyMod(root, &review.Items[0], &catalog)
	}
	unlock()
	if err != nil {
		t.Fatal(err)
	}
	result, err := applyLegacyMods(root, []string{external}, review.Revision)
	if err != nil {
		t.Fatal(err)
	}
	after, err := os.ReadFile(path)
	if err != nil || strings.Contains(string(after), "[music:"+base+"]") ||
		!strings.Contains(string(after), "[music:"+split+"]") || !strings.Contains(string(after), custom) ||
		strings.Contains(string(after), "[replace:title-logo]") {
		t.Fatalf("unexpected migrated manifest: %v %s", err, after)
	}
	if backup, err := os.ReadFile(filepath.Join(root, result.Backup)); err != nil || string(backup) != legacy.String() {
		t.Fatal("original manifest missing from recovery files", err)
	}
	manualID := review.Items[4].Pack
	if backup, err := os.ReadFile(filepath.Join(assets, legacyModBackupDir, manualID+".pdf")); err != nil || !bytes.Equal(backup, manual) {
		t.Fatal("legacy manual is missing from recovery files", err)
	}
	catalog, err = updateModCatalog(root, nil, nil, nil)
	if err != nil || !slices.ContainsFunc(catalog.Slots, func(slot modSlot) bool {
		return slot.ID == base && slot.Winner == "launcher-music" && len(slot.Candidates) == 2
	}) || !slices.ContainsFunc(catalog.Slots, func(slot modSlot) bool {
		return slot.ID == titleModTarget && slot.Winner == bundledTitles[1].id
	}) {
		t.Fatalf("migration lost launcher choice or Japanese title: %+v %v", catalog, err)
	}
	if _, err := applyLegacyMods(root, nil, review.Revision); !errors.Is(err, errLegacyReviewChanged) {
		t.Fatalf("obsolete review accepted: %v", err)
	}
	if _, err := updateModCatalog(root, nil, nil, &modChange{Action: "disable", Pack: manualID}); err != nil {
		t.Fatal(err)
	}
	if _, installed, _ := activeManualPack(root); installed != nil || regularFileExists(manualPath(root)) {
		t.Fatal("disabled migrated manual remained active")
	}
	if got, err := os.ReadFile(foreign); err != nil || !bytes.Equal(got, foreignData) {
		t.Fatal("launcher archive changed during migration", err)
	}
}

func TestLegacyModMigrationConvertsLevelSplitAndKeepsReaderPDF(t *testing.T) {
	root := t.TempDir()
	target := "song-09-fillmore"
	const relative = "audio/fillmore.ogg"
	var manifest strings.Builder
	gain, loop := 79, false
	loopStart, loopEnd := uint32(7), uint32(24)
	writeModMusicSection(&manifest, target, musicModTargets[target], modTrack{
		Gain: &gain, Loop: &loop, LoopStart: &loopStart, LoopEnd: &loopEnd,
	}, relative)
	writeAssetTestFile(t, liveAssetManifestPath(root), []byte(manifest.String()))
	writeAssetTestFile(t, filepath.Join(root, "game-assets", relative), testModAudio(t))
	pdf := []byte("%PDF-1.4\nA reader-only PDF with no scanned pages\n%%EOF\n")
	installManual(t, root, pdf)
	review, err := reviewLegacyMods(root, false)
	if err != nil || len(review.Items) != 2 || review.Items[0].Reason != "" || review.Items[1].Reason != "manual_format" {
		t.Fatalf("unexpected review: %+v %v", review, err)
	}
	if _, err := applyLegacyMods(root, nil, review.Revision); err != nil {
		t.Fatal(err)
	}
	assets := filepath.Join(root, "game-assets")
	overlay, err := os.ReadFile(filepath.Join(assets, modOverlayName))
	if err != nil || !strings.Contains(string(overlay), "[music:"+target+"]") ||
		!strings.Contains(string(overlay), "when = wram[0018]==0x01") ||
		!strings.Contains(string(overlay), "loop = 0\nloop_start = 7\nloop_end = 24\ngain = 79") {
		t.Fatalf("split or tuning lost: %s %v", overlay, err)
	}
	if got, err := os.ReadFile(manualPath(root)); err != nil || !bytes.Equal(got, pdf) {
		t.Fatal("migration changed the reader-only PDF", err)
	}
}
