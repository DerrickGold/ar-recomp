#ifndef AR_EDITOR_VOXEL_PICK_H
#define AR_EDITOR_VOXEL_PICK_H

#include <stdbool.h>

struct DioramaVoxelPickFace;

typedef struct EditorVoxelPickHit {
  float world_x, world_y;
  float depth; /* Screen-linear NDC depth; smaller values are nearer. */
} EditorVoxelPickHit;

/* Uses the submitted quad's 0,1,2 / 0,2,3 triangulation. Native map coordinates
 * are perspective corrected, unlike NDC depth. Both windings are accepted.
 * A miss or malformed face leaves out_hit unchanged. */
bool EditorVoxelPick_Hit(const struct DioramaVoxelPickFace *face,
    float screen_x, float screen_y, EditorVoxelPickHit *out_hit);

#endif /* AR_EDITOR_VOXEL_PICK_H */
