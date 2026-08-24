#include "presentation_outcome.h"

#include <stdio.h>

int main(void) {
  const PresentationOutcome outcomes[] = {
    kPresentationOutcome_Complete,
    kPresentationOutcome_OptionalOmitted,
    kPresentationOutcome_CoreFailure,
  };

  for (int first = 0; first < 3; first++) {
    for (int second = 0; second < 3; second++) {
      const PresentationOutcome combined = PresentationOutcome_Combine(
          outcomes[first], outcomes[second]);
      const PresentationOutcome expected =
          outcomes[first] > outcomes[second] ? outcomes[first]
                                             : outcomes[second];
      if (combined != expected) {
        fprintf(stderr,
                "presentation_outcome_test: combine(%d, %d) = %d, want %d\n",
                first, second, (int)combined, (int)expected);
        return 1;
      }
    }
  }

  if (!PresentationOutcome_IsUsable(kPresentationOutcome_Complete) ||
      !PresentationOutcome_IsUsable(
          kPresentationOutcome_OptionalOmitted) ||
      PresentationOutcome_IsUsable(kPresentationOutcome_CoreFailure) ||
      PresentationOutcome_IsUsable((PresentationOutcome)99)) {
    fputs("presentation_outcome_test: usability contract failed\n", stderr);
    return 1;
  }

  puts("presentation_outcome_test: PASS");
  return 0;
}
