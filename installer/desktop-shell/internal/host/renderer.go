package host

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"net"
	"net/http"
	"strings"
	"time"
)

// StartRenderer serves the Workshop over real HTTP, not the webview's custom
// scheme. In WebKitGTK, media and Blob-backed multipart bodies require normal
// browser transport. Only an unguessable path on loopback can reach the handler.
func StartRenderer(handler http.Handler) (address string, stop func(), err error) {
	secret := make([]byte, 18)
	if _, err = rand.Read(secret); err != nil {
		return
	}
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return "", nil, err
	}
	prefix := "/" + hex.EncodeToString(secret)
	stripped := http.StripPrefix(prefix, handler)
	server := &http.Server{ReadHeaderTimeout: 10 * time.Second, ReadTimeout: 2 * time.Minute, IdleTimeout: 30 * time.Second,
		Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if !strings.HasPrefix(r.URL.Path, prefix+"/") {
				http.NotFound(w, r)
				return
			}
			stripped.ServeHTTP(w, r)
		}),
	}
	go server.Serve(listener)
	stop = func() {
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		if server.Shutdown(ctx) != nil {
			server.Close()
		}
	}
	return "http://" + listener.Addr().String() + prefix + "/", stop, nil
}
