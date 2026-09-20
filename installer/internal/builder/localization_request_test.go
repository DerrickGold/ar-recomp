package builder

import (
	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
	"github.com/DerrickGold/ar-recomp/installer/internal/textpreview"
)

// HTTP fixture for exercising multiple endpoints in one workflow.
type localizationRequest struct {
	Scenario         *textpreview.Scenario `json:"scenario,omitzero"`
	SourceScenario   *textpreview.Scenario `json:"sourceScenario,omitzero"`
	ProjectID        string                `json:"projectID,omitzero"`
	Revision         string                `json:"revision,omitzero"`
	ID               string                `json:"id,omitzero"`
	Body             string                `json:"body,omitzero"`
	Status           lk.TranslationStatus  `json:"status,omitzero"`
	Metadata         lk.PackMetadata       `json:"metadata,omitzero"`
	Notes            string                `json:"notes,omitzero"`
	NoticeName       string                `json:"noticeName,omitzero"`
	NoticeText       string                `json:"noticeText,omitzero"`
	Directory        string                `json:"directory,omitzero"`
	NewID            string                `json:"newID,omitzero"`
	Replace          bool                  `json:"replace,omitzero"`
	Expected         string                `json:"expected,omitzero"`
	ConfirmRights    bool                  `json:"confirmRights,omitzero"`
	IncludeWIP       bool                  `json:"includeWIP,omitzero"`
	PrepareDownload  bool                  `json:"prepareDownload,omitzero"`
	SaveMessage      bool                  `json:"saveMessage,omitzero"`
	SaveDetails      bool                  `json:"saveDetails,omitzero"`
	SaveNotice       bool                  `json:"saveNotice,omitzero"`
	ConfirmUninstall bool                  `json:"confirmUninstall,omitzero"`
	SaveFonts        bool                  `json:"saveFonts,omitzero"`
	Fonts            lk.PackFonts          `json:"fonts,omitzero"`
	FontPaths        []string              `json:"fontPaths,omitzero"`
	Samples          []string              `json:"samples,omitzero"`
	fontUploads      map[string][]byte
	PreviewImport    bool   `json:"previewImport,omitzero"`
	ImportToken      string `json:"importToken,omitzero"`
	Enabled          bool   `json:"enabled,omitzero"`
}
