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

type assetTrack struct {
	ID            string
	Name          string
	Src           string
	PreviewSource uint32
	PreviewSong   byte   `json:"preview_song"`
	DefaultFile   string `json:"file"`
	Loop          *bool
	// Regions are the action map groups ($18) whose maps DECLARE this song,
	// read straight out of the ROM by `tools/act_content.py --songs`: the
	// per-map asset script at $05:8000 carries a song-change command per map
	// whose pointer is the same SPC image source a [music:] entry names, so
	// the level/song map is a static fact rather than something to observe by
	// playing every stage.
	//
	// EMPTY MEANS NO SPLIT IS OFFERED, which covers two cases that both come
	// out the same way: a song only the non-action group ($18=$00 -- title,
	// world map, sim) uses, and one no map script declares at all because it
	// is started from an event site instead. Offering regions for either would
	// be inviting a gate that can never fire.
	Regions []trackRegion
}

// trackRegion is one region a song plays in, and which of that region's two
// acts it covers. The gate the builder writes is region-level either way --
// $19 advances within an act, so an act-level gate would stop matching a room
// later -- but the act is what a player recognises, so it belongs in the label.
type trackRegion struct {
	Group byte
	Acts  byte // bit0 = act 1, bit1 = act 2; 0 = the region has no acts
}

const (
	actOne = 1 << 0
	actTwo = 1 << 1
)

// actSuffix names the acts a song covers within one region.
func actSuffix(acts byte) string {
	switch acts {
	case actOne:
		return " \u2014 Act 1"
	case actTwo:
		return " \u2014 Act 2"
	case actOne | actTwo:
		return " \u2014 Acts 1 & 2"
	}
	return ""
}

// splitRegions is the region set offered for one song: the regions the ROM says
// actually play it, and nothing else.
//
// A song confined to ONE region is not offered at all. Splitting it could only
// produce a gated record covering every place it plays, which is what the
// slot's own ungated entry already does -- the same audio, reached through an
// extra manifest record and an extra file, with a condition that can never
// distinguish anything. Fillmore's theme, both Northwall themes and Death
// Heim's are the cases: one region each.
//
// The save path reads this too, so a crafted request cannot create a split the
// panel would not offer.
func splitRegions(track assetTrack) []assetRegion {
	if len(track.Regions) < 2 {
		return nil
	}
	var offered []assetRegion
	for _, region := range assetRegions {
		for _, played := range track.Regions {
			if region.Group != played.Group {
				continue
			}
			region.Label += actSuffix(played.Acts)
			region.Acts = played.Acts
			offered = append(offered, region)
			break
		}
	}
	return offered
}

// assetRegion is one action region, keyed by the map-group byte the game keeps
// at WRAM $18. Mirrors ActRaiserMapGroup in src/actraiser_game.h -- the game is
// the authority; this table exists so the builder can WRITE a gate rather than
// asking a player to look the byte up and type it.
//
// The region byte is the only level identity worth gating a song on. $19 (the
// room) advances WITHIN an act, and a gate is sampled once when the song
// starts, so a room-level gate stops matching the moment the song restarts a
// room later; $18 holds for a whole act. That is also why the split is offered
// per region and not per act: a region's act 1 and act 2 have different songs
// anyway, so the region byte already separates them.
type assetRegion struct {
	Slug  string
	Label string
	Group byte
	Acts  byte
}

var assetRegions = []assetRegion{
	{Slug: "fillmore", Label: "Fillmore", Group: 0x01},
	{Slug: "bloodpool", Label: "Bloodpool", Group: 0x02},
	{Slug: "kasandora", Label: "Kasandora", Group: 0x03},
	{Slug: "aitos", Label: "Aitos", Group: 0x04},
	{Slug: "marahna", Label: "Marahna", Group: 0x05},
	{Slug: "northwall", Label: "Northwall", Group: 0x06},
	{Slug: "death-heim", Label: "Death Heim", Group: 0x07},
}

// splitSectionName is the namespace the builder owns. A record under this exact
// name is one the split action created and may remove again; anything else in
// the manifest is hand-authored and is only ever read.
func splitSectionName(trackID, slug string) string {
	return trackID + "-" + slug
}

func splitStubFile(trackID, slug string) string {
	return "audio/" + splitSectionName(trackID, slug) + ".ogg"
}

func splitGate(group byte) string {
	return fmt.Sprintf("wram[%04X]==0x%02X", 0x0018, group)
}
