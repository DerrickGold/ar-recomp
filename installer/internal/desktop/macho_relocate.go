package desktop

import (
	"bytes"
	"debug/macho"
	"encoding/binary"
	"errors"
	"fmt"
	"os"
)

func isDylibImport(command uint32) bool {
	switch command {
	case 0xc, 0x80000018, 0x8000001f, 0x20, 0x80000023: // load, weak, reexport, lazy, upward
		return true
	}
	return false
}

func machoCommandString(raw []byte, order binary.ByteOrder) (string, error) {
	if len(raw) < 12 {
		return "", errors.New("truncated Mach-O string command")
	}
	start := uint64(order.Uint32(raw[8:12]))
	if start < 12 || start >= uint64(len(raw)) {
		return "", errors.New("invalid Mach-O string offset")
	}
	end := bytes.IndexByte(raw[start:], 0)
	if end < 0 {
		return "", errors.New("unterminated Mach-O command string")
	}
	return string(raw[start : start+uint64(end)]), nil
}

// Relocate only load commands, never segment offsets or executable content.
// install_name_tool is an Xcode command-line-tools shim, not a base macOS
// dependency. Keeping this small operation in Go lets ROM owners build without
// Xcode. System codesign seals the modified code afterwards.
func relocateMachO(path string, changes map[string]string, id string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	reader := bytes.NewReader(data)
	if fat, err := macho.NewFatFile(reader); err == nil {
		for _, arch := range fat.Arches {
			end := uint64(arch.Offset) + uint64(arch.Size)
			if end > uint64(len(data)) {
				return errors.New("fat Mach-O slice exceeds file")
			}
			if err := relocateMachOSlice(data[arch.Offset:end], arch.File, changes, id); err != nil {
				return fmt.Errorf("relocate %s: %w", path, err)
			}
		}
	} else {
		file, err := macho.NewFile(reader)
		if err != nil {
			return err
		}
		if err := relocateMachOSlice(data, file, changes, id); err != nil {
			return fmt.Errorf("relocate %s: %w", path, err)
		}
	}
	return atomicWrite(path, data, 0755)
}

func relocateMachOSlice(data []byte, file *macho.File, changes map[string]string, id string) error {
	headerSize, alignment := 28, 4
	if file.Magic == macho.Magic64 {
		headerSize, alignment = 32, 8
	}
	oldEnd := uint64(headerSize) + uint64(file.Cmdsz)
	limit := uint64(len(data))
	for _, section := range file.Sections {
		if section.Offset != 0 && uint64(section.Offset) < limit {
			limit = uint64(section.Offset)
		}
	}
	for _, load := range file.Loads {
		if segment, ok := load.(*macho.Segment); ok && segment.Offset != 0 && segment.Filesz != 0 && segment.Offset < limit {
			limit = segment.Offset
		}
	}
	if oldEnd > limit {
		return errors.New("Mach-O load commands overlap file content")
	}
	var commands []byte
	var count uint32
	for _, load := range file.Loads {
		raw := load.Raw()
		command := file.ByteOrder.Uint32(raw[:4])
		if command == uint32(macho.LoadCmdRpath) {
			continue
		}
		if isDylibImport(command) || command == 0xd { // LC_ID_DYLIB
			name, err := machoCommandString(raw, file.ByteOrder)
			if err != nil {
				return err
			}
			replacement := changes[name]
			if command == 0xd {
				replacement = id
			}
			if replacement != "" {
				start := int(file.ByteOrder.Uint32(raw[8:12]))
				if start < 24 {
					return errors.New("Mach-O dylib name overlaps command metadata")
				}
				size := (start + len(replacement) + 1 + alignment - 1) & ^(alignment - 1)
				updated := make([]byte, size)
				copy(updated, raw[:start])
				copy(updated[start:], replacement)
				file.ByteOrder.PutUint32(updated[4:8], uint32(size))
				raw = updated
			}
		}
		commands = append(commands, raw...)
		count++
	}
	newEnd := uint64(headerSize + len(commands))
	if newEnd > limit {
		return errors.New("insufficient Mach-O header padding; link with -headerpad_max_install_names")
	}
	for i := oldEnd; i < newEnd; i++ {
		if data[i] != 0 {
			return errors.New("Mach-O header expansion would overwrite non-padding bytes")
		}
	}
	clear(data[headerSize:oldEnd])
	copy(data[headerSize:], commands)
	file.ByteOrder.PutUint32(data[16:20], count)
	file.ByteOrder.PutUint32(data[20:24], uint32(len(commands)))
	return nil
}
