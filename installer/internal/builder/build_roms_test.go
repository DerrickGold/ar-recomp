package builder

import (
	"bytes"
	"context"
	"io"
	"mime/multipart"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2game"
	"github.com/DerrickGold/ar-recomp/installer/internal/ar2master"
)

func TestLocalMultiROMBuildRequest(t *testing.T) {
	directory := os.Getenv("AR_ROM_SET_DIR")
	if directory == "" {
		t.Skip("set AR_ROM_SET_DIR for local ROM upload validation")
	}
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "user-rom.sfc"), []byte("previous ROM"), 0600); err != nil {
		t.Fatal(err)
	}
	baseline, err := os.ReadFile(filepath.Join(directory, "ar.sfc"))
	if err != nil {
		t.Fatal(err)
	}
	built := make(chan bool, 1)
	app := newApplication(context.Background(), Options{ProjectRoot: root,
		Build: func(_ context.Context, rom string, _ io.Writer) (Result, error) {
			got, err := os.ReadFile(rom)
			built <- err == nil && bytes.Equal(got, baseline) && ar2game.Status(root) == "installed" && ar2master.Status(root) == "installed"
			return Result{}, nil
		}}, "tok")
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	_ = writer.WriteField("reuseROM", "true") // Explicit selection still wins.
	for _, name := range []string{"ar2.sfc", "ar.sfc"} {
		data, err := os.ReadFile(filepath.Join(directory, name))
		if err != nil {
			t.Fatal(err)
		}
		part, err := writer.CreateFormFile("rom", "unordered.SMC")
		if err != nil {
			t.Fatal(err)
		}
		_, _ = part.Write(append(make([]byte, 512), data...))
	}
	writer.Close()
	request := httptest.NewRequest("POST", "/tok/build", &body)
	request.Header.Set("Content-Type", writer.FormDataContentType())
	response := httptest.NewRecorder()
	app.ServeHTTP(response, request)
	if response.Code != 202 {
		t.Fatal(response.Code, response.Body.String())
	}
	select {
	case valid := <-built:
		if !valid {
			t.Fatal("build started with wrong baseline or missing content")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("build did not start")
	}
}
