#ifndef AR_DEV_AUTOMATION_H
#define AR_DEV_AUTOMATION_H
/* DevAutomation: developer actions scheduled from the environment, so headless
 * and replay runs can do at a chosen game frame what a developer would do by
 * hand: arm a diorama layer dump (AR_DIORAMA_DUMP_GF), write screenshots
 * (AR_SHOT_AT_GF, AR_SHOT_EVERY/FROM/TO, AR_SHOT_REQUIRE_COMPOSITE), fire the
 * configured warp (AR_WARP_AT), switch diorama mode on (AR_DIORAMA_AT), and
 * end the run (AR_QUIT_FRAMES). Each variable is read once.
 * Phase: host (main thread, between frames). */

#include <stdbool.h>

/* After the frame draw: arms the layer dump when this game frame is listed. */
void DevAutomation_ArmScheduledDioramaDump(void);
/* After the frame draw: writes the screenshot scheduled for this game frame. */
void DevAutomation_CaptureScheduledScreenshot(void);
/* After a batch of emulated ticks: fires the scheduled warp and diorama
 * switch once their game frame is reached. */
void DevAutomation_AfterTicks(void);
/* True once the run has reached AR_QUIT_FRAMES emulated frames. */
bool DevAutomation_ShouldQuit(void);

#endif  /* AR_DEV_AUTOMATION_H */
