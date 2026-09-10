package workshopart

import (
	"encoding/binary"
	"fmt"
	"image"
)

type Pose struct {
	Frame string `json:"frame"`
	Ticks int    `json:"ticks"`
}

type Animation struct {
	ID    string `json:"id"`
	Poses []Pose `json:"poses"`
}

// The common player table is $06:8000; its first word locates the visual
// pointer table, and (state+1)*2 locates a four-byte animation program.
// This is a bounded art profile, not an interpreter for gameplay commands.
const masterTable = 0x30000

var masterVisuals = [...]int{4, 0, 1, 2, 3, 11, 12, 13, 14, 15, 0x16, 0x36, 0x19, 0x1a, 0x1b, 0x1c}
var masterClips = [...]struct {
	id    string
	state int
	count int
}{
	{"idle", 0, 1}, {"walk", 2, 8}, {"sword", 8, 5},
	{"jump", 3, 11}, {"air-sword", 0x0c, 15}, {"fall", 7, 1},
}

func masterID(facing string, visual int) string {
	return fmt.Sprintf("master.%s.%02x", facing, visual)
}

func expectedFrames() []Frame {
	var frames []Frame
	add := func(id string, width, height int) {
		i := len(frames)
		frames = append(frames, Frame{id, i % 10 * cellSize, i / 10 * cellSize, width, height})
	}
	for _, row := range compositions() {
		add(row.id, row.width, row.height)
	}
	for _, facing := range []string{"right", "left"} {
		for _, visual := range masterVisuals {
			add(masterID(facing, visual), cellSize, cellSize)
		}
	}
	boss := 0
	for _, actor := range encounterActors {
		for _, facing := range []string{"normal", "flipped"} {
			for _, visual := range actor.visuals {
				id := actorID(actor.id, facing, visual)
				if actor.cell == bossCellSize {
					frames = append(frames, Frame{id, boss % 6 * bossCellSize, bossRow + boss/6*bossCellSize, bossCellSize, bossCellSize})
					boss++
				} else {
					add(id, cellSize, cellSize)
				}
			}
		}
	}
	frames = append(frames, Frame{"fillmore.forest", 0, backgroundRow, 256, 256}, Frame{"fillmore.ground", 256, backgroundRow, 256, 256})
	return frames
}

func relativePointer(data []byte, at int) (int, error) {
	if at < masterTable || at+2 > masterTable+0x2000 || at+2 > len(data) {
		return 0, fmt.Errorf("Master pointer outside reviewed table")
	}
	p := masterTable + int(binary.LittleEndian.Uint16(data[at:]))
	if p < masterTable || p >= masterTable+0x2000 || p >= len(data) {
		return 0, fmt.Errorf("Master target outside reviewed table")
	}
	return p, nil
}

func extractMaster(atlas *image.NRGBA, data []byte, assets *Assets) error {
	table, err := relativePointer(data, masterTable)
	if err != nil {
		return err
	}
	frames := expectedFrames()
	for _, facing := range []string{"right", "left"} {
		for _, visual := range masterVisuals {
			address, err := relativePointer(data, table+visual*2)
			if err != nil {
				return err
			}
			frame := frames[len(assets.Catalog.Frames)]
			if err := drawMaster(atlas, data, address, facing == "left", frame.X, frame.Y); err != nil {
				return err
			}
			assets.Catalog.Frames = append(assets.Catalog.Frames, frame)
		}
		for _, clip := range masterClips {
			address, err := relativePointer(data, masterTable+(clip.state+1)*2)
			if err != nil || address+clip.count*4 >= len(data) {
				return fmt.Errorf("invalid Master animation program")
			}
			animation := Animation{ID: "master." + facing + "." + clip.id}
			for i := 0; i < clip.count; i++ {
				part := data[address+i*4 : address+i*4+4]
				// DEC $24 / BMI advances: a stored duration of zero still
				// displays for one game tick. Movement bytes are not UI motion.
				animation.Poses = append(animation.Poses, Pose{masterID(facing, int(part[0])), int(part[1]) + 1})
			}
			if data[address+clip.count*4] != 255 {
				return fmt.Errorf("unterminated Master animation program")
			}
			assets.Catalog.Animations = append(assets.Catalog.Animations, animation)
		}
	}
	return validateAnimations(assets.Catalog.Animations)
}

func validateAnimations(animations []Animation) error {
	if len(animations) != 2*len(masterClips) {
		return fmt.Errorf("invalid Master animation count")
	}
	for direction, facing := range []string{"right", "left"} {
		for i, clip := range masterClips {
			a := animations[direction*len(masterClips)+i]
			if a.ID != "master."+facing+"."+clip.id || len(a.Poses) != clip.count {
				return fmt.Errorf("invalid Master animation metadata")
			}
			for _, pose := range a.Poses {
				found := false
				for _, visual := range masterVisuals {
					found = found || pose.Frame == masterID(facing, visual)
				}
				if !found || pose.Ticks < 1 || pose.Ticks > 256 {
					return fmt.Errorf("invalid Master animation pose")
				}
			}
		}
	}
	return nil
}

// Action compositions differ from sim: four signed extent bytes, a count,
// then seven-byte parts (flags, x/right, x/left, y/normal, y/flipped, attr16).
// $00:95F0 initializes the Master's +$28 from record $00:9810's $09 high
// byte. $00:8D68 XORs $0100, yielding common bank 0 and palette 4 ($0800).
// $02:BC9E loads $07:8000 chars and $07:D040 palettes (CGRAM $C0-$EF).
func drawMaster(dst *image.NRGBA, data []byte, address int, left bool, ox, oy int) error {
	if address < masterTable || address+5 > len(data) || len(data) < 0x3d0a0 {
		return fmt.Errorf("Master composition outside ROM")
	}
	return drawAction(dst, data[address:], data[0x38000:0x3a000], data[0x3d040:0x3d0a0], left, ox, oy)
}

// The caller resolves each reviewed actor's OBJ bank and palette slice; this
// shared compositor handles geometry and part ordering for either actor.
func drawAction(dst *image.NRGBA, definition, characters, palette []byte, flipped bool, ox, oy int) error {
	return drawActionCell(dst, definition, characters, palette, flipped, ox, oy, cellSize)
}

func drawActionCell(dst *image.NRGBA, definition, characters, palette []byte, flipped bool, ox, oy, cell int) error {
	if len(definition) < 5 || len(characters) != 0x2000 || len(palette)%32 != 0 || (cell != cellSize && cell != bossCellSize) || !image.Rect(ox, oy, ox+cell, oy+cell).In(dst.Bounds()) {
		return fmt.Errorf("invalid action sprite inputs")
	}
	n := int(definition[4])
	if n < 1 || n > 64 || 5+n*7 > len(definition) {
		return fmt.Errorf("invalid action composition size")
	}
	xt, extent := 1, 0
	flip := uint16(0)
	if flipped {
		xt, extent, flip = 2, 1, 0x4000
	}
	// Shared world anchor at cell centre; the Master's feet end at y=72.
	// Do not tight-crop: variable hitbox extents aren't a stable sprite origin.
	xOrigin := cell/2 - int(int8(definition[extent]))
	yOrigin := cell/2 - int(int8(definition[2])) - 1 // native OAM Y SBC carry
	// Lower-numbered OAM parts win overlap, so paint in reverse record order.
	for i := n - 1; i >= 0; i-- {
		part := definition[5+i*7 : 12+i*7]
		size := 8 + int(part[0]&1)*8
		x, y := xOrigin+int(part[xt]), yOrigin+int(part[3])
		attr := binary.LittleEndian.Uint16(part[5:]) ^ flip
		if part[0] > 1 || attr&0x100 != 0 || int((attr>>9)&7)*32+32 > len(palette) || x < 0 || y < 0 || x+size > cell || y+size > cell {
			return fmt.Errorf("unreviewed action sprite part")
		}
		if err := drawObjectPart(dst, characters, palette, ox+x, oy+y, size, attr); err != nil {
			return err
		}
	}
	return nil
}
