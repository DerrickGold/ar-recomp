package builder

import (
	"context"
	"encoding/json"
	"errors"
	"net/http/httptest"
	"testing"
)

func TestDirectoryRecoveryCodesAndCancellationDoNotMutate(t *testing.T) {
	for _, language := range []string{"en", "fr", "de", "ja"} {
		app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "tok")
		if w := interfacePOST(app, "/tok/interface/preferences", `{"language":"`+language+`"}`); w.Code != 200 {
			t.Fatal(w.Code, w.Body.String())
		}
		for _, tc := range []struct {
			path string
			err  error
			code string
		}{
			{"", errDirectoryChooserUnavailable, "builder.language.chooser_unavailable"},
			{"", errors.New("opaque platform detail 100% {name}"), "builder.language.chooser_failed"},
			{"relative/path", nil, "builder.language.chooser_failed"},
			{"", nil, ""},
		} {
			app.options.pickDirectory = func(context.Context) (string, error) { return tc.path, tc.err }
			w := httptest.NewRecorder()
			app.ServeHTTP(w, httptest.NewRequest("POST", "/tok/localization/choose-directory", nil))
			var result struct {
				ErrorCode string
				Cancelled bool
			}
			if err := json.Unmarshal(w.Body.Bytes(), &result); err != nil {
				t.Fatal(err)
			}
			if result.ErrorCode != tc.code || (tc.code == "" && (!result.Cancelled || w.Code != 200)) || (tc.code != "" && w.Code != 400) {
				t.Fatal(language, w.Code, w.Body.String())
			}
			if app.localization.store != nil || app.localization.current != nil {
				t.Fatal("chooser changed project state")
			}
		}
	}
}
