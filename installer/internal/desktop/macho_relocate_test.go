package desktop

import (
	"bytes"
	"debug/macho"
	"encoding/binary"
	"path/filepath"
	"strings"
	"testing"
)

func syntheticMachO() []byte {
	order := binary.LittleEndian
	command := func(kind uint32, start int, value string) []byte {
		size := (start + len(value) + 1 + 7) & ^7
		raw := make([]byte, size)
		order.PutUint32(raw, kind)
		order.PutUint32(raw[4:], uint32(size))
		order.PutUint32(raw[8:], uint32(start))
		copy(raw[start:], value)
		return raw
	}
	segment := make([]byte, 72)
	order.PutUint32(segment, uint32(macho.LoadCmdSegment64))
	order.PutUint32(segment[4:], 72)
	copy(segment[8:], "__DATA")
	order.PutUint64(segment[40:], 512)
	order.PutUint64(segment[48:], 32)
	commands := segment
	for _, kind := range []uint32{0xc, 0x80000018, 0x8000001f, 0x20, 0x80000023, 0xd} {
		commands = append(commands, command(kind, 24, "/old/library")...)
	}
	commands = append(commands, command(uint32(macho.LoadCmdRpath), 12, "/build/path")...)
	data := make([]byte, 544)
	order.PutUint32(data, macho.Magic64)
	order.PutUint32(data[4:], uint32(macho.CpuArm64))
	order.PutUint32(data[12:], uint32(macho.TypeDylib))
	order.PutUint32(data[16:], 8)
	order.PutUint32(data[20:], uint32(len(commands)))
	copy(data[32:], commands)
	copy(data[512:], "content must not move or change")
	return data
}

func TestMachORelocationHandlesAllImportCommandsAndPreservesContent(t *testing.T) {
	data := syntheticMachO()
	file, err := macho.NewFile(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	original := bytes.Clone(data[512:])
	if err := relocateMachOSlice(data, file, map[string]string{"/old/library": "@loader_path/lib.dylib"}, "@rpath/lib.dylib"); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(data[512:], original) {
		t.Fatal("relocation modified segment content")
	}
	path := filepath.Join(t.TempDir(), "library.dylib")
	put(t, path, string(data))
	info, err := inspectMachO(path)
	if err != nil || len(info.rpaths) != 0 || len(info.imports) != 1 || info.imports[0] != "@loader_path/lib.dylib" {
		t.Fatalf("imports/rpaths: %+v %v", info, err)
	}
	updated, err := macho.NewFile(bytes.NewReader(data))
	if err != nil || updated.Ncmd != 7 {
		t.Fatalf("invalid rewritten load commands: %v", err)
	}
}

func TestMachORelocationRejectsInsufficientPaddingWithoutMutation(t *testing.T) {
	data := syntheticMachO()
	original := bytes.Clone(data)
	file, err := macho.NewFile(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	if err := relocateMachOSlice(data, file, map[string]string{"/old/library": strings.Repeat("x", 1024)}, ""); err == nil {
		t.Fatal("overwrote segment data")
	}
	if !bytes.Equal(original, data) {
		t.Fatal("failed relocation modified input")
	}
}

func TestMachORelocationUpdatesEveryUniversalSlice(t *testing.T) {
	slice := syntheticMachO()
	data := make([]byte, 8192+len(slice))
	order := binary.BigEndian
	order.PutUint32(data, macho.MagicFat)
	order.PutUint32(data[4:], 2)
	for i, cpu := range []macho.Cpu{macho.CpuArm64, macho.CpuAmd64} {
		header := data[8+i*20:]
		order.PutUint32(header, uint32(cpu))
		order.PutUint32(header[8:], uint32((i+1)*4096))
		order.PutUint32(header[12:], uint32(len(slice)))
		order.PutUint32(header[16:], 12)
		copy(data[(i+1)*4096:], slice)
		binary.LittleEndian.PutUint32(data[(i+1)*4096+4:], uint32(cpu))
	}
	path := filepath.Join(t.TempDir(), "universal.dylib")
	put(t, path, string(data))
	if err := relocateMachO(path, map[string]string{"/old/library": "@loader_path/lib.dylib"}, "@rpath/lib.dylib"); err != nil {
		t.Fatal(err)
	}
	fat, err := macho.OpenFat(path)
	if err != nil {
		t.Fatal(err)
	}
	defer fat.Close()
	for _, arch := range fat.Arches {
		imports, err := arch.File.ImportedLibraries()
		if err != nil || len(imports) != 1 || imports[0] != "@loader_path/lib.dylib" || arch.File.Ncmd != 7 {
			t.Fatalf("slice %s: %v %v", arch.Cpu, imports, err)
		}
	}
}
