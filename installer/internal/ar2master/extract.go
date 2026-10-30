// Package ar2master extracts the optional Master from a player-supplied AR2 ROM.
// The resulting pack contains only character data, never executable ROM code.
package ar2master

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2rom"
	"github.com/DerrickGold/ar-recomp/installer/internal/quintet"
)

const (
	ROMBytes       = ar2rom.Size
	MaximumBytes   = 256 << 10
	MotionBytes    = 0x109c
	AnimationCount = 37
	HeaderBytes    = 48
	Filename       = "ar2-master.armaster"
)

// Extract accepts the supported USA release, with or without a copier header.
func Extract(rom []byte) ([]byte, error) {
	normalized, err := ar2rom.Normalize(rom)
	if err != nil {
		return nil, err
	}
	return extract(normalized)
}

func extract(rom []byte) ([]byte, error) {
	word := func(at int) int { return int(binary.LittleEndian.Uint16(rom[at:])) }
	motion, _, err := quintet.Decompress(rom, 0xf06f2)
	if err != nil || len(motion) != MotionBytes {
		return nil, fmt.Errorf("invalid AR2 motion data: %v", err)
	}
	var payload bytes.Buffer
	payload.Write(rom[0x178139:0x178159])
	payload.Write(motion)
	// Frame IDs follow first use in the animation table, matching the runtime.
	frames := []int{}
	var animations bytes.Buffer
	for anim := 0; anim < AnimationCount; anim++ {
		list := 0x70000 + word(0x78000+anim*2)
		var steps bytes.Buffer
		for step := 0; ; step++ {
			at := list + step*4
			if at < 0x78000 || at+4 > 0x80000 {
				return nil, fmt.Errorf("invalid AR2 animation pointer")
			}
			duration, definition := word(at), 0x70000+word(at+2)
			if duration&0x8000 != 0 {
				break
			}
			if step >= 12 || definition < 0x78005 || definition+15 > 0x80000 {
				return nil, fmt.Errorf("invalid AR2 animation")
			}
			id := 0
			for id < len(frames) && frames[id] != definition {
				id++
			}
			if id == len(frames) {
				frames = append(frames, definition)
			}
			if len(frames) > 96 {
				return nil, fmt.Errorf("too many AR2 frames")
			}
			steps.Write([]byte{byte(duration), byte(duration >> 8), byte(id)})
		}
		animations.WriteByte(byte(steps.Len() / 3))
		animations.Write(steps.Bytes())
	}
	payload.Write([]byte{byte(len(frames)), byte(len(frames) >> 8)})
	payload.Write(animations.Bytes())
	for _, at := range frames {
		parts := int(rom[at+14])
		size, source := word(at-5), word(at-3)
		start := (8+int(rom[at-1]))*0x10000 + 0x8000 + source
		if parts > 24 || at+15+parts*7 > len(rom) || size == 0 || size > 2048 || size%32 != 0 || source >= 0x8000 || start+size > len(rom) {
			return nil, fmt.Errorf("invalid AR2 frame tiles")
		}
		payload.Write(rom[at : at+15+parts*7])
		payload.Write([]byte{byte(size), byte(size >> 8)})
		payload.Write(rom[start : start+size])
	}
	data := make([]byte, HeaderBytes, HeaderBytes+payload.Len())
	copy(data, "AR2MASTR")
	binary.LittleEndian.PutUint32(data[8:], 1)
	binary.LittleEndian.PutUint32(data[12:], uint32(payload.Len()))
	digest := sha256.Sum256(payload.Bytes())
	copy(data[16:], digest[:])
	data = append(data, payload.Bytes()...)
	if err := Validate(data); err != nil {
		return nil, err
	}
	return data, nil
}

// Graphics borrows validated palette, sprite and animation data from a pack.
// Keep the pack alive and unchanged while using these slices.
type Graphics struct {
	Palette    []byte
	Animations [AnimationCount][]Step
	Frames     []Frame
}

type Step struct {
	Frame int
	Ticks int // Native duration + 1, in 60 Hz game ticks.
}

type Frame struct {
	Left, Right, Top byte
	Parts, Tiles     []byte
}

// Validate and the Workshop renderer share one parser for the pack format.
func Validate(data []byte) error {
	_, err := ParseGraphics(data)
	return err
}

// ParseGraphics bounds every variable-length record before exposing its data.
func ParseGraphics(data []byte) (*Graphics, error) {
	bad := func() (*Graphics, error) {
		return nil, fmt.Errorf("character assets are incomplete, modified or unsupported; import the AR2 ROM again")
	}
	if len(data) < HeaderBytes+32+MotionBytes+2 || len(data) > MaximumBytes || string(data[:8]) != "AR2MASTR" || binary.LittleEndian.Uint32(data[8:]) != 1 || int(binary.LittleEndian.Uint32(data[12:])) != len(data)-HeaderBytes {
		return bad()
	}
	digest := sha256.Sum256(data[HeaderBytes:])
	if !bytes.Equal(data[16:48], digest[:]) {
		return bad()
	}
	at := HeaderBytes + 32 + MotionBytes
	frames := int(binary.LittleEndian.Uint16(data[at:]))
	at += 2
	if frames == 0 || frames > 96 {
		return bad()
	}
	graphics := &Graphics{Palette: data[HeaderBytes : HeaderBytes+32]}
	for anim := 0; anim < AnimationCount; anim++ {
		if at >= len(data) {
			return bad()
		}
		steps := int(data[at])
		at++
		if steps == 0 || steps > 12 || at+steps*3 > len(data) {
			return bad()
		}
		for i := 0; i < steps; i++ {
			if data[at+1]&0x80 != 0 || int(data[at+2]) >= frames {
				return bad()
			}
			graphics.Animations[anim] = append(graphics.Animations[anim], Step{
				Frame: int(data[at+2]), Ticks: int(binary.LittleEndian.Uint16(data[at:])) + 1,
			})
			at += 3
		}
	}
	for i := 0; i < frames; i++ {
		if at+15 > len(data) {
			return bad()
		}
		parts := int(data[at+14])
		frame := Frame{Left: data[at], Right: data[at+1], Top: data[at+2]}
		at += 15
		end := at + parts*7
		if parts > 24 || end+2 > len(data) {
			return bad()
		}
		size := int(binary.LittleEndian.Uint16(data[end:]))
		if size == 0 || size > 2048 || size%32 != 0 || end+2+size > len(data) {
			return bad()
		}
		frame.Parts, frame.Tiles = data[at:end], data[end+2:end+2+size]
		for ; at < end; at += 7 {
			last := int(data[at+5])
			if data[at]&1 != 0 {
				last += 17
			}
			if data[at+6]&0x3f != 0 || last >= size/32 {
				return bad()
			}
		}
		at = end + 2 + size
		graphics.Frames = append(graphics.Frames, frame)
	}
	if at != len(data) {
		return bad()
	}
	return graphics, nil
}
