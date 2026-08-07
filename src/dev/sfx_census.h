#ifndef AR_SFX_CENSUS_H
#define AR_SFX_CENSUS_H

#include <stdint.h>

/* SFX census (AR_SFXCENSUS=1) — builds the sound-effect map that a music/SFX
 * volume split and per-source panning both depend on.
 *
 * The request path is already mapped (see the sfx-request-path-map notes):
 *
 *   LDA #id : BRK  ->  $035B  ->  NMI packs $035A/$035B into one 16-bit
 *                                 $2142/$2143 store on EVEN frames
 *                             ->  SPC driver keys a voice with some srcn
 *
 * The census observes both ends and joins them: SfxCensus_OnRequest records
 * the CPU-side request (id + calling recomp function + the register/actor
 * context that identifies WHERE the sound came from), and the DSP key-on seam
 * records what the driver actually played. Requests and key-ons are correlated
 * by APU-cycle proximity, since nothing in the protocol carries a tag through.
 *
 * Everything here is observation only — disabled unless AR_SFXCENSUS is set,
 * and it never influences audio output. */

void SfxCensus_Init(void);

/* CPU thread, from the BRK hook, before the id reaches the mailbox. */
void SfxCensus_OnRequest(uint8_t id, const char *fn, unsigned game_frame,
                         uint16_t cpu_x, uint16_t cpu_y);

/* Writes the report (stderr + <run-dir>/sfx_census.txt). Safe to call when
 * disabled or when nothing was captured. */
void SfxCensus_Report(void);

#endif /* AR_SFX_CENSUS_H */
