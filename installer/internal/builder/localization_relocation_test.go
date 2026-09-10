package builder

import (
	"bytes"
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

// A rejected debug state must exit, not strand an unattended run in a native
// message box. No snapshot restoration or ROM mutation is permitted here.
func TestHeadlessDebugStateRejectionExits(t *testing.T) {
	game, rom := os.Getenv("AR_AUTHOR_FONT_PROBE"), os.Getenv("AR_LOCALIZATION_BUILD_ROM")
	if game == "" || rom == "" {
		t.Skip("set AR_AUTHOR_FONT_PROBE and AR_LOCALIZATION_BUILD_ROM")
	}
	dir := t.TempDir()
	config := filepath.Join(dir, "test.ini")
	if err := os.WriteFile(config, []byte("[General]\n"), 0644); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, game, rom, "--config", config)
	cmd.Dir = dir
	cmd.Env = append(os.Environ(), "SDL_AUDIODRIVER=dummy", "AR_HEADLESS=1", "AR_HEADLESS_VIDEO=0", "AR_AUDIO=0", "AR_SIM3D=0", "AR_NO_RUN_DIR=1", "AR_INPUT_REPLAY=", "AR_INPUT_RECORD=", "AR_LOCALIZATION_CONTENT=Native US", "AR_LOCALIZATION_PRESENTATION=Native", "AR_LOADSTATE=unrestorable-debug-state", "AR_SETTINGS_PATH="+filepath.Join(dir, "settings.ini"), "AR_SAVE_NATIVE_PATH="+filepath.Join(dir, "save.srm"), "AR_SAVE_INI_PATH="+filepath.Join(dir, "save.ini"))
	output, err := cmd.CombinedOutput()
	var exit *exec.ExitError
	if ctx.Err() != nil || !errors.As(err, &exit) || exit.ExitCode() != 1 || !bytes.Contains(output, []byte("AR_LOADSTATE is unsupported")) {
		t.Fatalf("debug rejection did not exit normally: %v %v\n%s", ctx.Err(), err, output)
	}
}

func TestHeadlessReplayEndExitsBeforeSafetyCap(t *testing.T) {
	game, rom, probe := os.Getenv("AR_AUTHOR_FONT_PROBE"), os.Getenv("AR_LOCALIZATION_BUILD_ROM"), os.Getenv("AR_REPLAY_WRITER_PROBE")
	if game == "" || rom == "" || probe == "" {
		t.Skip("set AR_AUTHOR_FONT_PROBE, AR_LOCALIZATION_BUILD_ROM and AR_REPLAY_WRITER_PROBE")
	}
	dir := t.TempDir()
	input, output := filepath.Join(dir, "input.rec"), filepath.Join(dir, "output.rec")
	config := filepath.Join(dir, "test.ini")
	if err := os.WriteFile(config, []byte("[General]\n"), 0644); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if out, err := exec.CommandContext(ctx, probe, "--write-smoke", input).CombinedOutput(); err != nil {
		t.Fatal(err, string(out))
	}
	cmd := exec.CommandContext(ctx, game, rom, "--config", config)
	cmd.Dir = dir
	for _, item := range os.Environ() {
		if !strings.HasPrefix(item, "AR_REPLAY_NOSTOP=") && !strings.HasPrefix(item, "AR_REPLAY_LIVE_AFTER_END=") {
			cmd.Env = append(cmd.Env, item)
		}
	}
	cmd.Env = append(cmd.Env, "SDL_AUDIODRIVER=dummy", "AR_HEADLESS=1", "AR_HEADLESS_VIDEO=0", "AR_AUDIO=0", "AR_SIM3D=0", "AR_NO_RUN_DIR=1", "AR_QUIT_FRAMES=100", "AR_INPUT_REPLAY="+input, "AR_INPUT_RECORD="+output, "AR_LOCALIZATION_CONTENT=Native US", "AR_LOCALIZATION_PRESENTATION=Native", "AR_LOADSTATE=", "AR_SETTINGS_PATH="+filepath.Join(dir, "settings.ini"), "AR_SAVE_NATIVE_PATH="+filepath.Join(dir, "save.srm"), "AR_SAVE_INI_PATH="+filepath.Join(dir, "save.ini"))
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatal(err, string(out))
	}
	// Successful exit alone is insufficient: the 100-tick safety cap used to
	// hide an inverted stop/running flag. The public reader must see exactly 3.
	if out, err := exec.CommandContext(ctx, probe, "--count", output).CombinedOutput(); err != nil || strings.TrimSpace(string(out)) != "3" {
		t.Fatalf("replay continued past its last transaction: %v %s", err, out)
	}
}

// This qualifies the language workflow independently of full-game regeneration.
// Supply a packaged builder and an already-built game; it does NOT assert that
// a clean package can regenerate/build the game, or replace that release gate.
func TestLocalizationRelocatedBuilderGameWorkflow(t *testing.T) {
	builder, game, probe := os.Getenv("AR_LOCALIZATION_BUILDER_PROBE"), os.Getenv("AR_AUTHOR_FONT_PROBE"), os.Getenv("AR_AUTHOR_RUNTIME_PROBE")
	if builder == "" || game == "" || probe == "" {
		t.Skip("set AR_LOCALIZATION_BUILDER_PROBE, AR_AUTHOR_FONT_PROBE and AR_AUTHOR_RUNTIME_PROBE")
	}
	repo, err := filepath.Abs("../../..")
	if err != nil {
		t.Fatal(err)
	}
	romRoot := os.Getenv("AR_LOCALIZATION_GUI_ROM_ROOT")
	if romRoot == "" {
		romRoot = repo
	}
	rom := filepath.Join(romRoot, "ar.sfc")
	if _, err := os.Stat(rom); err != nil {
		t.Fatal("this explicit integration gate requires the local US ROM", err)
	}
	base := t.TempDir()
	bundle := filepath.Join(base, "First install with spaces")
	utils := filepath.Join(bundle, "utils")
	copyFile := func(source, target string, mode os.FileMode) {
		t.Helper()
		data, err := os.ReadFile(source)
		if err != nil {
			t.Fatal(err)
		}
		if err = os.MkdirAll(filepath.Dir(target), 0755); err != nil {
			t.Fatal(err)
		}
		if err = os.WriteFile(target, data, mode); err != nil {
			t.Fatal(err)
		}
	}
	copyFile(builder, filepath.Join(utils, "tools", "actraiser-builder"), 0755)
	copyFile(game, filepath.Join(bundle, "ActRaiserRecomp"), 0755)
	copyFile(filepath.Join(repo, "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf"), filepath.Join(utils, "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf"), 0644)
	archive := filepath.Join(base, "source.arproject")
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, filepath.Join(utils, "tools", "actraiser-builder"), "localization-extract", "--rom", rom, "--out", archive)
	cmd.Dir = base // Neither the repository nor the executable's directory.
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatal(err, string(out))
	}
	data, err := os.ReadFile(archive)
	if err != nil {
		t.Fatal(err)
	}
	source, err := lk.ReadAuthorArchive(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		t.Fatal(err)
	}
	if _, err = lk.InstallNativeUSSource(filepath.Join(utils, "game-assets/languages/native-us"), source.Pack()); err != nil {
		t.Fatal(err)
	}
	start := func() *application {
		app := newApplication(ctx, Options{ProjectRoot: utils}, "secret")
		app.result = Result{BinaryPath: filepath.Join(bundle, "ActRaiserRecomp"), WorkingDir: utils}
		locGET(t, app, "state", nil)
		return app
	}
	app := start()
	m := source.Pack().Manifest().Metadata()
	m.ID, m.Name, m.Locale = "test.excellent", "Z Excellent Adventure", "en-CA"
	m.Author, m.License = "Invented test contributor", "CC0-1.0"
	locJSON(t, app, "create", localizationRequest{Metadata: m}, 200)
	q := locIdentity(app)
	q.ID, q.Body, q.Status = "credits.page_00", "- Most excellent team -\nBill and Ted\n@end\n", lk.TranslationDone
	locJSON(t, app, "edit", q, 200)
	q = locIdentity(app)
	q.ID, q.Body, q.Status = "sky.action_mode.confirm", "@anchor reset_text_cursor.00\nMost excellent, {master_name}!\n@page\nAdventure awaits.\n@anchor yield.01\n@end\n", lk.TranslationWIP
	locJSON(t, app, "edit", q, 200)
	q = locIdentity(app)
	q.ConfirmRights, q.IncludeWIP = true, true
	publication := locJSON(t, app, "publish", q, 200).Body.Bytes() // Real C/SDL font gate, not a fake coverage result.
	locJSON(t, app, "install", q, 200)
	locUpload(t, app, "import", publication, map[string]string{"newID": "test.encore"}, 200)
	q = locIdentity(app)
	q.Metadata = app.localization.current.Pack().Manifest().Metadata()
	q.Metadata.Name, q.SaveDetails = "A Excellent Encore", true
	locJSON(t, app, "save", q, 200)
	locJSON(t, app, "install", locIdentity(app), 200)

	// Relocate everything together; no absolute font/script/project paths should
	// have entered either publication or installed metadata.
	previous := bundle
	bundle = filepath.Join(base, "Moved install with spaces")
	if err = os.Rename(previous, bundle); err != nil {
		t.Fatal(err)
	}
	utils = filepath.Join(bundle, "utils")
	app = start()
	locJSON(t, app, "open", localizationRequest{ID: "test.encore"}, 200)
	locJSON(t, app, "font-coverage", locIdentity(app), 200)
	rows := locGET(t, app, "catalog", nil).Body.String()
	if strings.Index(rows, "A Excellent Encore") < 0 || strings.Index(rows, "A Excellent Encore") >= strings.Index(rows, "Z Excellent Adventure") || strings.Count(rows, `"locale":"en-CA"`) != 2 {
		t.Fatal("same-locale package names lost or unsorted", rows)
	}
	packsRoot := filepath.Join(utils, "game-assets/languages/packs")
	packs, err := lk.ListInstalledPacks(packsRoot)
	if err != nil || len(packs) != 2 {
		t.Fatal(packs, err)
	}
	for _, pack := range packs {
		manifest := filepath.Join(packsRoot, pack.Key, "pack.ini")
		cmd := exec.CommandContext(ctx, probe, "--inspect", manifest)
		cmd.Dir = base
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatal(err, string(out))
		}
		for _, enabled := range []bool{false, true} {
			current, err := lk.InspectInstalledPack(packsRoot, pack.Key)
			if err != nil {
				t.Fatal(err)
			}
			locJSON(t, app, "set-enabled", localizationRequest{Directory: pack.Key, ID: pack.Metadata.ID, Expected: current.Revision, Enabled: enabled}, 200)
		}
	}
	q = locIdentity(app)
	q.ID = "sky.action_mode.confirm"
	query := locQuery(app)
	query.Set("id", q.ID)
	if body := locGET(t, app, "message", query).Body.String(); !strings.Contains(body, "Most excellent") || !strings.Contains(body, `"status":"done"`) {
		t.Fatal("publication/import dropped included WIP text", body)
	}
	// Publications deliberately omit private progress. The original workshop
	// must nevertheless retain its WIP marker through the same relocation.
	locJSON(t, app, "open", localizationRequest{ID: "test.excellent"}, 200)
	query = locQuery(app)
	query.Set("id", q.ID)
	if body := locGET(t, app, "message", query).Body.String(); !strings.Contains(body, `"status":"wip"`) {
		t.Fatal("relocation dropped original progress", body)
	}
}
