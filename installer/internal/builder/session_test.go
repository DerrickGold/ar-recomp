package builder

import (
	"context"
	"errors"
	"io"
	"net/http"
	"testing"
	"time"
)

func TestSessionReadyRunsAfterListeningAndCloseStopsServer(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	err := Run(ctx, Options{ProjectRoot: t.TempDir(), Build: func(context.Context, string, io.Writer) (Result, error) { return Result{}, nil },
		Ready: func(url string) error {
			client := &http.Client{Timeout: time.Second, Transport: &http.Transport{Proxy: nil}}
			response, err := client.Get(url + "status")
			if err != nil {
				return err
			}
			response.Body.Close()
			if response.StatusCode != 200 {
				t.Errorf("status: %d", response.StatusCode)
			}
			response, err = client.Post(url+"close", "application/json", nil)
			if err != nil {
				return err
			}
			response.Body.Close()
			return nil
		}})
	if err != nil {
		t.Fatal(err)
	}
}

func TestSessionReadyFailureAbortsStartup(t *testing.T) {
	want := errors.New("host startup failed")
	err := Run(context.Background(), Options{ProjectRoot: t.TempDir(), Build: func(context.Context, string, io.Writer) (Result, error) { return Result{}, nil }, Ready: func(string) error { return want }})
	if !errors.Is(err, want) {
		t.Fatal(err)
	}
}
