#include "diorama_aperture.h"

#include <math.h>

static int MinimumInt(int left, int right) {
  return left < right ? left : right;
}

bool DioramaAperture_ConstrainGrid(
    ArRenderVertex2D *grid, const ArRenderVertex2D *aperture,
    int subdiv_x, int subdiv_y, float transition_cells) {
  if (!grid || !aperture || subdiv_x <= 0 || subdiv_y <= 0 ||
      !isfinite(transition_cells) || transition_cells < 0.0f)
    return false;

  const int vertex_count = (subdiv_x + 1) * (subdiv_y + 1);
  for (int i = 0; i < vertex_count; i++) {
    if (!isfinite(grid[i].position.x) || !isfinite(grid[i].position.y) ||
        !isfinite(aperture[i].position.x) ||
        !isfinite(aperture[i].position.y))
      return false;
  }

  for (int row = 0; row <= subdiv_y; row++) {
    for (int column = 0; column <= subdiv_x; column++) {
      const int horizontal_distance =
          MinimumInt(column, subdiv_x - column);
      const int vertical_distance = MinimumInt(row, subdiv_y - row);
      const int edge_distance =
          MinimumInt(horizontal_distance, vertical_distance);
      float aperture_weight = edge_distance == 0 ? 1.0f : 0.0f;
      if (edge_distance > 0 && transition_cells > 0.0f) {
        aperture_weight =
            1.0f - (float)edge_distance / transition_cells;
        if (aperture_weight < 0.0f) aperture_weight = 0.0f;
      }
      if (aperture_weight <= 0.0f) continue;

      const int index = row * (subdiv_x + 1) + column;
      grid[index].position.x +=
          (aperture[index].position.x - grid[index].position.x) *
          aperture_weight;
      grid[index].position.y +=
          (aperture[index].position.y - grid[index].position.y) *
          aperture_weight;
    }
  }
  return true;
}
