package textpreview

import (
	"context"
	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
	"os"
	"path/filepath"
	"testing"
	"testing/fstest"
)

func TestAbilityNamePlaybackUsesDraftLabelWithFallbackDialogue(t *testing.T) {
	worker := os.Getenv("AR_AUTHOR_TEXT_PREVIEW")
	if worker == "" {
		t.Skip("native preview worker unavailable")
	}
	load := func(script string) *lk.AuthorPack {
		p, err := lk.LoadAuthorPack(fstest.MapFS{"pack.ini": {Data: []byte(testManifest)}, "text/example.artext": {Data: []byte(script)}})
		if err != nil {
			t.Fatal(err)
		}
		return p
	}
	native := load(":: sim.menu.lightning\nLightning\n@end\n:: sim.help.category.2\nName {miracle.lightning.name}.\n@end\n")
	draft := load(":: sim.menu.lightning\nStorm\n@end\n")
	root, err := filepath.Abs("../../..")
	if err != nil {
		t.Fatal(err)
	}
	scenario := Defaults()
	scenario.Values["miracle.lightning.name"] = "Wrong scenario name"
	font := filepath.Join(root, "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf")
	for _, tc := range []struct {
		pack     *lk.AuthorPack
		want     string
		fallback bool
	}{{draft, "Name Storm.", true}, {native, "Name Lightning.", false}} {
		movie, err := Run(context.Background(), worker, font, "sim.help.category.2", tc.pack, native, scenario)
		if err != nil {
			t.Fatal(err)
		}
		if movie.Fallback != tc.fallback || movie.Frames[len(movie.Frames)-1].Revealed != uint32(len(tc.want)) {
			t.Fatal("dialogue did not resolve the actual label", movie.Fallback, movie.Frames)
		}
	}
}
