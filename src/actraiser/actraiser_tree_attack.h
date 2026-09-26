#ifndef AR_ACTRAISER_TREE_ATTACK_H
#define AR_ACTRAISER_TREE_ATTACK_H
/* ActRaiser tree-attack hooks: the tree enemy's preparation and controller,
 * using the regional motion rules' tree seeds.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_tree_attack_test.c */
#include "snesrecomp/game/cpu.h"
#include <stdbool.h>
bool ActRaiser_TreePrepareEntry(CpuState *cpu);
RecompReturn ActRaiser_TreePrepare(CpuState *cpu);
bool ActRaiser_TreeControllerEntry(CpuState *cpu);
RecompReturn ActRaiser_TreeController(CpuState *cpu);
#endif
