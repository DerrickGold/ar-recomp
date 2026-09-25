package builder

import (
	"archive/zip"
	"context"
	"errors"
	"io"
	"net/http"
	"os"
	"syscall"

	"github.com/DerrickGold/ar-recomp/installer/internal/fontprobe"
	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

var (
	errLocalizationBuildRequired     = errors.New("finish building the game before checking font coverage or installing/exporting language packs")
	errLocalizationSourceRequired    = errors.New("extract the native US source before checking this translation's font coverage")
	errLocalizationMissingCharacters = errors.New("font stack is missing characters")
)

type localizationErrorResponse struct {
	Error       string `json:"error"`
	ErrorCode   string `json:"errorCode"`
	RecoveryKey string `json:"recoveryKey,omitempty"`
}

// Presentation keys describe the recovery; Error keeps the original diagnostic
// for the report. Never infer a failure category by matching error strings.
func describeLocalizationError(err error) (localizationErrorResponse, int) {
	response := localizationErrorResponse{Error: err.Error(), ErrorCode: "builder.language.request_failed"}
	status, category := http.StatusBadRequest, "request"
	var pathError *os.PathError
	var linkError *os.LinkError
	var authorError *lk.AuthorError
	switch {
	case errors.Is(err, lk.ErrProjectConflict):
		response.ErrorCode = "builder.language.request_conflict"
		status, category = http.StatusConflict, "conflict"
	case errors.Is(err, errLocalizationBuildRequired), errors.Is(err, fontprobe.ErrUnavailable):
		status, category = http.StatusConflict, "game_required"
	case errors.Is(err, errLocalizationSourceRequired):
		status, category = http.StatusConflict, "source_required"
	case errors.Is(err, fontprobe.ErrProtocol):
		status, category = http.StatusInternalServerError, "font_protocol"
	case errors.Is(err, fontprobe.ErrFailed):
		status, category = http.StatusInternalServerError, "font_failed"
	case errors.Is(err, errLocalizationMissingCharacters):
		status, category = http.StatusUnprocessableEntity, "font_missing"
	case errors.Is(err, context.DeadlineExceeded):
		status, category = http.StatusGatewayTimeout, "timeout"
	case errors.Is(err, context.Canceled):
		status, category = http.StatusRequestTimeout, "cancelled"
	case errors.As(err, &pathError), errors.As(err, &linkError), errors.Is(err, os.ErrPermission),
		errors.Is(err, syscall.ENOSPC), errors.Is(err, syscall.EROFS), errors.Is(err, io.ErrShortWrite):
		status, category = http.StatusInternalServerError, "storage"
	case errors.As(err, &authorError), errors.Is(err, zip.ErrFormat), errors.Is(err, zip.ErrChecksum):
		category = "invalid_pack"
	}
	if category != "request" && category != "conflict" {
		response.ErrorCode = "builder.errors." + category
	}
	response.RecoveryKey = "builder.recovery." + category
	return response, status
}

func writeLocalizationError(w http.ResponseWriter, err error) {
	response, status := describeLocalizationError(err)
	writeJSON(w, status, response)
}
