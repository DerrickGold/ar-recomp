package localization

import (
	"compress/gzip"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"testing"
)

func readRegressionFixture(t *testing.T, name string, destination any) {
	t.Helper()
	file, err := os.Open(filepath.Join("testdata", name))
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	reader, err := gzip.NewReader(file)
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close()
	decoder := json.NewDecoder(io.LimitReader(reader, 32<<20))
	if err := decoder.Decode(destination); err != nil {
		t.Fatal(err)
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		t.Fatal("fixture has trailing or corrupt data", err)
	}
}
