//go:build linux

package main

/*
#cgo pkg-config: glib-2.0
#include <glib.h>

static void prepare_builder_program_name(void) {
    g_set_prgname("ActRaiserRecompBuilderWebView");
}
*/
import "C"

func init() {
	// Wails v2 sets options.Linux.ProgramName only AFTER NewWindow has created
	// WebKit's default context. Set it before GTK/WebKit initialization as well,
	// or WebKit pre-creates our global build workspace as its browser profile.
	C.prepare_builder_program_name()
}
