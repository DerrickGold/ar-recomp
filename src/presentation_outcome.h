#ifndef AR_PRESENTATION_OUTCOME_H
#define AR_PRESENTATION_OUTCOME_H

#include <stdbool.h>

/* Result shared by presentation stages that can lose optional polish while
 * still producing a complete, playable selected view.  The ordering is part
 * of the contract: combining stage results keeps the most severe outcome, so
 * a later successful or merely cosmetic stage can never erase a core failure. */
typedef enum PresentationOutcome {
  kPresentationOutcome_Complete = 0,
  kPresentationOutcome_OptionalOmitted = 1,
  kPresentationOutcome_CoreFailure = 2,
} PresentationOutcome;

static inline PresentationOutcome PresentationOutcome_Combine(
    PresentationOutcome first, PresentationOutcome second) {
  return first > second ? first : second;
}

static inline bool PresentationOutcome_IsUsable(PresentationOutcome outcome) {
  return outcome == kPresentationOutcome_Complete ||
      outcome == kPresentationOutcome_OptionalOmitted;
}

#endif /* AR_PRESENTATION_OUTCOME_H */
