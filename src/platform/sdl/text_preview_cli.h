#ifndef AR_SDL_TEXT_PREVIEW_CLI_H
#define AR_SDL_TEXT_PREVIEW_CLI_H
/* Bounded, ROM-free playback worker used by the Workshop and integration tests.
 * The application supplies private pack snapshots, never executable pack code.
 */
int ArSdlTextPreview_Run(int argc, char **argv);
#endif
