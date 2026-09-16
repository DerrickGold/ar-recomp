package builder

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"testing"
)

// CROSS-CHECKING THE PORT AGAINST THE READER.
//
// carveManualAlbum and judgeManualAlbum are a Go port of ManualPages_CarveAlbum
// and ManualPages_LooksLikeAlbum (src/manual/manual_pages.c). The port only
// earns its keep while it agrees with the C: a disagreement means the workshop
// accepts a manual the game refuses, or warns about one the game would open,
// which is the exact confusion the check exists to prevent.
//
// The Go tests alone cannot catch a drift, because they assert the port against
// itself. This dumps the fixtures and this side's verdicts so the real carver
// can be run over the same bytes:
//
//	mkdir -p /tmp/xcheck && cat > /tmp/xcheck/main.c <<'EOF'
//	#include <stdio.h>
//	#include <stdlib.h>
//	#include "manual_pages.h"
//	int main(int argc, char **argv) {
//	  for (int i = 1; i < argc; i++) {
//	    FILE *f = fopen(argv[i], "rb");
//	    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
//	    unsigned char *b = malloc((size_t)n);
//	    size_t got = fread(b, 1, (size_t)n, f); fclose(f);
//	    static ManualPageIndex ix;
//	    int count = ManualPages_CarveAlbum(b, got, &ix);
//	    int album = ManualPages_LooksLikeAlbum(&ix, got) ? 1 : 0;
//	    int w = 0, h = 0; ManualPages_NominalGeometry(&ix, &w, &h);
//	    printf("%s pages=%d album=%d geom=%dx%d\n", argv[i], count, album, w, h);
//	  }
//	  return 0;
//	}
//	EOF
//	cc -std=c11 -I src -I src/manual -I snesrecomp-go/runtime/include \
//	   -o /tmp/xcheck/probe /tmp/xcheck/main.c src/manual/manual_pages.c \
//	   src/scene3d_math.c -lm
//	mkdir -p /tmp/xcheck/fx
//	(cd installer && AR_XCHECK_DIR=/tmp/xcheck/fx go test ./internal/builder/ \
//	   -run TestDumpAlbumFixturesForCrossCheck -v) | grep '^GO ' | sort
//	(cd /tmp/xcheck/fx && /tmp/xcheck/probe *.bin) | sort
//
// The two listings must match line for line. Last verified identical on all
// seven fixtures, including the EXIF-thumbnail and progressive-JPEG cases.
func TestDumpAlbumFixturesForCrossCheck(t *testing.T) {
	dir := os.Getenv("AR_XCHECK_DIR")
	if dir == "" {
		t.Skip("set AR_XCHECK_DIR to dump cross-check fixtures")
	}
	text := append([]byte("%PDF-1.4\n"), bytes.Repeat([]byte("text stream "), 4096)...)
	logos := append([]byte{}, text...)
	for i := 0; i < 8; i++ {
		logos = append(logos, syntheticJPEG(64, 64, 64)...)
	}
	mixed := append([]byte("%PDF-1.4\n"), syntheticJPEG(1024, 1448, 4096)...)
	mixed = append(mixed, syntheticJPEG(1024, 2048, 4096)...)

	page := syntheticJPEG(800, 1000, 2048)
	thumb := []byte{0xFF, 0xD8, 0xFF, 0xD9}
	app1 := append([]byte{0xFF, 0xE1, 0x00, byte(len(thumb) + 2)}, thumb...)
	withThumb := append(append(append([]byte{}, page[:2]...), app1...), page[2:]...)

	progressive := bytes.Replace(syntheticJPEG(1024, 1448, 1024),
		[]byte{0xFF, 0xC0}, []byte{0xFF, 0xC2}, 1)

	// The real-scan shapes the tolerance was measured from: a majority size, a
	// large minority one pixel wider, and one crooked sheet.
	drift := []byte("%PDF-1.4\n")
	for i := 0; i < 8; i++ {
		drift = append(drift, syntheticJPEG(1009, 1767, 2048)...)
	}
	for i := 0; i < 4; i++ {
		drift = append(drift, syntheticJPEG(1010, 1767, 2048)...)
	}
	drift = append(drift, syntheticJPEG(1014, 1770, 2048)...)

	// A cover that differs, with a consistent body behind it.
	oddCover := append([]byte("%PDF-1.4\n"), syntheticJPEG(1040, 1800, 2048)...)
	for i := 0; i < 12; i++ {
		oddCover = append(oddCover, syntheticJPEG(1009, 1767, 2048)...)
	}

	// Drift is percent; a figure is a multiple. This must stay refused.
	figure := []byte("%PDF-1.4\n")
	for i := 0; i < 12; i++ {
		figure = append(figure, syntheticJPEG(1009, 1767, 2048)...)
	}
	figure = append(figure, syntheticJPEG(1120, 1767, 2048)...)

	for _, fixture := range []struct {
		name    string
		content []byte
	}{
		{"album12", albumPDF(t, 12)},
		{"album1", albumPDF(t, 1)},
		{"text", text},
		{"logos", logos},
		{"mixed", mixed},
		{"thumbnail", withThumb},
		{"progressive", progressive},
		{"drift", drift},
		{"oddcover", oddCover},
		{"figure", figure},
	} {
		if err := os.WriteFile(filepath.Join(dir, fixture.name+".bin"),
			fixture.content, 0o644); err != nil {
			t.Fatal(err)
		}
		verdict := judgeManualAlbum(fixture.content)
		album := 0
		if verdict.OK {
			album = 1
		}
		fmt.Printf("GO %s.bin pages=%d album=%d geom=%dx%d\n",
			fixture.name, verdict.Pages, album, verdict.Width, verdict.Height)
	}
}
