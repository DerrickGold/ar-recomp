// Package workshopui shares small UI components between the desktop startup
// screen and the Workshop without linking the backend into the desktop shell.
package workshopui

import "embed"

//go:embed interface-text.js controls.css feedback.js feedback.css big-screen.js
var Assets embed.FS
