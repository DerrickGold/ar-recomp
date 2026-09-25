package builder

import (
	"archive/zip"
	"context"
	"encoding/json"
	"fmt"
	"net/http/httptest"
	"os"
	"syscall"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/fontprobe"
	"github.com/DerrickGold/ar-recomp/installer/internal/interfacecatalog"
	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

func TestLocalizationRecoveryCategories(t *testing.T) {
	entries, err := interfacecatalog.Entries()
	if err != nil {
		t.Fatal(err)
	}
	keys := map[string]bool{}
	for _, entry := range entries {
		keys[entry.Key] = true
	}
	for _, test := range []struct {
		err      error
		status   int
		category string
	}{
		{errLocalizationBuildRequired, 409, "game_required"},
		{errLocalizationSourceRequired, 409, "source_required"},
		{fmt.Errorf("%w: %w", fontprobe.ErrUnavailable, os.ErrNotExist), 409, "game_required"},
		{fmt.Errorf("%w: record 2", fontprobe.ErrProtocol), 500, "font_protocol"},
		{fontprobe.ErrFailed, 500, "font_failed"},
		{errLocalizationMissingCharacters, 422, "font_missing"},
		{fmt.Errorf("worker: %w", context.DeadlineExceeded), 504, "timeout"},
		{context.Canceled, 408, "cancelled"},
		{&os.PathError{Op: "write", Path: "project.arproject", Err: syscall.ENOSPC}, 500, "storage"},
		{&os.LinkError{Op: "rename", Old: "stage", New: "pack", Err: os.ErrPermission}, 500, "storage"},
		{&lk.AuthorError{Path: "text/main.artext", Line: 7, Message: "invalid tag"}, 400, "invalid_pack"},
		{zip.ErrChecksum, 400, "invalid_pack"},
	} {
		t.Run(test.category, func(t *testing.T) {
			w := httptest.NewRecorder()
			writeLocalizationError(w, test.err)
			var body localizationErrorResponse
			if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
				t.Fatal(err)
			}
			if w.Code != test.status || body.ErrorCode != "builder.errors."+test.category ||
				body.RecoveryKey != "builder.recovery."+test.category || body.Error != test.err.Error() {
				t.Fatalf("status %d, response %+v", w.Code, body)
			}
			if !keys[body.ErrorCode] || !keys[body.RecoveryKey] {
				t.Fatal("untranslated error category", body)
			}
		})
	}
}
