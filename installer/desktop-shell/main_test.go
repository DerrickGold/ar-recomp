package main

import "testing"

func TestDefaultBigScreen(t *testing.T) {
	tests := []struct {
		name, platform, launcher, session, desktop string
		want                                       bool
	}{
		{name: "RecompLauncher Big Picture on Windows", platform: "windows", launcher: "bigpicture", want: true},
		{name: "RecompLauncher desktop on Linux", platform: "linux", launcher: "desktop"},
		{name: "SteamOS Gaming Mode", platform: "linux", session: "gamescope", desktop: "gamescope", want: true},
		{name: "Gamescope Wayland session", platform: "linux", session: "gamescope-wayland", want: true},
		{name: "Gamescope with no login session exported", platform: "linux", desktop: "gamescope", want: true},
		{name: "SteamOS Desktop Mode", platform: "linux", session: "KDE", desktop: "KDE"},
		{name: "nested Gamescope in Desktop Mode", platform: "linux", session: "KDE", desktop: "gamescope"},
		{name: "Gamescope environment outside Linux", platform: "darwin", desktop: "gamescope"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			env := map[string]string{
				"RECOMPLAUNCHER_MODE": test.launcher,
				"XDG_SESSION_DESKTOP": test.session,
				"XDG_CURRENT_DESKTOP": test.desktop,
			}
			if got := defaultBigScreen(test.platform, func(key string) string { return env[key] }); got != test.want {
				t.Errorf("defaultBigScreen = %t, want %t", got, test.want)
			}
		})
	}
}
