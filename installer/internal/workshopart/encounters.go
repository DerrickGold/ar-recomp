package workshopart

import (
	"encoding/binary"
	"fmt"
	"image"
)

// These are explicit reviewed programs, not an enemy-AI/asset-script VM.
// A clip can join programs where the native handler alternates states (the
// club wielder's $00/$01). Only timing and pictures enter the workshop.
type actorProgram struct {
	state   int
	visuals []int
}
type actorClip struct {
	id       string
	programs []actorProgram
}
type encounterSheet int

const (
	ordinarySheet encounterSheet = iota
	centaurSheet
)

type encounterActor struct {
	id      string
	sheet   encounterSheet
	cell    int
	visuals []int
	clips   []actorClip
}

var encounterActors = []encounterActor{
	{"fillmore.bird", ordinarySheet, cellSize, []int{0x1f, 0x20, 0x21, 0x22}, []actorClip{
		{"fly", []actorProgram{{0x1d, []int{0x1f, 0x20, 0x21, 0x22}}}},
	}},
	// Type $1B ($00:ACE7 -> $C576) alternates these two eight-tick states.
	{"fillmore.club", ordinarySheet, cellSize, []int{0x12, 0x13}, []actorClip{
		{"walk", []actorProgram{{0, []int{0x12}}, {1, []int{0x13}}}},
	}},
	// Type $02, handler $00:A9E6/$AA29: rest $2E, short advancing hop $31.
	{"fillmore.leaper", ordinarySheet, cellSize, []int{0x29, 0x2a, 0x2c}, []actorClip{
		{"rest", []actorProgram{{0x2e, []int{0x2a, 0x29}}}},
		{"hop", []actorProgram{{0x31, []int{0x2a, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c}}}},
	}},
	// First Fillmore boss: $00:AD51. Native $00:AD63 changes OBSEL to $09,
	// selecting the $4000 sheet; $AD69-$AD76 loads CGRAM $80-$BF from $0B:8000.
	// Never reuse the ordinary enemy palette for this actor.
	{"fillmore.centaur", centaurSheet, bossCellSize, []int{0x0c, 0x0f, 0x12, 0x15, 0x0e, 0x11, 0x14, 0x17, 6, 7, 8, 9, 10, 11}, []actorClip{
		{"idle", []actorProgram{{0x10, []int{6}}}},
		{"walk", []actorProgram{{0, []int{0x0c, 0x0f, 0x12, 0x15}}}},
		{"charge", []actorProgram{{1, []int{0x0e, 0x11, 0x14, 0x17}}}},
		{"cast", []actorProgram{{2, []int{10, 9, 8, 7, 11, 9, 7}}}},
	}},
}

func actorID(actor, facing string, visual int) string {
	return fmt.Sprintf("%s.%s.%02x", actor, facing, visual)
}

func tablePointer(blob []byte, at int) (int, error) {
	if at < 0 || at+2 > len(blob) {
		return 0, fmt.Errorf("scenery animation pointer outside blob")
	}
	p := int(binary.LittleEndian.Uint16(blob[at:]))
	if p >= len(blob) {
		return 0, fmt.Errorf("scenery animation target outside blob")
	}
	return p, nil
}

func extractEncounterActors(dst *image.NRGBA, data []byte, assets *Assets) error {
	enemy, err := unpack(data, 0xcd695, 3091)
	if err != nil {
		return err
	}
	characters, err := unpack(data, 0x80000, 8192)
	if err != nil {
		return err
	}
	boss, err := unpack(data, 0x3efc7, 5353)
	if err != nil {
		return err
	}
	bossCharacters, err := unpack(data, 0x9b12f, 8192)
	if err != nil {
		return err
	}
	frames := expectedFrames()
	for _, actor := range encounterActors {
		blob, chr, palette := enemy, characters, data[0xe4ef8:0xe4f78]
		if actor.sheet == centaurSheet {
			blob, chr, palette = boss, bossCharacters, data[0x58000:0x58080]
		}
		table, err := tablePointer(blob, 0)
		if err != nil {
			return err
		}
		for _, facing := range []string{"normal", "flipped"} {
			for _, visual := range actor.visuals {
				address, err := tablePointer(blob, table+visual*2)
				if err != nil {
					return err
				}
				frame := frames[len(assets.Catalog.Frames)]
				if err := drawActionCell(dst, blob[address:], chr, palette, facing == "flipped", frame.X, frame.Y, actor.cell); err != nil {
					return fmt.Errorf("%s: %w", frame.ID, err)
				}
				assets.Catalog.Frames = append(assets.Catalog.Frames, frame)
			}
			for _, clip := range actor.clips {
				animation := Animation{ID: actor.id + "." + facing + "." + clip.id}
				for _, program := range clip.programs {
					address, err := tablePointer(blob, (program.state+1)*2)
					if err != nil || address+len(program.visuals)*4 >= len(blob) {
						return fmt.Errorf("invalid encounter animation program")
					}
					for i, visual := range program.visuals {
						p := blob[address+i*4:]
						if int(p[0]) != visual {
							return fmt.Errorf("unexpected encounter visual")
						}
						animation.Poses = append(animation.Poses, Pose{actorID(actor.id, facing, visual), int(p[1]) + 1})
					}
					if blob[address+len(program.visuals)*4] != 255 {
						return fmt.Errorf("unterminated encounter animation")
					}
				}
				assets.Catalog.Animations = append(assets.Catalog.Animations, animation)
			}
		}
	}
	return validateEncounterAnimations(assets.Catalog.Animations[2*len(masterClips):])
}

func validateEncounterAnimations(animations []Animation) error {
	n := 0
	for _, actor := range encounterActors {
		for _, facing := range []string{"normal", "flipped"} {
			for _, clip := range actor.clips {
				if n >= len(animations) {
					return fmt.Errorf("missing encounter animation")
				}
				a, p := animations[n], 0
				n++
				if a.ID != actor.id+"."+facing+"."+clip.id {
					return fmt.Errorf("invalid encounter animation ID")
				}
				for _, program := range clip.programs {
					for _, visual := range program.visuals {
						if p >= len(a.Poses) {
							return fmt.Errorf("missing encounter pose")
						}
						pose := a.Poses[p]
						p++
						if pose.Frame != actorID(actor.id, facing, visual) || pose.Ticks < 1 || pose.Ticks > 256 {
							return fmt.Errorf("invalid encounter pose")
						}
					}
				}
				if p != len(a.Poses) {
					return fmt.Errorf("extra encounter poses")
				}
			}
		}
	}
	if n != len(animations) {
		return fmt.Errorf("extra encounter animations")
	}
	return nil
}
