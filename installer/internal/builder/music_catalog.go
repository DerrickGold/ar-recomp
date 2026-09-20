package builder

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
)

// music.json is the single song identity catalog for Workshop, the ROM census,
// and the generated default manifest. Region/act coverage comes from the ROM's
// map scripts; a track whose musical name is uncertain keeps its table slot.
//
//go:embed assets/music.json
var musicCatalogJSON []byte

var assetTracks = loadMusicCatalog()

func loadMusicCatalog() []assetTrack {
	var tracks []assetTrack
	if err := json.Unmarshal(musicCatalogJSON, &tracks); err != nil {
		panic(fmt.Errorf("embedded music catalog: %w", err))
	}
	for i := range tracks {
		source, err := strconv.ParseUint(strings.ReplaceAll(tracks[i].Src, ":", ""), 16, 24)
		if err != nil {
			panic(fmt.Errorf("music catalog %s: %w", tracks[i].ID, err))
		}
		tracks[i].PreviewSource = uint32(source)
	}
	return tracks
}
