package localizationkit

import (
	"bytes"
	"encoding/json"
	"io"
	"testing"
)

func TestCanonicalProfileData(t *testing.T) {
	// Profile JSON is now the primary declarative input, not an output of a
	// separate extractor. Reject stale/typo fields instead of ignoring them.
	for _, fixture := range []struct {
		name   string
		data   []byte
		target any
	}{
		{"decoder", decoderProfileJSON, new(decoderFactsData)},
		{"source", sourceProfileJSON, new([]sourceProfile)},
		{"catalog", catalogProfileJSON, new(catalogFactsData)},
		{"destination", destinationProfileJSON, new(destinationFactsData)},
		{"composer-surfaces", composeSurfaceJSON, new(map[string]nativeComposeSurface)},
	} {
		t.Run(fixture.name, func(t *testing.T) {
			decoder := json.NewDecoder(bytes.NewReader(fixture.data))
			decoder.DisallowUnknownFields()
			if err := decoder.Decode(fixture.target); err != nil {
				t.Fatal(err)
			}
			if err := decoder.Decode(new(any)); err != io.EOF {
				t.Fatal("trailing profile data", err)
			}
		})
	}
	for kind, surface := range nativeComposeSurfaces {
		r := surface.Region
		if surface.SurfaceID <= 0 || surface.Destination < 0 || surface.Destination > 0xffff ||
			r[0] < 0 || r[1] < 0 || r[2] < 1 || r[3] < 1 || r[0]+r[2] > 32 || r[1]+r[3] > 32 ||
			(surface.NativeFontPixels != 7 && surface.NativeFontPixels != 8) {
			t.Fatal("invalid native composer surface", kind)
		}
	}
}
