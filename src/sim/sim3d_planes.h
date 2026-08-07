#ifndef SIM3D_PLANES_H
#define SIM3D_PLANES_H

/* Pitch-zero Mode-1 painter order, lowest rank first. The order is itself a
 * contract: the CPU reference compositor and later GPU geometry consume the
 * same indices, so priority changes cannot hide in two independent tables. */
typedef enum Sim3DPlane {
  kSim3DPlane_Bg3Low,
  kSim3DPlane_Obj0,
  kSim3DPlane_Obj1,
  kSim3DPlane_Bg2Low,
  kSim3DPlane_Bg1Low,
  kSim3DPlane_Obj2,
  kSim3DPlane_Bg2High,
  kSim3DPlane_Bg1High,
  kSim3DPlane_Obj3,
  kSim3DPlane_Bg3High,
  kSim3DPlane_Count,
} Sim3DPlane;

#endif  /* SIM3D_PLANES_H */
