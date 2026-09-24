// Package gameassets decodes ActRaiser's declarative scene resource table.
// It never executes actor code or interprets source addresses as host pointers.
package gameassets

import (
	"fmt"
	"math/bits"
)

const ScriptBase = 0x28000
const ScriptLimit = ScriptBase + 0x7ff0

var operandCounts = [...]int{6, 5, 3, 1, 4, 7, 6, 6}

type Command struct {
	Code     byte
	Operands []byte
}
type Entry struct {
	Mode, Submode byte
	Start, End    int
	Commands      []Command
}

// Script returns detached operands and bounds every read, including synthetic
// test inputs. Callers identify a retail ROM separately before extracting assets.
func Script(rom []byte) ([]Entry, error) {
	if len(rom) < ScriptBase+3 {
		return nil, fmt.Errorf("truncated asset script table")
	}
	cursor := ScriptBase + 3
	read := func(count int) ([]byte, error) {
		if cursor > ScriptLimit-count || cursor > len(rom)-count {
			return nil, fmt.Errorf("asset script crosses its bounded table")
		}
		data := rom[cursor : cursor+count]
		cursor += count
		return data, nil
	}
	var entries []Entry
	for cursor < ScriptLimit {
		start := cursor
		selectors, err := read(2)
		if err != nil {
			return nil, err
		}
		entry := Entry{Mode: selectors[0], Submode: selectors[1], Start: start}
		for {
			raw, err := read(1)
			if err != nil {
				return nil, err
			}
			code := raw[0]
			if code == 0 {
				break
			}
			operands, err := read(operandCounts[bits.Len8(code)-1])
			if err != nil {
				return nil, err
			}
			entry.Commands = append(entry.Commands, Command{code, append([]byte(nil), operands...)})
		}
		entry.End = cursor
		entries = append(entries, entry)
		// 07/08 is the final boss, not the final entry: 08/01 owns credits.
		if entry.Mode == 8 && entry.Submode == 1 {
			return entries, nil
		}
	}
	return nil, fmt.Errorf("asset script end marker missing")
}
