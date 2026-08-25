package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/DerrickGold/snesrecomp-go/internal/spcaudio"
)

var actRaiserPreviewTracks = []spcaudio.Track{
	{ID: "title-theme", Name: "Title Theme", Source: 0x1a94b8, Song: 1},
	{ID: "song-00", Name: "Fillmore", Source: 0x18947f, Song: 1},
	{ID: "song-01", Name: "Sky Palace", Source: 0x1ca988, Song: 1},
	{ID: "song-02", Name: "Bloodpool", Source: 0x18ddcc, Song: 1},
	{ID: "song-03", Name: "Song 03", Source: 0x1ae9e2, Song: 1},
	{ID: "song-04", Name: "Song 04", Source: 0x1ca5fb, Song: 1},
	{ID: "song-05", Name: "Song 05", Source: 0x1b8554, Song: 1},
	{ID: "song-06", Name: "Song 06", Source: 0x1b9470, Song: 1},
	{ID: "song-08", Name: "Advent", Source: 0x1ca7cc, Song: 1},
	{ID: "song-09", Name: "Act 2 Theme", Source: 0x0ef69f, Song: 1},
	{ID: "song-10", Name: "Birth of the People", Source: 0x1babed, Song: 1},
	{ID: "song-11", Name: "Level Up", Source: 0x1cafeb, Song: 1},
	{ID: "song-12", Name: "Song 12", Source: 0x19fa4b, Song: 1},
	{ID: "song-13", Name: "Song 13", Source: 0x17c027, Song: 1},
	{ID: "song-14", Name: "Song 14", Source: 0x18d4fa, Song: 1},
	{ID: "song-15", Name: "Song 15", Source: 0x1c9f3d, Song: 1},
	{ID: "song-16", Name: "Kasandora", Source: 0x1aef63, Song: 1},
}

func selectAudioPreviewTracks(selection string) ([]spcaudio.Track, error) {
	if selection == "" || selection == "all" {
		return append([]spcaudio.Track(nil), actRaiserPreviewTracks...), nil
	}
	wanted := make(map[string]bool)
	for _, id := range strings.Split(selection, ",") {
		id = strings.TrimSpace(id)
		if id != "" {
			wanted[id] = true
		}
	}
	var selected []spcaudio.Track
	for _, track := range actRaiserPreviewTracks {
		if wanted[track.ID] {
			selected = append(selected, track)
			delete(wanted, track.ID)
		}
	}
	if len(wanted) != 0 {
		for id := range wanted {
			return nil, fmt.Errorf("unknown audio track %q", id)
		}
	}
	if len(selected) == 0 {
		return nil, fmt.Errorf("no audio tracks selected")
	}
	return selected, nil
}

func runAudioPreview(args []string) error {
	flags := flag.NewFlagSet("audio-preview", flag.ContinueOnError)
	rom := flags.String("rom", "game.sfc", "path to the US ActRaiser ROM")
	output := flags.String("out", "audio-previews", "output/cache directory")
	seconds := flags.Int("seconds", 30, "preview length in seconds (1-600)")
	selection := flags.String("tracks", "all", "comma-separated track IDs, or all")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *seconds < 1 || *seconds > 600 {
		return fmt.Errorf("--seconds must be between 1 and 600")
	}
	tracks, err := selectAudioPreviewTracks(*selection)
	if err != nil {
		return err
	}
	romPath, err := filepath.Abs(*rom)
	if err != nil {
		return err
	}
	outputPath, err := filepath.Abs(*output)
	if err != nil {
		return err
	}
	fmt.Fprintln(os.Stdout, "Rendering local ActRaiser audio previews with the pure-Go APU…")
	paths, fingerprint, err := spcaudio.RenderActRaiserPreviews(
		context.Background(), romPath, outputPath, tracks, time.Duration(*seconds)*time.Second,
		func(progress spcaudio.Progress) {
			action := "rendered"
			if progress.Reused {
				action = "reused"
			}
			fmt.Fprintf(os.Stdout, "[%d/%d] %s: %s\n",
				progress.Completed, progress.Total, progress.Track.Name, action)
		},
	)
	if err != nil {
		return err
	}
	location := outputPath
	if first := paths[tracks[0].ID]; first != "" {
		location = filepath.Dir(first)
	}
	fmt.Fprintf(os.Stdout, "Wrote %d preview WAVs under %s (ROM %s).\n",
		len(paths), location, fingerprint)
	fmt.Fprintln(os.Stdout,
		"These files are extracted game content. Keep them local; they are not covered by the MIT license.")
	return nil
}
