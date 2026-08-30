package tooling

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

const chrRenderVersion = 1

type CHRRenderReport struct {
	Version       int    `json:"version"`
	Mode          string `json:"mode"`
	NoWrite       bool   `json:"no_write"`
	InputSHA256   string `json:"input_sha256"`
	PaletteSHA256 string `json:"palette_sha256,omitempty"`
	OutputPath    string `json:"output_path"`
	Width         int    `json:"width"`
	Height        int    `json:"height"`
	Tiles         int    `json:"tiles,omitempty"`
	FirstTile     int    `json:"first_tile,omitempty"`
	Palette       int    `json:"palette,omitempty"`
	Icons         int    `json:"icons,omitempty"`
	BaseTile      int    `json:"base_tile,omitempty"`
}

func decodeSNES4BPPTile(data []byte, offset int) ([8][8]byte, error) {
	var pixels [8][8]byte
	if offset < 0 || offset+32 > len(data) {
		return pixels, fmt.Errorf("4bpp tile offset %#x is outside %d-byte input", offset, len(data))
	}
	for y := 0; y < 8; y++ {
		plane0, plane1 := data[offset+y*2], data[offset+y*2+1]
		plane2, plane3 := data[offset+16+y*2], data[offset+16+y*2+1]
		for x := 0; x < 8; x++ {
			bit := uint(7 - x)
			pixels[y][x] = (plane0>>bit)&1 | ((plane1>>bit)&1)<<1 | ((plane2>>bit)&1)<<2 | ((plane3>>bit)&1)<<3
		}
	}
	return pixels, nil
}

func loadCHRPalette(cgram []byte, palette int) ([16]color.RGBA, error) {
	var colors [16]color.RGBA
	if palette < 0 || (palette+1)*32 > len(cgram) {
		return colors, fmt.Errorf("CGRAM palette %d is outside %d-byte input", palette, len(cgram))
	}
	for index := 0; index < 16; index++ {
		offset := (palette*16 + index) * 2
		value := uint16(cgram[offset]) | uint16(cgram[offset+1])<<8
		colors[index] = color.RGBA{
			R: byte((value & 0x1f) * 255 / 31),
			G: byte((value >> 5 & 0x1f) * 255 / 31),
			B: byte((value >> 10 & 0x1f) * 255 / 31), A: 0xff,
		}
	}
	return colors, nil
}

func writeCHRPNG(path string, picture image.Image) error {
	if directory := filepath.Dir(path); directory != "." {
		if err := os.MkdirAll(directory, 0o755); err != nil {
			return fmt.Errorf("create CHR output directory: %w", err)
		}
	}
	file, err := os.Create(path)
	if err != nil {
		return fmt.Errorf("create CHR output %s: %w", path, err)
	}
	encodeErr := png.Encode(file, picture)
	closeErr := file.Close()
	if encodeErr != nil {
		return fmt.Errorf("encode CHR output %s: %w", path, encodeErr)
	}
	if closeErr != nil {
		return fmt.Errorf("close CHR output %s: %w", path, closeErr)
	}
	return nil
}

func RenderROMCHR(inputPath string, offset, length, columns int, outputPath string) (CHRRenderReport, error) {
	content, err := os.ReadFile(inputPath)
	if err != nil {
		return CHRRenderReport{}, fmt.Errorf("read CHR input: %w", err)
	}
	if offset < 0 || length <= 0 || offset+length > len(content) || length%32 != 0 || columns <= 0 {
		return CHRRenderReport{}, fmt.Errorf("CHR range offset=%#x length=%#x columns=%d is invalid for %d-byte input", offset, length, columns, len(content))
	}
	tiles := length / 32
	rows, cell := (tiles+columns-1)/columns, 9
	picture := image.NewRGBA(image.Rect(0, 0, columns*cell+1, rows*cell+1))
	background := color.RGBA{16, 18, 28, 0xff}
	for y := 0; y < picture.Bounds().Dy(); y++ {
		for x := 0; x < picture.Bounds().Dx(); x++ {
			picture.SetRGBA(x, y, background)
		}
	}
	for tile := 0; tile < tiles; tile++ {
		pixels, decodeErr := decodeSNES4BPPTile(content, offset+tile*32)
		if decodeErr != nil {
			return CHRRenderReport{}, decodeErr
		}
		originX, originY := tile%columns*cell+1, tile/columns*cell+1
		for y := 0; y < 8; y++ {
			for x := 0; x < 8; x++ {
				shade := pixels[y][x] * 17
				picture.SetRGBA(originX+x, originY+y, color.RGBA{shade, shade, shade, 0xff})
			}
		}
	}
	if err := writeCHRPNG(outputPath, picture); err != nil {
		return CHRRenderReport{}, err
	}
	digest := sha256.Sum256(content)
	return CHRRenderReport{Version: chrRenderVersion, Mode: "rom-4bpp-sheet", NoWrite: false, InputSHA256: hex.EncodeToString(digest[:]), OutputPath: outputPath, Width: picture.Bounds().Dx(), Height: picture.Bounds().Dy(), Tiles: tiles}, nil
}

func RenderSnapshotCHR(vramPath, cgramPath string, palette, first, count, columns int, outputPath string) (CHRRenderReport, error) {
	vram, err := os.ReadFile(vramPath)
	if err != nil {
		return CHRRenderReport{}, fmt.Errorf("read VRAM: %w", err)
	}
	cgram, err := os.ReadFile(cgramPath)
	if err != nil {
		return CHRRenderReport{}, fmt.Errorf("read CGRAM: %w", err)
	}
	colors, err := loadCHRPalette(cgram, palette)
	if err != nil {
		return CHRRenderReport{}, err
	}
	available := len(vram) / 32
	if first < 0 || first > available || count < 0 || first+count > available || columns <= 0 {
		return CHRRenderReport{}, fmt.Errorf("snapshot CHR range first=%d count=%d columns=%d is invalid for %d tiles", first, count, columns, available)
	}
	if count == 0 {
		count = available - first
	}
	rows, cell := (count+columns-1)/columns, 9
	picture := image.NewRGBA(image.Rect(0, 0, columns*cell+1, rows*cell+1))
	background := color.RGBA{20, 22, 34, 0xff}
	for y := 0; y < picture.Bounds().Dy(); y++ {
		for x := 0; x < picture.Bounds().Dx(); x++ {
			picture.SetRGBA(x, y, background)
		}
	}
	for index := 0; index < count; index++ {
		pixels, decodeErr := decodeSNES4BPPTile(vram, (first+index)*32)
		if decodeErr != nil {
			return CHRRenderReport{}, decodeErr
		}
		originX, originY := index%columns*cell+1, index/columns*cell+1
		for y := 0; y < 8; y++ {
			for x := 0; x < 8; x++ {
				if pixels[y][x] != 0 {
					picture.SetRGBA(originX+x, originY+y, colors[pixels[y][x]])
				}
			}
		}
	}
	if err := writeCHRPNG(outputPath, picture); err != nil {
		return CHRRenderReport{}, err
	}
	vramHash, cgramHash := sha256.Sum256(vram), sha256.Sum256(cgram)
	return CHRRenderReport{Version: chrRenderVersion, Mode: "snapshot-4bpp-sheet", NoWrite: false, InputSHA256: hex.EncodeToString(vramHash[:]), PaletteSHA256: hex.EncodeToString(cgramHash[:]), OutputPath: outputPath, Width: picture.Bounds().Dx(), Height: picture.Bounds().Dy(), Tiles: count, FirstTile: first, Palette: palette}, nil
}

var chrDigits = map[rune][]string{
	'0': {"111", "101", "101", "101", "111"}, '1': {"010", "110", "010", "010", "111"},
	'2': {"111", "001", "111", "100", "111"}, '3': {"111", "001", "111", "001", "111"},
	'4': {"101", "101", "111", "001", "001"}, '5': {"111", "100", "111", "001", "111"},
	'6': {"111", "100", "111", "101", "111"}, '7': {"111", "001", "010", "010", "010"},
	'8': {"111", "101", "111", "101", "111"}, '9': {"111", "101", "111", "001", "111"},
}

func drawCHRLabel(picture *image.RGBA, x, y int, text string) {
	const scale = 2
	for _, digit := range text {
		glyph := chrDigits[digit]
		for row, line := range glyph {
			for column, value := range line {
				if value != '1' {
					continue
				}
				for sy := 0; sy < scale; sy++ {
					for sx := 0; sx < scale; sx++ {
						picture.SetRGBA(x+column*scale+sx, y+row*scale+sy, color.RGBA{255, 235, 120, 0xff})
					}
				}
			}
		}
		x += 4 * scale
	}
}

func RenderIconCHR(vramPath, cgramPath string, palette, baseTile, count, iconsPerRow, iconsPerGridRow int, outputPath string) (CHRRenderReport, error) {
	vram, err := os.ReadFile(vramPath)
	if err != nil {
		return CHRRenderReport{}, fmt.Errorf("read VRAM: %w", err)
	}
	cgram, err := os.ReadFile(cgramPath)
	if err != nil {
		return CHRRenderReport{}, fmt.Errorf("read CGRAM: %w", err)
	}
	colors, err := loadCHRPalette(cgram, palette)
	if err != nil {
		return CHRRenderReport{}, err
	}
	if baseTile < 0 || count <= 0 || iconsPerRow <= 0 || iconsPerGridRow <= 0 {
		return CHRRenderReport{}, fmt.Errorf("invalid icon sheet geometry")
	}
	const scale, padding, labelHeight = 3, 6, 14
	cellWidth, cellHeight := 16*scale+padding, 16*scale+labelHeight+padding
	rows := (count + iconsPerRow - 1) / iconsPerRow
	picture := image.NewRGBA(image.Rect(0, 0, iconsPerRow*cellWidth+padding, rows*cellHeight+padding))
	background := color.RGBA{24, 26, 38, 0xff}
	for y := 0; y < picture.Bounds().Dy(); y++ {
		for x := 0; x < picture.Bounds().Dx(); x++ {
			picture.SetRGBA(x, y, background)
		}
	}
	for icon := 0; icon < count; icon++ {
		iconColumn, iconRow := icon%iconsPerGridRow, icon/iconsPerGridRow
		topLeft := baseTile + iconRow*2*16 + iconColumn*2
		quadTiles := [2][2]int{{topLeft, topLeft + 1}, {topLeft + 16, topLeft + 17}}
		originX, originY := padding+icon%iconsPerRow*cellWidth, padding+icon/iconsPerRow*cellHeight+labelHeight
		for quadY := 0; quadY < 2; quadY++ {
			for quadX := 0; quadX < 2; quadX++ {
				pixels, decodeErr := decodeSNES4BPPTile(vram, quadTiles[quadY][quadX]*32)
				if decodeErr != nil {
					return CHRRenderReport{}, fmt.Errorf("icon %d: %w", icon, decodeErr)
				}
				for y := 0; y < 8; y++ {
					for x := 0; x < 8; x++ {
						value := pixels[y][x]
						if value == 0 {
							continue
						}
						for sy := 0; sy < scale; sy++ {
							for sx := 0; sx < scale; sx++ {
								picture.SetRGBA(originX+(quadX*8+x)*scale+sx, originY+(quadY*8+y)*scale+sy, colors[value])
							}
						}
					}
				}
			}
		}
		drawCHRLabel(picture, originX+2, originY-labelHeight+2, strconv.Itoa(icon))
	}
	if err := writeCHRPNG(outputPath, picture); err != nil {
		return CHRRenderReport{}, err
	}
	vramHash, cgramHash := sha256.Sum256(vram), sha256.Sum256(cgram)
	return CHRRenderReport{Version: chrRenderVersion, Mode: "snapshot-icon-sheet", NoWrite: false, InputSHA256: hex.EncodeToString(vramHash[:]), PaletteSHA256: hex.EncodeToString(cgramHash[:]), OutputPath: outputPath, Width: picture.Bounds().Dx(), Height: picture.Bounds().Dy(), Palette: palette, Icons: count, BaseTile: baseTile}, nil
}

func writeCHRRenderReport(writer io.Writer, report CHRRenderReport, format string) error {
	if format == "json" {
		encoder := json.NewEncoder(writer)
		encoder.SetIndent("", "  ")
		return encoder.Encode(report)
	}
	if format != "" && format != "text" {
		return fmt.Errorf("unknown CHR report format %q (want text or json)", format)
	}
	fmt.Fprintf(writer, "%s: %s %dx%d", report.OutputPath, report.Mode, report.Width, report.Height)
	if report.Tiles != 0 {
		fmt.Fprintf(writer, " tiles=%d first=%d", report.Tiles, report.FirstTile)
	}
	if report.Icons != 0 {
		fmt.Fprintf(writer, " icons=%d base=$%03X", report.Icons, report.BaseTile)
	}
	fmt.Fprintln(writer)
	return nil
}

func parseCHRInteger(name, value string) (int, error) {
	parsed, err := strconv.ParseInt(strings.TrimSpace(value), 0, 64)
	if err != nil || parsed < 0 || int64(int(parsed)) != parsed {
		return 0, fmt.Errorf("parse %s %q as a non-negative integer", name, value)
	}
	return int(parsed), nil
}

func RunCHRRenderCommand(args []string, defaultRoot string, output io.Writer) error {
	if len(args) == 0 {
		return fmt.Errorf("chr-render needs rom, snapshot, or icons")
	}
	mode := args[0]
	minimum := map[string]int{"rom": 4, "snapshot": 4, "icons": 6}[mode]
	if minimum == 0 || len(args)-1 < minimum {
		return fmt.Errorf("chr-render %s has missing positional inputs", mode)
	}
	positionals := args[1 : 1+minimum]
	flags := flag.NewFlagSet("chr-render "+mode, flag.ContinueOnError)
	rootValue := flags.String("root", defaultRoot, "game project root used to resolve relative paths")
	format := flags.String("format", "text", "report format: text or json")
	columns := flags.Int("cols", 16, "tile columns per row")
	first := flags.Int("first", 0, "snapshot mode: first tile")
	count := flags.Int("count", 0, "snapshot mode: tile count (zero means remaining tiles)")
	iconsPerRow := flags.Int("per-row", 10, "icons mode: output icons per row")
	iconsPerGridRow := flags.Int("grid-width", 8, "icons mode: source icons per 16-tile VRAM row")
	if err := flags.Parse(args[1+minimum:]); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return fmt.Errorf("chr-render options must follow all positional inputs")
	}
	root, err := filepath.Abs(*rootValue)
	if err != nil {
		return fmt.Errorf("resolve CHR project root: %w", err)
	}
	resolve := func(path string) string { return resolveWRAMPath(root, path) }
	var report CHRRenderReport
	switch mode {
	case "rom":
		offset, err := parseCHRInteger("offset", positionals[1])
		if err != nil {
			return err
		}
		length, err := parseCHRInteger("length", positionals[2])
		if err != nil {
			return err
		}
		report, err = RenderROMCHR(resolve(positionals[0]), offset, length, *columns, resolve(positionals[3]))
	case "snapshot":
		palette, err := parseCHRInteger("palette", positionals[2])
		if err != nil {
			return err
		}
		report, err = RenderSnapshotCHR(resolve(positionals[0]), resolve(positionals[1]), palette, *first, *count, *columns, resolve(positionals[3]))
	case "icons":
		palette, err := parseCHRInteger("palette", positionals[2])
		if err != nil {
			return err
		}
		baseTile, err := parseCHRInteger("base tile", positionals[3])
		if err != nil {
			return err
		}
		iconCount, err := parseCHRInteger("icon count", positionals[4])
		if err != nil {
			return err
		}
		report, err = RenderIconCHR(resolve(positionals[0]), resolve(positionals[1]), palette, baseTile, iconCount, *iconsPerRow, *iconsPerGridRow, resolve(positionals[5]))
	}
	if err != nil {
		return err
	}
	return writeCHRRenderReport(output, report, *format)
}
