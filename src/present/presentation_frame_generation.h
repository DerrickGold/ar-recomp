#ifndef PRESENTATION_FRAME_GENERATION_H
#define PRESENTATION_FRAME_GENERATION_H

#include <stdbool.h>
#include <stdint.h>

/* Renderer-independent analysis support for native-resolution frame
 * generation. This contract owns motion estimation, field sampling, and pair
 * phase only; production synthesis is the SDL geometry path in
 * diorama_frame_generation.c. Motion is estimated once for each completed
 * 60 Hz pair so any number of host presents can reuse it. The limits match the
 * largest action-layer capture. */
enum {
  /* A 16px field keeps native-plane analysis comfortably below the 60 Hz
   * budget while still resolving individual SNES metatiles and ordinary
   * sprite components. The synthesis mesh interpolates vectors between block
   * centres, so this is not a visible 16px staircase. */
  kPresentationFrameGenerationBlockSize = 16,
  kPresentationFrameGenerationSearchRadius = 7,
  kPresentationFrameGenerationMaximumWidth = 640,
  kPresentationFrameGenerationMaximumHeight = 352,
  kPresentationFrameGenerationMaximumBlocksX =
      (kPresentationFrameGenerationMaximumWidth +
       kPresentationFrameGenerationBlockSize - 1) /
      kPresentationFrameGenerationBlockSize,
  kPresentationFrameGenerationMaximumBlocksY =
      (kPresentationFrameGenerationMaximumHeight +
       kPresentationFrameGenerationBlockSize - 1) /
      kPresentationFrameGenerationBlockSize,
  kPresentationFrameGenerationMaximumBlocks =
      kPresentationFrameGenerationMaximumBlocksX *
      kPresentationFrameGenerationMaximumBlocksY,
};

/* A retained present without a meaningful sub-tick phase (menus, screenshots,
 * or interpolation disabled). Kept with the frame-generation contract rather
 * than a particular renderer so cadence code does not depend on Diorama's
 * former camera-scroll implementation. */
static const float kPresentationFrameGenerationPhaseNone = -1.0f;

typedef struct PresentationFrameGenerationMotionField {
  int width;
  int height;
  int blocks_x;
  int blocks_y;
  int8_t forward_dx[kPresentationFrameGenerationMaximumBlocks];
  int8_t forward_dy[kPresentationFrameGenerationMaximumBlocks];
  int8_t backward_dx[kPresentationFrameGenerationMaximumBlocks];
  int8_t backward_dy[kPresentationFrameGenerationMaximumBlocks];
  /* Global analysis produces one coherent vector. The renderer can submit a
   * single quad instead of tessellating an otherwise uniform block field. */
  bool uniform;
  bool valid;
} PresentationFrameGenerationMotionField;

typedef enum PresentationFrameGenerationAnalysisMode {
  /* Background and residual planes move coherently. A single robust vector is
   * both faster and less likely to lock onto a repeated tile one block away. */
  kPresentationFrameGenerationAnalysis_Global,
  /* OBJ priority planes can contain independently moving actors. */
  kPresentationFrameGenerationAnalysis_Blocks,
} PresentationFrameGenerationAnalysisMode;

/* Estimate A->B and B->A motion for packed ARGB8888 surfaces. Pitches are in
 * pixels. Empty blocks prefer zero motion; a small displacement penalty keeps
 * repeated pixel-art patterns from selecting needlessly distant matches.
 * Returns false when motion does not beat the stationary explanation by a
 * useful margin or the two directions disagree; callers must retain the exact
 * current endpoint in that case. */
bool PresentationFrameGeneration_Analyze(
    const uint32_t *previous, const uint32_t *current,
    int width, int height, int previous_pitch, int current_pitch,
    PresentationFrameGenerationAnalysisMode mode,
    PresentationFrameGenerationMotionField *field);

/* Bilinearly sample a field at a native pixel position. `forward` selects
 * A->B; false selects B->A. Used by the renderer-backed synthesis mesh. */
void PresentationFrameGeneration_MotionAt(
    const PresentationFrameGenerationMotionField *field,
    bool forward, int x, int y, float *out_dx, float *out_dy);

/* A captured pair can span multiple drained emulation ticks. Presentation is
 * deliberately one tick behind the newest completed state, so a k-tick pair
 * begins at (k-1)/k and reaches the current endpoint as the next tick lands. */
float PresentationFrameGeneration_PairPhase(float alpha,
                                             uint8_t capture_ticks);

#endif  /* PRESENTATION_FRAME_GENERATION_H */
