package regionalmedia

// The actor donor contains pictures, not foreign actor programs. Keys name the
// native asset declaration (scene, resource kind, slot), so repeated/inherited
// banks are not confused with an actor's animation-slot number.
import (
	"encoding/binary"
	"fmt"
	"sort"

	"github.com/DerrickGold/ar-recomp/installer/internal/gameassets"
	"github.com/DerrickGold/ar-recomp/installer/internal/quintet"
)

const (
	actorCharacters  = 1
	actorPalette     = 2
	actorPictures    = 3
	actorMaxEntries  = 256
	actorMaxPictures = 256
	actorMaxParts    = 128
)

type actorResource struct {
	scene      uint16
	kind, slot byte
	data       []byte
}

func (r actorResource) key() uint32 { return uint32(r.scene)<<16 | uint32(r.kind)<<8 | uint32(r.slot) }

// Convert each facing's native anchor into per-part drawing offsets, then
// discard the original four collision fields,
// animation instructions, source pointers and timing bytes. The four offsets
// retain native normal/flipped placement; the renderer still owns draw bias.
func actorPicturesFromAnimation(data []byte) ([]byte, error) {
	if len(data) < 4 {
		return nil, fmt.Errorf("truncated actor animation")
	}
	table := int(binary.LittleEndian.Uint16(data))
	if table < 2 || table > len(data)-2 {
		return nil, fmt.Errorf("invalid picture table")
	}
	first := int(binary.LittleEndian.Uint16(data[table:]))
	if first <= table || first > len(data) || (first-table)%2 != 0 {
		return nil, fmt.Errorf("invalid picture count")
	}
	count := (first - table) / 2
	if count > actorMaxPictures {
		return nil, fmt.Errorf("too many actor pictures")
	}
	out := make([]byte, 4+4*(count+1))
	binary.LittleEndian.PutUint16(out, uint16(count))
	for i := 0; i < count; i++ {
		at := int(binary.LittleEndian.Uint16(data[table+i*2:]))
		if at < first || at > len(data)-5 {
			return nil, fmt.Errorf("picture outside animation")
		}
		n := int(data[at+4])
		if n == 0 || n > actorMaxParts || n > (len(data)-at-5)/7 {
			return nil, fmt.Errorf("invalid picture parts")
		}
		binary.LittleEndian.PutUint32(out[4+i*4:], uint32(len(out)))
		out = binary.LittleEndian.AppendUint16(out, uint16(n))
		out = append(out, 0, 0)
		for part := 0; part < n; part++ {
			p := data[at+5+part*7 : at+12+part*7]
			if p[0] > 1 {
				return nil, fmt.Errorf("unknown sprite size flags")
			}
			out = append(out, p[0], 0)
			for axis, v := range p[1:5] {
				// $8EAF/$8EC5 exchange opposite extents when flipped.
				// Using only left/top here would shift asymmetric pictures.
				anchor := int(int8(data[at+axis]))
				out = binary.LittleEndian.AppendUint16(out, uint16(int16(int(v)-anchor)))
			}
			out = append(out, p[5], p[6])
		}
	}
	binary.LittleEndian.PutUint32(out[4+count*4:], uint32(len(out)))
	return out, nil
}

func actorResources(rom []byte) ([]actorResource, error) {
	entries, err := gameassets.Script(rom)
	if err != nil {
		return nil, err
	}
	var resources []actorResource
	for _, e := range entries {
		if e.Mode < 1 || e.Mode > 7 {
			continue
		}
		for _, c := range e.Commands {
			p := c.Operands
			r := actorResource{scene: uint16(e.Submode)<<8 | uint16(e.Mode)}
			switch {
			case c.Code == 0x80 && p[0] == 0 && p[1] == 0x10 && (p[2] == 0x30 || p[2] == 0x40):
				r.kind = actorCharacters
				r.slot = (p[2] - 0x30) / 0x10
			case c.Code == 0x40 && p[0] == 0 && p[1] == 0x40 && p[2] == 0x80:
				r.kind = actorPalette
			case c.Code == 1 && p[0] <= 1:
				r.kind = actorPictures
				r.slot = p[0]
			default:
				continue
			}
			source := int(p[3]) | int(p[4])<<8 | int(p[5])<<16
			if r.kind == actorPalette {
				if source > len(rom)-128 {
					return nil, fmt.Errorf("truncated actor palette")
				}
				r.data = append([]byte(nil), rom[source:source+128]...)
			} else {
				r.data, _, err = quintet.Decompress(rom, source)
				if err != nil {
					return nil, err
				}
				if r.kind == actorCharacters && len(r.data) != 8192 {
					return nil, fmt.Errorf("unexpected actor atlas size")
				}
				if r.kind == actorPictures {
					r.data, err = actorPicturesFromAnimation(r.data)
					if err != nil {
						return nil, fmt.Errorf("scene%04x slot%d: %w", r.scene, r.slot, err)
					}
				}
			}
			resources = append(resources, r)
		}
	}
	if len(resources) == 0 || len(resources) > actorMaxEntries {
		return nil, fmt.Errorf("invalid actor resource count")
	}
	sort.Slice(resources, func(i, j int) bool { return resources[i].key() < resources[j].key() })
	for i := 1; i < len(resources); i++ {
		if resources[i-1].key() == resources[i].key() {
			return nil, fmt.Errorf("duplicate actor resource")
		}
	}
	return resources, nil
}

func japaneseActorArt(rom []byte) ([]byte, error) {
	resources, err := actorResources(rom)
	if err != nil {
		return nil, err
	}
	out := make([]byte, 12+16*len(resources))
	copy(out, "ARACTOR1")
	binary.LittleEndian.PutUint32(out[8:], uint32(len(resources)))
	for i, r := range resources {
		record := out[12+i*16 : 12+(i+1)*16]
		binary.LittleEndian.PutUint16(record, r.scene)
		record[2] = r.kind
		record[3] = r.slot
		binary.LittleEndian.PutUint32(record[4:], uint32(len(out)))
		binary.LittleEndian.PutUint32(record[8:], uint32(len(r.data)))
		out = append(out, r.data...)
	}
	return out, nil
}
