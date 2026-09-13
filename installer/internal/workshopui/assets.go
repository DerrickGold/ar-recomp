// Package workshopui shares small UI components between the desktop startup
// screen and the Workshop without linking the backend into the desktop shell.
package workshopui

import "embed"

//go:embed feedback.js feedback.css
var Assets embed.FS
