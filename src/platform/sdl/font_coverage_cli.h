#ifndef AR_SDL_FONT_COVERAGE_CLI_H
#define AR_SDL_FONT_COVERAGE_CLI_H

/* Versioned host CLI, not a pack parser. argv[1..] are primary then ordered
 * fallback paths; stdin is one Unicode scalar per line in ASCII hexadecimal.
 * stdout echoes each scalar followed by TAB and 0/1. Exit 0 is complete,
 * 2 is input/backend/I/O failure. No ROM, window, settings or save is opened.
 * Scalar coverage does not certify shaping or layout. */
int ArSdlFontCoverage_Run(int argc, char **argv);

#endif /* AR_SDL_FONT_COVERAGE_CLI_H */
